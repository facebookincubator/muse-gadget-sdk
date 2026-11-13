# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Export the trained Hey Muse StreamingWakeWord checkpoint to ExecuTorch.

The SDK owns the wake word application (frontend, detector, runner), so the
export tool lives with the runner. The model architecture itself comes from
an ExecuTorch checkout (examples/models/mlperf_tiny/streaming_wakeword.py).

Outputs, by default:
  ../models/hey_muse.pte     ExecuTorch program, method "forward"
  ../models/hey_muse.json    provenance/config sidecar (not read on device)
  ../ww_frontend_config.h   generated frontend constants from the checkpoint

The export is int8 only: Cadence-backend PTQ (int8 weights and
activations, calibrated on the training bundle's
calibration_frames.pt via --calibration), lowered through the Cadence
passes to cadence:: ops. The registry component registers only the
kernels in a bundle generated at build time from the exported .pte
(see tools/make_kernel_bundle.py); no bundle is committed.
(A portable-fp32 export measured ~69 ms per 20 ms frame on ESP32-S3 —
3.5x over the real-time budget — and was removed; the eager model in
this script is the numerical reference for the parity check.) The
I/O contract: one normalized fp32 LFBE frame (1, 40, 1, 1) in, 3 fp32
posteriors out.

The checkpoint is loaded with weights_only=False, following the training
bundle's README. Only pass checkpoints from a trusted source.
"""

import argparse
import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import types
from pathlib import Path

import torch

EXPECTED_FRONTEND = {
    "sample_rate": 16000,
    "window_samples": 480,
    "hop_samples": 320,
    "n_fft": 512,
    "n_mels": 40,
    "fmin": 0.0,
    "fmax": 8000.0,
    "mel_scale": "slaney",
    "log_eps": 1e-6,
}
EXPECTED_CLASSES = ["wakeword", "silence", "other"]


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def load_streaming_class(executorch_root: Path):
# The model file imports EagerModelBase from the installed package. It is
# irrelevant to the wake word architecture, so stub it if that package is
# older than the checkout (the usual editable-install drift).
    try:
        import executorch.examples.models.model_base  # noqa: F401
    except Exception:
        stub = types.ModuleType("executorch.examples.models.model_base")
        stub.EagerModelBase = object
        sys.modules["executorch.examples.models.model_base"] = stub

    path = executorch_root / "examples/models/mlperf_tiny/streaming_wakeword.py"
    if not path.is_file():
        raise SystemExit(f"StreamingWakeWord source not found: {path}")
    spec = importlib.util.spec_from_file_location("streaming_wakeword", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module.StreamingWakeWord


def model_ops(executorch_root: Path, pte_path: Path) -> list[str]:
    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "selected_operators.yaml"
        subprocess.run(
            [
                sys.executable,
                "-m",
                "codegen.tools.gen_oplist",
                f"--output_path={out}",
                f"--model_file_path={pte_path}",
            ],
            cwd=executorch_root,
            check=True,
            capture_output=True,
            text=True,
        )
        in_ops = False
        ops = []
        for line in out.read_text().splitlines():
            if line.startswith("operators:"):
                in_ops = True
                continue
            if in_ops:
                if line.startswith("  ") and not line.startswith("    "):
                    key = line.strip()
                    ops.append(key[:-1] if key.endswith(":") else key)
                elif line and not line[0].isspace():
                    break
        return sorted(set(ops))


def load_calibration_streams(
    path: Path, n_calibration_clips: int, n_parity_per_kind: int = 8
) -> tuple[list[list[torch.Tensor]], list[list[torch.Tensor]]]:
    """Split the bundle's calibration clips into calibration/parity streams.

    The bundle stores features as [clip, 40, frames] plus per-clip kind
    metadata. Selection is deterministic: per kind, in metadata order,
    the first clips calibrate and the next few are held out for the
    fp32-vs-int8 parity check. Each stream is a list of (1, 40, 1, 1)
    frames to be fed sequentially after a state reset (the model is
    stateful; observers only see realistic ranges when streamed).
    """
    bundle = torch.load(path, map_location="cpu", weights_only=False)
    features = bundle["features"]
    metadata = bundle["metadata"]
    if features.ndim != 3 or features.shape[1] != 40:
        raise SystemExit(f"unexpected calibration features shape {features.shape}")
    if len(metadata) != features.shape[0]:
        raise SystemExit("calibration metadata/clip count mismatch")

    by_kind: dict[str, list[int]] = {}
    for i, entry in enumerate(metadata):
        by_kind.setdefault(entry["kind"], []).append(i)

    def stream(clip: int) -> list[torch.Tensor]:
        return [
            features[clip, :, t].reshape(1, 40, 1, 1)
            for t in range(features.shape[2])
        ]

    calibration: list[list[torch.Tensor]] = []
    parity: list[list[torch.Tensor]] = []
    for idxs in by_kind.values():
        n_cal = max(1, round(n_calibration_clips * len(idxs) / len(metadata)))
        calibration += [stream(i) for i in idxs[:n_cal]]
        parity += [stream(i) for i in idxs[n_cal : n_cal + n_parity_per_kind]]
    return calibration, parity


def zero_histories(module: torch.nn.Module) -> None:
    with torch.no_grad():
        for name, buf in module.named_buffers():
            if name.endswith("history"):
                buf.zero_()


def stream_posteriors(
    forward, reset, streams: list[list[torch.Tensor]]
) -> list[torch.Tensor]:
    """Run each stream frame by frame; return per-stream [frames, 3]."""
    out = []
    with torch.no_grad():
        for frames in streams:
            reset()
            rows = []
            for frame in frames:
                result = forward(frame)
                if isinstance(result, (tuple, list)):
                    result = result[0]
                rows.append(result.reshape(-1))
            out.append(torch.stack(rows))
    return out


def quantize_cadence_int8(model, example, args):
    """PTQ + Cadence lowering; returns (EdgeProgramManager, info dict)."""
    from executorch.backends.cadence.aot.compiler import (
        _lower_ep_to_cadence,
        apply_pre_edge_transform_passes,
        convert_pt2,
        prepare_pt2,
    )
    from executorch.backends.cadence.aot.quantizer.quantizer import (
        CadenceDefaultQuantizer,
    )

    calibration, parity_streams = load_calibration_streams(
        args.calibration, args.calibration_clips
    )
    print(
        f"calibration: {len(calibration)} clips, "
        f"parity: {len(parity_streams)} clips"
    )

    fp32_ref = stream_posteriors(model, model.reset, parity_streams)

    quantizer = CadenceDefaultQuantizer()
    prepared = prepare_pt2(model, (example,), quantizer)
    with torch.no_grad():
        for i, frames in enumerate(calibration):
            zero_histories(prepared)
            for frame in frames:
                prepared(frame)
            if (i + 1) % 16 == 0:
                print(f"  calibrated {i + 1}/{len(calibration)} clips")
    converted = convert_pt2(prepared)

    int8_out = stream_posteriors(
        converted, lambda: zero_histories(converted), parity_streams
    )
    wake_diff = torch.cat(
        [(a[:, 0] - b[:, 0]) for a, b in zip(int8_out, fp32_ref)]
    )
    parity = {
        "clips": len(parity_streams),
        "frames": sum(len(s) for s in parity_streams),
        "wake_posterior_mean_abs_diff": float(wake_diff.abs().mean()),
        "wake_posterior_max_abs_diff": float(wake_diff.abs().max()),
        "fp32_frames_over_0.95": int(
            sum(int((b[:, 0] > 0.95).sum()) for b in fp32_ref)
        ),
        "int8_frames_over_0.95": int(
            sum(int((a[:, 0] > 0.95).sum()) for a in int8_out)
        ),
    }
    print(f"parity vs fp32: {parity}")

    with torch.no_grad():
        exported = torch.export.export(converted, (example,), strict=True)
        exported = apply_pre_edge_transform_passes(exported, quantizer)
        edge = _lower_ep_to_cadence(exported)
    info = {
        "quantization": "cadence-int8-ptq",
        "calibration_clips": len(calibration),
        "parity_vs_fp32": parity,
    }
    return edge, info


def format_float_array(name: str, values: list[float]) -> str:
    lines = [f"static const float {name}[{len(values)}] = {{"]
    for i in range(0, len(values), 4):
        chunk = ", ".join(f"{v:.9g}f" for v in values[i : i + 4])
        lines.append(f"    {chunk},")
    lines.append("};")
    return "\n".join(lines)


def write_frontend_header(path: Path, frontend: dict, checkpoint_sha: str) -> None:
    text = f"""/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* GENERATED by tools/export_hey_muse.py from checkpoint
 * sha256:{checkpoint_sha}. Do not edit by hand. */

#pragma once

#define WW_FRONTEND_SAMPLE_RATE_HZ                                            \
   { frontend['sample_rate'] }
#define WW_FRONTEND_WINDOW_SAMPLES                                            \
   { frontend['window_samples'] }
#define WW_FRONTEND_HOP_SAMPLES                                               \
   { frontend['hop_samples'] }
#define WW_FRONTEND_N_FFT                                                     \
   { frontend['n_fft'] }
#define WW_FRONTEND_N_MELS                                                    \
   { frontend['n_mels'] }
#define WW_FRONTEND_LOG_EPS {frontend['log_eps']:.9g}f

{format_float_array('kWwFrontendNormMean', [float(v) for v in frontend['norm_mean']])
 }

 {format_float_array('kWwFrontendNormStd', [float(v) for v in frontend['norm_std']])
 }
"""
    path.write_text(text)


def main() -> None:
    component = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument(
        "--executorch-root",
        type=Path,
        default=os.environ.get("EXECUTORCH_ROOT"),
        help="ExecuTorch checkout (or set EXECUTORCH_ROOT)",
    )
    parser.add_argument("--output-dir", type=Path, default=component / "models")
    parser.add_argument(
        "--frontend-header", type=Path, default=component / "ww_frontend_config.h"
    )
    parser.add_argument(
        "--source",
        default="clean-voice training bundle (epoch 11)",
        help="Provenance string recorded in the JSON sidecar",
    )
    parser.add_argument(
        "--calibration",
        type=Path,
        required=True,
        help="Training bundle calibration_frames.pt",
    )
    parser.add_argument(
        "--calibration-clips",
        type=int,
        default=96,
        help="Clips to calibrate on, stratified by kind (default 96)",
    )
    args = parser.parse_args()

    if args.executorch_root is None:
        parser.error("--executorch-root or EXECUTORCH_ROOT is required")
    et_root = args.executorch_root.resolve()
    if not (et_root / "version.txt").is_file():
        parser.error(f"not an ExecuTorch checkout: {et_root}")

# Use the installed ExecuTorch Python package for exir. Do not force the
# checkout onto sys.path: an uninstalled checkout lacks package resources
# (for example exir/_serialize/program.fbs) that the install provides.
# The documented prerequisite is an install built from this checkout; warn
# when the environment does not look like one.
    import executorch.exir as exir

    exir_file = Path(exir.__file__).resolve()
    if et_root not in exir_file.parents:
        print(
            f"WARNING: executorch.exir resolved outside {et_root}: {exir_file}. "
            "Install ExecuTorch from --executorch-root to align exporter/runtime.",
            file=sys.stderr,
        )

    checkpoint = torch.load(args.checkpoint, map_location="cpu", weights_only=False)
    config = checkpoint["config"]
    frontend = config["frontend"]
    model_cfg = config["model"]

    for key, expected in EXPECTED_FRONTEND.items():
        actual = frontend.get(key)
        if actual != expected:
            raise SystemExit(
                f"unsupported frontend config {key}={actual!r}, expected {expected!r}; "
                "the C frontend in ww_frontend.c implements only the frozen config"
            )
    if model_cfg.get("class_names") != EXPECTED_CLASSES:
        raise SystemExit(f"unexpected class order: {model_cfg.get('class_names')!r}")
    if len(frontend["norm_mean"]) != 40 or len(frontend["norm_std"]) != 40:
        raise SystemExit("norm_mean/norm_std must each have 40 entries")
    if any(float(v) == 0.0 for v in frontend["norm_std"]):
        raise SystemExit("norm_std contains a zero entry")

    streaming_cls = load_streaming_class(et_root)
    model = streaming_cls(apply_softmax=True).eval()
    model.reset()
    missing, unexpected = model.load_state_dict(
        checkpoint["model_state_dict"], strict=False
    )
    if unexpected:
        raise SystemExit(f"unexpected checkpoint keys: {unexpected}")
    non_history_missing = [k for k in missing if not k.endswith(".history")]
    if non_history_missing:
        raise SystemExit(f"checkpoint is missing non-history keys: {non_history_missing}")

    from executorch.exir import ExecutorchBackendConfig
    from executorch.exir.passes.init_mutable_pass import InitializedMutableBufferPass

    example = torch.zeros(streaming_cls.FRAME_SHAPE)
    edge, quantize_info = quantize_cadence_int8(model, example, args)
    with torch.no_grad():
        program = edge.to_executorch(
            config=ExecutorchBackendConfig(
# Histories are zero on a fresh eager model; serialize those
# zeros so a newly loaded method starts in the same state.
                passes=[InitializedMutableBufferPass(["history"])],
            )
        )

    args.output_dir.mkdir(parents=True, exist_ok=True)
    pte_path = args.output_dir / "hey_muse.pte"
    with pte_path.open("wb") as f:
        program.write_to_file(f)

    # The registry component registers only the kernels in a bundle
    # generated at build time from this .pte; the build refuses a bundle
    # that does not cover this model's ops. Record the ops here so the
    # bundle check has a sidecar to compare against.
    ops = model_ops(et_root, pte_path)

    checkpoint_sha = sha256_file(args.checkpoint)
    write_frontend_header(args.frontend_header, frontend, checkpoint_sha)

    et_version = (et_root / "version.txt").read_text().strip()
    try:
        import executorch.version as et_installed_version

        exporter_git_version = getattr(et_installed_version, "git_version", None)
    except Exception:
        exporter_git_version = None
    try:
        et_commit = subprocess.run(
            ["git", "-C", str(et_root), "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
    except Exception:
        et_commit = None

    sidecar = {
        "schema_version": 1,
        "model": "StreamingWakeWord",
        "wake_phrases": config.get("data", {}).get("wake_phrases", []),
        "class_names": EXPECTED_CLASSES,
        "source": args.source,
        "checkpoint_sha256": checkpoint_sha,
        "checkpoint_epoch": checkpoint.get("epoch"),
        "pte_sha256": sha256_file(pte_path),
        "pte_size": pte_path.stat().st_size,
        "architecture_executorch_version": et_version,
        "architecture_executorch_commit": et_commit,
        "exporter_git_version": exporter_git_version,
        "quantization": quantize_info["quantization"],
        "model_ops": ops,
        "frontend": {
            k: frontend[k]
            for k in (
                "sample_rate",
                "window_samples",
                "hop_samples",
                "n_fft",
                "n_mels",
                "fmin",
                "fmax",
                "mel_scale",
                "log_eps",
                "clip_frames",
            )
        },
        "receptive_field_frames": model_cfg.get("receptive_field"),
        "notes": (
            "Research/synthetic-data checkpoint; see the training bundle README "
            "for metrics and limitations. Runtime reading uses the sidecar only "
            "as provenance; the C frontend constants are regenerated alongside."
        ),
    }
    sidecar["calibration"] = {
        "file": args.calibration.name,
        "sha256": sha256_file(args.calibration),
        "clips": quantize_info["calibration_clips"],
    }
    sidecar["parity_vs_fp32"] = quantize_info["parity_vs_fp32"]
    (args.output_dir / "hey_muse.json").write_text(
        json.dumps(sidecar, indent=2, sort_keys=True) + "\n"
    )
    print(f"wrote {pte_path} ({pte_path.stat().st_size} bytes)")
    print(f"wrote {args.output_dir / 'hey_muse.json'}")
    print(f"wrote {args.frontend_header}")
    print(f"model ops ({len(ops)}): {', '.join(ops)}")


if __name__ == "__main__":
    main()
