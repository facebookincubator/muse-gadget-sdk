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

"""Layer 1: deterministic policy rules for Muse gadget commands.

These rules need no knowledge of what the user asked for: sudo, destructive
shell commands, reads of credential paths and writes to system areas are
decided from the command and its parameters alone. Anything the rules do
not recognise falls back to the policy's default shell action, because
over-blocking is the worst failure mode for a guard: a guard that gets in
the way gets switched off, and a switched-off guard detects nothing.
"""

from __future__ import annotations

import json
import os
import re
import shlex
from dataclasses import dataclass, field

ALLOW = "allow"
CONFIRM = "confirm"
BLOCK = "block"

_SEVERITY = {ALLOW: 0, CONFIRM: 1, BLOCK: 2}


def worse(a: str, b: str) -> str:
    return a if _SEVERITY[a] >= _SEVERITY[b] else b


@dataclass(frozen=True)
class RuleDecision:
    action: str
    reason: str


DEFAULT_POLICY = {
    "block_sudo": True,
    # Paths a gadget command may never read or write. "~" is expanded
    # against the account's home directory.
    "sensitive_paths": ["/var/lib/musegadget/", "~/.ssh/", "/etc/shadow"],
    # Prefixes a gadget command may never write to.
    "system_write_paths": ["/etc/", "/opt/musegadget/", "/usr/"],
    # First words of shell commands that only read state.
    "readonly_commands": [
        "ls", "df", "du", "cat", "head", "tail", "pwd", "whoami",
        "hostname", "uname", "free", "uptime", "date", "wc", "stat",
        "echo", "printf", "grep", "find", "ps", "lsblk", "mount",
    ],
    # systemctl subcommands that only read state.
    "readonly_systemctl": ["status", "is-active", "is-enabled", "show", "cat"],
    # Extra substrings that always block a shell command (user policy).
    "extra_block_substrings": [],
    # Action for shell commands no rule recognises.
    "default_shell_action": CONFIRM,
}


@dataclass
class Policy:
    block_sudo: bool = True
    sensitive_paths: list = field(default_factory=list)
    system_write_paths: list = field(default_factory=list)
    readonly_commands: list = field(default_factory=list)
    readonly_systemctl: list = field(default_factory=list)
    extra_block_substrings: list = field(default_factory=list)
    default_shell_action: str = CONFIRM
    home: str = ""

    @classmethod
    def from_dict(cls, data: dict, home: str = "") -> "Policy":
        merged = dict(DEFAULT_POLICY)
        merged.update({k: v for k, v in (data or {}).items() if k in DEFAULT_POLICY})
        home = home or os.path.expanduser("~")

        def expand(p: str) -> str:
            # "~" means the *account's* home, which is not necessarily the
            # home of whoever loads the policy.
            if p == "~":
                return os.path.normpath(home)
            if p.startswith("~/"):
                return os.path.normpath(os.path.join(home, p[2:]))
            return os.path.normpath(p)
        return cls(
            block_sudo=bool(merged["block_sudo"]),
            sensitive_paths=[expand(p) for p in merged["sensitive_paths"]],
            system_write_paths=[expand(p) for p in merged["system_write_paths"]],
            readonly_commands=list(merged["readonly_commands"]),
            readonly_systemctl=list(merged["readonly_systemctl"]),
            extra_block_substrings=list(merged["extra_block_substrings"]),
            default_shell_action=str(merged["default_shell_action"]),
            home=home,
        )

    def expand(self, path: str) -> str:
        if path == "~":
            return os.path.normpath(self.home)
        if path.startswith("~/"):
            return os.path.normpath(os.path.join(self.home, path[2:]))
        return os.path.normpath(path)

    def is_sensitive(self, path: str) -> bool:
        expanded = self.expand(path)
        for prefix in self.sensitive_paths:
            if expanded == prefix or expanded.startswith(prefix.rstrip("/") + "/"):
                return True
        return False

    def is_system_write(self, path: str) -> bool:
        expanded = self.expand(path)
        for prefix in self.system_write_paths:
            if expanded == prefix or expanded.startswith(prefix.rstrip("/") + "/"):
                return True
        return False


def default_policy_path() -> str:
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "policy.json")


def load_policy(path: str | None = None, home: str = "") -> Policy:
    """Load policy.json (next to this module by default); fall back to the
    built-in defaults when no file exists. JSON is used so the guard needs
    no dependency beyond the standard library."""
    data = {}
    candidate = path or default_policy_path()
    if candidate and os.path.exists(candidate):
        with open(candidate, encoding="utf-8") as fh:
            data = json.load(fh)
    return Policy.from_dict(data, home=home)


# -- shell helpers -------------------------------------------------------------

def _tokens(command: str) -> list:
    try:
        return shlex.split(command)
    except ValueError:
        return command.split()


def _first_word(command: str) -> str:
    tokens = _tokens(command)
    return os.path.basename(tokens[0]) if tokens else ""


_SIMPLE_SHELL = re.compile(r"^[^;&|<>`$\\]+$")
_REMOTE_PIPE = re.compile(r"\b(curl|wget)\b[^|]*\|\s*(sudo\s+)?(sh|bash|zsh)\b")
_INSTALL = re.compile(
    r"\b(apt|apt-get|dnf|yum|pacman)\b[^;&|]*\binstall\b"
    r"|\bpip3?\b[^;&|]*\binstall\b"
    r"|\bnpm\b[^;&|]*\b(install|i)\b"
    r"|\bbrew\b[^;&|]*\binstall\b"
)
_SUDO = re.compile(r"(^|[\s;&|])sudo(\s|$)")
_POWER = ("shutdown", "reboot", "poweroff", "halt")


def _check_system_run(policy: Policy, command: str) -> RuleDecision:
    cmd = command or ""
    tokens = _tokens(cmd)
    words = [os.path.basename(t) for t in tokens]

    for needle in policy.extra_block_substrings:
        if needle and needle in cmd:
            return RuleDecision(BLOCK, f"command matches blocked substring {needle!r} (policy)")

    if policy.block_sudo and _SUDO.search(cmd):
        return RuleDecision(BLOCK, "sudo is blocked by the default policy")

    if _REMOTE_PIPE.search(cmd):
        return RuleDecision(BLOCK, "piping a remote download into a shell is blocked")

    if words and words[0] in _POWER:
        return RuleDecision(BLOCK, f"{words[0]} is blocked")

    if "mkfs" in words or any(w.startswith("mkfs") for w in words):
        return RuleDecision(BLOCK, "mkfs is blocked")

    if "dd" in words:
        for token in tokens:
            if token.startswith("of=/dev/"):
                return RuleDecision(BLOCK, "dd writing to a device is blocked")

    # Sensitive paths mentioned anywhere in the command line.
    for token in tokens:
        candidate = token.strip("'\"")
        if candidate.startswith(("/", "~")) and policy.is_sensitive(candidate):
            return RuleDecision(BLOCK, f"command touches sensitive path {candidate}")

    # Redirection into a system or sensitive path is a write.
    for match in re.finditer(r">{1,2}\s*(\S+)", cmd):
        target = match.group(1).strip("'\"")
        if policy.is_sensitive(target):
            return RuleDecision(BLOCK, f"redirection writes to sensitive path {target}")
        if policy.is_system_write(target):
            return RuleDecision(BLOCK, f"redirection writes to system path {target}")

    if "rm" in words:
        idx = words.index("rm")
        args = tokens[idx + 1:]
        flags = "".join(a for a in args if a.startswith("-"))
        targets = [a for a in args if not a.startswith("-")]
        recursive = "r" in flags or "R" in flags or "--recursive" in args
        if recursive and targets:
            target = policy.expand(targets[-1].strip("'\""))
            home = os.path.normpath(policy.home)
            if target in ("/", "/*") or target == home or home.startswith(target.rstrip("/") + "/"):
                return RuleDecision(BLOCK, f"recursive delete of {targets[-1]} is blocked")
        return RuleDecision(CONFIRM, "delete is irreversible and a reversible alternative "
                                     "(move to trash) may exist; confirmation required")

    if "mv" in words or "rmdir" in words:
        return RuleDecision(CONFIRM, "move/remove is hard to undo; confirmation required")

    if _INSTALL.search(cmd):
        return RuleDecision(CONFIRM, "package installation changes the system; "
                                     "confirmation required")

    first = _first_word(cmd)
    if first == "systemctl":
        sub = tokens[1] if len(tokens) > 1 else ""
        if sub in policy.readonly_systemctl and _SIMPLE_SHELL.match(cmd):
            return RuleDecision(ALLOW, f"read-only command: systemctl {sub}")
        return RuleDecision(CONFIRM, f"systemctl {sub} may change the system; confirmation required")
    if first in policy.readonly_commands and _SIMPLE_SHELL.match(cmd):
        return RuleDecision(ALLOW, f"read-only command: {first}")
    if first in policy.readonly_commands:
        return RuleDecision(CONFIRM, "read-only command combined with shell operators; "
                                     "confirmation required")

    return RuleDecision(policy.default_shell_action,
                        "shell command is not on the read-only allowlist")


def evaluate(policy: Policy, command: str, params: dict) -> RuleDecision:
    """Layer 1 decision for one gadget command."""
    params = params or {}
    if command == "device.health":
        return RuleDecision(ALLOW, "device.health is always allowed")
    if command == "system.run":
        return _check_system_run(policy, str(params.get("command") or ""))
    if command in ("file.read", "file.write"):
        path = str(params.get("path") or "")
        if not path:
            return RuleDecision(ALLOW, "no path; the executor will reject the call")
        if policy.is_sensitive(path):
            return RuleDecision(BLOCK, f"{command} on sensitive path {path} is blocked")
        if command == "file.write":
            if policy.is_system_write(path):
                return RuleDecision(BLOCK, f"write to system path {path} is blocked")
            home = os.path.normpath(policy.home)
            expanded = policy.expand(path)
            if expanded == home or expanded.startswith(home.rstrip("/") + "/"):
                return RuleDecision(ALLOW, "single file write inside the account home")
            return RuleDecision(CONFIRM, f"write outside the account home ({path}); "
                                         "confirmation required")
        return RuleDecision(ALLOW, "file read on a non-sensitive path")
    return RuleDecision(ALLOW, "unknown command; the executor will reject it")
