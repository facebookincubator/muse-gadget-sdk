# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.

"""Optional local detector: reused C signal code plus wheel Runtime inference.

feed() consumes 16 kHz mono signed-16-bit little-endian PCM, including split
samples, and reports any fire while consuming the entire chunk. Importing this
module loads neither torch nor ExecuTorch. Construction requires a compatible
ExecuTorch wheel and torch installation; run the prerequisite checker first.
"""

from __future__ import annotations

import ctypes
import os
import threading
import warnings
import weakref
from contextlib import ExitStack
from importlib import resources
from pathlib import Path


class WakeWordUnavailableError(RuntimeError):
    """The optional signal library, compatible wheel, or model is unavailable."""


class ResearchWakeWordWarning(UserWarning):
    """The bundled model is a pipeline artifact, not a production detector."""


_SENSITIVITIES = {"low": 0, "default": 1, "high": 2}
# Public TensorInfo.dtype() uses ExecuTorch's serialized ScalarType enum.
_ET_FLOAT32 = 6
_FRAME_CALLBACK = ctypes.CFUNCTYPE(
    ctypes.c_int,
    ctypes.c_void_p,
    ctypes.POINTER(ctypes.c_float),
    ctypes.POINTER(ctypes.c_float),
)


def _release(library: ctypes.CDLL, handle: int, stack: ExitStack) -> None:
    try:
        library.ww_signal_destroy(handle)
    finally:
        stack.close()


def _load_library(stack: ExitStack) -> ctypes.CDLL:
    override = os.environ.get("MUSE_WAKEWORD_LIBRARY")
    if override:
        candidates = [Path(override).expanduser().resolve()]
    else:
        packaged = resources.files("musegadget").joinpath(
            "data/wakeword/libmuse_wakeword_signal.so"
        )
        candidates = []
        if packaged.is_file():
            candidates.append(stack.enter_context(resources.as_file(packaged)))
        candidates.append(
            Path(__file__).resolve().parents[2]
            / "wakeword"
            / "build"
            / "libmuse_wakeword_signal.so"
        )
    failures = []
    for path in candidates:
        try:
            library = ctypes.CDLL(str(path))
            library.ww_signal_create.argtypes = [
                ctypes.c_int,
                ctypes.c_void_p,
                ctypes.c_size_t,
            ]
            library.ww_signal_create.restype = ctypes.c_void_p
            library.ww_signal_feed.argtypes = [
                ctypes.c_void_p,
                ctypes.c_void_p,
                ctypes.c_size_t,
                _FRAME_CALLBACK,
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.c_int),
            ]
            library.ww_signal_feed.restype = ctypes.c_int
            library.ww_signal_reset.argtypes = [ctypes.c_void_p]
            library.ww_signal_reset.restype = ctypes.c_int
            library.ww_signal_destroy.argtypes = [ctypes.c_void_p]
            library.ww_signal_destroy.restype = None
            library.ww_signal_last_error.argtypes = [ctypes.c_void_p]
            library.ww_signal_last_error.restype = ctypes.c_char_p
            return library
        except (OSError, AttributeError) as exc:
            failures.append(str(exc))
    raise WakeWordUnavailableError(
        "Hey Muse signal library unavailable. Run linux/wakeword/build_signal.sh "
        "or set MUSE_WAKEWORD_LIBRARY to libmuse_wakeword_signal.so. "
        + "; ".join(failures)
    )


class _WheelModel:
    """Only the public Runtime/Program/Method APIs; no binding attribute patches."""

    def __init__(self, model_path: Path) -> None:
        self._program = None
        self._method = None
        self._path = model_path
        try:
            import torch
            from executorch.runtime import Runtime

            self._torch = torch
            self._runtime = Runtime.get()
            if not self._runtime.backend_registry.is_available("XnnpackBackend"):
                raise RuntimeError("Installed ExecuTorch wheel has no XNNPACK backend")
            self.reset()
        except Exception as exc:
            self.close()
            raise WakeWordUnavailableError(
                "Hey Muse requires a compatible ExecuTorch wheel, torch and XNNPACK "
                "runtime. Run the wheel prerequisite setup and "
                "python -m musegadget.wakeword_check; do not use stale editable bindings. "
                + str(exc)
            ) from exc

    def reset(self) -> None:
        # Program caches loaded methods; re-loading that method alone is not a reset.
        self._method = None
        self._program = None
        program = self._runtime.load_program(self._path)
        if "forward" not in program.method_names:
            raise ValueError("Wake-word model lacks the forward method")
        method = program.load_method("forward")
        if method is None:
            raise ValueError("Wake-word forward method could not be loaded")
        meta = method.metadata
        if meta.num_inputs() != 1 or meta.num_outputs() != 1:
            raise ValueError("Wake-word forward must have one input and one output")
        for info, shape in (
            (meta.input_tensor_meta(0), (1, 40, 1, 1)),
            (meta.output_tensor_meta(0), (1, 3)),
        ):
            if tuple(info.sizes()) != shape or info.dtype() != _ET_FLOAT32:
                raise ValueError(
                    "Wake-word forward requires float32 [1,40,1,1] -> [1,3]"
                )
        self._program = program
        self._method = method

    def posteriors(self, features) -> tuple[float, float, float]:
        if self._method is None:
            raise RuntimeError("Wake-word model is unavailable; reset required")
        torch = self._torch
        frame = torch.tensor(features, dtype=torch.float32, device="cpu").reshape(
            1, 40, 1, 1
        )
        with torch.no_grad():
            outputs = self._method.execute((frame,))
        if len(outputs) != 1:
            raise RuntimeError("Wake-word inference returned unexpected output count")
        output = outputs[0]
        if (
            not isinstance(output, torch.Tensor)
            or tuple(output.shape) != (1, 3)
            or output.dtype != torch.float32
            or output.device.type != "cpu"
            or not bool(torch.isfinite(output).all())
        ):
            raise RuntimeError(
                "Wake-word inference returned invalid float32 posteriors"
            )
        return tuple(output.reshape(3).tolist())

    def close(self) -> None:
        self._method = None
        self._program = None


class WakeWordDetector:
    """One serialized PCM stream with independent signal and model histories."""

    def __init__(self, model_path=None, sensitivity="default") -> None:
        if sensitivity not in _SENSITIVITIES:
            raise ValueError("sensitivity must be 'low', 'default', or 'high'")
        self._lock = threading.Lock()
        self._handle = None
        self._model = None
        self._callback_error = None
        self._poisoned = False
        stack = ExitStack()
        try:
            if model_path is None:
                model = resources.files("musegadget").joinpath(
                    "data/wakeword/hey_muse_xnnpack.pte"
                )
                if not model.is_file():
                    raise WakeWordUnavailableError("Bundled Hey Muse model is missing")
                path = stack.enter_context(resources.as_file(model))
            else:
                path = Path(model_path).expanduser().resolve()
                if not path.is_file():
                    raise WakeWordUnavailableError(f"Wake-word model not found: {path}")
            library = _load_library(stack)
            self._model = _WheelModel(path)
            error = ctypes.create_string_buffer(1024)
            handle = library.ww_signal_create(
                _SENSITIVITIES[sensitivity], error, len(error)
            )
            if not handle:
                raise WakeWordUnavailableError(
                    error.value.decode("utf-8", errors="replace")
                )
            self._library = library
            self._handle = handle
            self._finalizer = weakref.finalize(self, _release, library, handle, stack)
            self._callback = _FRAME_CALLBACK(self._on_frame)
            try:
                warnings.warn(
                    "Hey Muse is an experimental research checkpoint, not "
                    "validated on real microphone recordings. Keep opt-in.",
                    ResearchWakeWordWarning,
                    stacklevel=2,
                )
            except BaseException:
                self.close()
                raise
        except BaseException:
            if self._model is not None:
                self._model.close()
            stack.close()
            raise

    def _on_frame(self, context, frame, posterior) -> int:
        try:
            posterior[0] = self._model.posteriors(tuple(frame[i] for i in range(40)))[0]
            return 0
        except BaseException as exc:
            # ctypes otherwise prints and swallows exceptions raised by callbacks.
            self._callback_error = exc
            return 1

    def _check_open(self) -> None:
        if self._handle is None:
            raise RuntimeError("WakeWordDetector is closed")

    def _check_result(self, status: int) -> None:
        if status:
            error = self._library.ww_signal_last_error(self._handle)
            raise RuntimeError(
                "Hey Muse signal processing failed: "
                + (error or b"unknown native error").decode("utf-8", errors="replace")
            )

    def feed(self, pcm_s16le: bytes) -> bool:
        if not isinstance(pcm_s16le, bytes):
            raise TypeError("pcm_s16le must be bytes (16 kHz mono signed-16-bit LE)")
        with self._lock:
            self._check_open()
            if self._poisoned:
                raise RuntimeError("Wake-word stream unavailable; reset required")
            self._callback_error = None
            fired = ctypes.c_int()
            status = self._library.ww_signal_feed(
                self._handle,
                ctypes.c_char_p(pcm_s16le),
                len(pcm_s16le),
                self._callback,
                None,
                ctypes.byref(fired),
            )
            if self._callback_error is not None:
                self._poisoned = True
                error = self._callback_error
                self._callback_error = None
                raise error
            if status:
                self._poisoned = True
            self._check_result(status)
            return bool(fired.value)

    def reset(self) -> None:
        with self._lock:
            self._check_open()
            self._poisoned = True
            self._model.reset()
            self._check_result(self._library.ww_signal_reset(self._handle))
            self._callback_error = None
            self._poisoned = False

    def close(self) -> None:
        with self._lock:
            if self._handle is not None:
                self._handle = None
                self._finalizer()
            if self._model is not None:
                self._model.close()
            self._callback_error = None
            # Drop the bound callback's reference cycle even when closed explicitly.
            self._callback = None

    def __enter__(self) -> WakeWordDetector:
        with self._lock:
            self._check_open()
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.close()
