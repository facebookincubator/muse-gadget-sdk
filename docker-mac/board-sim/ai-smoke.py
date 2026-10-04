#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
"""Exercise live Muse replies and a read-only command on enrolled simulators."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import subprocess
import uuid

BOARDS = ("ma35d1-a1", "ma35d1-s1", "ma35h0-a1", "ma35h0-a2")
ROOT = Path(__file__).resolve().parents[2]


def call(command, **kwargs):
    return subprocess.run(command, check=True, capture_output=True, text=True,
                          timeout=240, **kwargs).stdout


def test_board(board):
    container = "muse-" + board + "-sim"
    info = call(["docker", "exec", container, "musegadget", "info"])
    if "paired:    yes" not in info:
        raise RuntimeError(board + " needs phone enrollment first")
    node = re.search(r"node id:\s+(\S+)", info).group(1)
    session = str(uuid.uuid4())
    tests = [
        ("greeting", "Say a short hello. This is a live AI chat test from " + board + "."),
        ("arithmetic", "What is 17 times 23? Reply with the number only."),
        ("board-command", "On the Linux gadget with node id " + node + " (" + board +
         "), use its system.run command to execute `uname -m && id -un`. "
         "Report the actual output. This is a read-only smoke test; keep your reply short."),
    ]
    results = []
    for name, prompt in tests:
        since = datetime.now(timezone.utc).isoformat()
        print(board + ": testing " + name, flush=True)
        output = call(["docker", "exec", "-i", container, "musegadget", "chat",
                       "--json", "--session-id", session, "-"], input=prompt)
        events = [json.loads(line) for line in output.splitlines() if line.strip()]
        replies = {}
        for event in events:
            if event.get("type") == "reply":
                replies[event["message_id"]] = event["text"]
        answer = "\n".join(replies.values()).strip()
        complete = any(e.get("type") == "done" for e in events)
        passed = bool(answer) and complete
        invoked = False
        if name == "arithmetic":
            passed = passed and bool(re.search(r"\b391\b", answer))
        if name == "board-command":
            log_result = subprocess.run(["docker", "logs", "--since", since, container],
                check=True, capture_output=True, text=True, timeout=15)
            logs = log_result.stdout + log_result.stderr
            invoked = "invoke system.run" in logs and "system.run as muse" in logs
            passed = passed and "aarch64" in answer and bool(re.search(r"\bmuse\b", answer)) and invoked
        result = dict(board=board, node_id=node, test=name, prompt=prompt,
                      answer=answer, passed=passed, invoked=invoked)
        results.append(result)
        print(("PASS " if passed else "FAIL ") + board + " " + name + ": " + answer, flush=True)
    return results


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("boards", nargs="+", choices=BOARDS)
    args = parser.parse_args()
    results = []
    for board in args.boards:
        results.extend(test_board(board))
    destination = ROOT / ".board-sim/live-ai-results.json"
    destination.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    print("Results: " + str(destination))
    raise SystemExit(0 if all(result["passed"] for result in results) else 1)
