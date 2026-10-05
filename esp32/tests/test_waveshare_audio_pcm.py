# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0

"""Check the Audio Board's microphone channel selection and PCM boundaries."""

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r"""
#include <assert.h>
#include <limits.h>
#include "waveshare_audio_pcm.h"
int main(void) {
    // Different reference, mic1 and mic2 samples expose a swapped slot or
    // accidental reference-channel recording. Include full negative scale.
    const int32_t raw[] = {
        (int32_t)0x03e87fffU, (int32_t)0x11112222U,
        (int32_t)0x80001234U, (int32_t)0x33334444U,
        (int32_t)0x7fff5678U, (int32_t)0x55556666U,
        (int32_t)0xffff9abcU, (int32_t)0x77778888U,
    };
    int16_t pcm[5] = {0, 0, 0, 0, 123};
    int peak = -1;
    waveshare_audio_extract_mic(raw, pcm, 4, &peak);
    assert(pcm[0] == 1000 && pcm[1] == INT16_MIN);
    assert(pcm[2] == INT16_MAX && pcm[3] == -1);
    assert(peak == 32768 && pcm[4] == 123);
    waveshare_audio_extract_mic(raw, pcm, 0, &peak);
    assert(peak == 0 && pcm[0] == 1000);

    // Stereo remains separate, including maximum values whose three-frame
    // sums overflow a 32-bit accumulator. A second frame checks advancement.
    const int32_t in[] = {
        INT32_MAX, INT32_MIN, INT32_MAX, INT32_MIN, INT32_MAX, INT32_MIN,
        0, -30, 30, 0, 60, 30,
    };
    int32_t out[5] = {0, 0, 0, 0, 123};
    waveshare_audio_downsample(in, out, 2);
    assert(out[0] == INT32_MAX && out[1] == INT32_MIN);
    assert(out[2] == 30 && out[3] == 0 && out[4] == 123);
    waveshare_audio_downsample(in, out, 0);
    assert(out[0] == INT32_MAX && out[4] == 123);
    return 0;
}
"""


class WaveshareAudioPCM(unittest.TestCase):
    def test_microphone_selection_and_stereo_conversion(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / "pcm.c"
            binary = Path(folder) / "pcm"
            source.write_text(HARNESS)
            subprocess.run(
                shlex.split(os.environ.get("CC", "cc"))
                + [
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=undefined,address",
                    "-I",
                    str(ROOT / "main"),
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)
