# SPDX-License-Identifier: Apache-2.0
"""Deployment settings for the MA35 gateway; credentials stay in SDK state."""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parent
STATE = Path(os.environ.get("MUSEGADGET_STATE_DIR", str(ROOT / "state"))).expanduser()
SSH_TARGET = os.environ.get("MA35_SSH_TARGET", "root@192.168.137.2")
SSH = ["ssh", "-p", os.environ.get("MA35_SSH_PORT", "22"),
       "-o", "BatchMode=yes", "-o", "ConnectTimeout=5",
       "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=2"]
if os.environ.get("MA35_SSH_HOST_ALIAS"):
    SSH += ["-o", "HostKeyAlias=" + os.environ["MA35_SSH_HOST_ALIAS"]]
SSH += [SSH_TARGET]
