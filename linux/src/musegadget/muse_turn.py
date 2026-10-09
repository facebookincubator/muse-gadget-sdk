# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""One request a gadget sends to Muse, followed through Muse's chat event stream.

Create a :class:`MuseTurn` before sending the request, feed it every event from
``LinkSession.subscribe_chat``, and acknowledge it with the ``send_chat`` or
``send_voice`` response. It returns typed events for this request only:

* :class:`AgentStatus` when Muse reports public progress on the request.
* :class:`Reply` each time one of Muse's answer messages changes.
* :class:`TaskFinished` when Muse reports the request's task terminal.

Events that arrive before the acknowledgement are held and returned by
:meth:`MuseTurn.acknowledge`. Events from other chats, other requests, and
earlier turns (see :class:`ReplayScope`) are dropped. :meth:`MuseTurn.complete`
says when Muse is done answering.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass, replace
import re
import time
from types import MappingProxyType
from typing import Iterable, Mapping, Optional, Union


REPLY_QUIET_S = 3.0
"""Silence after which a turn without a terminal task status counts as answered."""
DETAIL_REPLY_QUIET_S = 30.0
"""Silence after which an unanswered follow-up stops holding the turn open."""
EMPTY_REPLY_GRACE_S = 1.0
"""Wait after a terminal task with blank replies, for reordered text to arrive."""

_ACTIVITY_CODES = frozenset(("working", "responding", "idle", "online", "thinking", "researching",
                             "planning", "listening", "speaking", "processing", "generating", "executing"))
_TERMINAL_TASK_STATUSES = frozenset(("completed", "failed", "errored", "cancelled", "canceled"))
_STATUS_EVENTS = ("agent.status", "task.status")
_MESSAGE_EVENTS = ("delta.message_start", "delta.text_append", "delta.message_done", "message.assistant")
_MAX_ID_BYTES = 512
_MAX_PENDING_EVENTS = 64
_MAX_TEXT_BYTES = 64 * 1024
_MAX_MESSAGES = 32
_MAX_OBSERVED_IDS = 256
_MAX_IGNORED_MESSAGES = 256
# C0 and C1 controls except tab and newline, so reply text cannot drive a terminal.
_CONTROL_CHARACTERS = re.compile("[\x00-\x08\x0b-\x1f\x7f-\x9f]")


def _strip_controls(text: str) -> str:
    return _CONTROL_CHARACTERS.sub("", text)


@dataclass(frozen=True)
class AgentStatus:
    """Muse's public status for this request; either field is None when absent.

    ``activity_text`` has its control characters removed, except tab and newline.
    """

    activity_code: Optional[str]
    activity_text: Optional[str]


@dataclass(frozen=True)
class Reply:
    """One assistant message answering the request, as it stands after the latest event.

    ``text`` is the best current text: the streamed deltas, replaced by Muse's final
    text when it sends one. ``streamed`` is the deltas alone, in arrival order.
    ``bound`` is True once the message is proven to answer this request (it arrived
    on the gadget's own chat, or its parent chain reaches the request) rather than
    merely carrying no parent on a shared chat. Both texts have their control
    characters removed, except tab and newline.
    """

    message_id: str
    text: str = ""
    streamed: str = ""
    done: bool = False
    bound: bool = False


@dataclass(frozen=True)
class TaskFinished:
    """Muse reported the request's task terminal."""


TurnEvent = Union[AgentStatus, Reply, TaskFinished]


class ReplayScope:
    """Chat IDs retired by earlier turns on one connection, bounded to ``limit`` entries.

    Share one scope across the turns of a gadget that owns its chat: events carrying
    a retired ID are replays and never reach a later turn.
    """

    def __init__(self, limit: int = 2048):
        self.limit = limit
        self.ids: set[tuple[str, str]] = set()
        self.order: deque[tuple[str, str]] = deque()

    @staticmethod
    def event_ids(event: dict, *, include_parents: bool = True) -> set[tuple[str, str]]:
        payload = event.get("payload")
        if not isinstance(payload, dict):
            return set()
        result = set()
        keys = ("message_id", "id", "task_id")
        if include_parents:
            keys += ("reply_to_message_id", "parent_message_id")
        for key in keys:
            value = payload.get(key)
            if isinstance(value, str) and value:
                if len(value) > _MAX_ID_BYTES:
                    raise ValueError("Muse returned an oversized chat identifier")
                result.add(("task" if key == "task_id" else "message", value))
        value = event.get("message_id")
        if isinstance(value, str) and value:
            if len(value) > _MAX_ID_BYTES:
                raise ValueError("Muse returned an oversized chat identifier")
            result.add(("message", value))
        return result

    def retire(self, ids: Iterable[tuple[str, str]]) -> None:
        for identifier in ids:
            if identifier not in self.ids:
                self.ids.add(identifier)
                self.order.append(identifier)
        while len(self.order) > self.limit:
            self.ids.discard(self.order.popleft())

    def retire_request(self, response: dict) -> None:
        """Retire the user message of a request no turn will follow, such as a stop request."""
        self.retire(("message", value) for value in _acknowledged_message_ids(response))

    def observe_idle(self, event: dict, session_id: Optional[str]) -> None:
        """Retire the IDs of an event seen on ``session_id`` while no turn was open."""
        payload = event.get("payload")
        if (event.get("type") == "event" and isinstance(payload, dict) and session_id
                and (payload.get("session_id") or event.get("session_id")) == session_id
                and event.get("event") in _STATUS_EVENTS + _MESSAGE_EVENTS):
            self.retire(self.event_ids(event))


def _acknowledged_message_ids(response: dict) -> set[str]:
    result = response.get("result", response)
    if not isinstance(result, dict):
        raise ValueError("Muse returned an invalid chat acknowledgement")
    user_ids = {result[k] for k in ("message_id", "reply_to_message_id")
                if isinstance(result.get(k), str) and result[k]}
    if not user_ids:
        raise ValueError("Muse chat acknowledgement omitted the user message ID")
    return user_ids


def transcript_text(value, message_id: Optional[str] = None) -> str:
    """Read only assistant text content from Muse's structured transcript."""
    if isinstance(value, str):
        return value
    if not isinstance(value, dict):
        return ""
    messages = value.get("messages", [])
    if not isinstance(messages, list):
        return ""
    assistants = [message for message in messages
                  if isinstance(message, dict) and message.get("role") == "assistant"]
    if message_id is not None:
        assistants = [message for message in assistants if message.get("id") == message_id]
    if not assistants:
        return ""
    content = assistants[-1].get("content", [])
    if not isinstance(content, list):
        return ""
    texts = [item["text"] for item in content
             if isinstance(item, dict) and item.get("type") == "text" and isinstance(item.get("text"), str)]
    return "\n".join(texts)


class MuseTurn:
    """Muse's work on one request, bound to the request's acknowledged user message.

    ``session_id`` is the chat the request goes to; None learns it from the
    acknowledgement. ``owns_chat`` means the gadget owns that chat, so every event on
    it belongs to the current turn, and the acknowledgement must confirm the chat as
    a thread; ``replay_scope`` then filters replays of earlier turns. Assistant
    messages without a parent that first appear before ``ignore_parentless_until``
    (a ``time.monotonic()`` value) are ignored, for example answers to a stop request.

    Raises ValueError when Muse's events exceed this turn's size bounds.

    Read-only state:

    * ``acknowledged``: the request's user message is known, so events bind as they arrive.
    * ``busy``: Muse reports work in progress on the request.
    * ``task_finished``: Muse reported the request's task terminal, and no other task of it runs.
    * ``activity_code``: Muse's latest agent activity code, or ``"other"`` for an unknown code.
    * ``last_activity``: the ``time.monotonic()`` of the last event that changed this turn.
    """

    def __init__(self, session_id: Optional[str] = None, *, owns_chat: bool = False,
                 replay_scope: Optional[ReplayScope] = None, ignore_parentless_until: float = 0.0):
        self.session_id = session_id
        self.owns_chat = owns_chat
        self.acknowledged = False
        self.busy = False
        self.task_finished = False
        self.activity_code: Optional[str] = None
        self.last_activity = time.monotonic()
        self._replay_scope = replay_scope
        self._ignore_parentless_until = ignore_parentless_until
        self._user_ids: set[str] = set()
        self._replies: dict[str, Reply] = {}
        self._pending: list[dict] = []
        self._pending_text_bytes = 0
        self._observed_ids: set[tuple[str, str]] = set()
        self._running_task_ids: set[str] = set()
        self._unanswered_follow_up_ids: set[str] = set()
        self._follow_up_task_ids: set[str] = set()
        self._ignored_messages: dict[str, None] = {}

    @property
    def replies(self) -> Mapping[str, Reply]:
        """Every assistant message bound to the request, by message ID, in arrival order."""
        return MappingProxyType(self._replies)

    def acknowledge(self, response: dict, *, follow_up: bool = False) -> list[TurnEvent]:
        """Bind the turn to the user message in Muse's acknowledgement; returns the held events.

        With ``follow_up``, the acknowledged message joins the earlier ones, and the
        turn stays open until a reply to it is done, a task started after it finishes,
        or ``DETAIL_REPLY_QUIET_S`` passes quietly.
        """
        user_ids = _acknowledged_message_ids(response)
        if self.owns_chat and any(len(value) > _MAX_ID_BYTES for value in user_ids):
            raise ValueError("Muse returned an oversized chat identifier")
        result = response.get("result", response)
        if follow_up:
            self._user_ids = self._user_ids | user_ids
            self._unanswered_follow_up_ids |= user_ids
            self.task_finished = False
            self.last_activity = time.monotonic()
        else:
            self._user_ids = user_ids
        acknowledged_session = result.get("session_id")
        if self.owns_chat and (not self.session_id or acknowledged_session != self.session_id
                               or result.get("is_thread") is not True):
            raise ValueError("Muse did not acknowledge the gadget's dedicated side chat")
        if self.session_id is None and isinstance(acknowledged_session, str):
            self.session_id = acknowledged_session
        self.acknowledged = True
        held = []
        pending, self._pending = self._pending, []
        self._pending_text_bytes = 0
        for event in pending:
            held.extend(self.feed(event))
        return held

    def expect_follow_up(self) -> None:
        """Hold events until :meth:`acknowledge` binds one more user message to this request."""
        self.acknowledged = False

    def feed(self, event: dict) -> list[TurnEvent]:
        """Read one raw chat event; returns what it means for this request, often nothing."""
        if event.get("type") != "event":
            return []
        payload = event.get("payload")
        if not isinstance(payload, dict):
            return []
        event_session = payload.get("session_id") or event.get("session_id")
        if self.owns_chat:
            if not self.session_id or event_session != self.session_id:
                return []
            identifiers = ReplayScope.event_ids(event)
            if self._replay_scope is not None:
                own_ids = ReplayScope.event_ids(event, include_parents=False)
                if own_ids & self._replay_scope.ids:
                    return []
                parents = identifiers - own_ids
                authorized_parents = {("message", value) for value in self._user_ids}
                if self.acknowledged and (parents - authorized_parents) & self._replay_scope.ids:
                    return []
            self._observed_ids.update(identifiers)
            if len(self._observed_ids) > _MAX_OBSERVED_IDS:
                raise ValueError("Muse returned too many activity identifiers for one turn")
        if event_session and self.session_id and event_session != self.session_id:
            return []
        if not self.acknowledged:
            if len(self._pending) >= _MAX_PENDING_EVENTS:
                raise ValueError("too many chat events before Muse acknowledged the turn")
            self._pending_text_bytes += sum(len(payload[key].encode("utf-8")) for key in
                                            ("text", "display_text", "content", "transcript")
                                            if isinstance(payload.get(key), str))
            if self._pending_text_bytes > _MAX_TEXT_BYTES:
                raise ValueError("Muse returned more than 64 KiB of pending reply text")
            self._pending.append(event)
            return []
        scoped = bool(self.owns_chat and self.session_id and event_session == self.session_id)
        name = event.get("event")
        if name in _STATUS_EVENTS:
            return self._status(name, payload, scoped)
        if name in _MESSAGE_EVENTS:
            return self._message(name, event, payload, scoped)
        return []

    def abandon_reply(self, message_id: str) -> None:
        """Stop waiting for one reply, such as one the gadget cannot read: it counts as done."""
        self._replies[message_id] = replace(self._replies[message_id], done=True)

    def complete(self, now: float) -> bool:
        """Muse is done answering: every reply is done and Muse is idle, finished or quiet."""
        return (not self.busy
                and (not self._unanswered_follow_up_ids or now - self.last_activity >= DETAIL_REPLY_QUIET_S)
                and all(reply.done for reply in self._replies.values())
                and (self.task_finished or now - self.last_activity >= REPLY_QUIET_S))

    def settled(self, now: float) -> bool:
        """Muse finished the task, every reply is done, and no late text arrived within the grace period.

        A settled turn whose replies hold nothing to present is an empty answer.
        """
        return (self.task_finished and not self.busy and not self._unanswered_follow_up_ids
                and all(reply.done for reply in self._replies.values())
                and now - self.last_activity >= (EMPTY_REPLY_GRACE_S if self._replies else REPLY_QUIET_S))

    def retire(self) -> None:
        """Retire every ID this turn saw into its replay scope, once the turn is over."""
        if self.owns_chat and self._replay_scope is not None:
            self._replay_scope.retire(self._observed_ids)
            self._replay_scope.retire(("message", value) for value in self._user_ids)
            for event in self._pending:
                self._replay_scope.retire(ReplayScope.event_ids(event))

    def _status(self, name: str, payload: dict, scoped: bool) -> list[TurnEvent]:
        activity = payload.get("activity_code")
        status = payload.get("status")
        parent = payload.get("reply_to_message_id") or payload.get("parent_message_id")
        message_id = payload.get("message_id")
        reply = self._replies.get(message_id) if isinstance(message_id, str) else None
        explicitly_linked = parent in self._user_ids or (reply is not None and reply.bound)
        terminal = isinstance(status, str) and status in _TERMINAL_TASK_STATUSES
        was_finished = self.task_finished
        task_id = payload.get("task_id")
        if not isinstance(task_id, str):
            task_id = None
        if self.owns_chat and name == "task.status":
            if terminal and task_id not in self._running_task_ids and not explicitly_linked:
                return []
            if status == "running" and task_id:
                if self._unanswered_follow_up_ids and task_id not in self._running_task_ids:
                    self._follow_up_task_ids.add(task_id)
                self._running_task_ids.add(task_id)
            if terminal:
                self._running_task_ids.discard(task_id)
        if terminal and (parent in self._unanswered_follow_up_ids or task_id in self._follow_up_task_ids):
            self._unanswered_follow_up_ids.discard(parent)
            if task_id in self._follow_up_task_ids:
                self._unanswered_follow_up_ids.clear()
        if name == "task.status" and isinstance(status, str):
            self.task_finished = terminal and not (self.owns_chat and self._running_task_ids)
        if isinstance(activity, str):
            self.busy = activity not in ("", "online", "idle") and activity not in _TERMINAL_TASK_STATUSES
            if name == "agent.status":
                self.activity_code = activity if activity in _ACTIVITY_CODES else "other"
        elif isinstance(status, str):
            self.busy = status not in ("", "idle") and not terminal
        if self.owns_chat and self._running_task_ids:
            self.task_finished = False
            self.busy = True
        elif name == "task.status" and self.task_finished:
            self.busy = False
        self.last_activity = time.monotonic()
        if name == "agent.status" and (scoped or explicitly_linked) and not self.task_finished:
            text = payload.get("activity_text")
            return [AgentStatus(activity if isinstance(activity, str) else None,
                                _strip_controls(text) if isinstance(text, str) else None)]
        return [TaskFinished()] if self.task_finished and not was_finished else []

    def _message(self, name: str, event: dict, payload: dict, scoped: bool) -> list[TurnEvent]:
        message_id = payload.get("message_id") or event.get("message_id") or payload.get("id")
        if not isinstance(message_id, str) or not message_id or payload.get("role") == "user":
            return []
        parent = payload.get("reply_to_message_id") or payload.get("parent_message_id")
        reply = self._replies.get(message_id)
        if (parent and parent not in self._user_ids and parent not in self._replies
                and not (scoped and parent == message_id)):
            if reply is None:
                self._ignore(message_id)
            return []
        if reply is None:
            if message_id in self._ignored_messages or (
                    not parent and time.monotonic() < self._ignore_parentless_until):
                self._ignore(message_id)
                return []
            if len(self._replies) >= _MAX_MESSAGES:
                raise ValueError("Muse returned too many messages for one request")
            reply = Reply(message_id)
        bound = reply.bound or scoped
        if parent:
            parent_reply = self._replies.get(parent)
            bound = scoped or parent in self._user_ids or (parent_reply is not None and parent_reply.bound)
        text, streamed, done = reply.text, reply.streamed, reply.done
        self.last_activity = time.monotonic()
        if name == "delta.text_append" and payload.get("text") and not done:
            if isinstance(payload["text"], str):
                delta = _strip_controls(payload["text"])
                text += delta
                streamed += delta
        if name in ("delta.message_done", "message.assistant"):
            completed_text = payload.get("display_text")
            if not isinstance(completed_text, str):
                transcript = payload.get("transcript")
                completed_text = (transcript if isinstance(transcript, str)
                                  else transcript_text(transcript, message_id))
                if not completed_text and not isinstance(transcript, str):
                    completed_text = payload.get("content")
            if isinstance(completed_text, str):
                text = _strip_controls(completed_text)
            if name == "delta.message_done" or payload.get("display_text_ready") is not False:
                done = True
                self._unanswered_follow_up_ids.discard(parent)
        reply = self._replies[message_id] = Reply(message_id, text, streamed, done, bound)
        if (len(text.encode("utf-8")) > _MAX_TEXT_BYTES
                or len(streamed.encode("utf-8")) > _MAX_TEXT_BYTES):
            raise ValueError("Muse returned more than 64 KiB of reply text")
        return [reply]

    def _ignore(self, message_id: str) -> None:
        """Drop every later event of ``message_id``, even one that names no parent."""
        self._ignored_messages.pop(message_id, None)
        self._ignored_messages[message_id] = None
        if len(self._ignored_messages) > _MAX_IGNORED_MESSAGES:
            del self._ignored_messages[next(iter(self._ignored_messages))]
