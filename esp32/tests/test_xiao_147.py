"""Exercise the XIAO 1.47 touch report decoding."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Xiao147Tests(unittest.TestCase):
    def test_touch_report_bounds_and_release(self):
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or not shutil.which(cc[0]):
            self.skipTest("host C compiler is not installed")
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "touch.exe"
            subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "components/muse/boards"),
                            str(ROOT / "tests/xiao_147_touch_harness.c"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=10)
