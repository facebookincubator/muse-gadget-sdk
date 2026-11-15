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

`hey_muse.pte` is the wake word model the runner (`../ww_runner_espnn.cpp`)
embeds.
`hey_muse.json` records its contract (input, output and state tensors,
front end, detector cutoffs), provenance and evaluation; `../ww_model.h`
holds the same contract for the C code, and the host tests check that
the two agree. [`ATTRIBUTION.md`](ATTRIBUTION.md) credits the training data.

## The model

- **Wake phrase:** "Hey Muse".
- **Architecture:** [microWakeWord](https://github.com/kahrendt/microWakeWord)'s
  streaming MixedNet: a 24-filter first convolution (kernel 5, stride 3)
  and four blocks of mixed depthwise convolutions (kernels 5; 7 and 11;
  9 and 15; 23) with 32 pointwise filters each, then one output unit.
  11,089 parameters and 10.7 thousand multiply-adds per inference
  (0.36 million a second). Trained with distillation from an ensemble of
  three larger models trained on the same data.
- **Front end:** TensorFlow Lite Micro's microfrontend, as microWakeWord
  and ESPHome run it (`esphome/esp-micro-speech-features`): 16 kHz audio,
  30 ms windows every 10 ms, 40 channels from 125 to 7500 Hz, noise
  reduction, PCAN gain control and log scale.
- **Streaming:** every 30 ms the model takes the newest three feature
  frames plus its six int8 state tensors (2,160 bytes) and returns the
  wake probability and the next state. It keeps no state of its own, so
  the runner holds the state and resets it by writing zero points.
- **Export:** int8 everywhere, through ExecuTorch's ESP-NN delegate: the
  whole program is one delegate call, with no CPU operators, so no
  kernel library is linked. Graph inputs and outputs are left out of
  ExecuTorch's memory plan so the runner binds its own buffers once.
  The probabilities are bit-identical to TensorFlow Lite Micro running
  the same weights converted with microWakeWord's streaming TFLite
  converter on the same machine: 101,018 of 101,018 inferences on x86-64
  Linux and 330 of 330 on an ESP32-S3. (One convolution's int8 weights
  and per-channel scales come from that conversion: PyTorch's PT2E weight
  observer floors small per-channel scales at 2^-12, which would
  otherwise make that layer coarser.) Across machines, the
  microfrontend's start-up tables, which it computes with the C math
  library, can differ in the last bit: on the test clips that changed one
  feature value and two of 1,329 probabilities (by at most 4/256) between
  x86-64 and Arm or the ESP32, with the same detections.
- **Detector** (`../ww_detector.c`): fires when the mean of the last five
  probabilities is above the sensitivity's cutoff, then ignores 2 s.
- **Size:** 23,492 bytes. On an ESP32-S3 at 240 MHz an inference takes
  0.6 ms with the runner's buffers in internal RAM (about 49 KB of it,
  front end included; a test image without the UI) and 1.2 to 1.5 ms with
  them in PSRAM, as on a SenseCAP Watcher with its UI (14 KB internal,
  29 KB PSRAM). `../README.md` has the measurements.

## Measured accuracy

All wake word examples are **synthetic speech**: text-to-speech voices,
including one engine, that the model never trained on, in nine
conditions (clean, quiet, TV at 10, 5 and 0 dB SNR, music, noise,
reverberation, reverberation with TV), with 23.5 hours of real non-wake
audio (speech from meetings, debates, podcasts and broadcasts, music
and room noise) for the false wakes. None of it was used in training or
to choose the model. Real voices haven't been measured.

| Sensitivity | Cutoff | Wake words caught | False wakes per hour |
| --- | --- | --- | --- |
| Low | 0.26 | 71.4% | 0.34 |
| Default | 0.22 | 73.9% | 0.38 |
| High | 0.13 | 80.1% | 1.36 |

At the Default cutoff, 5.7% of near-miss phrases ("hey music",
"hey news", "hey mute" and similar) are taken for the wake word. Each
false wake followed by speech within 2.5 s sends a voice note.

## Training data and recipe

Training used only public-domain, CC0 and CC BY material: synthetic
"Hey Muse" and near-miss phrases from Piper voices (including blends of
LibriTTS speakers and accent variants), and real non-wake speech, music,
noise and room impulse responses. [`ATTRIBUTION.md`](ATTRIBUTION.md) lists
every source and credits each CC BY work. The network and its streaming
conversion follow microWakeWord; it was trained in PyTorch and exported
with ExecuTorch 1.6.0.dev20261112 (commit `2a00808`), the release the
firmware's `pytorch/executorch` component is built from.
