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

import asyncio
import json
import logging
import wave

from musegadget import cli, config, identity, muse_api
from musegadget.link_client import LinkSession
from test_link_client import FakeVm, Pipe
from test_streaming import chunk, response
from test_voice_call import TONE, cavd, line


def test_cli_uses_host_pairing_exact_vm_and_noise_upgrade_app_id(tmp_path, monkeypatch, capsys, caplog):
    monkeypatch.setenv(config.STATE_DIR_ENV, str(tmp_path / "state"))
    ident = identity.load_or_create()
    pairing = {"access_token": "synthetic-device-token", "noise_host": "noise.example",
               "api_url_v2": "https://api.example"}
    config.save_json(config.PAIRING_FILE, pairing)
    original_pairing = (config.state_dir() / config.PAIRING_FILE).read_bytes()
    calls = []

    def fetch(token, root):
        assert token == pairing["access_token"] and root == pairing["api_url_v2"]
        return [{"vm_id": "other", "vm_auth_token": "wrong", "is_default": True},
                {"vm_id": "target", "vm_auth_token": "synthetic-vm-token", "is_default": False}], 200

    async def open_fake(self):
        calls.append((self._url, self._token))
        incoming, outgoing = asyncio.Queue(), asyncio.Queue()
        client, server = Pipe(incoming, outgoing), Pipe(outgoing, incoming)
        vm = FakeVm(server)

        async def serve():
            await vm.handshake()
            media = await vm.next_frame()
            assert media.value.path.startswith("/voice/realtime?")
            assert self._register_device is False
            start = json.loads((await vm.next_frame()).value.data)
            assert json.loads((await vm.next_frame()).value.data)["type"] == "hatch.voice.capability.offer"
            assert start["device_id"] == ident.node_id
            await response(vm, media.stream_id, line({"type": "hatch.voice.session",
                           "call_id": start["call_id"], "session_id": "synthetic-session"}))
            assert json.loads((await vm.next_frame()).value.data)["type"] == "input_audio_buffer.append"
            await chunk(vm, media.stream_id, line(cavd()), end=True)
            end = await vm.next_frame()
            assert end.value.path == "/voice/end-call"
            await response(vm, end.stream_id, b'{"ok":true,"result":{"ok":true}}', end=True)
            assert (await vm.next_frame()).value.end_body

        task = asyncio.create_task(serve())
        calls.append(task)
        return client

    monkeypatch.setattr(muse_api, "fetch_vms_with_status", fetch)
    monkeypatch.setattr(LinkSession, "_open", open_fake)
    in_wav, out_wav = tmp_path / "in.wav", tmp_path / "out.wav"
    with wave.open(str(in_wav), "wb") as wav:
        wav.setparams((1, 2, 24000, 0, "NONE", "not compressed"))
        wav.writeframes(TONE)
    assert cli.main(["-v", "voice-call", "--vm-id", "target", "--app-id", "1234",
                     "--in-wav", str(in_wav), "--out-wav", str(out_wav)]) == 0
    assert calls[0] == ("wss://noise.example/v1/noise?vm_id=target&app_id=1234", "synthetic-vm-token")
    assert calls[1].done() and calls[1].exception() is None
    assert (config.state_dir() / config.PAIRING_FILE).read_bytes() == original_pairing
    text = capsys.readouterr().out + caplog.text
    assert "synthetic-device-token" not in text and "synthetic-vm-token" not in text
    assert logging.getLogger("websockets").level >= logging.WARNING


def test_cli_does_not_fall_back_to_default_vm(tmp_path, monkeypatch):
    monkeypatch.setenv(config.STATE_DIR_ENV, str(tmp_path / "state"))
    config.save_json(config.PAIRING_FILE, {"access_token": "synthetic"})
    monkeypatch.setattr(muse_api, "fetch_vms_with_status", lambda *a: (
        [{"vm_id": "default", "is_default": True}], 200))
    opened = []
    monkeypatch.setattr(LinkSession, "_open", lambda *a: opened.append(True))
    in_wav = tmp_path / "in.wav"
    with wave.open(str(in_wav), "wb") as wav:
        wav.setparams((1, 2, 24000, 0, "NONE", "not compressed"))
    assert cli.main(["voice-call", "--vm-id", "missing", "--in-wav", str(in_wav),
                     "--out-wav", str(tmp_path / "out.wav")]) == 1
    assert not opened
