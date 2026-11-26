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

# smp: vendored copy of the MCUmgr SMP core

**A copy of the Muse Gadgets MCUmgr SMP core, release smp-v11**.
smp-v11 adds two optional, NULL-safe platform hooks (API v3, appended; a port that sets neither answers as
smp-v10): `refusal_rc(ctx)`, the rc of a `may_change`/`may_upload` refusal (only `MG_SMP_EBUSY` passes, the
gadget asking for its confirm press while the client retries; any other answer is rc 11), and
`may_upload(ctx, len, sha32)`, asked instead of `may_change(MG_SMP_OP_UPLOAD)` with the image the upload
names (at offset 0 its len and whole-image SHA-256, on a chunk at an offset the upload it continues; the
SHA-256 only where the core checks it at the end, `MG_SMP_FILE_SHA`, so a confirmation bound to it covers
that image alone). `../mg_ble.c` sets both (`smp_may_upload`, `smp_refusal_rc`); `conformance/smplink.py`
gains `Link.press()` for a test server's confirm prompt.
smp-v10: `mg_smp_healthy()` returns the backend confirm's answer (0, `MG_SMP_PENDING`, or its failure:
the image stays a test image and reverts at the next reset unless confirmed later; `../mg_ble.c` tries
the keep again, paced and bounded); after a dropped link only the interrupted upload's own resume (a chunk
at the core's offset, or offset 0 with the same len and SHA-256) stops its hold's clock, so image state
reads on a new link no longer keep it, or the update claim; `pack.py` writes its output 0600. smp-v9: a
test or confirm naming the staged upload commits it before any "already running" answer; an upload carrying
the running image's id but not the hash its slot reports is refused at its last chunk (rc 6, EBADSTATE),
unless the slot reports `MG_SMP_SLOT_HAS_HASH`; two ids match only with equal build ids (0 matches only 0).
This port's running slot reports its ELF build id and no hash of its own.
smp-v8 adds the optional `may_change(ctx, op)` platform hook
(platform API v3): the core asks it before each upload, image state write,
OS reset and erase, answers a refusal with rc 11 (`MG_SMP_EACCESSDENIED`)
with nothing changed, never asks for a read, and lets a transfer a
connection started while allowed finish on that connection (its chunks, the
retry from offset 0, the state write once staged, the reset after the
commit) until the link drops. `../mg_ble.c` wires it (`smp_may_change`,
`../mg_smp_gate.h`: at once for a connection whose token proof matched,
after the wired console's `update.arm`, in a SAFE the user entered with the
key and on bench, automation and test builds; any other link, set up or
not, after one confirm press on the gadget). smp-v7 changed docs and tests only. smp-v6 caps every response
notification, and the `max_writes` sizing, at min(MTU - 3, 512)
(mgcommands.h `MG_MAX_ATT_VALUE`): smp-v1..v5 cut them at MTU - 3, so at a
negotiated MTU 517 a response over 512 B went out in 514 B pieces, which
Android refuses. smp-v5 added compile-time
code-budget options, all off here (`MG_SMP_REVERT` must stay on: the ESP32
backends revert and list slots); smp-v4 added the optional `max_writes`
config field, left 0 here (the glue's ring FIFO holds the client's whole
window and never drops a write). All build on smp-v3
(update support takes no
base memory: the core keeps one pointer and takes its state and request slot
through the platform's `mem_get`/`mem_put` only while an update runs; const
env tables; the healthy mark is an event, `mg_smp_healthy()`; a link drop
mid-upload keeps the state alone for `resume_hold_s`). Every file here except
this one is the release's: `mg_smp_backend.h` (backend and platform API v3),
`mg_smp.h`, `mg_smp.c`, `mg_smp_sha256.c`, `pack.py` (the envelope packer,
byte-identical to imgtool) and `conformance/smplink.py`, but for comments:
this copy words some comments and docstrings differently (in `mg_smp_backend.h`,
`mg_smp.h`, `mg_smp.c` and `pack.py` it doesn't name the vendor SDKs the release's
backends were written for, which this SDK doesn't support); their code is the
release's but for the one local change listed below. Never patch them here
otherwise: take a new release whole, word those comments again, and update
this note and the hashes below. `tests/test_mg_smp.py` runs the ESP32 configuration against it.

## Local changes

One local change, until a release of the core carries it:

- `mg_smp.c` `h_upload`: a request the `may_change`/`may_upload` hooks allowed marks the transfer as this
  connection's (`F_XAUTH`, after which its chunks, the state write and the reset ask no hook) only once the core
  has taken it (`h_upload_take` returned no error); smp-v11 marked it before checking the request, so a request the
  core then refused (a missing field, a short len, a committed upload) still left the mark. Found with a
  request-smuggling report (the front now never forwards the bytes of a request it can't hold, see
  `../mg_smp_front.c`).

The smuggling fix here is a local change until the smp-v12.1 re-vendor: the core's smp-v12.1 release fixes
the same reports in the core itself (it carries only the upload an allowed request starts, and never parses the rest
of a refused request), and `tests/test_mg_smp.py` `Smuggling` ports its Smuggling rows to this port (one, "the
unasked state write names the carried upload", waits for the re-vendor). Until then the front also stands in for
v12.1's "carry only the upload an allowed request starts" on links the press opened (`../mg_smp_front.c`
`mg_smp_front_gate`).

The `mg_smp.c` line below is smp-v11's: this copy's code differs from it by that change.

## Code hashes

The SHA-256 of each file's code: a C file's tokens without its comments,
one per line; a Python file's syntax tree without docstrings. A comment edit
leaves them as they are; a code change no longer matches until the copy is a
release again and these lines name it.

```
5e93ba881deeaaf00fbc423a10e86868de112a35b4e0d60615455f04e3987316  mg_smp_backend.h
dd5e5b412492233433a355822e27415e202105aaeb5644a2081e10cb970f2971  mg_smp.h
5b4d41b460effb10c8e7b44ac7d9dfeaad6c949154654146e6d71ae59f74eeb5  mg_smp.c
921f0bb6c55b8a6e2781b4c6921acbb75188c8901f8751c97e03c7fce70b2baf  mg_smp_sha256.c
676d89cf366f7deee0641878a00a193cc73c1be611c267491dc99b195e36f183  pack.py
38fcdf7bf2dd9d41c04041290f94355954ed246ca2f69f0b476e9a0ba2cdea9a  conformance/smplink.py
```

The ESP32 side is outside this directory: `../mg_smp_esp.c` (the ESP-IDF
backend, the record in NVS, the running image's id, the per-unit rollback
probe), `../mg_smp_esp_util.c` (its pure helpers) and the GATT glue in
`../mg_ble.c` (inline, one 1 KB slot, buf_count 3 held by a FIFO between the
NimBLE host task and the worker), with the request front `../mg_smp_front.c`
ahead of the core: it hands the core whole requests, refuses one nested
deeper than 8 CBOR levels, and on a link that needs the confirm press asks
about each update change itself (mg-update-v2.json), since this core asks
`may_change`/`may_upload` only at a transfer's start.
