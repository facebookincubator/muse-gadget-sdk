"""Optional local speech recognition for recorded microphone audio."""
import sys
from faster_whisper import WhisperModel
model = WhisperModel("tiny.en", device="cpu", compute_type="int8", download_root=sys.argv[2])
segments, _ = model.transcribe(sys.argv[1], language="en", vad_filter=True, beam_size=3)
print(" ".join(segment.text.strip() for segment in segments).strip())
