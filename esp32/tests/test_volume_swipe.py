"""Exercise the production volume recognizer without ESP-IDF or a display."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <assert.h>
#include "muse_volume_swipe.h"

int main(void) {
    const int heights[] = {128, 240, 320, 466, 800};
    for (unsigned i = 0; i < sizeof(heights) / sizeof(heights[0]); i++) {
        int h = heights[i], span = h * 3 / 4;
        muse_volume_swipe_t s;
        muse_volume_swipe_begin(&s, 50, h / 2, h, 50);
        assert(!muse_volume_swipe_move(&s, 52, h / 2 - 2));
        assert(!s.claimed && s.value == 50); /* jitter and taps */
        assert(muse_volume_swipe_move(&s, 50, h / 2 - span / 4));
        assert(s.value >= 74 && s.value <= 75); /* up */
        assert(muse_volume_swipe_move(&s, 50, h / 2 + span / 4));
        assert(s.value >= 25 && s.value <= 26); /* reverse down */
        assert(muse_volume_swipe_move(&s, 50, h / 2 - span));
        assert(s.value == 100);
        assert(muse_volume_swipe_move(&s, 50, h / 2 - span + span / 10));
        assert(s.value < 100); /* no dead travel after saturation */
        assert(muse_volume_swipe_move(&s, 50, h / 2 + span));
        assert(s.value == 0);

        muse_volume_swipe_begin(&s, 50, h / 2, h, 50);
        assert(!muse_volume_swipe_move(&s, 50 + s.threshold * 2, h / 2 - 1));
        assert(!s.tracking && !s.claimed);
        assert(!muse_volume_swipe_move(&s, 50, 0)); /* horizontal stays rejected */
        assert(s.value == 50);

        muse_volume_swipe_begin(&s, 50, h / 2, h, 50);
        assert(!muse_volume_swipe_move(&s, 50 + s.threshold, h / 2 - s.threshold));
        assert(!s.claimed); /* ambiguous diagonal does not change volume */
        s.tracking = false;
        assert(!muse_volume_swipe_move(&s, 50, 0)); /* cancelled/finished touch */
    }
    return 0;
}
'''


class VolumeSwipe(unittest.TestCase):
    def test_direction_limits_reversal_and_screen_sizes(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "volume_swipe.c"
            binary = Path(tmp) / "volume_swipe"
            source.write_text(HARNESS)
            subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=c11",
                            "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                            "-I", str(ROOT / "components/muse"), str(source),
                            "-o", str(binary)], check=True, capture_output=True)
            subprocess.run([str(binary)], check=True)
