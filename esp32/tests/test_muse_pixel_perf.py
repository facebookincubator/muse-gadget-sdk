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

"""Exact default-avatar regression and native-host cost comparison.

Run: python3 -m unittest discover -s esp32/tests -p test_muse_pixel_perf.py -v
Optional benchmark (not a timing assertion, NOT C6 FPS):
    python3 esp32/tests/test_muse_pixel_perf.py --benchmark

No ESP-IDF, shared managed_components, hardware, serial or BLE is used.
The reference is a frozen copy of the pre-optimization renderer, not a
reimplementation of the optimized arithmetic. Both use the same compiler/libm.
"""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest

TESTS = Path(__file__).resolve().parent
ESP32 = TESTS.parent
INCLUDE = ESP32 / "components" / "muse"
CC = shlex.split(os.environ.get("CC", "cc"))
FLAGS = [
    "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
    "-fno-fast-math", "-ffp-contract=off", "-D_POSIX_C_SOURCE=200809L",
    "-I", str(INCLUDE),
]


def run(args, **kwargs):
    result = subprocess.run(args, text=True, capture_output=True, **kwargs)
    if result.returncode:
        raise RuntimeError(
            f"command failed ({result.returncode}): {shlex.join(map(str, args))}\n"
            f"{result.stdout}{result.stderr}"
        )
    return result


def wrapper(prefix, source):
    # Include real production/reference source in separate translation units.
    # Private state is exposed read-only to compare more than just final RGB565.
    # Only the sinf/cosf wrappers count operations; they return unmodified libm
    # values. Reset animation/palette state identically, leaving warm math LUTs.
    rename = "\n".join(
        f"#define muse_pixel_{name} {prefix}_{name}"
        for name in ("render", "set_size", "scale", "accent", "working_accent", "set_background")
    )
    cell = (
        "#define muse_pixel_cell_rgb565 opt_cell\n"
        if prefix == "opt" else ""
    )
    reference_cell = (
        "uint16_t ref_cell(int x, int y) { return s_pal[get_px(x, y)]; }\n"
        if prefix == "ref" else ""
    )
    return f"""#include <math.h>
static unsigned long trig_calls;
#define sinf(x) (++trig_calls, sinf(x))
#define cosf(x) (++trig_calls, cosf(x))
{rename}
{cell}
#include "{source.as_posix()}"
const uint8_t *{prefix}_fb(void) {{ return s_fb; }}
const uint8_t *{prefix}_mask(void) {{ return s_mask; }}
const uint16_t *{prefix}_pal(void) {{ return s_pal; }}
const uint16_t *{prefix}_dim(void) {{ return s_pal_dim; }}
int {prefix}_colors(void) {{ return C_COUNT; }}
unsigned long {prefix}_trig(void) {{ return trig_calls; }}
void {prefix}_reset(void) {{
    s_scheme_init = false;
    memset(s_scheme, 0, sizeof(s_scheme));
    memset(s_pal, 0, sizeof(s_pal));
    memset(s_pal_dim, 0, sizeof(s_pal_dim));
    s_eyes = (eyes_t){{ .next_blink = 1.5f, .blink_start = -10, .next_gaze = 1.0f }};
    s_rng = 0x9e3779b9u;
    trig_calls = 0;
}}
{reference_cell}
"""


def build(directory, sanitized=False):
    directory = Path(directory)
    opt = directory / "opt.c"
    ref = directory / "ref.c"
    opt.write_text(wrapper("opt", ESP32 / "avatar" / "muse_pixel.c"))
    ref.write_text(wrapper("ref", TESTS / "muse_pixel_perf_reference.c"))
    exe = directory / ("renderer-sanitized" if sanitized else "renderer")
    extra = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"] if sanitized else []
    run(CC + FLAGS + extra + [str(opt), str(ref), str(TESTS / "muse_pixel_perf_harness.c"), "-lm", "-o", str(exe)])
    return exe


class MusePixelPerfTests(unittest.TestCase):

    def test_address_and_undefined_behavior_sanitizers(self):
        with tempfile.TemporaryDirectory(prefix="muse-pixel-perf-san-") as directory:
            # Clang on macOS has no LeakSanitizer; ASan and UBSan remain enabled.
            env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1")
            result = run([str(build(directory, sanitized=True))], env=env, timeout=180)
            self.assertIn("exact match: 12400 frames", result.stdout)
            self.assertEqual(result.stderr, "")
            print("ASan+UBSan: " + result.stdout.strip())

    def test_custom_renderer_can_omit_accessor(self):
        with tempfile.TemporaryDirectory(prefix="muse-pixel-perf-link-") as directory:
            directory = Path(directory)
            # Use the actual optional-caller header contract, just like muse_ui.
            # The default renderer's declaration and definition remain strong.
            main = directory / "optional.c"
            main.write_text('''#define MUSE_PIXEL_OPTIONAL_CELLS
#include "muse_pixel.h"
#ifdef __APPLE__
extern uint16_t muse_pixel_cell_rgb565(int, int) __attribute__((weak_import));
#else
extern uint16_t muse_pixel_cell_rgb565(int, int) __attribute__((weak));
#endif
int main(void) {
#ifdef EXPECT_PRESENT
    if (!muse_pixel_cell_rgb565) return 1;
    muse_pose_t p = { .mode = MUSE_MODE_IDLE };
    muse_pixel_render(&p);
    return muse_pixel_cell_rgb565(-1, 0) != 0;
#else
    return muse_pixel_cell_rgb565 != 0;
#endif
}
''')
            exe = directory / "optional"
            # Apple ld requires explicitly permitting an absent weak import in
            # an executable; dyld then supplies NULL. ELF weak needs no flag.
            absent_flags = ["-Wl,-U,_muse_pixel_cell_rgb565"] if sys.platform == "darwin" else []
            run(CC + FLAGS + absent_flags + [str(main), "-o", str(exe)])
            run([str(exe)])
            run(CC + FLAGS + ["-DEXPECT_PRESENT", str(main), str(ESP32 / "avatar" / "muse_pixel.c"), "-lm", "-o", str(exe)])
            run([str(exe)])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--benchmark", action="store_true")
    args, remaining = parser.parse_known_args()
    if args.benchmark:
        with tempfile.TemporaryDirectory(prefix="muse-pixel-perf-bench-") as directory:
            print("Compiler: " + run(CC + ["--version"]).stdout.splitlines()[0])
            print(run([str(build(directory)), "--benchmark"], timeout=120).stdout, end="")
    else:
        unittest.main(argv=[__file__] + remaining)
