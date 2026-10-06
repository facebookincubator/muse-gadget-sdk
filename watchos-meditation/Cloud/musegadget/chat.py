# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).

"""Bounded NDJSON subscription and correlation for device-originated chat turns."""
from __future__ import annotations

import asyncio
import json
import time

MAX_EVENT_BYTES = 1024 * 1024
MAX_PENDING_EVENTS = 256
MAX_REPLY_BYTES = 1024 * 1024


class SubscriptionHTTPError(ConnectionError):
    def __init__(self, status):
        self.status = status
        super().__init__(f"Muse chat subscription: HTTP {status}")


class Subscription:
    """Receives /chat/subscribe frames without blocking the control stream."""

    def __init__(self):
        self.ready = asyncio.get_running_loop().create_future()
        self.done = asyncio.get_running_loop().create_future()
        self.events = asyncio.Queue(maxsize=MAX_PENDING_EVENTS)
        self.buffer = bytearray()

    def fail(self, error):
        # Completion is a value so a disconnected, abandoned subscriber never
        # leaves an unobserved Future exception behind.
        if not self.ready.done():
            self.ready.set_result(error)
        if not self.done.done():
            self.done.set_result(error)

    def on_frame(self, frame):
        if self.done.done():
            return
        if frame.kind == "reset":
            self.fail(ConnectionError("Muse chat subscription was reset"))
            return
        if frame.kind == "response":
            if frame.value.status != 200:
                self.fail(SubscriptionHTTPError(frame.value.status))
                return
            if not self.ready.done():
                self.ready.set_result(None)
            data, ended = frame.value.body, frame.value.end_body
        else:
            data, ended = frame.value.data, frame.value.end_body
        self.buffer.extend(data)
        while b"\n" in self.buffer:
            raw, _, remaining = self.buffer.partition(b"\n")
            self.buffer = bytearray(remaining)
            if len(raw) > MAX_EVENT_BYTES:
                self.fail(ValueError("Muse chat event is too large"))
                return
            if not raw.strip():
                continue
            try:
                event = json.loads(raw)
            except (ValueError, UnicodeError):
                continue
            if isinstance(event, dict) and event.get("type") == "event":
                try:
                    self.events.put_nowait(event)
                except asyncio.QueueFull:
                    self.fail(ValueError("Muse chat subscription overflow"))
                    return
        if len(self.buffer) > MAX_EVENT_BYTES:
            self.fail(ValueError("Muse chat event is too large"))
        elif ended:
            self.fail(ConnectionError("Muse chat subscription ended"))

    async def next_event(self, timeout):
        queued = asyncio.ensure_future(self.events.get())
        try:
            ready, _ = await asyncio.wait({queued, self.done}, timeout=timeout,
                                          return_when=asyncio.FIRST_COMPLETED)
            if queued in ready:
                return queued.result()
            if self.done in ready:
                raise self.done.result()
            raise asyncio.TimeoutError()
        finally:
            if not queued.done():
                queued.cancel()


class Turn:
    """Filter unrelated chats; coalesce streamed and persisted assistant text."""

    def __init__(self, ack):
        if not isinstance(ack, dict):
            raise ValueError("Muse returned an invalid chat acknowledgement")
        result = ack.get("result", ack)
        if not isinstance(result, dict) or not isinstance(result.get("message_id"), str):
            raise ValueError("Muse chat acknowledgement has no message id")
        self.parents = {result["message_id"]}
        if isinstance(result.get("reply_to_message_id"), str):
            self.parents.add(result["reply_to_message_id"])
        self.session_id = result.get("session_id")
        self.messages = {}
        self.completed = set()
        self.busy = False
        self.last_seq = 0
        self.last_activity = time.monotonic()

    @property
    def settled(self):
        return bool(self.completed) and set(self.messages) <= self.completed and not self.busy

    def feed(self, event):
        seq = event.get("seq")
        if isinstance(seq, int) and seq > 0:
            if seq <= self.last_seq:
                return None
            self.last_seq = seq
        name = event.get("event_name", event.get("event"))
        payload = event.get("payload")
        if not isinstance(payload, dict):
            return None
        session_id = payload.get("session_id")
        same_session = isinstance(self.session_id, str) and session_id == self.session_id
        if isinstance(session_id, str) and isinstance(self.session_id, str) and not same_session:
            return None
        if name in ("agent.status", "task.status"):
            code, status = payload.get("activity_code"), payload.get("status")
            if isinstance(code, str):
                self.busy = bool(code) and code not in ("online", "idle")
            elif isinstance(status, str):
                self.busy = bool(status) and status not in ("completed", "failed")
            self.last_activity = time.monotonic()
            return {"type": "status", "busy": self.busy}
        if name not in ("delta.message_start", "delta.text_append", "delta.message_done", "message.assistant"):
            return None
        mid = payload.get("message_id", payload.get("id", event.get("message_id")))
        parent = payload.get("reply_to_message_id") or payload.get("parent_message_id")
        # Current Muse starts omit a reply parent; text_append.parent_message_id
        # names the assistant message itself. Bind such starts only inside the
        # acknowledged session, then follow subsequent deltas by message id.
        related = isinstance(parent, str) and (parent in self.parents or parent in self.messages)
        scoped_start = name == "delta.message_start" and same_session and not parent
        if not isinstance(mid, str) or (mid not in self.messages and not related and not scoped_start):
            return None
        if payload.get("role") in ("user", "system", "tool"):
            return None
        previous = self.messages.setdefault(mid, "")
        text = next((payload[key] for key in ("display_text", "content", "text")
                     if isinstance(payload.get(key), str)), "")
        if name == "delta.text_append":
            self.messages[mid] = previous + text
        elif name == "delta.message_start":
            self.completed.discard(mid)
        elif name == "delta.message_done" or payload.get("display_text_ready", True):
            if text:
                self.messages[mid] = text
            self.completed.add(mid)
        else:
            return None
        if sum(len(value.encode("utf-8")) for value in self.messages.values()) > MAX_REPLY_BYTES:
            raise ValueError("Muse reply is too large")
        self.last_activity = time.monotonic()
        return {"type": "reply", "message_id": mid, "text": self.messages[mid],
                "complete": mid in self.completed}
