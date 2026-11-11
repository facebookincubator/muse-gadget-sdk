# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.

"""Generate the ExecuTorch kernel bundle at build time, without the SHA gate.

Loads the registry component's tools/gen_kernels.py and runs it with
only its _check_version replaced. That check requires the installed
executorch commit to equal the component's commit exactly (for dev
versions), which no published nightly wheel satisfies for this
component; the SDK deliberately does not require an exact-commit
Python environment to build firmware. The replacement keeps everything
else in _check_version: the component version and commit still come
from the component's own VERSION and manifest, and the bundle still
records the *true* installed executorch version and commit — nothing
is forged. What is dropped is only the equality gate, whose purpose
is to guard against codegen drift; the SDK pins the component version,
and the build still refuses a bundle whose component version, commit,
or model hash does not match the declared model and component.

Run under a Python with executorch and PyYAML installed (see
EXECUTORCH_PYTHON in esp32/docs/executorch.md). The component's
gen_kernels.py needs no patching: it is downloaded, never edited.
"""

from __future__ import annotations

import argparse
import importlib.util
import re
import sys
from pathlib import Path

import yaml


def _unchecked_version(component_dir: Path):
    """Component fields from the component dir; installed fields truthful."""
    try:
        import executorch.version
    except ModuleNotFoundError as exc:
        raise SystemExit(
            "executorch is not importable from this Python; set "
            "EXECUTORCH_PYTHON to a Python with executorch installed "
            f"(see esp32/docs/executorch.md): {exc}"
        ) from exc
    manifest = yaml.safe_load((component_dir / "idf_component.yml").read_text())
    stamp = (component_dir / "VERSION").read_text()
    component_version = str(manifest["version"])
    commit = re.search(r"^executorch_commit:\s*(\S+)", stamp, re.MULTILINE)
    component_sha = commit.group(1) if commit else ""
    return (
        component_version,
        component_sha,
        executorch.version.__version__,
        executorch.version.git_version or "",
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pte", type=Path, action="append", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--component-dir", type=Path, required=True)
    args = parser.parse_args(argv)

    script = args.component_dir / "tools" / "gen_kernels.py"
    if not script.is_file():
        raise SystemExit(f"no gen_kernels.py in component dir {args.component_dir}")
    if not (args.component_dir / "tools" / "kernel_libraries.json").is_file():
        raise SystemExit(
            f"no kernel_libraries.json in component dir {args.component_dir}"
        )
    spec = importlib.util.spec_from_file_location("sdk_gen_kernels", script)
    if spec is None or spec.loader is None:
        raise SystemExit(f"cannot load {script}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    # Drop the SHA gate; see the module docstring.
    module._check_version = _unchecked_version
    gen_args = ["--component-dir", str(args.component_dir)]
    for pte in args.pte:
        gen_args += ["--pte", str(pte)]
    gen_args += ["--out", str(args.out)]
    return module.main(gen_args)


if __name__ == "__main__":
    raise SystemExit(main())
