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

"""The servers the device takes for its credentials (server_hosts.py).

The same cases as the ESP32's test_link_server_hosts.py: setup refuses a host
before applying any of it, a saved one is ignored at startup, and refused
credentials never bring in new hosts.
"""

from __future__ import annotations

import asyncio
import wave
from pathlib import Path

import pytest

from musegadget import cli, config, muse_api, server_hosts
from musegadget import pair as pair_module
from musegadget import service as service_module
from musegadget.executor import Account, Executor
from musegadget.identity import Identity
from musegadget.link_client import LinkSession, Outcome
from musegadget.pairing import PairingState
from musegadget.service import Service

from test_ble_setup import PROVISION, Harness

API_ALLOWED = ["https://api.muse.ai", "https://hatch-api.meta.ai"]
NOISE_ALLOWED = ["hatch.metaaivm.com"]

API_REFUSED = [
    "http://api.muse.ai",
    "HTTPS://api.muse.ai",
    "https://API.MUSE.AI",
    "api.muse.ai",
    "//api.muse.ai",
    "https://api.muse.ai/",
    "https://api.muse.ai/hatch",
    "https://api.muse.ai?x=1",
    "https://api.muse.ai#x",
    "https://api.muse.ai.",
    "https://api.muse.ai:443",
    "https://api.muse.ai:8443",
    "https://user@api.muse.ai",
    "https://api.muse.ai@evil.example",
    "https://api.muse.ai\\@evil.example",
    "https://evil.example/https://api.muse.ai",
    "https://api.muse.ai.evil.example",
    "https://evilapi.muse.ai",
    "https://evil.api.muse.ai",
    "https://muse.ai",
    "https://evilmuse.ai",
    "https://meta.ai",
    "https://evil.meta.ai",
    "https://www.meta.ai",
    "https://hatch-api.meta.ai.evil.example",
    "https://hatch.metaaivm.com",
    "https://vm-1.metaaivm.com",
    " https://api.muse.ai",
    "https://api.muse.ai ",
    "https://api.muse.ai\r\nX-Injected: 1",
    "wss://api.muse.ai",
]
NOISE_REFUSED = [
    "evilmetaaivm.com",
    "metaaivm.com",
    "metaaivm.com.evil.com",
    "hatch.metaaivm.com.evil.com",
    "evil.hatch.metaaivm.com",
    "hatchmetaaivm.com",
    "vm-1.metaaivm.com",
    "0123abcd-0000-4000-8000-000000000000.metaaivm.com",
    "HATCH.METAAIVM.COM",
    "hatch.metaaivm.com.",
    "hatch.metaaivm.com:443",
    "hatch.metaaivm.com:8443",
    "user@hatch.metaaivm.com",
    "hatch.metaaivm.com@evil.com",
    "https://hatch.metaaivm.com",
    "wss://hatch.metaaivm.com",
    "hatch.metaaivm.com/",
    "hatch.metaaivm.com/v1/noise",
    "hatch.metaaivm.com?vm_id=x",
    "hatch.metaaivm.com\r\nX-Injected: 1",
    " hatch.metaaivm.com",
    "hatch.metaaivm.com ",
    "api.muse.ai",
    "evil.example",
]
REFUSED_FIELDS = ([("api_url", v) for v in API_REFUSED] + [("api_url_v2", v) for v in API_REFUSED]
                  + [("noise_host", v) for v in NOISE_REFUSED])


@pytest.fixture(autouse=True)
def release_hosts(monkeypatch):
    # The pairing vectors Harness uses offer gadget- node ids.
    monkeypatch.setenv(config.GADGET_NODE_ID_ENV, "1")
    monkeypatch.delenv(config.CUSTOM_SERVER_HOSTS_ENV, raising=False)


def custom_hosts(monkeypatch, on: bool) -> None:
    if on:
        monkeypatch.setenv(config.CUSTOM_SERVER_HOSTS_ENV, "1")


@pytest.mark.parametrize("custom", [False, True])
def test_allowlist(monkeypatch, custom):
    custom_hosts(monkeypatch, custom)
    for value in API_ALLOWED + API_REFUSED:
        # A development build that opts in takes any host.
        assert server_hosts.api_allowed(value) is (custom or value in API_ALLOWED), value
    for value in NOISE_ALLOWED + NOISE_REFUSED:
        assert server_hosts.noise_allowed(value) is (custom or value in NOISE_ALLOWED), value
    # Absent or empty means the default, either way.
    for check in (server_hosts.api_allowed, server_hosts.noise_allowed):
        assert check(None) and check("")
        # A saved value that isn't text is never a host.
        assert not check(1) and not check(["hatch.metaaivm.com"])


@pytest.mark.parametrize("value", ["0", "true", "yes", ""])
def test_only_1_opts_in(monkeypatch, value):
    monkeypatch.setenv(config.CUSTOM_SERVER_HOSTS_ENV, value)
    assert not config.custom_server_hosts()
    assert not server_hosts.api_allowed("https://api.example")


@pytest.mark.parametrize("field, value", REFUSED_FIELDS)
def test_setup_refuses_a_host_before_applying_any(field, value):
    h = Harness()
    h.pair()
    h.send_encrypted({**PROVISION, field: value})
    assert h.statuses()[-1] == "error_invalid_host"
    # Nothing was started or handed on, and the session can still set up.
    assert h.pairing.state is PairingState.READY
    assert not h.saved and not h.transport.disconnects


def test_a_refused_setup_can_be_sent_again_with_the_apps_hosts():
    h = Harness()
    h.pair()
    h.send_encrypted({**PROVISION, "noise_host": "hatch.metaaivm.com.evil.com"})
    assert h.statuses()[-1] == "error_invalid_host"
    h.send_encrypted(PROVISION)
    h.wait_for_status("auth_ok")
    assert [c.noise_host for c in h.saved] == ["hatch.metaaivm.com"]


@pytest.mark.parametrize("field, value", REFUSED_FIELDS)
def test_custom_hosts_take_any_host_in_setup(monkeypatch, field, value):
    custom_hosts(monkeypatch, True)
    h = Harness()
    h.pair()
    h.send_encrypted({**PROVISION, field: value})
    h.wait_for_status("auth_ok")
    assert getattr(h.saved[0], field) == value


def test_setup_ignores_host_fields_that_are_not_text():
    h = Harness()
    h.pair()
    h.send_encrypted({**PROVISION, "api_url_v2": 1, "noise_host": None,
                      "api_url": ["https://evil.example"]})
    h.wait_for_status("auth_ok")
    saved = h.saved[0]
    assert (saved.api_url, saved.api_url_v2, saved.noise_host) == ("", "", "")


@pytest.mark.parametrize("custom", [False, True])
@pytest.mark.parametrize("failure", ["auth_failed", "error_storage", None])
def test_hosts_come_in_only_with_the_credentials_they_were_sent_with(
        tmp_path, monkeypatch, custom, failure):
    # The pairing file holds the tokens and their hosts in one write, and the
    # service reads both from it, so hosts are applied only once their
    # credentials are stored. Refused credentials leave the old pairing,
    # hosts and all.
    custom_hosts(monkeypatch, custom)
    monkeypatch.setenv(config.STATE_DIR_ENV, str(tmp_path))
    old = {"access_token": "old", "refresh_token": "old-r", "token_type": "device",
           "api_url": "", "api_url_v2": "", "noise_host": "", "access_token_saved_at": 1}
    config.save_json(config.PAIRING_FILE, old)
    before = (tmp_path / config.PAIRING_FILE).read_bytes()
    hosts = ({"api_url": "https://legacy.example", "api_url_v2": "https://api.example",
              "noise_host": "noise.example"} if custom else
             {"api_url": "https://hatch-api.meta.ai", "api_url_v2": "https://api.muse.ai",
              "noise_host": "hatch.metaaivm.com"})
    checked = []

    def fetch(token, root):
        checked.append((token, root))
        return ([], 401) if failure == "auth_failed" else ([{"vm_id": "vm"}], 200)

    monkeypatch.setattr(pair_module.muse_api, "fetch_vms_with_status", fetch)

    def provision(credentials, commit):
        def ended(save):
            return False    # the session ended before the write

        pair_module._verify_and_save(credentials, ended if failure == "error_storage" else commit)

    h = Harness(provision=provision)
    h.pair()
    h.send_encrypted({**PROVISION, **hosts})
    h.wait_for_status(failure or "auth_ok")
    # Only the new token is checked, with its own API: the old one never goes
    # to a host it wasn't paired with.
    assert checked == [("device-access", hosts["api_url_v2"])]
    if failure:
        assert (tmp_path / config.PAIRING_FILE).read_bytes() == before
    else:
        saved = config.load_json(config.PAIRING_FILE)
        assert saved["access_token"] == "device-access"
        assert {key: saved[key] for key in hosts} == hosts


SAVED = [
    # (api_url_v2, noise_host) as an earlier client may have saved them
    ("https://api.muse.ai.evil.example", "evilmetaaivm.com"),
    ("https://api.muse.ai:8443", "hatch.metaaivm.com:443"),
    ("https://evil.example", "hatch.metaaivm.com.evil.com"),
    ("http://api.muse.ai", "HATCH.METAAIVM.COM"),
]


def run_service(monkeypatch, pairing: dict) -> dict:
    """Run the service until its first session, with the API and Noise faked."""
    seen = {"fetch": [], "refresh": [], "url": []}
    monkeypatch.setattr(service_module.tls, "client_context", lambda: None)
    monkeypatch.setattr(service_module.config, "load_json", lambda name, *args: dict(pairing))
    monkeypatch.setattr(service_module.config, "save_json", lambda *args: None)

    def fetch(token, root):
        seen["fetch"].append(root)
        return [{"vm_id": "vm", "vm_name": "vm", "vm_auth_token": "t", "is_default": True}], 200

    def refresh(refresh_token, node_id, root, sdk_token):
        seen["refresh"].append(root)
        return {"access_token": "a2", "refresh_token": "r2"}, 200

    monkeypatch.setattr(service_module.muse_api, "fetch_vms_with_status", fetch)
    monkeypatch.setattr(service_module.muse_api, "refresh_device_token", refresh)

    class Probe(Service):
        async def serve(self, session, vm, stop):
            seen["url"].append(session._url)
            return Outcome.STOPPED

    async def scenario():
        await Probe(identity=Identity("02:00:00:00:00:01"), executor=Executor(Account.current())).run()

    asyncio.run(scenario())
    return seen


@pytest.mark.parametrize("custom", [False, True])
@pytest.mark.parametrize("api, noise", SAVED)
def test_the_service_ignores_saved_hosts_it_does_not_allow(monkeypatch, caplog, custom, api, noise):
    custom_hosts(monkeypatch, custom)
    # Due a token refresh, so all three uses are seen.
    pairing = {"access_token": "a", "refresh_token": "r", "access_token_saved_at": 0,
               "api_url_v2": api, "noise_host": noise}
    seen = run_service(monkeypatch, pairing)
    root = api if custom else "https://api.muse.ai"
    host = noise if custom else "hatch.metaaivm.com"
    assert seen == {"fetch": [root], "refresh": [root], "url": [f"wss://{host}/v1/noise?vm_id=vm"]}
    if not custom:
        assert "saved Muse API server not allowed" in caplog.text
        assert "saved Noise host not allowed" in caplog.text
    # The log never has the saved value.
    assert api not in caplog.text and noise not in caplog.text


@pytest.mark.parametrize("pairing", [
    {"api_url_v2": "https://api.muse.ai", "noise_host": "hatch.metaaivm.com"},
    {"api_url_v2": "", "noise_host": ""},
    {},
    # Not text: never a host, even with custom hosts on.
    {"api_url_v2": 7, "noise_host": ["hatch.metaaivm.com.evil.com"]},
])
@pytest.mark.parametrize("custom", [False, True])
def test_the_service_keeps_the_apps_hosts_and_defaults(monkeypatch, pairing, custom):
    custom_hosts(monkeypatch, custom)
    seen = run_service(monkeypatch, {"access_token": "a", "refresh_token": "r",
                                     "access_token_saved_at": 0, **pairing})
    assert seen == {"fetch": ["https://api.muse.ai"], "refresh": ["https://api.muse.ai"],
                    "url": ["wss://hatch.metaaivm.com/v1/noise?vm_id=vm"]}


def test_voice_call_ignores_saved_hosts_it_does_not_allow(tmp_path, monkeypatch):
    monkeypatch.setenv(config.STATE_DIR_ENV, str(tmp_path / "state"))
    config.save_json(config.PAIRING_FILE, {"access_token": "synthetic",
                                           "api_url_v2": "https://api.muse.ai.evil.example",
                                           "noise_host": "hatch.metaaivm.com.evil.com"})
    roots, urls = [], []

    def fetch(token, root):
        roots.append(root)
        return [{"vm_id": "target", "vm_auth_token": "synthetic-vm-token"}], 200

    async def open_refused(self):
        urls.append(self._url)
        raise ConnectionError("synthetic")

    monkeypatch.setattr(muse_api, "fetch_vms_with_status", fetch)
    monkeypatch.setattr(LinkSession, "_open", open_refused)
    in_wav = tmp_path / "in.wav"
    with wave.open(str(in_wav), "wb") as wav:
        wav.setparams((1, 2, 24000, 0, "NONE", "not compressed"))
    assert cli.main(["voice-call", "--vm-id", "target", "--in-wav", str(in_wav),
                     "--out-wav", str(tmp_path / "out.wav")]) != 0
    assert roots == ["https://api.muse.ai"]
    assert urls and all(url.startswith("wss://hatch.metaaivm.com/v1/noise?vm_id=target") for url in urls)


def test_custom_hosts_stay_off_in_what_ships():
    linux = Path(__file__).resolve().parents[1]
    for path in (linux / "install.sh", linux / "src/musegadget/data/musegadget.service"):
        assert config.CUSTOM_SERVER_HOSTS_ENV not in path.read_text(), path
