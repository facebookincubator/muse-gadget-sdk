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
import base64
import json
import sys
import wave
from types import SimpleNamespace

import pytest

from musegadget import cli, config, muse_api
from musegadget.link_client import LinkSession
from test_link_client import FakeVm, Pipe
from test_streaming import chunk, response
from test_voice_call import TONE, cavd, line


class Detector:
    fire_at = 3
    calls = 0
    closed = False

    def __init__(self, model_path=None, sensitivity="default"):
        self.model_path = model_path
        self.sensitivity = sensitivity
        self.calls = 0
        self.closed = False
        type(self).instance = self

    def reset(self):
        self.calls = 0

    def feed(self, pcm):
        assert len(pcm) == 1280  # 40 ms at the model's 16 kHz, not wire rate
        self.calls += 1
        return self.calls == self.fire_at

    def close(self):
        self.closed = True


def prepare_audio(tmp_path, monkeypatch, pcm):
    monkeypatch.setenv(config.STATE_DIR_ENV, str(tmp_path / "state"))
    config.save_json(config.PAIRING_FILE, {"access_token": "synthetic"})
    monkeypatch.setitem(
        sys.modules,
        "musegadget.wakeword",
        SimpleNamespace(
            WakeWordDetector=Detector, WakeWordUnavailableError=RuntimeError
        ),
    )
    in_wav = tmp_path / "in.wav"
    with wave.open(str(in_wav), "wb") as wav:
        wav.setparams((1, 2, 24000, 0, "NONE", "not compressed"))
        wav.writeframes(pcm)
    return [
        "voice-call",
        "--vm-id",
        "target",
        "--wake-word",
        "--in-wav",
        str(in_wav),
        "--out-wav",
        str(tmp_path / "out.wav"),
    ]


def test_no_wake_means_no_vm_lookup_or_voice_admission(tmp_path, monkeypatch, capsys):
    args = prepare_audio(tmp_path, monkeypatch, TONE)

    def forbidden(*unused):
        pytest.fail("waiting audio must not reach the network")

    monkeypatch.setattr(muse_api, "fetch_vms_with_status", forbidden)
    monkeypatch.setattr(LinkSession, "_open", forbidden)
    assert cli.main(args) == 0
    assert Detector.instance.closed
    assert "no_wake_word" in capsys.readouterr().out


def test_wake_admits_existing_noise_voice_call_with_original_rate_preroll(
    tmp_path, monkeypatch
):
    args = prepare_audio(tmp_path, monkeypatch, TONE * 4)
    tasks = []

    def fetch(*unused):
        assert Detector.instance.calls == 3  # never before activation
        return [{"vm_id": "target", "vm_auth_token": "synthetic"}], 200

    async def open_fake(self):
        incoming, outgoing = asyncio.Queue(), asyncio.Queue()
        client, server = Pipe(incoming, outgoing), Pipe(outgoing, incoming)
        vm = FakeVm(server)

        async def serve():
            await vm.handshake()
            media = await vm.next_frame()
            assert media.value.path.startswith("/voice/realtime?")
            start = json.loads((await vm.next_frame()).value.data)
            assert start["type"] == "hatch.voice.call.start"
            assert (
                json.loads((await vm.next_frame()).value.data)["type"]
                == "hatch.voice.capability.offer"
            )
            await response(
                vm,
                media.stream_id,
                line(
                    {
                        "type": "hatch.voice.session",
                        "call_id": start["call_id"],
                        "session_id": "synthetic-session",
                    }
                ),
            )
            for _ in range(4):
                append = json.loads((await vm.next_frame()).value.data)
                assert append["type"] == "input_audio_buffer.append"
                assert base64.b64decode(append["audio"]) == TONE
            await chunk(vm, media.stream_id, line(cavd()), end=True)
            end = await vm.next_frame()
            assert end.value.path == "/voice/end-call"
            await response(
                vm, end.stream_id, b'{"ok":true,"result":{"ok":true}}', end=True
            )
            assert (await vm.next_frame()).value.end_body

        tasks.append(asyncio.create_task(serve()))
        return client

    monkeypatch.setattr(muse_api, "fetch_vms_with_status", fetch)
    monkeypatch.setattr(LinkSession, "_open", open_fake)
    assert (
        cli.main(args + ["--wake-model", "custom.pte", "--wake-sensitivity", "low"])
        == 0
    )
    assert tasks[0].done() and tasks[0].exception() is None
    assert Detector.instance.calls == 3 and Detector.instance.closed
    assert (
        Detector.instance.model_path == "custom.pte"
        and Detector.instance.sensitivity == "low"
    )


def test_invalid_wake_options_are_rejected_before_audio_or_credentials(capsys):
    assert (
        cli.main(
            ["voice-call", "--vm-id", "synthetic", "--live", "--wake-model", "x.pte"]
        )
        == 2
    )
    assert "require --wake-word" in capsys.readouterr().err


def test_wake_wait_timeout_closes_capture_without_network(tmp_path, monkeypatch):
    args = prepare_audio(tmp_path, monkeypatch, TONE)

    class Live:
        closed = False

        def __init__(self):
            type(self).instance = self

        async def frames(self):
            yield TONE
            await asyncio.Event().wait()

        def close(self):
            self.closed = True

    from musegadget import voice_audio

    monkeypatch.setattr(voice_audio, "LiveAudio", Live)
    monkeypatch.setattr(
        muse_api, "fetch_vms_with_status", lambda *unused: pytest.fail("no wake")
    )
    args = args[:4] + ["--live", "--wake-timeout", "0.01"]
    assert cli.main(args) == 1
    assert Detector.instance.closed and Live.instance.closed


def test_live_rearms_after_call_cleanup_and_reads_rotated_pairing(
    tmp_path, monkeypatch, capsys
):
    prepare_audio(tmp_path, monkeypatch, TONE)
    monkeypatch.setattr(Detector, "fire_at", 1)
    from musegadget import voice_audio, voice_call

    captures = []
    tokens = []
    calls = []

    class Live:
        closed = False

        def __init__(self):
            type(self).instance = self

        async def frames(self):
            captures.append(True)
            yield TONE
            await asyncio.Event().wait()

        def close(self):
            self.closed = True

    class Session:
        def __init__(self, **options):
            assert options["register_device"] is False

        async def run(self, stop):
            await stop.wait()

        async def wait_transport_ready(self, timeout):
            pass

    class Call:
        def __init__(self, session, **options):
            pass

        async def run(self, source, output, **options):
            calls.append(True)
            assert await source.__anext__() == TONE
            assert Detector.instance.calls == 1
            if len(calls) == 1:
                config.save_json(
                    config.PAIRING_FILE, {"access_token": "synthetic-rotated"}
                )
                return "remote_stop"
            raise asyncio.CancelledError

    def fetch(token, root):
        tokens.append(token)
        return [{"vm_id": "target", "vm_auth_token": "synthetic"}], 200

    monkeypatch.setattr(voice_audio, "LiveAudio", Live)
    monkeypatch.setattr(voice_call, "VoiceCall", Call)
    monkeypatch.setattr(muse_api, "fetch_vms_with_status", fetch)
    monkeypatch.setattr("musegadget.link_client.LinkSession", Session)
    assert cli.main(["voice-call", "--vm-id", "target", "--live", "--wake-word"]) == 130
    assert len(captures) == len(calls) == 2
    assert tokens == ["synthetic", "synthetic-rotated"]
    assert Detector.instance.closed and Live.instance.closed
    assert "returning to wake-word listening" in capsys.readouterr().out
