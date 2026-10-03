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

# minimp3

A small MP3 decoder by lieff, for playing replies through a TTS API of your own.

| | |
|---|---|
| Upstream | https://github.com/lieff/minimp3 |
| Version | commit `ea99364f61c14656440e8d77e9c233ccf3124633` (2026-07-27) |
| License | CC0-1.0, public domain. See [`LICENSE`](LICENSE). |

## What's upstream and what's ours

| File | From | License |
|---|---|---|
| `include/minimp3.h` | upstream, unmodified | CC0-1.0 |
| `LICENSE` | upstream, unmodified | CC0-1.0 |
| `src/minimp3.c` | Meta: builds the decoder once, MP3 only, no SIMD | Apache-2.0 |
| `CMakeLists.txt` | Meta | Apache-2.0 |
| `README.md` | Meta | Apache-2.0 |

Don't edit or restyle `include/minimp3.h`, and don't add a Meta copyright
header to it. Set build options in `src/minimp3.c` instead.

## Updating

1. Copy `minimp3.h` from the new upstream commit into `include/`, unchanged.
2. Check upstream's `LICENSE` is still CC0 and copy it over too.
3. Update the commit and date above.
