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

# Lua

The Lua interpreter, for the SenseCAP Watcher's on-device scripts
(`main/muse_script.c`). It's built only with `CONFIG_MUSE_SCRIPTS`.

| | |
|---|---|
| Upstream | https://www.lua.org |
| Version | 5.4.9, from <https://www.lua.org/ftp/lua-5.4.9.tar.gz> (SHA-256 `2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6`) |
| License | MIT. See [`LICENSE`](LICENSE), copied from the notice at the end of `src/lua.h`. |

## What's upstream and what's ours

| File | From | License |
|---|---|---|
| `src/*` | upstream, unmodified, except `lstrlib.c` | MIT |
| `src/lstrlib.c` | upstream, with one change marked `SANDBOX PATCH`: `LUAI_MATCHSTEP_FUNC` lets the sandbox charge pattern-matching steps to a script's CPU budget, since count hooks don't run inside C | MIT |
| `LICENSE` | upstream's notice from `src/lua.h` | MIT |
| `CMakeLists.txt` | Meta | Apache-2.0 |
| `README.md` | Meta | Apache-2.0 |

Only the core and the libraries scripts get are here: no `lua.c`, `luac.c`,
`liolib.c`, `loslib.c`, `loadlib.c`, `ldblib.c` or `linit.c`.

Don't edit or restyle `src/`, and don't add a Meta copyright header to it.

## Updating

1. Copy the files listed in `src/` from the new release's `src/`, unchanged.
2. Apply the `SANDBOX PATCH` to `lstrlib.c` again.
3. Check that the notice at the end of `src/lua.h` still matches `LICENSE`.
4. Update the version and hash above.
