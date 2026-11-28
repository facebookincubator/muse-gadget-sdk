<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# smp_link: the Muse apps' firmware update, end to end, on the host

The XIAO's update path, driven by the Muse apps' own update code, with no
board and no phone:

- **The gadget** (`src/`, native_sim): Zephyr's real MCUmgr (img_mgmt and
  os_mgmt with the board's buffers, 512 B x 4), fed through Zephyr's own
  `smp_reassembly` as `smp_bt` feeds it; the board's own `mg_ota.c` (the
  healthy-mark confirm, the orderly reset, the reassembly gap check); and
  MCUboot's real loader (`boot_go`, built from the MCUboot module) run at
  every start on the simulated flash: swap using move, as on the XIAO.
  An SMP reset ends the process with status 77 and `run.py serve` starts it
  again, MCUboot first, on the same flash file.
- **The link** (`src/link.c`) is the conformance link that
  `esp32/components/muse_gadget_ble/smp/conformance/smplink.py` speaks: fake
  GATT over UDP. ATT writes in, at most
  MTU-3 and 512 bytes each (Android refuses longer writes), and
  notifications out; plus connect, disconnect (the gadget
  sends one before its reset, so the phone sees the disconnect at once; a
  client's disconnect with the value byte 1 is a central that never
  subscribed, whose partial request stays, as Zephyr's smp_bt leaves it), ready, drop-next (lose writes)
  and status (a JSON line: running and staged versions, confirmed, MCUboot's
  swap type, healthy, boot number), and auth (0x09: this connection proven
  or not, as a token proof or its lapse would; each connection starts as
  `--proven` says and a disconnect clears it; the update gate with
  `--release`, set up or not), press (0x41: a real press of the talk button),
  injected press (0x42: a console word, never a confirm on release) and
  Forget (0x43: the connection's proof ends and the store's erase is
  reported, as `mg_setup_factory_reset()` does).
- **The clients** are `smplink.AndroidFlow` and `IOSFlow` from that same
  file, which model the Muse apps' update flows.

`e2e.py` is the suite: an update from both apps at MTU 247 (Android's at 23 and 517 too),
resume after a dropped link, a stale partial request dropped after the gap (a
lost write), a partial request from a central that never subscribed dropped
before the next connection's writes can complete it (`SourceGen`),
a corrupted payload refused by MCUboot (`OLD_IMAGE_RUNNING`), and the truth of
image state: a never-healthy image reverts, an image stays on test until a
link proves ATT works (a connect does, as the gadget's own MTU exchange) and
then keeps itself without a client's confirm. Memory (update support takes no base memory): nothing held at boot, after requests, after
an upload, or after a stopped upload (its writer freed at the session's end;
both apps then resume it from what is on flash, or start over when it stopped
within the first block) (status `smp_blocks`).

## Running it

native_sim needs Linux: build and run the gadget in the x86 container from
`../../AGENTS.md` (From a Mac), with the workspace in the `zws86` volume. One
long-lived container serves the link on TCP 47100; the clients run on the Mac.

```sh
docker --context colima-zephyr run -d --name mg-smp-link --platform linux/amd64 \
  -v zws86:/zp -e ZEPHYR_TOOLCHAIN_VARIANT=host -e ZEPHYR_BASE=/zp/zephyr \
  -p 127.0.0.1:47100:47100/tcp -w /zp mgzephyr86 sleep infinity
# stream the sources in
tar -C <sdk> --no-xattrs -cf - zephyr/src zephyr/tests/smp_link protocols | \
  docker --context colima-zephyr exec -i mg-smp-link bash -c 'mkdir -p /work && tar -xf - -C /work'
# build, into a new private directory
B=$(docker --context colima-zephyr exec mg-smp-link mktemp -d)
docker --context colima-zephyr exec mg-smp-link bash -c "source /venv/bin/activate && cd /zp && \
  west build -b native_sim /work/zephyr/tests/smp_link -d $B"
```

Under emulation the compiler or Python now and then crashes
(`internal compiler error`, `Segmentation fault`): run the build again.

Test images need no SDK token: build them without one. `run.py image` refuses
to write an image that has the token compiled in anywhere but a directory of
yours with mode 0700 (its files 0600): such an image is a secret (`../../../AGENTS.md`).

Test images are the board's own build, signed again with other versions
(MCUboot's dev key; the simulated loader checks hashes, not signatures),
copied to `/simimg/sim` in the container. A board build may hold your SDK
token, and so do these copies: `run.py` writes them (and the simulated
flash) owner-only; keep `sim/` in a private directory and the container to
yourself, never in shared storage:

```sh
python3 run.py image <build-xiao>/zephyr/zephyr/zephyr.bin sim/xn54-A.bin --version 0.2.0+428
python3 run.py image <build-xiao>/zephyr/zephyr/zephyr.bin sim/xn54-B.bin --version 0.2.1+1
python3 run.py image <build-xiao>/zephyr/zephyr/zephyr.bin sim/xn54-C.bin --version 0.2.2+1
# and <prefix>-B-corrupt.bin: B with one payload bit flipped (the TLV hash untouched)
tar -C <dir holding sim/> --no-xattrs -cf - sim | \
  docker --context colima-zephyr exec -i mg-smp-link bash -c 'mkdir -p /simimg && tar -xf - -C /simimg'
```

The two relay ends carry the UDP link over colima's TCP forward. relay-in
also paces the client's writes like a BLE link (10 per 7.5 ms), so a burst of
small writes isn't lost in the gadget's UDP buffer. A BLE link never loses a
write that way.

```sh
docker --context colima-zephyr exec -d mg-smp-link bash -c \
  'python3 /work/zephyr/tests/smp_link/run.py relay-in > /sim/relay.log 2>&1'
python3 run.py relay-out &          # on the Mac: the gadget at 127.0.0.1:47000
```

Then the suite, on the Mac (it starts and restarts the gadget itself):

```sh
SMP_LINK_EXE=$B/zephyr/zephyr.exe SMP_LINK_IMAGES=<dir holding sim/>/sim python3 -m unittest e2e -v
```

Each run keeps its flash and log in its own directory in the container and
stops only the gadget it started. A second run at the same time needs its own
gadget port (`SMP_LINK_PORT`) and relay ends for it (`--udp`, `--tcp`).
