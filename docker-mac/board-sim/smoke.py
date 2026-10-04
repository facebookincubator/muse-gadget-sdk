#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
"""Verify encrypted synthetic enrollment over real SSH to the ARM64 simulator."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[2]
BOARDS = {"ma35d1-a1": 2224, "ma35d1-s1": 2225, "ma35h0-a1": 2226, "ma35h0-a2": 2227}
sys.path[:0] = [str(ROOT / "linux/src"), str(ROOT / "docker-mac")]
spec = importlib.util.spec_from_file_location("pair_board", ROOT / "docker-mac/pair-board.py")
pair_board = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pair_board)


def check_board(board):
    state = "/tmp/muse-board-smoke-" + uuid.uuid4().hex
    args = argparse.Namespace(target="root@127.0.0.1", port=BOARDS[board],
        ssh_key=ROOT / ".board-sim/client_key", known_hosts=ROOT / ".board-sim" / (board + "_known_hosts"),
        state_dir=state, remote_python="/opt/musegadget/venv/bin/python",
        sudo=False, force=False, timeout=30)
    ssh = pair_board.ssh_command(args)[:-1]

    def run(code):
        command = " ".join(shlex.quote(x) for x in [args.remote_python, "-c", code])
        return subprocess.run([*ssh, command], check=True, capture_output=True,
                              text=True, timeout=30).stdout

    run("from pathlib import Path; from musegadget import config; "
        "config.save_json(config.IDENTITY_FILE, "
        "{'mac':'02:00:00:00:00:01'}, Path(" + repr(state) + "))")
    remote = None
    try:
        command = " ".join(shlex.quote(x) for x in ["env", "MUSEGADGET_STATE_DIR=" + state,
            "PYTHONPATH=/opt/musegadget/source/tests", args.remote_python,
            "/opt/musegadget/source/tests/relay_peer.py", "board", "--timeout", "30"])
        remote = subprocess.Popen([*ssh, command], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8")
        def helper(name):
            assert name == "MuseGadget000001"
            env = dict(os.environ, PYTHONPATH=os.pathsep.join([
                str(ROOT / "linux/src"), str(ROOT / "linux/tests")]))
            return subprocess.Popen([sys.executable, str(ROOT / "linux/tests/relay_peer.py"), "phone"],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding="utf-8", env=env)
        assert pair_board.relay(remote, timeout=30, sdk_token="mgst_" + "A" * 43,
                                launch_helper=helper) == 0
        result = json.loads(run("import json, os, stat; from musegadget import config; "
            "from pathlib import Path; p=Path(" + repr(state) + "); "
            "r=config.load_json(config.PAIRING_FILE,p); "
            "print(json.dumps({'arch':os.uname().machine, "
            "'identity':config.load_json(config.IDENTITY_FILE,p)['mac'], "
            "'credentials_ok':r['access_token']=='synthetic-board-access' and "
            "r['refresh_token']=='synthetic-board-refresh', "
            "'file_mode':stat.S_IMODE((p/config.PAIRING_FILE).stat().st_mode), "
            "'directory_mode':stat.S_IMODE(p.stat().st_mode)}))"))
        assert result == dict(arch="aarch64", identity="02:00:00:00:00:01",
                              credentials_ok=True, file_mode=0o600, directory_mode=0o700)
        print("PASS " + board + ": ARM64 SDK enrollment over SSH; identity retained; credentials private on target.")
    finally:
        if remote is not None:
            pair_board.close_process(remote)
            remote.stderr.close()
        run("import shutil; shutil.rmtree(" + repr(state) + ")")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("board", choices=["all", *BOARDS], default="all", nargs="?")
    args = parser.parse_args()
    for board in BOARDS if args.board == "all" else [args.board]:
        check_board(board)
    print("Synthetic phone and Muse API fixtures used; real enrollment on each simulator is unchanged.")
