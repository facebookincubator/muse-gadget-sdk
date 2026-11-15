# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
from pathlib import Path
import os, shlex, subprocess, tempfile, unittest
ROOT = Path(__file__).resolve().parents[1]

class MuseEinkRefreshTest(unittest.TestCase):
    def test_board_refresh_contract(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            for name in ("EInkDisplay.h", "esp_log.h", "esp_timer.h"):
                (out / name).write_text("#pragma once\n")
            for malloc, calloc in (("malloc(n)", "calloc(n, m)"), ("nullptr", "nullptr")):
                (out / "esp_heap_caps.h").write_text(f"""#pragma once
#define MALLOC_CAP_SPIRAM 0
static void *heap_caps_malloc(size_t n, int) {{ (void)n; return {malloc}; }}
static void *heap_caps_calloc(size_t n, size_t m, int) {{ (void)n; (void)m; return {calloc}; }}
static void heap_caps_free(void *p) {{ free(p); }}
""")
                binary = out / "refresh"
                result = subprocess.run([
                    *shlex.split(os.environ.get("CXX", "c++")), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", str(out),
                    "-I", str(ROOT / "components/muse"),
                    str(ROOT / "tests/muse_eink_refresh_harness.cpp"), "-o", str(binary),
                ], capture_output=True, text=True, timeout=120)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
