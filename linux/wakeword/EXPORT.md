# Hey Muse export and wheel compatibility

The trained checkpoint is exported once to an fp32 **XNNPACK** `.pte`. Linux
inference uses the installed wheel's public APIs:

```python
runtime = Runtime.get()
program = runtime.load_program(model_path)
method = program.load_method("forward")
posteriors = method.execute((torch_float32_lfbe_frame,))
```

There are no private binding monkeypatches or custom embedded ExecuTorch builds.
The C signal library implements only PCM decoding, the exact ESP frontend and
detector; see [README.md](README.md).

## Model contract and provenance

`hey_muse_xnnpack.pte`: **210,316 bytes**, SHA-256
`ab7ecd0708523788e8a64d2959140c5f8f7586ed147fc774737375aa009af9b1`.
`forward` consumes one normalized float32 LFBE frame `[1,40,1,1]` and returns
float32 softmax `[1,3]`, in wakeword/silence/other order. Four mutable convolution
histories are zero-initialized in the file and persist across forward calls.
A reset discards the old program/method and loads a fresh one.

Checkpoint loading and the architecture compatibility shim are shared with
the ESP (Cadence) export in the single exporter at
`esp32/components/muse_wakeword/tools/export_hey_muse.py`; this invocation
selects its XNNPACK backend with `--backend xnnpack`. All parameter keys
must match except missing history buffers. Frontend config/normalization
must match the shared ESP header. Neither ESP nor Cadence output is
rewritten. No quantization is necessary or performed.


## Reproduce export

Read ExecuTorch's export/building skills first. Use a trusted checkpoint and an
export environment with compatible torch/executorch.exir. Loading uses
`weights_only=False`; never pass an untrusted pickle. The architecture source
comes from `examples/models/mlperf_tiny/streaming_wakeword.py` in the supplied
ExecuTorch checkout.

```sh
"$EXECUTORCH_PYTHON" esp32/components/muse_wakeword/tools/export_hey_muse.py \
  --backend xnnpack \
  --checkpoint /path/to/hey-muse/hey_muse_checkpoint.pt \
  --calibration /path/to/hey-muse/calibration_frames.pt \
  --executorch-root "$EXECUTORCH_ROOT" \
  --frontend-header esp32/components/muse_wakeword/ww_frontend_config.h \
  --output-dir linux/src/musegadget/data/wakeword \
  --parity-dir /tmp/hey-muse-parity \
  --defer-runtime-validation
```

The tool functionalizes export to retain all history mutations, lowers through
the fp32 XNNPACK partitioner, and applies `InitializedMutableBufferPass` to
serialize zero histories. It requires a real XNNPACK delegate. The calibration
bundle is used only as a sequential validation corpus, not for fitting or
quantization. Without `--defer-runtime-validation`, export requires installed
Python Runtime parity. Deferred export intentionally marks validation pending
until actual wheel inference passes. A stale editable source/compiled-extension
pair is not repaired by inventing private binding attributes.


## Shipped synthetic compatibility corpus

The package includes three independent 100-frame **self-authored arithmetic**
LFBE-shaped streams (zeros, sinusoidal, modular ramp). They contain no recordings
or calibration samples. The expected posteriors are from the trusted checkpoint's
eager streaming model, resetting between streams.

- `validation_frames.f32`: little-endian float32 `[3,100,40]`, 48,000 bytes.
- `validation_expected.f32`: little-endian float32 `[3,100,3]`, 3,600 bytes.
- `validation.json`: shapes, sizes, file/PTE/checkpoint hashes, provenance,
  `atol=1e-5`, `rtol=1e-4`, and explicit accuracy limitations.

Regenerate with `tools/generate_validation_fixture.py --checkpoint ...
--executorch-root ... --model ... --output-dir ...` in the trusted export
Python environment. Do not hand-edit reference values.

`python -m musegadget.wakeword_check --check-audio` validates the actual installed
wheel and C signal shim, including all sequential frames, contaminated-history
reset replay, independent programs, PCM chunking, sounddevice metadata, and the
PortAudio version symbol. It does not initialize PortAudio or open/enumerate a
microphone. Missing/bad wheels, model hashes, tensor contracts, backends, or audio
prerequisites fail actionably. The setup helper runs this automatically.

The larger research-bundle validation corpus stays outside the repository:

```sh
python linux/wakeword/tools/verify_parity.py \
  --library /path/to/libmuse_wakeword_signal.so \
  --model linux/src/musegadget/data/wakeword/hey_muse_xnnpack.pte \
  --parity-dir /tmp/hey-muse-parity \
  --metadata linux/src/musegadget/data/wakeword/hey_muse_xnnpack.json \
  --wheel /path/to/the_actual_tested_executorch.whl
```

This uses wheel Runtime inference and updates `wheel_runtime_vs_eager` only
after parity passes. 

## Identified public reference wheel

Tested in an isolated Linux x86-64 CPython 3.12.14 environment:

- `executorch-1.5.1-cp312-cp312-manylinux_2_28_x86_64.whl`, SHA-256
  `04897dffff8d96e8af32f554c758d0a7d4f50af5c3530c90dc8d5319bb44c878`.
- `torch-2.15.0+cpu-cp312-cp312-manylinux_2_28_x86_64.whl`, SHA-256
  `0955c7e3446326293f48af18fad67872fa31dee5a36a87690eb9e5be8b465be3`.
