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

"""BlueZ GATT peripheral for Muse Gadget setup.

Advertises the setup service and exposes two characteristics: RX (the phone
writes commands) and TX (the device notifies responses). Runs a GLib main
loop on the calling thread; ``send_packets`` must be called from another
thread because it paces notifications.
"""

from __future__ import annotations

import logging
import signal
import threading
import time
from typing import Callable

import dbus
import dbus.exceptions
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

from musegadget.ble_framing import CHUNK_STAGGER_S, MAX_PACKET_BYTES

log = logging.getLogger(__name__)

BLUEZ_SERVICE = "org.bluez"
DBUS_OM_IFACE = "org.freedesktop.DBus.ObjectManager"
DBUS_PROP_IFACE = "org.freedesktop.DBus.Properties"
GATT_MANAGER_IFACE = "org.bluez.GattManager1"
GATT_SERVICE_IFACE = "org.bluez.GattService1"
GATT_CHRC_IFACE = "org.bluez.GattCharacteristic1"
LE_ADV_MANAGER_IFACE = "org.bluez.LEAdvertisingManager1"
LE_ADV_IFACE = "org.bluez.LEAdvertisement1"
ADAPTER_IFACE = "org.bluez.Adapter1"
DEVICE_IFACE = "org.bluez.Device1"

SERVICE_UUID = "7fdd3d1c-38ea-46cf-8b46-314ecf5f240c"
RX_UUID = "4d593029-28a2-4a6e-a1f0-3c2d5e8f9b01"
TX_UUID = "d75dc4ca-7b2b-4e9c-8f0a-1d2e3f4a5b6c"

# Manufacturer data the apps read as the device's paired flag. 0xFFFF is the
# unassigned company id; the value is informational, not authenticated.
PAIRED_FLAG_COMPANY_ID = 0xFFFF

APP_PATH = "/org/musegadget/app0"
ADV_PATH = "/org/musegadget/advertisement0"

# Until BlueZ reports the negotiated MTU, assume one large enough for full
# 160-byte packets; phones running the Muse app negotiate at least that.
_ASSUMED_MTU = MAX_PACKET_BYTES + 3


def _find_adapter(bus: dbus.SystemBus) -> str:
    objects = bus.get_object(BLUEZ_SERVICE, "/").GetManagedObjects(dbus_interface=DBUS_OM_IFACE)
    for path, ifaces in objects.items():
        if ADAPTER_IFACE in ifaces:
            return path
    raise RuntimeError("no Bluetooth adapter found")


class Advertisement(dbus.service.Object):
    def __init__(self, bus: dbus.SystemBus, local_name: str) -> None:
        self._local_name = local_name
        super().__init__(bus, ADV_PATH)

    def properties(self) -> dict:
        return {
            "Type": dbus.String("peripheral"),
            "LocalName": dbus.String(self._local_name),
            "ServiceUUIDs": dbus.Array([SERVICE_UUID], signature="s"),
            "ManufacturerData": dbus.Dictionary(
                {dbus.UInt16(PAIRED_FLAG_COMPANY_ID): dbus.Array([dbus.Byte(0)], signature="y")},
                signature="qv",
            ),
            "Includes": dbus.Array([], signature="s"),
        }

    @dbus.service.method(DBUS_PROP_IFACE, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return self.properties() if interface == LE_ADV_IFACE else {}

    @dbus.service.method(LE_ADV_IFACE, in_signature="", out_signature="")
    def Release(self):
        log.info("advertisement released by BlueZ")


class Application(dbus.service.Object):
    def __init__(self, bus: dbus.SystemBus) -> None:
        self.services: list[Service] = []
        super().__init__(bus, APP_PATH)

    @dbus.service.method(DBUS_OM_IFACE, out_signature="a{oa{sa{sv}}}")
    def GetManagedObjects(self):
        objects = {}
        for service in self.services:
            objects[service.path] = service.properties()
            for chrc in service.characteristics:
                objects[chrc.path] = chrc.properties()
        return objects


class Service(dbus.service.Object):
    def __init__(self, bus: dbus.SystemBus, index: int, uuid: str) -> None:
        self.path = dbus.ObjectPath(f"{APP_PATH}/service{index}")
        self.uuid = uuid
        self.characteristics: list[Characteristic] = []
        super().__init__(bus, self.path)

    def properties(self) -> dict:
        return {
            GATT_SERVICE_IFACE: {
                "UUID": self.uuid,
                "Primary": True,
                "Characteristics": dbus.Array([c.path for c in self.characteristics], signature="o"),
            }
        }

    @dbus.service.method(DBUS_PROP_IFACE, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return self.properties().get(interface, {})


class Characteristic(dbus.service.Object):
    def __init__(
        self,
        bus: dbus.SystemBus,
        service: Service,
        index: int,
        uuid: str,
        flags: list[str],
        on_write: Callable[[bytes, dict], None] | None = None,
    ) -> None:
        self.path = dbus.ObjectPath(f"{service.path}/char{index}")
        self._service_path = service.path
        self._uuid = uuid
        self._flags = flags
        self._on_write = on_write
        self.notifying = False
        self._value: list = []
        super().__init__(bus, self.path)

    def properties(self) -> dict:
        return {
            GATT_CHRC_IFACE: {
                "Service": self._service_path,
                "UUID": self._uuid,
                "Flags": dbus.Array(self._flags, signature="s"),
                "Descriptors": dbus.Array([], signature="o"),
            }
        }

    @dbus.service.method(DBUS_PROP_IFACE, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return self.properties().get(interface, {})

    @dbus.service.method(GATT_CHRC_IFACE, in_signature="a{sv}", out_signature="ay")
    def ReadValue(self, options):
        return dbus.Array(self._value, signature="y")

    @dbus.service.method(GATT_CHRC_IFACE, in_signature="aya{sv}", out_signature="")
    def WriteValue(self, value, options):
        if self._on_write:
            self._on_write(bytes(value), dict(options))

    @dbus.service.method(GATT_CHRC_IFACE)
    def StartNotify(self):
        log.info("client subscribed to %s", self._uuid)
        self.notifying = True

    @dbus.service.method(GATT_CHRC_IFACE)
    def StopNotify(self):
        self.notifying = False

    def notify(self, value: bytes) -> bool:
        self._value = [dbus.Byte(b) for b in value]
        if self.notifying:
            self.PropertiesChanged(GATT_CHRC_IFACE, {"Value": dbus.Array(self._value, signature="y")}, [])
        return False  # one-shot GLib idle callback

    @dbus.service.signal(DBUS_PROP_IFACE, signature="sa{sv}as")
    def PropertiesChanged(self, interface, changed, invalidated):
        pass


class BleServer:
    """Owns the adapter while setup is open; implements the setup Transport."""

    def __init__(
        self,
        local_name: str,
        on_write: Callable[[bytes], None],
        on_disconnect: Callable[[], None],
    ) -> None:
        self._local_name = local_name
        self._on_write = on_write
        self._on_disconnect = on_disconnect
        self._mtu = _ASSUMED_MTU
        self._device_path: str | None = None
        self._lock = threading.Lock()
        self._loop: GLib.MainLoop | None = None
        self._bus: dbus.SystemBus | None = None
        self._adapter_path = ""
        self._saved_adapter: dict = {}
        self._tx: Characteristic | None = None

    # -- Transport --------------------------------------------------------------

    def mtu(self) -> int:
        with self._lock:
            return self._mtu

    def send_packets(self, packets: list[bytes]) -> None:
        for i, packet in enumerate(packets):
            if i:
                time.sleep(CHUNK_STAGGER_S)
            GLib.idle_add(self._tx.notify, packet)

    def disconnect(self, delay: float) -> None:
        GLib.timeout_add(int(delay * 1000), self._disconnect_now)

    # -- Lifecycle --------------------------------------------------------------

    def run(self) -> None:
        """Start advertising and serve until :meth:`stop`. Blocks."""
        dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
        bus = self._bus = dbus.SystemBus()
        self._adapter_path = _find_adapter(bus)
        adapter = dbus.Interface(bus.get_object(BLUEZ_SERVICE, self._adapter_path), DBUS_PROP_IFACE)
        self._saved_adapter = {k: adapter.Get(ADAPTER_IFACE, k) for k in ("Alias", "Pairable")}
        adapter.Set(ADAPTER_IFACE, "Powered", dbus.Boolean(True))
        # The phone reads the GAP name as well as the advertised one.
        adapter.Set(ADAPTER_IFACE, "Alias", dbus.String(self._local_name))
        adapter.Set(ADAPTER_IFACE, "Pairable", dbus.Boolean(False))

        app = Application(bus)
        service = Service(bus, 0, SERVICE_UUID)
        service.characteristics.append(Characteristic(
            bus, service, 0, RX_UUID, ["write", "write-without-response"], on_write=self._handle_write,
        ))
        self._tx = Characteristic(bus, service, 1, TX_UUID, ["read", "notify"])
        service.characteristics.append(self._tx)
        app.services.append(service)
        self._objects = (app, service, self._tx, Advertisement(bus, self._local_name))

        bus.add_signal_receiver(
            self._handle_device_change,
            dbus_interface=DBUS_PROP_IFACE,
            signal_name="PropertiesChanged",
            arg0=DEVICE_IFACE,
            path_keyword="path",
        )
        adapter_obj = bus.get_object(BLUEZ_SERVICE, self._adapter_path)
        dbus.Interface(adapter_obj, GATT_MANAGER_IFACE).RegisterApplication(
            APP_PATH, {},
            reply_handler=lambda: log.info("GATT service registered"),
            error_handler=lambda e: self._fatal("GATT registration failed", e),
        )
        dbus.Interface(adapter_obj, LE_ADV_MANAGER_IFACE).RegisterAdvertisement(
            ADV_PATH, {},
            reply_handler=lambda: log.info("advertising as %s", self._local_name),
            error_handler=lambda e: self._fatal("advertising failed", e),
        )
        self._loop = GLib.MainLoop()
        # The GLib loop blocks Python's own signal handling; without these a
        # `systemctl stop` would skip the teardown that restores the adapter.
        for signum in (signal.SIGTERM, signal.SIGINT):
            GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signum, self._loop.quit)
        try:
            self._loop.run()
        finally:
            self._teardown()

    def stop(self) -> None:
        if self._loop:
            GLib.idle_add(self._loop.quit)

    # -- Internals (GLib thread) ------------------------------------------------

    def _handle_write(self, value: bytes, options: dict) -> None:
        with self._lock:
            if "mtu" in options and int(options["mtu"]) != self._mtu:
                self._mtu = int(options["mtu"])
                log.info("ATT MTU %d", self._mtu)
            if "device" in options:
                self._device_path = str(options["device"])
        self._on_write(value)

    def _handle_device_change(self, interface, changed, invalidated, path=None):
        if "Connected" in changed:
            log.info("device %s %s", path, "connected" if changed["Connected"] else "disconnected")
        # dbus-python hands over dbus.Boolean values, which are never the
        # False singleton, so test the value rather than its identity.
        if "Connected" in changed and not changed["Connected"]:
            with self._lock:
                ours = self._device_path in (None, path)
                if ours:
                    self._device_path = None
                    self._mtu = _ASSUMED_MTU
            if ours:
                self._on_disconnect()

    def _disconnect_now(self) -> bool:
        with self._lock:
            path = self._device_path
        if path:
            try:
                device = dbus.Interface(self._bus.get_object(BLUEZ_SERVICE, path), DEVICE_IFACE)
                device.Disconnect(timeout=5)
            except dbus.exceptions.DBusException as err:
                log.warning("disconnect failed: %s", err)
        return False

    def _fatal(self, what: str, err: Exception) -> None:
        log.error("%s: %s", what, err)
        self.stop()

    def _teardown(self) -> None:
        bus = self._bus
        adapter_obj = bus.get_object(BLUEZ_SERVICE, self._adapter_path)
        for iface, method, path in (
            (LE_ADV_MANAGER_IFACE, "UnregisterAdvertisement", ADV_PATH),
            (GATT_MANAGER_IFACE, "UnregisterApplication", APP_PATH),
        ):
            try:
                getattr(dbus.Interface(adapter_obj, iface), method)(path)
            except dbus.exceptions.DBusException:
                pass
        adapter = dbus.Interface(adapter_obj, DBUS_PROP_IFACE)
        for key, value in self._saved_adapter.items():
            try:
                adapter.Set(ADAPTER_IFACE, key, value)
            except dbus.exceptions.DBusException as err:
                log.warning("could not restore adapter %s: %s", key, err)
