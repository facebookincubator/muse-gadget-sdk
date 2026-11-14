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

"""Command guard demo: no device, no Bluetooth, no SDK token needed.

    PYTHONPATH=linux/examples python3 -m command_guard.demo
    # or, from this directory:  python3 demo.py

Runs six scenarios against a guard for a simulated account whose home is
/home/pi, prints each decision, then shows the ledger verifying — and
failing to verify after one row is tampered with.
"""

from __future__ import annotations

import base64
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from command_guard.guard import Guard  # noqa: E402
from command_guard.ledger import Ledger, verify  # noqa: E402
from command_guard.policy import load_policy  # noqa: E402

HOME = "/home/pi"

SCENARIOS = [
    ("Check the disk usage on my Pi",
     "system.run", {"command": "df -h"}),
    ("Install Home Assistant on my Pi",
     "system.run", {"command": "sudo apt install -y homeassistant"}),
    ("Delete the stale draft at /home/pi/draft.md",
     "system.run", {"command": "rm -rf /home/pi/"}),
    ("Save a note for me",
     "file.write", {"path": "/home/pi/notes.txt",
                    "data_b64": base64.b64encode(b"hello\n").decode(), "final": True}),
    ("(no request supplied)",
     "file.read", {"path": "/var/lib/musegadget/sdk_token"}),
]


def main() -> int:
    tmp = tempfile.mkdtemp(prefix="command-guard-demo-")
    ledger_path = os.path.join(tmp, "guard-ledger.jsonl")
    guard = Guard(policy=load_policy(home=HOME), ledger=Ledger(ledger_path), home=HOME)

    print(f"Command guard demo (simulated account home: {HOME})\n")
    for i, (request, command, params) in enumerate(SCENARIOS, start=1):
        shown_request = None if request.startswith("(") else request
        decision = guard.check(command, params, request=shown_request)
        label = params.get("command") or params.get("path")
        print(f"{i}. {command}  {label}")
        print(f"   request: {request}")
        print(f"   -> {decision.action.upper()}: {decision.reason} "
              f"[layer1={decision.layer1}, layer2={decision.layer2}]\n")

    print(f"6. Ledger tamper-evidence ({ledger_path})")
    ok, message = verify(ledger_path)
    print(f"   verify before tampering: {'PASS' if ok else 'FAIL'}: {message}")
    with open(ledger_path, encoding="utf-8") as fh:
        rows = [json.loads(line) for line in fh if line.strip()]
    rows[0]["reason"] = "rewritten after the fact"
    with open(ledger_path, "w", encoding="utf-8") as fh:
        for row in rows:
            fh.write(json.dumps(row) + "\n")
    ok, message = verify(ledger_path)
    print(f"   verify after tampering:  {'PASS' if ok else 'FAIL'}: {message}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
