<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Hey Muse wake word model

`hey_muse.pte` is the int8 ExecuTorch export embedded by
`ww_runner_et.cpp` (Cadence-backend PTQ; runs on kernels registered
from a bundle generated at build time from this file — Cadence
generic/portable kernels).
`hey_muse.json` is its generated provenance sidecar (checkpoint and
PTE hashes, frontend config, class order, model ops, and the int8
calibration/parity record). The
runner does not read the JSON on device — the matching frontend
constants are generated into `../ww_frontend_config.h` by the same
export. No bundle is committed: the build generates one from this
file at configure time (see `../tools/make_kernel_bundle.py`) and
refuses to proceed if it does not cover this model.

- Architecture: ExecuTorch `StreamingWakeWord`
  (`examples/models/mlperf_tiny/streaming_wakeword.py`), one LFBE
  frame in, posterior over `wakeword | silence | other` out.
- Wake phrases: "Hey Muse", "Ok Muse", "Okay Muse" (see the
  sidecar for the exact training variants).
- Frontend (frozen; `ww_frontend.c` implements exactly this):
  16 kHz PCM, 480-sample periodic Hann window centered in a
  512-point FFT, 320-sample hop, power spectrum, 40 Slaney mel
  filters with Slaney area normalization, `log(mel + 1e-6)`, then
  the checkpoint's per-mel mean/std normalization.
- No VAD. Endpointing after a wake is the SDK-side energy tier.

## Measured accuracy

From the checkpoint's own streaming test (fp32, the clean-voice training
bundle's test set), at the detector thresholds `ww_detector.c` uses:

| Sensitivity | Threshold | Wake words caught | False wakes per hour |
| --- | --- | --- | --- |
| Low | 0.99 | 51.3% | 3.98 |
| Default | 0.95 | 78.7% | 33.8 |
| High | 0.90 | not measured | not measured |

No threshold reaches one false wake an hour or fewer. These are not
measurements of the int8 model on a device, which haven't been made; the
int8 export's parity check against fp32 is in `hey_muse.json`. A false
wake followed by speech within 2.5 s sends a voice note.



## Attribution

This model (`hey_muse.pte`, Cadence int8 export of checkpoint epoch 11
(checkpoint SHA-256 `cafa78914c2280b653c2589b11985b5a6116c63a3bfdabd0d446c1a59febc5d3`),
was trained on synthetic speech generated with Piper, plus Google
Speech Commands recordings as negatives and backgrounds. No Piper code
ships in the firmware; Piper was used only to generate training audio
on the training host (phonemization via espeak-ng at generation time
only). The voice model cards below are the authoritative per-voice
license statements. All eight voice files were pinned to HuggingFace
revision `e7229f7d33a10e145e109b886d661984201ac3c4`.

- Piper: [piper-tts 1.8.0](https://pypi.org/project/piper-tts/),
  [piper1-gpl repository](https://github.com/OHF-voice/piper1-gpl),
  licensed under [GPL-3.0-or-later](https://spdx.org/licenses/GPL-3.0-or-later.html).
- Espeak-ng (phonemizer used by the Piper voices):
  [espeak-ng](https://github.com/espeak-ng/espeak-ng/), GPL-3.0.

Voices used (training, validation, and held-out test pools; LibriTTS
splits are 724 train / 90 validation / 90 test speakers, and Norman is
test-only):

| Voice | Dataset / source | License | Trained from |
| --- | --- | --- | --- |
| [en_GB-cori-medium](https://huggingface.co/rhasspy/piper-voices/blob/e7229f7d33a10e145e109b886d661984201ac3c4/en/en_GB/cori/medium/MODEL_CARD) | LibriVox | Public domain | Scratch |
| [en_GB-cori-high](https://huggingface.co/rhasspy/piper-voices/blob/e7229f7d33a10e145e109b886d661984201ac3c4/en/en_GB/cori/high/MODEL_CARD) | LibriVox | Public domain | Scratch |
| [en_US-kristin-medium](https://huggingface.co/rhasspy/piper-voices/blob/e7229f7d33a10e145e109b886d661984201ac3c4/en/en_US/kristin/medium/MODEL_CARD) | LibriVox | Public domain | Scratch |
| [en_US-john-medium](https://huggingface.co/rhasspy/piper-voices/blob/e7229f7d33a10e145e109b886d661984201ac3c4/en/en_US/john/medium/MODEL_CARD) | LibriVox | Public domain | Kristin |
| [en_US-ljspeech-medium](https://huggingface.co/rhasspy/piper-voices/blob/e7229f7d33a10e145e109b886d661984201ac3c4/en/en_US/ljspeech/medium/MODEL_CARD) | LJ Speech | Public domain | Scratch |
| [en_US-ljspeech-high](https://huggingface.co/rhasspy/piper-voices/blob/e7229f7d33a10e145e109b886d661984201ac3c4/en/en_US/ljspeech/high/MODEL_CARD) | LJ Speech | Public domain | Scratch |
| [en_US-norman-medium](https://huggingface.co/rhasspy/piper-voices/blob/e7229f7d33a10e145e109b886d661984201ac3c4/en/en_US/norman/medium/MODEL_CARD) | LibriVox | Public domain | Scratch |
| [en_US-libritts-high](https://huggingface.co/rhasspy/piper-voices/blob/e7229f7d33a10e145e109b886d661984201ac3c4/en/en_US/libritts/high/MODEL_CARD) | LibriTTS | CC BY 4.0 | Scratch |

LibriTTS attribution (required by its licence): LibriTTS, a corpus
based on public-domain LibriVox audiobooks, from
[OpenSLR 60](http://www.openslr.org/60/), is licensed under
[CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). The
`en_US-libritts-high` Piper voice was trained on LibriTTS, and this
wake-word model was trained on speech synthesized with that voice;
it does not contain LibriTTS recordings.

LibriVox and LJ Speech recordings used to train the other Piper
voices above are in the public domain, as stated on their model
cards: [LibriVox](https://librivox.org) and
[LJ Speech](https://keithito.com/LJ-Speech-Dataset/).

Google Speech Commands v0.02, used for negative speech and
backgrounds, is licensed under
[CC BY 4.0](https://creativecommons.org/licenses/by/4.0/); dataset:
[http://download.tensorflow.org/data/speech_commands_v0.02.tar.gz](http://download.tensorflow.org/data/speech_commands_v0.02.tar.gz).
