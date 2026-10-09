# SPDX-License-Identifier: Apache-2.0
"""Check actual streaming-note JSON in both C and C++ without a device."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class BriefVoiceReplies(unittest.TestCase):
    def test_request_variants(self):
        with tempfile.TemporaryDirectory() as tmp:
            for compiler, suffix in [("cc", "c"), ("c++", "cpp")]:
                source = Path(tmp) / ("request." + suffix)
                source.write_text('#include <stdio.h>\n#include "muse_chat_priv.h"\nint main(void) { puts(MUSE_HATCH_NOTE_HEAD "AQID" MUSE_HATCH_NOTE_TAIL); }\n')
                for enabled in [None, 0, 1]:
                    binary = Path(tmp) / "request"
                    flags = [] if enabled is None else [f"-DCONFIG_MUSE_BRIEF_VOICE_REPLIES={enabled}"]
                    subprocess.run([compiler, "-Wall", "-Wextra", "-Werror", *flags,
                                    "-I", str(ROOT / "components/muse"), str(source),
                                    "-o", str(binary)], check=True, capture_output=True)
                    payload = json.loads(subprocess.check_output([str(binary)], text=True))
                    with self.subTest(compiler=compiler, enabled=enabled):
                        self.assertEqual(payload["output_modality"], "text")
                        self.assertEqual(payload["items"], [{"type": "file", "mime_type": "audio/wav",
                            "filename": "voice_note.wav", "data_base64": "AQID"}])
                        if enabled:
                            self.assertIn("two short sentences", payload["message"])
                            self.assertIn("unless I explicitly ask for more detail", payload["message"])
                        else:
                            self.assertEqual(payload["message"], "")

if __name__ == "__main__":
    unittest.main()
