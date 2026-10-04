# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
import asyncio
import json

import pytest

from musegadget.chat import Subscription, Turn, MAX_EVENT_BYTES, MAX_PENDING_EVENTS
from musegadget.noise import ApplicationResponse, BodyChunk, ServiceFrame


def event(name, mid="answer", parent="prompt", **payload):
    return {"type": "event", "event": name, "payload": {
        "message_id": mid, "reply_to_message_id": parent, **payload}}


def test_correlated_deltas_and_final_snapshot_do_not_duplicate_text():
    turn = Turn({"result": {"message_id": "prompt"}})
    assert turn.feed(event("delta.text_append", parent="another-chat", text="private")) is None
    assert turn.feed(event("delta.message_start"))["text"] == ""
    assert turn.feed(event("delta.text_append", parent=None, text="Hi 🌟"))["text"] == "Hi 🌟"
    assert not turn.settled
    assert turn.feed(event("delta.message_done", display_text="Hi 🌟"))["complete"]
    assert turn.feed(event("message.assistant", content="Hi 🌟", display_text_ready=True))["text"] == "Hi 🌟"
    assert turn.settled
    turn.feed(event("agent.status", activity_code="thinking"))
    assert not turn.settled
    turn.feed(event("agent.status", activity_code="idle"))
    assert turn.settled
    turn.feed(event("delta.message_start", mid="second"))
    assert not turn.settled
    turn.feed(event("delta.message_done", mid="second", content="Next"))
    assert turn.settled


def test_malformed_ids_roles_and_repeated_sequences_are_ignored():
    turn = Turn({"message_id": "prompt", "reply_to_message_id": []})
    assert turn.feed(event("delta.text_append", parent=[], text="x")) is None
    assert turn.feed(event("message.assistant", role="user", text="x")) is None
    first = event("delta.text_append", text="a"); first["seq"] = 7
    assert turn.feed(first)["text"] == "a"
    assert turn.feed(first) is None
    with pytest.raises(ValueError):
        Turn({"accepted": True})


def test_scoped_muse_starts_without_parent_and_self_parented_deltas():
    turn = Turn({"message_id": "prompt", "session_id": "side-1"})
    assert turn.feed(event("delta.message_start", parent=None, session_id="other")) is None
    assert turn.feed(event("delta.message_start", parent=None, session_id="side-1"))["text"] == ""
    update = event("delta.text_append", parent=None, session_id="side-1", parent_message_id="answer", text="Hello")
    assert turn.feed(update)["text"] == "Hello"
    assert turn.feed(event("delta.message_done", parent=None, session_id="side-1"))["complete"]
    assert turn.settled
    assert turn.feed(event("agent.status", activity_code="thinking", session_id="other")) is None
    assert turn.settled


def test_subscription_buffers_fragmented_utf8_and_propagates_disconnect():
    async def scenario():
        subscription = Subscription()
        subscription.on_frame(ServiceFrame.response(1, ApplicationResponse(status=200)))
        assert await subscription.ready is None
        encoded = json.dumps(event("delta.text_append", text="你好🌟"), ensure_ascii=False).encode() + b"\n"
        for byte in encoded:
            subscription.on_frame(ServiceFrame.body_chunk(1, BodyChunk(data=bytes([byte]))))
        assert (await subscription.next_event(1))["payload"]["text"] == "你好🌟"
        subscription.fail(ConnectionError("disconnected"))
        with pytest.raises(ConnectionError, match="disconnected"):
            await subscription.next_event(1)
    asyncio.run(scenario())


@pytest.mark.parametrize("mode", ["refused", "too-large", "overflow"])
def test_subscription_bounds_and_http_rejection(mode):
    async def scenario():
        subscription = Subscription()
        if mode == "refused":
            subscription.on_frame(ServiceFrame.response(1, ApplicationResponse(status=403)))
            assert isinstance(await subscription.ready, ConnectionError)
        elif mode == "too-large":
            subscription.on_frame(ServiceFrame.body_chunk(1, BodyChunk(data=b"x" * (MAX_EVENT_BYTES + 1))))
        else:
            data = (json.dumps(event("delta.message_start")).encode() + b"\n") * (MAX_PENDING_EVENTS + 1)
            subscription.on_frame(ServiceFrame.body_chunk(1, BodyChunk(data=data)))
        assert subscription.done.done()
    asyncio.run(scenario())
