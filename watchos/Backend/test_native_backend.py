"""Direct Mac connection behavior without hardware, Docker or an account."""
import asyncio
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import native_backend as native

class BackendTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.directory = tempfile.TemporaryDirectory(dir="/tmp")
        self.environment = patch.dict(os.environ, {native.config.STATE_DIR_ENV: self.directory.name})
        self.environment.start()
        os.environ.pop(native.config.SDK_TOKEN_ENV, None)
        self.events = []
        self.output = patch.object(native, "emit", self.events.append); self.output.start()
        self.backend = native.Backend()

    async def asyncTearDown(self):
        for task in (self.backend.chat_task, self.backend.pair_task):
            if task:
                task.cancel(); await asyncio.gather(task, return_exceptions=True)
        self.output.stop(); self.environment.stop(); self.directory.cleanup()

    async def test_unpaired_chat_finishes_with_error_and_allows_retry(self):
        self.backend.command(dict(op="chat", message="hello", session_id=str(native.uuid.uuid4())))
        await self.backend.chat_task
        self.assertEqual([event["type"] for event in self.events], ["error", "turn_finished"])
        self.assertIsNone(self.backend.chat_task)

    async def test_two_text_turns_share_the_direct_session(self):
        calls = []
        class Session:
            registered_at = 1
            async def chat_events(self, text, session):
                calls.append((text, session))
                yield dict(type="ack")
                yield dict(type="reply", message_id="reply", text="hello", complete=True)
                yield dict(type="done")
        self.backend.service._current = Session()
        session_id = str(native.uuid.uuid4())
        for text in ("first", "second"):
            self.backend.command(dict(op="chat", message=text, session_id=session_id))
            await self.backend.chat_task
            self.assertIsNone(self.backend.chat_task)
        self.assertEqual(calls, [("first", session_id), ("second", session_id)])
        self.assertEqual([event["type"] for event in self.events].count("turn_finished"), 2)

    async def test_cancellation_finishes_the_turn(self):
        class Session:
            registered_at = 1
            async def chat_events(self, *args):
                yield dict(type="ack")
                await asyncio.sleep(100)
        self.backend.service._current = Session()
        self.backend.command(dict(op="chat", message="hello", session_id=str(native.uuid.uuid4())))
        task = self.backend.chat_task
        await asyncio.sleep(0)
        self.backend.command(dict(op="cancel")); await task
        self.assertEqual(self.events[-1]["type"], "turn_finished")
        self.assertIsNone(self.backend.chat_task)

    async def test_cancellation_before_task_starts_also_finishes(self):
        self.backend.command(dict(op="chat", message="hello", session_id=str(native.uuid.uuid4())))
        task = self.backend.chat_task
        self.backend.command(dict(op="cancel"))
        await asyncio.gather(task, return_exceptions=True)
        self.assertIsNone(self.backend.chat_task)
        self.assertEqual(self.events[-1]["type"], "turn_finished")

    async def test_token_validation_and_owner_only_storage(self):
        with self.assertRaises(ValueError): native.save_sdk_token("invalid")
        self.assertFalse((Path(self.directory.name) / native.config.SDK_TOKEN_FILE).exists())
        token = "mgst_" + "A" * 43
        self.assertEqual(native.save_sdk_token(token), token)
        saved = Path(self.directory.name) / native.config.SDK_TOKEN_FILE
        self.assertEqual(saved.read_text().strip(), token)
        self.assertEqual(saved.stat().st_mode & 0o777, 0o600)

    async def test_mac_registration_has_no_linux_commands(self):
        params = native.MacDescription("homelink-123456", "Mac", "1", {}).register_params()
        self.assertEqual(params["platform"], "macos")
        self.assertEqual(params["device_family"], "homehub")
        self.assertEqual(params["commands_v2"], {})

    async def test_cancel_pairing_before_hardware_starts(self):
        self.backend.command(dict(op="cancel_pair"))
        with patch.object(native.mac_ble, "BleServer", side_effect=AssertionError("started")):
            self.assertFalse(self.backend.pair_sync())

if __name__ == "__main__": unittest.main()
