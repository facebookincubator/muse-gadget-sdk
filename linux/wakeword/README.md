# Linux Hey Muse engine

The engine combines the **ExecuTorch wheel's public Runtime API** with a tiny
C-only PCM/frontend/detector shim. It is independent of microphone capture,
resampling, call admission, and network transport. No ExecuTorch checkout,
C++ runtime compilation, or custom embedded backend is required.

The current signal library is **17,304 bytes**, links only libc/libm, and has no
ELF text relocations. The separately shipped fp32 XNNPACK PTE is 210,316 bytes.
See [EXPORT.md](EXPORT.md) for export provenance and numerical validation.

## Prerequisites and setup

Supply the actual trusted, host-compatible ExecuTorch wheel and matching Torch
wheel. Compatibility is verified, not inferred from a filename or revision.
An unknown wheel is not covered by tests of the public reference wheel.

```sh
bash linux/wakeword/setup.sh \
  --wheel /path/to/executorch.whl \
  --torch-wheel /path/to/matching_torch.whl \
  --python python3 --venv /path/to/isolated-env
```

The helper installs into an isolated environment, builds the signal library,
and checks sequential model/reset/PCM compatibility plus sounddevice/PortAudio
prerequisites. It does not pair, start a service, initialize a microphone, or
contact a Muse VM. Wheel checksums can be required with `--wheel-sha256` and
`--torch-sha256`. Existing environments require explicit `--reuse-venv`.
Without a supplied Torch wheel, setup requires an already working compatible
Torch installation; it does not choose an unrelated latest ABI.

Building the shim needs only a C11 compiler and libm. A trusted prebuilt library
can instead be supplied with `--signal-library FILE`. Rebuild on the deployment
architecture/distro; the reference x86-64 binary is not a universal wheel asset.
System prerequisites are installed only with explicit
`--install-system-packages`. `--dry-run` changes nothing.

## Python contract

```python
from musegadget.wakeword import WakeWordDetector

with WakeWordDetector(model_path=None, sensitivity="default") as detector:
    woke = detector.feed(pcm_s16le)
    detector.reset()
```

- `feed(bytes) -> bool`: signed 16-bit LE, **16 kHz mono**, arbitrary byte chunks.
  Empty chunks and split samples are supported. Every successful chunk is fully
  consumed, even if an earlier frame fired; the result reports any fire.
- `reset() -> None`: creates a fresh wheel program/method and clears frontend,
  detector, odd-byte remainder, and failed-stream state. Re-loading a cached
  method on the same program is not a reset.
- `close() -> None`: idempotent; feed/reset after close raise `RuntimeError`.
- Import is lightweight. Only construction imports torch/ExecuTorch, validates
  XNNPACK and float32 tensor metadata, and loads the signal library/model.
- One instance serializes calls. Independent instances own independent histories.
  Callback exceptions propagate through Python rather than being swallowed by
  ctypes. A failed stream stays unavailable until reset succeeds; failed input
  may have consumed a prefix and must not be blindly replayed.

`model_path=None` uses packaged `data/wakeword/hey_muse_xnnpack.pte`.
`MUSE_WAKEWORD_LIBRARY` explicitly selects a signal library; otherwise lookup is
packaged `data/wakeword/libmuse_wakeword_signal.so`, then source-tree
`linux/wakeword/build/libmuse_wakeword_signal.so`. Missing dependencies raise
`WakeWordUnavailableError` with setup/checker guidance. There is no automatic
model download, random fallback, private binding patch, or ambient upload.

## ESP signal processing

The frontend, detector, and runner header are used directly from
`esp32/components/muse_wakeword/` in this repo; there are no copies
under `linux/wakeword/`. The signal shim and its tests compile
`ww_frontend.c` and `ww_detector.c` from there, with the component
root and `include/` on the include path. The runner header supplies
sensitivity types only. The regression suite checks that no copies
remain and that the shared frontend header matches the shipped
sidecar.

The updated ESP runner embeds its Cadence PTE 16-byte aligned and copies
misaligned caller-supplied buffers into owned aligned storage. Linux passes a
file `Path` to the wheel's `Runtime.load_program` instead of a borrowed raw
program buffer, so that ESP allocation/assembly change does not belong in the
C-only signal shim. The frontend and detector code did not change, but the
checkpoint did (clean-voice epoch 11): the frontend header was regenerated
from it, and the Linux XNNPACK PTE re-exported with matching normalization.

Frontend: 512-point FFT, centered 480-sample periodic Hann window, 320-sample
hop, 40 Slaney-normalized mel filters, `log(mel + 1e-6)`, checkpoint mean/std.
First frame requires 512 samples; subsequent frames require 320. No EOS padding.
Detector: 29-frame warm-up, up-to-five-frame mean, 50-frame cooldown. Sensitivity
is `low` (.99), `default` (.95), or `high` (.90). These presets do not fix model
quality. Counter wrap does not repeat warm-up; invalid callback probabilities
are sanitized as in the ESP runner.

## C signal ABI

`ww_signal.h` exposes only six functions. The C shim loads **no model/runtime**:

- `ww_signal_create(sensitivity, error_buf, error_size)` returns an opaque handle
  or NULL with a bounded diagnostic. Sensitivity enum is 0 low, 1 default, 2 high.
- `ww_signal_feed(handle, bytes, byte_count, callback, context, &fired)` reframes
  PCM and calls `int callback(context, const float frame[40], float *posterior)`.
  The caller writes the wake posterior and returns zero. The unchanged detector
  applies warm-up/smoothing/cooldown after the callback.
- `ww_signal_reset(handle)` clears signal state only; Python resets the model.
- `ww_signal_destroy(handle)`, `ww_signal_last_error(handle)`, and
  `ww_signal_frames_seen(handle)` provide lifetime/error/frame-count operations.

Zero status is success, not a wake event. Nonzero callback status clears any
fire from that call and poisons the stream until reset, preventing later model
callbacks in the buffered batch. The frame pointer is borrowed only during the
callback. The caller must serialize access and must not reenter the same handle.
The shim has no native threads or per-feed allocations.

## Build and tests

```sh
bash linux/wakeword/build_signal.sh [OUTPUT_DIR]
# Defaults to linux/wakeword/build; honors CC.
bash linux/wakeword/test_native.sh
```

An optional pure-C CMake recipe is also provided; it has no ExecuTorch or Python
build dependencies. Configure/build/CTest passed in laptop scratch. The direct
Linux compiler build is the tested primary path.

Base wrapper tests use the real C shim with a scripted model, so they can run
without torch. Real wheel tests are explicitly enabled:

```sh
MUSE_WAKEWORD_RUN_WHEEL_TESTS=1 MUSE_WAKEWORD_LIBRARY=/path/to/libmuse_wakeword_signal.so \
  python -m pytest linux/wakeword/tests
python -m musegadget.wakeword_check --check-audio
```
