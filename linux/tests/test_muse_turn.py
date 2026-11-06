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

import time

import pytest

from musegadget.muse_turn import AgentStatus, MuseTurn, ReplayScope, Reply, TaskFinished, transcript_text


def event(name, message_id=None, *, parent=None, session="chat", **payload):
    data = {"session_id": session, **payload}
    if message_id is not None:
        data["message_id"] = message_id
    if parent is not None:
        data["reply_to_message_id"] = parent
    return {"type": "event", "event": name, "payload": data}


def shared_turn(**options):
    turn = MuseTurn("chat", **options)
    assert turn.acknowledge({"result": {"message_id": "u1", "session_id": "chat"}}) == []
    return turn


def side_ack(user_id):
    return {"result": {"message_id": user_id, "session_id": "side", "is_thread": True}}


def side_turn(scope=None, user_id="u1"):
    turn = MuseTurn("side", owns_chat=True, replay_scope=scope)
    assert turn.acknowledge(side_ack(user_id)) == []
    return turn


def test_events_before_the_acknowledgement_are_held_and_returned_in_order():
    turn = MuseTurn("chat")
    assert turn.feed(event("delta.text_append", "r1", parent="u1", text="Hel")) == []
    assert turn.feed(event("delta.text_append", "r1", parent="u1", text="lo")) == []
    assert turn.feed(event("delta.message_done", "r1", parent="u1", content="Hello.")) == []
    assert turn.acknowledge({"result": {"message_id": "u1"}}) == [
        Reply("r1", "Hel", "Hel", done=False, bound=True),
        Reply("r1", "Hello", "Hello", done=False, bound=True),
        Reply("r1", "Hello.", "Hello", done=True, bound=True),
    ]
    assert dict(turn.replies) == {"r1": Reply("r1", "Hello.", "Hello", done=True, bound=True)}


def test_acknowledgement_teaches_an_unknown_session():
    turn = MuseTurn()
    turn.acknowledge({"message_id": "u1", "session_id": "learned"})
    assert turn.session_id == "learned"
    assert turn.feed(event("delta.message_done", "r1", parent="u1", session="other", content="No")) == []
    assert turn.feed(event("delta.message_done", "r1", parent="u1", session="learned", content="Yes")) == [
        Reply("r1", "Yes", "", done=True, bound=True)]


def test_other_chats_requests_and_event_kinds_are_dropped():
    turn = shared_turn()
    for raw in [
        event("delta.message_done", "r1", parent="u1", session="phone-chat", content="Other chat"),
        event("delta.message_done", "r2", parent="someone-else", content="Other request"),
        event("message.assistant", "u1", parent="u1", role="user", content="My own question"),
        event("message.tool", "t1", parent="u1", content="Tool output"),
        {"type": "ack", "payload": {"message_id": "r3"}},
        {"type": "event", "event": "delta.message_done", "payload": None},
    ]:
        assert turn.feed(raw) == []
    assert dict(turn.replies) == {}


def test_parentless_reply_on_a_shared_chat_is_read_but_not_bound():
    turn = shared_turn()
    assert turn.feed(event("delta.message_done", "r1", content="Hi")) == [Reply("r1", "Hi", "", done=True, bound=False)]
    assert turn.feed(event("delta.message_done", "r2", parent="r1", content="More")) == [
        Reply("r2", "More", "", done=True, bound=False)]


def test_parentless_replies_inside_the_ignore_window_stay_ignored():
    turn = shared_turn(ignore_parentless_until=time.monotonic() + 60)
    assert turn.feed(event("delta.message_done", "stop-answer", content="Stopped.")) == []
    assert turn.feed(event("delta.message_done", "stop-answer", parent="u1", content="Stopped.")) == []
    assert turn.feed(event("delta.message_done", "r1", parent="u1", content="Answer")) == [
        Reply("r1", "Answer", "", done=True, bound=True)]
    assert list(turn.replies) == ["r1"]


def test_reply_first_seen_answering_another_request_stays_dropped():
    turn = shared_turn()
    assert turn.feed(event("delta.text_append", "r2", parent="someone-else", text="Other ")) == []
    assert turn.feed(event("delta.text_append", "r2", text="answer")) == []
    assert turn.feed(event("delta.message_done", "r2", parent="r2", content="Other answer")) == []
    assert turn.feed(event("delta.message_done", "r1", parent="u1", content="Mine")) == [
        Reply("r1", "Mine", "", done=True, bound=True)]
    assert list(turn.replies) == ["r1"]


def test_reply_and_status_text_lose_control_characters():
    turn = shared_turn()
    assert turn.feed(event("agent.status", parent="u1", activity_code="working",
                           activity_text="Look\x1b[2Jing\r")) == [AgentStatus("working", "Look[2Jing")]
    assert turn.feed(event("delta.text_append", "r1", parent="u1", text="Red \x1b[31malert\x07\n")) == [
        Reply("r1", "Red [31malert\n", "Red [31malert\n", done=False, bound=True)]
    assert turn.feed(event("delta.message_done", "r1", parent="u1", content="Done\x9b\x00.\tOK\n")) == [
        Reply("r1", "Done.\tOK\n", "Red [31malert\n", done=True, bound=True)]


def test_replays_of_an_earlier_turn_never_reach_the_next_one():
    scope = ReplayScope()
    first = side_turn(scope)
    first.feed(event("task.status", session="side", task_id="t1", status="running"))
    first.feed(event("delta.message_done", "r1", session="side", content="First answer"))
    first.retire()
    second = side_turn(scope, user_id="u2")
    assert second.feed(event("delta.message_done", "r1", session="side", content="First answer")) == []
    assert second.feed(event("task.status", session="side", task_id="t1", status="completed")) == []
    assert second.feed(event("delta.message_done", "r9", parent="u1", session="side", content="Late")) == []
    assert second.feed(event("delta.message_done", "r2", session="side", content="Second answer")) == [
        Reply("r2", "Second answer", "", done=True, bound=True)]
    assert not second.task_finished


def test_retired_stop_request_and_idle_events_are_replays():
    scope = ReplayScope()
    scope.retire_request({"result": {"message_id": "stop-1"}})
    scope.observe_idle(event("agent.status", "w1", session="side", activity_code="working"), "side")
    turn = side_turn(scope, user_id="u2")
    assert turn.feed(event("delta.message_done", "r1", parent="stop-1", session="side", content="Stopped")) == []
    assert turn.feed(event("agent.status", "w1", session="side", activity_code="working")) == []
    assert not turn.busy


def test_linked_status_is_reported_as_sent_and_unlinked_status_only_marks_busy():
    turn = shared_turn()
    assert turn.feed(event("agent.status", activity_code="working", activity_text="Searching web")) == []
    assert turn.busy
    assert turn.feed(event("agent.status", parent="u1", activity_code="dancing",
                           activity_text="Searching web")) == [AgentStatus("dancing", "Searching web")]
    assert turn.activity_code == "other"
    assert turn.feed(event("agent.status", parent="u1", activity_code="idle", activity_text=7)) == [
        AgentStatus("idle", None)]
    assert not turn.busy and turn.activity_code == "idle"


def test_side_chat_status_needs_no_parent():
    turn = side_turn()
    assert turn.feed(event("agent.status", session="side", activity_code="researching",
                           activity_text="Comparing options")) == [AgentStatus("researching", "Comparing options")]


def test_task_finished_after_the_reply_completes_at_once():
    turn = shared_turn()
    assert turn.feed(event("delta.message_start", "r1", parent="u1")) == [Reply("r1", bound=True)]
    assert not turn.complete(turn.last_activity + 10)
    turn.feed(event("delta.message_done", "r1", parent="u1", content="Answer"))
    assert not turn.complete(turn.last_activity + 2.9)
    assert turn.complete(turn.last_activity + 3.01)
    assert turn.feed(event("task.status", parent="u1", status="completed")) == [TaskFinished()]
    assert turn.complete(turn.last_activity)


def test_task_finished_before_the_reply_waits_for_late_text():
    turn = shared_turn()
    assert turn.feed(event("task.status", parent="u1", status="completed")) == [TaskFinished()]
    assert not turn.settled(turn.last_activity + 2.9)
    assert turn.settled(turn.last_activity + 3.01)
    assert turn.feed(event("delta.message_start", "r1", parent="u1")) == [Reply("r1", bound=True)]
    assert not turn.settled(turn.last_activity + 10) and not turn.complete(turn.last_activity + 10)
    turn.feed(event("delta.message_done", "r1", parent="u1", content=" "))
    assert not turn.settled(turn.last_activity + 0.9)
    assert turn.settled(turn.last_activity + 1.01)
    assert turn.feed(event("task.status", parent="u1", status="completed")) == []


def test_busy_agent_holds_the_turn_open_past_the_quiet_period():
    turn = shared_turn()
    turn.feed(event("delta.message_done", "r1", parent="u1", content="Answer"))
    turn.feed(event("agent.status", parent="u1", activity_code="working"))
    assert not turn.complete(turn.last_activity + 60)
    turn.feed(event("agent.status", parent="u1", activity_code="idle"))
    assert turn.complete(turn.last_activity + 3.01)


def test_side_chat_turn_finishes_when_its_last_running_task_does():
    turn = side_turn()
    assert turn.feed(event("task.status", session="side", task_id="startup", status="completed")) == []
    turn.feed(event("task.status", session="side", task_id="root", status="running"))
    turn.feed(event("task.status", session="side", task_id="sub", status="running"))
    assert turn.feed(event("task.status", session="side", task_id="sub", status="cancelled")) == []
    assert turn.busy and not turn.task_finished
    assert turn.feed(event("task.status", session="side", task_id="root", status="completed")) == [TaskFinished()]
    assert not turn.busy and turn.complete(turn.last_activity)


def test_follow_up_without_its_own_reply_stays_open_until_a_later_task_or_long_quiet():
    turn = side_turn()
    turn.feed(event("task.status", session="side", task_id="t1", status="running"))
    turn.expect_follow_up()
    turn.acknowledge(side_ack("u2"), follow_up=True)
    turn.feed(event("delta.message_done", "r1", parent="u1", session="side", content="Answer one"))
    turn.feed(event("task.status", session="side", task_id="t1", status="completed"))
    assert not turn.complete(turn.last_activity + 29.9)
    assert turn.complete(turn.last_activity + 30.01)
    turn.feed(event("task.status", session="side", task_id="t2", status="running"))
    assert not turn.complete(turn.last_activity + 60)
    assert turn.feed(event("task.status", session="side", task_id="t2", status="completed")) == [TaskFinished()]
    assert turn.complete(turn.last_activity)


def test_display_text_waits_for_readiness_and_text_after_done_is_ignored():
    turn = shared_turn()
    assert turn.feed(event("message.assistant", "r1", parent="u1", content="Draft", display_text_ready=False)) == [
        Reply("r1", "Draft", "", done=False, bound=True)]
    assert turn.feed(event("message.assistant", "r1", parent="u1", display_text="Final")) == [
        Reply("r1", "Final", "", done=True, bound=True)]
    assert turn.feed(event("delta.text_append", "r1", parent="u1", text=" extra")) == [
        Reply("r1", "Final", "", done=True, bound=True)]


def test_completed_message_reads_its_own_text_from_a_structured_transcript():
    transcript = {"messages": [
        {"id": "r0", "role": "assistant", "content": [{"type": "text", "text": "Prior answer"}]},
        {"id": "r1", "role": "assistant", "content": [{"type": "text", "text": "Current answer"}]},
    ]}
    turn = shared_turn()
    assert turn.feed(event("delta.message_done", "r1", parent="u1", transcript=transcript)) == [
        Reply("r1", "Current answer", "", done=True, bound=True)]


def test_abandoned_reply_counts_as_done():
    turn = shared_turn()
    turn.feed(event("delta.text_append", "r1", parent="u1", text="{broken"))
    turn.feed(event("task.status", parent="u1", status="completed"))
    assert not turn.complete(turn.last_activity + 10)
    turn.abandon_reply("r1")
    assert dict(turn.replies) == {"r1": Reply("r1", "{broken", "{broken", done=True, bound=True)}
    assert turn.complete(turn.last_activity)


@pytest.mark.parametrize("session, owns_chat, response, problem", [
    ("side", True, {"result": {"message_id": "u1", "session_id": "chat", "is_thread": True}},
     "dedicated side chat"),
    ("side", True, {"result": {"message_id": "u1", "session_id": "side", "is_thread": False}},
     "dedicated side chat"),
    ("chat", False, {"message_id": ""}, "acknowledgement"),
])
def test_unusable_acknowledgement_is_rejected(session, owns_chat, response, problem):
    with pytest.raises(ValueError, match=problem):
        MuseTurn(session, owns_chat=owns_chat).acknowledge(response)


def test_held_events_and_reply_text_are_bounded():
    turn = MuseTurn("chat")
    for _ in range(64):
        turn.feed(event("delta.message_start", "r1", parent="u1"))
    with pytest.raises(ValueError, match="too many chat events"):
        turn.feed(event("delta.message_start", "r1", parent="u1"))
    with pytest.raises(ValueError, match="64 KiB"):
        shared_turn().feed(event("delta.text_append", "r1", parent="u1", text="é" * 33000))


def test_transcript_reads_only_assistant_text():
    transcript = {"messages": [
        {"role": "user", "content": [{"type": "text", "text": "Question"}]},
        {"role": "tool", "content": [{"type": "text", "text": "Private tool data"}]},
        {"role": "assistant", "content": [
            {"type": "text", "text": "Hello."},
            {"type": "image", "text": "Do not read this"},
            {"type": "text", "text": "Goodbye."},
        ]},
    ]}
    assert transcript_text(transcript) == "Hello.\nGoodbye."


@pytest.mark.parametrize("parent_key", ["reply_to_message_id", "parent_message_id"])
@pytest.mark.parametrize("name, payload", [
    ("task.status", {"status": "completed"}),
    ("task.status", {"status": "running"}),
    ("agent.status", {"activity_code": "idle"}),
    ("agent.status", {"activity_code": "working"}),
])
@pytest.mark.parametrize("finished", [False, True])
def test_shared_chat_unrelated_status_does_not_change_turn(parent_key, name, payload, finished):
    turn = shared_turn()
    turn.feed(event("task.status", parent="u1", status="completed" if finished else "running"))
    before = (turn.busy, turn.task_finished, turn.activity_code, turn.last_activity)
    assert turn.feed(event(name, **{parent_key: "another-request", **payload})) == []
    assert (turn.busy, turn.task_finished, turn.activity_code, turn.last_activity) == before
    assert turn.complete(turn.last_activity) == finished


def test_shared_chat_unrelated_status_held_before_ack_is_ignored():
    turn = MuseTurn("chat")
    turn.feed(event("task.status", parent="u1", status="running"))
    turn.feed(event("task.status", parent="another-request", status="completed"))
    assert turn.acknowledge({"message_id": "u1"}) == []
    assert turn.busy and not turn.task_finished


def test_shared_chat_parentless_status_keeps_legacy_behavior():
    turn = shared_turn()
    turn.feed(event("task.status", status="running"))
    assert turn.busy
    assert turn.feed(event("task.status", status="completed")) == [TaskFinished()]
    assert turn.complete(turn.last_activity)


def test_shared_chat_status_can_reference_a_bound_reply():
    turn = shared_turn()
    turn.feed(event("delta.message_done", "r1", parent="u1", content="Answer"))
    assert turn.feed(event("agent.status", parent="r1", activity_code="working")) == [
        AgentStatus("working", None)]
    assert turn.feed(event("task.status", parent="r1", status="completed")) == [TaskFinished()]
    assert turn.complete(turn.last_activity)


def test_shared_chat_foreign_parent_overrides_a_known_reply_id():
    turn = shared_turn()
    turn.feed(event("delta.message_done", "r1", parent="u1", content="Answer"))
    turn.feed(event("task.status", parent="u1", status="running"))
    assert turn.feed(event("task.status", "r1", parent="another-request", status="completed")) == []
    assert turn.busy and not turn.task_finished


def test_shared_chat_unrelated_status_does_not_release_follow_up():
    turn = shared_turn()
    turn.expect_follow_up()
    turn.acknowledge({"message_id": "u2"}, follow_up=True)
    turn.feed(event("task.status", parent="another-request", status="completed"))
    assert not turn.complete(turn.last_activity + 10)
    assert turn.feed(event("task.status", parent="u2", status="completed")) == [TaskFinished()]
    assert turn.complete(turn.last_activity)


def test_turn_is_not_complete_before_its_first_acknowledgement():
    turn = MuseTurn("chat")
    turn.feed(event("delta.message_done", "r1", parent="u1", content="Answer"))
    turn.feed(event("task.status", parent="u1", status="completed"))
    assert not turn.complete(turn.last_activity + 60) and not turn.settled(turn.last_activity + 60)
    assert turn.acknowledge({"message_id": "u1"}) == [
        Reply("r1", "Answer", "", done=True, bound=True), TaskFinished()]
    assert turn.complete(turn.last_activity) and turn.settled(turn.last_activity + 1.01)


def test_pending_follow_up_holds_a_finished_turn_open_until_its_own_answer():
    turn = shared_turn()
    turn.feed(event("delta.message_done", "r1", parent="u1", content="Answer one"))
    turn.feed(event("task.status", parent="u1", status="completed"))
    assert turn.complete(turn.last_activity) and turn.settled(turn.last_activity + 1.01)
    turn.expect_follow_up()
    assert not turn.complete(turn.last_activity + 60) and not turn.settled(turn.last_activity + 60)
    assert turn.feed(event("delta.text_append", "r2", parent="u2", text="Answer")) == []
    assert turn.feed(event("delta.message_done", "r2", parent="u2", content="Answer two")) == []
    assert turn.feed(event("task.status", parent="u2", status="completed")) == []
    assert not turn.complete(turn.last_activity + 60) and not turn.settled(turn.last_activity + 60)
    assert turn.acknowledge({"message_id": "u2"}, follow_up=True) == [
        Reply("r2", "Answer", "Answer", done=False, bound=True),
        Reply("r2", "Answer two", "Answer", done=True, bound=True),
        TaskFinished(),
    ]
    assert turn.complete(turn.last_activity)
    assert not turn.settled(turn.last_activity + 0.9) and turn.settled(turn.last_activity + 1.01)


def test_acknowledged_follow_up_completes_on_its_own_replies_not_the_earlier_ones():
    turn = shared_turn()
    turn.feed(event("delta.message_done", "r1", parent="u1", content="Answer one"))
    turn.feed(event("task.status", parent="u1", status="completed"))
    turn.expect_follow_up()
    assert turn.acknowledge({"message_id": "u2"}, follow_up=True) == []
    assert not turn.task_finished
    assert not turn.complete(turn.last_activity + 29.9) and not turn.settled(turn.last_activity + 60)
    turn.feed(event("delta.text_append", "r2", parent="u2", text="Answer"))
    assert not turn.complete(turn.last_activity + 60) and not turn.settled(turn.last_activity + 60)
    turn.feed(event("delta.message_done", "r2", parent="u2", content="Answer two"))
    assert not turn.complete(turn.last_activity + 2.9) and turn.complete(turn.last_activity + 3.01)
    assert not turn.settled(turn.last_activity + 60)
    assert turn.feed(event("task.status", parent="u2", status="completed")) == [TaskFinished()]
    assert turn.complete(turn.last_activity) and turn.settled(turn.last_activity + 1.01)


def test_side_chat_pending_follow_up_waits_for_the_follow_up_task():
    turn = side_turn()
    turn.feed(event("task.status", session="side", task_id="t1", status="running"))
    turn.feed(event("delta.message_done", "r1", session="side", content="Answer one"))
    turn.feed(event("task.status", session="side", task_id="t1", status="completed"))
    assert turn.complete(turn.last_activity)
    turn.expect_follow_up()
    for raw in [event("task.status", session="side", task_id="t2", status="running"),
                event("delta.message_done", "r2", session="side", content="Answer two"),
                event("task.status", session="side", task_id="t2", status="completed")]:
        assert turn.feed(raw) == []
    assert not turn.complete(turn.last_activity + 60) and not turn.settled(turn.last_activity + 60)
    assert turn.acknowledge(side_ack("u2"), follow_up=True) == [
        Reply("r2", "Answer two", "", done=True, bound=True), TaskFinished()]
    assert turn.complete(turn.last_activity) and turn.settled(turn.last_activity + 1.01)
