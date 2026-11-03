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

"""Host checks for the board's streaming resampler, including odd DMA splits."""
import ctypes
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


class SpotPearPCMTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(__file__).resolve().parents[1]
        wrapper = Path(cls.tmp.name) / "pcm.c"
        wrapper.write_text("""
#include "spotpear_pcm.h"
#include <stddef.h>
static spotpear_resampler_t state;
void reset(void) { state = (spotpear_resampler_t){0}; }
int convert(const int16_t *in, int n, int32_t *out) {
    int count = 0;
    for (int i = 0; i < n; ++i)
        count += spotpear_resample(&state, in[i], out + count);
    return count;
}
""")
        lib = Path(cls.tmp.name) / "pcm.so"
        subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-shared", "-fPIC",
                        "-I", str(root / "components/muse/boards"),
                        str(wrapper), "-o", str(lib)], check=True)
        cls.lib = ctypes.CDLL(str(lib))
        cls.lib.convert.argtypes = [ctypes.POINTER(ctypes.c_int16), ctypes.c_int,
                                    ctypes.POINTER(ctypes.c_int32)]
        cls.lib.convert.restype = ctypes.c_int

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.lib.reset()

    def convert(self, samples):
        output = (ctypes.c_int32 * (len(samples) * 2 + 1))()
        output[-1] = 123456789
        count = self.lib.convert((ctypes.c_int16 * len(samples))(*samples),
                                 len(samples), output)
        self.assertEqual(output[-1], 123456789)
        return list(output[:count])

    def test_one_second_duration_and_silence(self):
        self.assertEqual(self.convert([0] * 16000), [0] * 24000)

    def test_dc_and_full_scale(self):
        for value in [-32768, -1234, 0, 1234, 32767]:
            self.lib.reset()
            self.assertEqual(self.convert([value] * 320), [value * 65536] * 480)

    def test_interpolation(self):
        self.assertEqual(self.convert([0, 3000, 6000, 9000]),
                         [v * 65536 for v in [0, 0, 1000, 3000, 5000, 7000]])

    def test_chunk_boundaries(self):
        samples = [((i * 997) % 65536) - 32768 for i in range(1001)]
        reference = self.convert(samples)
        for chunk in [1, 3, 127, 128, 319, 320]:
            self.lib.reset()
            output = []
            for offset in range(0, len(samples), chunk):
                output.extend(self.convert(samples[offset:offset + chunk]))
            self.assertEqual(output, reference)

    def test_extremes_stay_in_range(self):
        result = self.convert([-32768, 32767] * 100)
        self.assertTrue(all(-2147483648 <= n <= 2147418112 for n in result))


if __name__ == "__main__":
    unittest.main()
