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

# Avatar studio

Customize supported character templates in a local browser, without a board,
SDK token, or Muse connection. The preview compiles and runs the same C renderer
as the firmware. There are no remote scripts, fonts, or network services.

## Start

From `esp32/`, with Python 3.9 or newer and a C compiler on PATH:

```sh
python3 -m pip install pillow
python3 tools/muse/customize.py
```

The tool prints a loopback URL and opens it in your browser. Use `--no-browser`
to open it yourself, or `--port 8765` to choose a port. Ctrl-C stops the server.
`CC` can select a compiler, including compiler flags.

Choose a character, change fur/face/eye/cheek colours, body width and height,
face placement, and a scarf or bow. Colours retain the template's shading.
Preview boot, idle, listening, thinking, speaking, happy, off, and error;
pause or scrub any animation. Speaking and listening use the SDK's sample
speech envelope. These recorded previews do not replace live audio responses
in the firmware. Shape controls have bounded ranges to fit the 64x64 canvas.

The preset name labels the export; it does not rename the paired device.
Saving a preset downloads JSON; Load preset restores it. Changes are not
installed on your board until you build and flash.

## Export and install

Download character pack produces a ZIP containing:

- `preset.json`: versioned settings, with no tokens or machine paths.
- `muse_pixel.c`: the selected renderer with compile-time appearance overrides.
- `gifs/`: one native-resolution animated GIF per state.
- `sprites/`: one PNG sprite atlas per state and `manifest.json` with frame
  dimensions, count, grid columns, 40 ms frame timing, and loop hints.
- `ARTWORK_NOTICE.txt`: the selected character's attribution/licensing notice.

Sprites are opaque RGB, including the glow, shadow, and black background.
Frames are row-major; unused cells at the end of an atlas are not frames.
Boot, happy, and off are one-shot clips. The sprite export is for other tools;
the ESP32 still uses the procedural C renderer to keep continuous motion and
live speech without storing hundreds of bitmap frames in flash.

Extract the ZIP, then install its preset locally:

```sh
python3 tools/muse/customize.py --preset /path/to/preset.json --install
```

This validates the settings, compiles the renderer, and runs all eight states
before replacing `components/muse/avatar/muse_pixel.c`. The previous renderer
is saved as `muse_pixel.c.prev`. Both renderer and preset stay in the ignored
local avatar directory. It does not contact Muse, build ESP-IDF, or flash.

For an M5Stack StickS3, use your existing private SDK-token build configuration:

```sh
tools/muse/board.sh build sticks3
tools/muse/board.sh flash sticks3
```

Other boards use their own profile; see `../../devices/README.md`. Firmware,
GIF tools, and desktop simulator choose the same local override. Remove the
local `muse_pixel.c` to return to the default. Existing `avatar.py --reply`
and `--edit` workflows continue to work; `--edit` can change the generated C,
while loading the JSON preset recreates the selected template.

For headless exports:

```sh
python3 tools/muse/customize.py --preset /path/to/preset.json --out character.zip
```

## Templates and appearance hooks

The tool always provides the default Jollybot template. Additional character
packs live at `avatar/packs/<id>/`, with `muse_pixel.c` and a `pack.json` holding
`name` and `notice`. Pack IDs use lowercase letters, numbers, and hyphens.
Only registered local templates are compiled; browser presets cannot include
C code or filesystem paths.

Supported renderers include `muse_avatar_style.h` and use its `MUSE_STYLE_*`
compile-time constants. Default values preserve the original renderer exactly.
New palettes, proportions, and accessories add no runtime configuration,
dynamic allocation, or changes to the public `muse_pixel.h` API. A template
must implement the hooks it exposes. Arbitrary AI-generated renderers still
work through `avatar.py`, but are not automatically parameterized by this tool.

The server binds only to 127.0.0.1, validates Host and Origin, and requires a
per-session header for preview/export requests. It never serves arbitrary
workspace files or exposes a browser endpoint that installs or flashes.

## Artwork

The customizer tooling is Apache-licensed. This does not change the licensing
of the character templates or their generated artwork. The SDK's Apache
license excludes Jollybot. Preserve its Meta copyright notice, and do not
label derived renderers or sprite exports Apache-licensed. A pack's notice
travels with every exported ZIP.

## Test and CI integration

Run the customizer tests with Pillow installed:

```sh
python3 -m unittest discover -s tests -p 'test_avatar_customizer.py' -v
CC="cc -fsanitize=address,undefined -fno-sanitize-recover=all" \
  python3 -m unittest discover -s tests -p 'test_avatar_customizer.py' -v
```

The tests cover preset validation, exact default rendering, all animation
states, accessories, exports, backup/install, and local HTTP access. Renderer
tests skip when Pillow is unavailable; the remaining schema/HTTP tests run
without it. Some macOS compiler runtimes cannot initialize AddressSanitizer;
use UndefinedBehaviorSanitizer locally and run both on Linux.

`customizer-ci.patch` adds Pillow installation and a dedicated sanitized
customizer run to the existing ESP32 host-test job. It is provided as an
unapplied patch for maintainers because the contributor's GitHub sign-in
could not modify workflow files. Review and apply it from the repository root:

```sh
git apply --unidiff-zero esp32/tools/muse/customizer-ci.patch
```
