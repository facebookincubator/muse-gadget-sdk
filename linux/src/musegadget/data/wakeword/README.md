# Hey Muse wake word model — attribution

This directory holds the Linux wake-word model
(`hey_muse_xnnpack.pte`, fp32 XNNPACK export of checkpoint epoch 11
(checkpoint SHA-256 `cafa78914c2280b653c2589b11985b5a6116c63a3bfdabd0d446c1a59febc5d3`),

The model was trained on synthetic speech generated with Piper, plus
Google Speech Commands recordings as negatives and backgrounds. No
Piper code ships in this package; Piper was used only to generate
training audio on the training host (phonemization via espeak-ng at
generation time only). The voice model cards below are the
authoritative per-voice license statements. All eight voice files
were pinned to HuggingFace revision
`e7229f7d33a10e145e109b886d661984201ac3c4`.

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

## Measured accuracy

The same checkpoint as the ESP32 model. Its own streaming test (fp32,
the precision of this export, on the clean-voice training bundle's
test set), at the detector thresholds the Linux client shares with the
ESP32:

| Sensitivity | Threshold | Wake words caught | False wakes per hour |
| --- | --- | --- | --- |
| Low | 0.99 | 51.3% | 3.98 |
| Default | 0.95 | 78.7% | 33.8 |
| High | 0.90 | not measured | not measured |

No threshold reaches one false wake an hour or fewer. None of this was
measured with a Linux microphone. On Linux each false wake starts a
voice call that sends audio to Muse.
