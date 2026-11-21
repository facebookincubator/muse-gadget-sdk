---
name: gadget-linux-wakeword
description: Install and verify the optional Hey Muse wake word for a Linux Muse gadget — build the native detector library from the SDK checkout, check it locally, and bring up wake-word voice calls only when asked.
---

# Linux Hey Muse wake-word setup

Use this skill when the user wants the optional local Hey Muse detector
installed, or needs help building or checking it. The model was trained and
measured on synthetic voices; keep it opt-in.

## Inspect first

1. Read `linux/AGENTS.md`, `linux/README.md`, and `linux/wakeword/README.md`
   in this checkout. Work from the Linux gadget host, not the Muse VM or phone.
2. Identify the Linux distribution, architecture, available Python, and any
   existing Muse installation. Keep existing pairing, the normal
   `musegadget run` service, and its environment untouched.

## Prerequisites and installation

The detector is a native library (`libmuse_wakeword.so`) built from this
checkout: the ESP32 firmware's runner and model, ExecuTorch's runtime and
ESP-NN delegate, esp-nn's portable C kernels and TensorFlow Lite Micro's
microfrontend. Building needs a C and C++ compiler, `unzip`, `sha256sum` and
`curl`; it downloads three pinned ESP Component Registry archives and checks
each against its SHA-256. The voice client also needs Python/venv/pip and
PortAudio. Nothing else: no torch, ExecuTorch wheel or source checkout.

Prefer a new isolated environment, separate from the root-owned paired service:

```sh
bash linux/wakeword/setup.sh --venv /path/to/muse-wakeword-env --dry-run
```

Review the printed commands, then run the same command without `--dry-run`.
It installs the voice extra, builds the library into `linux/wakeword/build/`,
installs a copy into the environment (`lib/musegadget/`, where the package
finds it), and runs the local check. It does not enable wake-word listening, record
audio, pair a device, restart a service, or contact a Muse VM.

- Offline host: copy the three archives named in `linux/wakeword/build.sh`
  into a directory and pass `--deps DIR`; their checksums are still checked.
- Already built library (the same architecture and C library):
  `--library FILE`; the check still runs it.
- Missing system packages: explain the proposed changes and obtain explicit
  approval before using `--install-system-packages`. Never add `--yes` to the
  base installer, replace system Python, or modify the paired service just to
  get voice prerequisites. Reusing an existing venv requires `--reuse-venv`.

If the base SDK is not installed/paired, follow `linux/install.sh` and the
normal pairing instructions as a separate operation. Ask before giving Muse an
account or interrupting active service work. The wake-word setup does not read
tokens or open pairing itself.

## Verify before claiming success

```sh
/path/to/muse-wakeword-env/bin/python -m musegadget.wakeword_check --check-audio
```

Success means the library loads, the runner starts on the embedded model,
gives the same answer for the same test audio in different chunk sizes and
after a reset, never wakes on the test tones, and sounddevice and PortAudio
are present; the microphone stays closed. Numerical consistency is not
acoustic accuracy. On a failure, report the failed check; do not substitute
another model or patch the library.

## Optional live bring-up

Only after the user explicitly asks to enable the microphone:

```sh
/path/to/muse-wakeword-env/bin/musegadget voice-call \
  --vm-id EXACT_LEASED_VM_ID --live --wake-word
```

The normal service remains unchanged. Waiting audio stays local; detection
pauses through calls and playback, then resets/re-arms after successful
cleanup. A wake-started voice call ends if no speech follows the wake word within
5 s of audio.
Ctrl-C stops listening. Omit `--wake-word` or use `--no-wake-word` for
immediate calls. Never choose a VM on the user's behalf or start a voice call
as an installation check; it can replace another active call and upload audio
after a false trigger.

## Sources

- [Linux SDK guide](../../linux/README.md)
- [Linux contributor and deployment instructions](../../linux/AGENTS.md)
- [Wake-word build and runtime guide](../../linux/wakeword/README.md)
- [Model card](../../esp32/components/muse_wakeword/models/README.md)
