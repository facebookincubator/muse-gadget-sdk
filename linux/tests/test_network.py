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

from musegadget import network

WPA_STATUS = """bssid=00:11:22:33:44:55
freq=5180
ssid=Example Net
id=0
mode=station
wifi_generation=6
pairwise_cipher=CCMP
group_cipher=CCMP
key_mgmt=WPA2-PSK
wpa_state=COMPLETED
ip_address=192.168.1.20
"""


def _fake_run(output: str):
    def run(argv, **kwargs):
        return subprocess.CompletedProcess(argv, 0, stdout=output, stderr="")
    return run


def _sysfs(monkeypatch, tmp_path, interfaces):
    """Point the module's sysfs lookup at tmp_path."""
    for name, wireless in interfaces:
        (tmp_path / name).mkdir()
        if wireless:
            (tmp_path / name / "wireless").mkdir()

    from pathlib import Path as RealPath

    def fake_path(arg):
        return tmp_path if str(arg) == "/sys/class/net" else RealPath(arg)

    monkeypatch.setattr(network, "Path", fake_path, raising=False)


def test_reports_the_ssid_on_a_host_networkmanager_does_not_manage(monkeypatch, tmp_path):
    """netplan / systemd-networkd drive wpa_supplicant directly, so nmcli
    reports every device as unmanaged and lists no networks even while Wi-Fi is
    associated. Without a second source this returned None, so
    current_connection_entry() fell back to CURRENT_CONNECTION_LABEL and the
    Muse app stopped before sending provision_v2.
    """
    monkeypatch.setattr(network.shutil, "which", lambda name: "/usr/bin/" + name)
    _sysfs(monkeypatch, tmp_path, [("eth0", False), ("wlan0", True)])

    def run(argv, **kwargs):
        if argv[0] == "nmcli":
            return subprocess.CompletedProcess(argv, 0, stdout="", stderr="")
        if argv[0] == "wpa_cli":
            return subprocess.CompletedProcess(argv, 0, stdout=WPA_STATUS, stderr="")
        raise AssertionError(f"unexpected command: {argv}")

    monkeypatch.setattr(network.subprocess, "run", run)

    assert network.active_wifi_ssid() == "Example Net"
    assert network.current_connection_entry()["ssid"] == "Example Net"


def test_wpa_cli_is_not_used_when_nmcli_answers(monkeypatch):
    monkeypatch.setattr(network, "_ssid_from_nmcli", lambda: "From NM")
    monkeypatch.setattr(network, "_ssid_from_wpa_cli", lambda: "From wpa")
    assert network.active_wifi_ssid() == "From NM"


def test_falls_back_to_wpa_cli_when_nmcli_finds_nothing(monkeypatch):
    monkeypatch.setattr(network, "_ssid_from_nmcli", lambda: None)
    monkeypatch.setattr(network, "_ssid_from_wpa_cli", lambda: "From wpa")
    assert network.active_wifi_ssid() == "From wpa"


def test_wpa_cli_reads_the_ssid_of_an_associated_interface(monkeypatch):
    monkeypatch.setattr(network.shutil, "which", lambda name: "/usr/sbin/wpa_cli")
    monkeypatch.setattr(network, "_wireless_interfaces", lambda: ["wlan0"])
    monkeypatch.setattr(network.subprocess, "run", _fake_run(WPA_STATUS))
    assert network._ssid_from_wpa_cli() == "Example Net"


def test_wpa_cli_ignores_an_interface_that_is_not_associated(monkeypatch):
    monkeypatch.setattr(network.shutil, "which", lambda name: "/usr/sbin/wpa_cli")
    monkeypatch.setattr(network, "_wireless_interfaces", lambda: ["wlan0"])
    monkeypatch.setattr(
        network.subprocess, "run", _fake_run("wpa_state=SCANNING\nssid=Example Net\n")
    )
    assert network._ssid_from_wpa_cli() is None


def test_wpa_cli_is_skipped_when_the_binary_is_missing(monkeypatch):
    monkeypatch.setattr(network.shutil, "which", lambda name: None)
    assert network._ssid_from_wpa_cli() is None


def test_wpa_cli_survives_a_timeout(monkeypatch):
    monkeypatch.setattr(network.shutil, "which", lambda name: "/usr/sbin/wpa_cli")
    monkeypatch.setattr(network, "_wireless_interfaces", lambda: ["wlan0"])

    def boom(argv, **kwargs):
        raise subprocess.TimeoutExpired(argv, 5)

    monkeypatch.setattr(network.subprocess, "run", boom)
    assert network._ssid_from_wpa_cli() is None


def test_wireless_interfaces_reads_sysfs(monkeypatch, tmp_path):
    for name, wireless in (("eth0", False), ("wlan1", True), ("wlan0", True)):
        (tmp_path / name).mkdir()
        if wireless:
            (tmp_path / name / "wireless").mkdir()

    real_path = network.Path

    def fake_path(arg):
        return tmp_path if str(arg) == "/sys/class/net" else real_path(arg)

    monkeypatch.setattr(network, "Path", fake_path)
    assert network._wireless_interfaces() == ["wlan0", "wlan1"]


def test_wireless_interfaces_is_empty_without_sysfs(monkeypatch):
    def boom(arg):
        raise OSError("no sysfs")

    monkeypatch.setattr(network, "Path", boom)
    assert network._wireless_interfaces() == []


def test_current_connection_entry_uses_the_label_without_an_ssid(monkeypatch):
    monkeypatch.setattr(network, "active_wifi_ssid", lambda: None)
    assert network.current_connection_entry()["ssid"] == network.CURRENT_CONNECTION_LABEL


def test_current_connection_entry_prefers_the_real_ssid(monkeypatch):
    monkeypatch.setattr(network, "active_wifi_ssid", lambda: "Example Net")
    assert network.current_connection_entry()["ssid"] == "Example Net"
