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

"""musegadgets BLE audio playback (mg_command_stream_audio) on the host: the
resampler at every supported rate, the playback ring's flow control,
prebuffer, underruns, drain and drop, SBC, PCM and LC3 streams decoded
end to end, and the decoder's resync at a flush
(components/muse_gadget_ble/mg_play.c, mg_resample.c)."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_mg_ble import MG, PROTOCOLS, ROOT, codec_build, compile_and_run  # noqa: E402


class MgPlayTest(unittest.TestCase):
    def test_playback_core(self) -> None:
        for lc3 in (False, True):
            with self.subTest(lc3=lc3):
                srcs, defs, incs = codec_build(lc3)
                out = compile_and_run(
                    self, [ROOT / "tests/mg_play_harness.c", MG / "mg_play.c", MG / "mg_resample.c"], defs,
                    [MG, PROTOCOLS, *incs], vendor=srcs)
                self.assertIn("PASS mg_play", out)
                self.assertIn("resample reset: the tables kept, output as a fresh one's; none after a failed open", out)
                self.assertIn("flush: the resampler's tables kept, decoding as a fresh decoder", out)
                if lc3:
                    self.assertIn("live start within its credit: depth 70 ms, output 30 ms, start 70 ms: playing at 40 ms held",
                                  out)

    def test_flush_resyncs_the_decoder(self) -> None:
        # A flush (a gadget once garbled its reply after a barge-in without this) drops
        # any partial frame and resets the decoder, and decoding stops exactly at the flush point. A reply is
        # flushed mid-way (frames decoded, a gap concealed, half-written frames on both sides of the flush,
        # the player's pop in flight); the next reply decodes bit-identical to a fresh decoder's, and a decoder
        # left with the old state is caught. On SBC, PCM and LC3 at 16 and 24 kHz, with liblc3 and with the
        # fixed-point lc3fx the C6 and the ESP-Mosaico build.
        for lc3, fixed in ((False, False), (True, False), (True, True)):
            with self.subTest(lc3=lc3, fixed=fixed):
                srcs, defs, incs = codec_build(lc3, fixed)
                out = compile_and_run(
                    self, [ROOT / "tests/mg_play_harness.c", MG / "mg_play.c", MG / "mg_resample.c"], defs,
                    [MG, PROTOCOLS, *incs], vendor=srcs, args=["resync"])
                self.assertIn("PASS mg_play resync", out)
                names = ["SBC 16 kHz", "PCM 8 kHz"] + (["LC3 16 kHz", "LC3 24 kHz"] if lc3 else [])
                for name in names:
                    self.assertRegex(out, r"flush resync %s: next reply bit-identical to a fresh decoder's "
                                          r"\(\d+ samples\); without the reset it differs" % name)
                self.assertEqual(out.count("flush resync "), len(names))
                self.assertIn("flush past the queue: 2 flushes", out)
                self.assertIn("flush past the queue (live depth 100 ms): 2 flushes", out)
        # mg_dec_flush starts the decoder over (LC3 setup with its concealment state, SBC, the resampler from
        # silence) without allocating: the resampler keeps its tables (mg_play_harness: "flush: ...").
        play = (MG / "mg_play.c").read_text()
        flush = play[play.index("bool mg_dec_flush(mg_dec_t *d)"):]
        flush = flush[:flush.index("\n}\n")]
        self.assertNotIn("mg_dec_close(d);", flush)
        self.assertNotIn("mg_dec_open(", flush)
        self.assertIn("mg_resample_reset(&d->rs);", flush)
        self.assertIn("lc3_setup_decoder(", flush)


if __name__ == "__main__":
    unittest.main()
