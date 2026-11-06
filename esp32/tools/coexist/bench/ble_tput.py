#!/usr/bin/env python3
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
"""BLE throughput against the basic service's throughput characteristic.

Downlink: write a 4-byte count, the device notifies that many bytes back.
Uplink: --up bytes of writes without response, timed by the device.
Needs bleak.
"""
import argparse
import asyncio
import struct
import time

from bleak import BleakClient, BleakScanner

TPUT = "4d475342-0003-4000-8000-6c696e6b6267"
STATUS = "4d475342-0002-4000-8000-6c696e6b6267"


async def run(name, down_bytes, up_bytes, hold):
    dev = await BleakScanner.find_device_by_name(name, timeout=20)
    if not dev:
        print(f"RESULT error=not_found name={name}")
        return
    async with BleakClient(dev) as c:
        mtu = c.mtu_size
        got = 0
        first = last = None
        done = asyncio.Event()

        def on_note(_, data):
            nonlocal got, first, last
            now = time.monotonic()
            first = first or now
            last = now
            got += len(data)
            if got >= down_bytes:
                done.set()

        await c.start_notify(TPUT, on_note)
        status = (await c.read_gatt_char(STATUS)).decode(errors="replace")
        print(f"RESULT connected mtu={mtu} status={status}")
        t0 = time.monotonic()
        await c.write_gatt_char(TPUT, struct.pack("<I", down_bytes), response=False)
        try:
            await asyncio.wait_for(done.wait(), timeout=120)
        except asyncio.TimeoutError:
            pass
        dt = (last - t0) if last else 0
        print(f"RESULT down bytes={got} secs={dt:.2f} kbps={got * 8 / dt / 1000 if dt else 0:.1f}")
        # Uplink: queue a fixed amount of writes without response, then read
        # what the device counted and over how long (first to last write).
        await c.write_gatt_char(TPUT, b"\x00", response=False)
        chunk = (bytes(range(256)) * 2)[: max(20, mtu - 3)]
        if len(chunk) == 4 or len(chunk) == 1:
            chunk += b"\x00"
        total = up_bytes
        for _ in range(total // len(chunk)):
            await c.write_gatt_char(TPUT, chunk, response=False)
        want = (total // len(chunk)) * len(chunk)
        st = {}
        t_end = time.monotonic() + 120
        while time.monotonic() < t_end:
            try:
                raw = (await asyncio.wait_for(c.read_gatt_char(STATUS), 10)).decode()
                st = dict(kv.split(":", 1) for kv in raw.strip("{}").replace('"', "").split(","))
                if int(st["rx"]) >= want:
                    break
            except (asyncio.TimeoutError, KeyError, ValueError):
                pass
            await asyncio.sleep(0.5)
        rx, ms = int(st.get("rx", 0)), int(st.get("rx_ms", 0))
        print(f"RESULT up bytes={rx}/{want} secs={ms / 1000:.2f} kbps={rx * 8 / ms if ms else 0:.1f}")
        if hold:
            await asyncio.sleep(hold)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True)
    ap.add_argument("--down", type=int, default=200_000)
    ap.add_argument("--up", type=int, default=200_000)
    ap.add_argument("--hold", type=float, default=0)
    a = ap.parse_args()
    asyncio.run(run(a.name, a.down, a.up, a.hold))


if __name__ == "__main__":
    main()
