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

# Test vectors

Byte-for-byte vectors for the musegadgets BLE protocol
([`../mgcommands.h`](../mgcommands.h)), and the Python references that make
and check them. Firmware and apps check themselves against the JSON files.

## License

Every file in this directory is under the same license as the rest of the
repository: Apache-2.0, Copyright (c) Meta Platforms, Inc. and affiliates
(see [`LICENSE`](../../LICENSE)). JSON can't carry a comment, so this
notice covers the `.json` files; the references carry the header themselves.

## Files

| File | What it is |
|---|---|
| `mg-token-proof-v1.json`, `mg_token_proof_ref.py` | Token proof: the key, the challenge and confirm MACs (mgcommands.h, Token proof). |
| `mg-session-v1.json`, `mg_session_ref.py` | Device session exchanges, with a reference device that plays the device side (mgcommands.h, Device session). |
| `mg-session-seal-v1.json`, `mg_session_seal_ref.py` | Sealed device session messages: credentials and data (mgcommands.h, Sealed credentials). |
| `mg-skills-v2.json`, `mg_skills_ref.py` | Skills exchanges, with a reference skills engine (mgcommands.h, Skills). |
| `mg-framed-v1.json`, `mg_framed_ref.py` | Framed Data: a video stream and audio playback sharing the Data characteristic (mgcommands.h, `mg_data_type_framed`). |
| `mg-update-v2.json`, `mg_update_ref.py` | Bluetooth firmware updates over SMP: the confirm press a link without a matched proof needs on a release build, with a reference update gate (mgcommands.h, Token proof). |
| `mg-haptic-v1.json` | Haptics scenarios: device_action packets and local events against the device's answers and its motor (mgcommands.h, Haptics). No reference; a device's haptics tests replay it. |

## Running the references

Python 3.9+, standard library only. From the repository root, each reference
checks the JSON file next to it and exits non-zero on a mismatch:

```sh
python3 protocols/test-vectors/mg_token_proof_ref.py
python3 protocols/test-vectors/mg_session_seal_ref.py
python3 protocols/test-vectors/mg_session_ref.py
python3 protocols/test-vectors/mg_skills_ref.py
python3 protocols/test-vectors/mg_framed_ref.py
python3 protocols/test-vectors/mg_update_ref.py
```

`--write` regenerates a reference's file. The inputs are fixed, so the
output doesn't change unless the reference does.
