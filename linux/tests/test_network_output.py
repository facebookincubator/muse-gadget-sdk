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

from __future__ import annotations

import subprocess
import sys

import pytest

from musegadget import network


@pytest.mark.parametrize("tool", ["nmcli", "wpa_cli"])
def test_connection_entry_survives_undecodable_diagnostics(monkeypatch, tool):
    """Decode real child output, including stderr that setup never uses."""
    real_run = subprocess.run
    output = (b"yes:Example Net\n" if tool == "nmcli" else
              b"wpa_state=COMPLETED\nssid=Example Net\n")
    monkeypatch.setattr(network.shutil, "which", lambda name: name if name == tool else None)
    monkeypatch.setattr(network, "_wireless_interfaces", lambda: ["wlan0"])

    def run(argv, **kwargs):
        assert argv[0] == tool
        return real_run(
            [sys.executable, "-c",
             "import os; os.write(1, " + repr(output) + "); os.write(2, b'\\xff')"],
            **kwargs,
        )

    monkeypatch.setattr(network.subprocess, "run", run)
    assert network.current_connection_entry()["ssid"] == "Example Net"


def test_unreadable_nmcli_output_still_allows_wpa_fallback(monkeypatch):
    real_run = subprocess.run
    monkeypatch.setattr(network.shutil, "which", lambda name: name)
    monkeypatch.setattr(network, "_wireless_interfaces", lambda: ["wlan0"])
    calls = []

    def run(argv, **kwargs):
        calls.append(argv[0])
        output = (b"\xff\n" if argv[0] == "nmcli" else
                  b"wpa_state=COMPLETED\nssid=Fallback Net\n")
        return real_run([sys.executable, "-c", "import os; os.write(1, " + repr(output) + ")"],
                        **kwargs)

    monkeypatch.setattr(network.subprocess, "run", run)
    assert network.current_connection_entry()["ssid"] == "Fallback Net"
    assert calls == ["nmcli", "wpa_cli"]
