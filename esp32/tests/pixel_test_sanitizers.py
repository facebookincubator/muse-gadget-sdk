# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Pixel harness sanitizer settings: retain ASan/UBSan on every platform.

Apple's ASan runtime cannot run LeakSanitizer and aborts if detect_leaks=1.
Linux CI still enables leak detection; no test or sanitizer is skipped.
"""
import sys

ASAN_OPTIONS = "detect_leaks=" + ("0" if sys.platform == "darwin" else "1") + ":halt_on_error=1"
