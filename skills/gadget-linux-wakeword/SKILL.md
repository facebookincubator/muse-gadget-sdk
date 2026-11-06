---
name: gadget-linux-wakeword
description: Install and verify the optional Hey Muse wake-word prerequisites for a Linux Muse gadget using a supplied ExecuTorch wheel; use for wheel compatibility, native frontend setup, and local-only detection bring-up.
---

# Linux Hey Muse wake-word setup

Use this skill when the user wants the optional local Hey Muse detector installed,
or needs help with its prerequisites or an ExecuTorch wheel from a different
source revision. This is community/research bring-up, not a production detector.

## Inspect first

1. Read `linux/AGENTS.md`, `linux/README.md`, and `linux/wakeword/README.md`
   in this checkout. Work from the Linux gadget host, not the Muse VM or phone.
2. Identify the Linux distribution, userland architecture, available Python,
   and existing Muse installation. Keep existing pairing, the normal
   `musegadget run` service, and its environment untouched.
3. For the standard CPU build, use the documented CPU package index below.
   Prefer an explicitly supplied custom wheel when the user requested one.
   Verify any supplied checksum; do not substitute the latest PyPI release or
   infer compatibility from a package name. Ask for a path/download URL only
   when a requested custom build cannot be located. Do not log tokens.
4. The wheel must support this host's architecture and Python. The base SDK
   supports Python 3.9, but the optional wheel may require a newer interpreter;
   follow the wheel's own metadata. A source-revision difference alone does not
   prove either incompatibility or compatibility.

## Where the CPU wheels are published

ExecuTorch's root `README.md` links to
[Getting Started — Installation](https://docs.pytorch.org/executorch/main/getting-started.html),
whose CPU variant uses **https://download.pytorch.org/whl/cpu** for both
`executorch` and `torch`. The nightly CPU channel is
**https://download.pytorch.org/whl/nightly/cpu**, with `--pre`; use nightly only
when the selected source/release requires it, and pin compatible build versions.

Find available CPU-channel versions for the target interpreter/architecture,
choose explicit compatible pins, and fetch the two root wheels. Replace the
version placeholders below; do not infer that the separately tested public
reference pair is necessarily available in every CPU channel.

```sh
python3.12 -m pip download --only-binary=:all: --no-deps \
  --index-url https://download.pytorch.org/whl/cpu \
  --dest /path/to/wheels \
  'executorch==<ET_VERSION>' 'torch==<TORCH_VERSION>'
```

The upstream installation command adds `--extra-index-url https://pypi.org/simple`
for dependencies. For this root-wheel download, `--no-deps` and no fallback index
make an unavailable CPU pin fail rather than silently selecting a Linux CUDA
wheel from PyPI. Pass the resulting exact files to `setup.sh` below; it resolves
other dependencies and still verifies the actual runtime/PTE pair. CPU-channel
provenance is not a substitute for numerical/reset checks. Avoid a minimal
export-only wheel: it omits the runtime bindings needed for detection.

## Prerequisites and installation

The voice client needs Python/venv/pip and PortAudio. The only custom native
build is the reused C LFBE frontend and posterior detector: it needs a C compiler
and the system math library, **not** an ExecuTorch source checkout. Model
inference uses the wheel's public `executorch.runtime` Python API.

Prefer a new isolated environment, separate from the root-owned paired service:

```sh
bash linux/wakeword/setup.sh --wheel /trusted/path/executorch.whl \
  --torch-wheel /trusted/path/compatible-torch.whl \
  --python python3.12 --venv /path/to/muse-wakeword-env --dry-run
```

Review the printed commands, then run the same command without `--dry-run`.
Use the interpreter matching the wheel; `python3.12` above is an example, not a
universal requirement. Supply a compatible Torch wheel too: some ExecuTorch
wheels do not declare Torch even though their runtime bindings require it. An
existing explicitly reused environment can provide Torch instead; do not choose
the latest Torch version blindly. `--wheel-sha256` and `--torch-sha256` verify
approved checksums before installation. The helper installs these explicit
wheels and the voice extra, builds the small signal library, and runs compatibility
checks. It does
not enable wake-word listening, record audio, pair a device, restart a service,
or contact a Muse VM.

If system packages are missing, explain the proposed changes and obtain explicit
approval before using `--install-system-packages`. Never add `--yes` to the base
installer, replace system Python, or modify the paired service just to get voice
prerequisites. Reusing an existing venv requires the explicit `--reuse-venv`
option; warn that package dependency resolution can change it. Setup clears
`PYTHONPATH` for wheel installation/checking and reinstalls the exact supplied
root wheels when reusing an environment, so a same-version editable or custom
build cannot silently stand in for the user's wheel. A trusted,
host-compatible `--signal-library FILE` can avoid compiler prerequisites; the
helper still verifies its actual runtime/PCM behavior. Setup also checks the
installed sounddevice package and PortAudio library without opening an audio
stream; a missing library is a failed prerequisite, not a successful install.

If the base SDK is not installed/paired, follow `linux/install.sh` and the normal
pairing instructions as a separate operation. Ask before giving Muse an account
or interrupting active service work. The wake-word setup does not read tokens or
open pairing itself.

## Verify before claiming success

Run the environment's local checker without accessing the microphone:

```sh
MUSE_WAKEWORD_LIBRARY=/path/to/libmuse_wakeword_signal.so \
  /path/to/muse-wakeword-env/bin/python -m musegadget.wakeword_check
```

Success must establish all of the following:

- The supplied wheel's compiled bindings import and XNNPACK is available.
- The shipped PTE hash matches its metadata and executes with the expected
  normalized float32 `[1,40,1,1]` input and `[1,3]` output contract.
- Sequential synthetic reference frames match expected outputs beyond warm-up.
- Reset replay and fresh independent instances agree; an import or zero-frame
  smoke test alone is insufficient for this stateful model.
- The frontend/detector library loads and processes PCM locally.

Numerical compatibility is not acoustic accuracy.

Record the wheel version, Python/architecture, model hash, and validation result.
Do not declare a different wheel compatible merely because another wheel passed.
On an ABI, operator, delegate, or numerical mismatch, stop and report the precise
failed check. Do not monkeypatch binding attributes, downgrade the tests, or
silently fall back to an untrained/portable model. Use a compatible wheel or
re-export and re-verify with the approved compatible source as a separate step.

## Optional live bring-up

Only after the user explicitly asks to enable the microphone, use the checked
environment and library with the existing voice client:

```sh
MUSE_WAKEWORD_LIBRARY=/path/to/libmuse_wakeword_signal.so \
  /path/to/muse-wakeword-env/bin/musegadget voice-call \
  --vm-id EXACT_LEASED_VM_ID --live --wake-word
```

The normal service remains unchanged. Waiting audio stays local; detection pauses
through calls and playback, then resets/re-arms after successful cleanup. Ctrl-C
stops listening. Omit `--wake-word` or use `--no-wake-word` for immediate calls.
Never choose a VM on the user's behalf or start a voice call as an installation
check; it can replace another active call and upload audio after a false trigger.

## Sources

- [Linux SDK guide](../../linux/README.md)
- [Linux contributor and deployment instructions](../../linux/AGENTS.md)
- [Wake-word build and runtime guide](../../linux/wakeword/README.md)
- [Export and validation contract](../../linux/wakeword/EXPORT.md)
