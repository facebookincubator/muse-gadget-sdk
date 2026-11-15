# Linux Hey Muse engine

The Linux detector is the ESP32 firmware's own runner, built as a native
library: `libmuse_wakeword.so`, which `musegadget.wakeword` loads with
ctypes. It runs TensorFlow Lite Micro's microfrontend, the int8 model
`esp32/components/muse_wakeword/models/hey_muse.pte` through ExecuTorch's
ESP-NN delegate (esp-nn's portable C kernels), and the detector, on one
thread, with no Python packages, torch or network at run time. The model is
embedded in the library.

| | |
|---|---|
| Library | about 145 KB (x86-64 and aarch64), model included; links libc, libm and libstdc++ |
| CPU, listening | about 0.2% of one core: an hour of audio in 6.6 s on an x86-64 server core, front end included |
| Memory | 2.7 MB peak RSS for `ww-run` over an hour of audio |
| Results | the ESP32 runner's code and model; bit-identical to TensorFlow Lite Micro with the same weights, up to last-bit differences in the front end's start-up tables between C math libraries |

## Build

```sh
bash linux/wakeword/build.sh [--out DIR] [--deps DIR] [--offline]
```

`build.sh` compiles, with `cc` and `c++`:

- from `esp32/components/muse_wakeword`: `ww_runner_espnn.cpp`,
  `ww_frontend.c`, `ww_detector.c`, the model, and the host stand-ins in
  `host/` (seven small headers and ExecuTorch's platform layer, which the
  ESP-NN delegate needs off the chip until ExecuTorch builds it for Linux
  itself);
- from three ESP Component Registry archives, the same pinned releases the
  firmware build fetches, each checked against its SHA-256 before it is
  unpacked: `pytorch/executorch` (runtime and delegate),
  `espressif/esp-nn` (kernels) and `esphome/esp-micro-speech-features`
  (microfrontend).

It downloads the archives from `components-file.espressif.com` unless
`--deps DIR` holds them (same file names; `--offline` refuses to download),
and writes `libmuse_wakeword.so` and `ww-run` to `linux/wakeword/build/`.
It needs a C11 and C++17 compiler, `unzip`, `sha256sum` and, to download,
`curl`. Compiling takes 8 s on one core of an Apple M4 Max and under
4 s on a many-core x86-64 server; a Raspberry Pi 4 should take well under a
minute with its four cores (not measured on one).

`ww-run FILE` runs the same library on a 16 kHz mono WAV (or raw PCM) and
prints each wake: `ww-run --scores OUT.f32 FILE` also writes every
inference's probability.

## Setup

```sh
bash linux/wakeword/setup.sh [--install-system-packages] [--dry-run]
```

The helper creates an isolated environment (`linux/.venv-wakeword`) with
the voice extra, builds the library, and checks it without opening the
microphone, pairing, or touching the normal service. `--library FILE` uses
an already built library instead; `--deps DIR` builds from local archives.
System prerequisites are installed only with `--install-system-packages`.

## Python contract

```python
from musegadget.wakeword import WakeWordDetector

with WakeWordDetector(model_path=None, sensitivity="default") as detector:
    woke = detector.feed(pcm_s16le)
    detector.reset()
```

- `feed(bytes) -> bool`: signed 16-bit LE, **16 kHz mono**, arbitrary byte
  chunks. Empty chunks and split samples are supported. Every chunk is fully
  consumed, even if an earlier frame fired; the result reports any wake.
- `reset() -> None`: clears the front end, the model's streaming state, the
  detector and an odd trailing byte. It never allocates.
- `close() -> None`: idempotent; feed/reset after close raise `RuntimeError`.
- Importing the module loads nothing. Construction loads the library from
  `MUSE_WAKEWORD_LIBRARY` or `linux/wakeword/build/` and raises
  `WakeWordUnavailableError`, with setup guidance, when it's missing.
- `model_path` loads another model with the same contract
  (`esp32/components/muse_wakeword/models/hey_muse.json`) instead of the
  embedded one; a file that doesn't match is refused.
- One instance serializes calls; independent instances have independent
  state. A failed inference makes the stream unavailable until `reset()`.

Sensitivity is `low` (cutoff 0.26, fewest false wakes), `default` (0.22) or
`high` (0.13); the model card in `esp32/components/muse_wakeword/models/`
has what each measured.

## Tests

```sh
bash linux/wakeword/build.sh
MUSE_WAKEWORD_LIBRARY="$PWD/linux/wakeword/build/libmuse_wakeword.so" \
  python -m pytest -q linux/wakeword/tests
```

The tests use self-made tones and silence. `MUSE_WAKEWORD_CLIPS=DIR` adds
a run over your own 16 kHz WAV clips that each contain the wake word. The
ESP32 host tests (`esp32/tests/test_muse_wakeword.py`) build the same runner
and pin its output bytes for a synthetic signal.
