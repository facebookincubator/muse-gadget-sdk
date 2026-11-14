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
decided from the command and its parameters alone.

How the shell command is inspected (hardened):

- The command line is **fully tokenised** with an operator-aware ``shlex``
  lexer (``punctuation_chars``), not matched as a raw string. Operators
  (``|``, ``&&``, ``;``, redirections, …) are real tokens, so detection does
  not depend on fragile regular expressions over the raw text, and *every*
  argument of *every* sub-command is examined.
- Every path-looking argument is **resolved against the call's working
  directory** (and ``~`` against the account home) and normalised before it
  is matched, so a relative path or a ``..`` traversal cannot slip a
  sensitive target past the rules.
- The command length is **capped before any matching runs** so an oversized
  input cannot make tokenising or matching expensive.
- The default is ``confirm``. A shell command is allowed **only** when the
  validator can account for the whole command line: one simple sub-command,
  no operators, redirections or substitutions, and a first word on the
  read-only allowlist. Anything the validator cannot fully account for is
  ``confirm`` (or ``block`` when a rule fires) — never a silent ``allow``.
  (A ``confirm`` the user accepts can be promoted by adding the command to
  the policy allowlist; over-blocking, not over-confirming, is the failure
  that gets a guard switched off.)
"""

from __future__ import annotations

import json
import os
import shlex
from dataclasses import dataclass, field

ALLOW = "allow"
CONFIRM = "confirm"
BLOCK = "block"

_SEVERITY = {ALLOW: 0, CONFIRM: 1, BLOCK: 2}

# Upper bound on the shell command length the guard will inspect. Anything
# longer is not parsed or matched (that work would be unbounded); it is
# ``confirm``-ed instead. Overridable with ``max_command_chars`` in policy.json.
DEFAULT_MAX_COMMAND_CHARS = 16 * 1024


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
    # Longest shell command the guard will tokenise and match.
    "max_command_chars": DEFAULT_MAX_COMMAND_CHARS,
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
    max_command_chars: int = DEFAULT_MAX_COMMAND_CHARS
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

        try:
            max_chars = int(merged["max_command_chars"])
        except (TypeError, ValueError):
            max_chars = DEFAULT_MAX_COMMAND_CHARS
        return cls(
            block_sudo=bool(merged["block_sudo"]),
            sensitive_paths=[expand(p) for p in merged["sensitive_paths"]],
            system_write_paths=[expand(p) for p in merged["system_write_paths"]],
            readonly_commands=list(merged["readonly_commands"]),
            readonly_systemctl=list(merged["readonly_systemctl"]),
            extra_block_substrings=list(merged["extra_block_substrings"]),
            default_shell_action=str(merged["default_shell_action"]),
            max_command_chars=max(1, max_chars),
            home=home,
        )

    def resolve(self, path: str, cwd: str = "") -> str:
        """Resolve ``path`` to a normalised absolute path.

        ``~`` and ``~/`` expand against the *account* home; a relative path is
        taken against ``cwd`` (the call's working directory, defaulting to the
        account home). ``..`` segments are collapsed by ``normpath``, so a
        traversal cannot escape the checks below.
        """
        p = path
        if p == "~":
            p = self.home
        elif p.startswith("~/"):
            p = os.path.join(self.home, p[2:])
        if not os.path.isabs(p):
            p = os.path.join(cwd or self.home, p)
        return os.path.normpath(p)

    # ``expand`` kept for callers that only need home/normalisation (no cwd).
    def expand(self, path: str) -> str:
        return self.resolve(path, cwd=self.home)

    def is_sensitive(self, path: str, cwd: str = "") -> bool:
        resolved = self.resolve(path, cwd)
        for prefix in self.sensitive_paths:
            if resolved == prefix or resolved.startswith(prefix.rstrip("/") + "/"):
                return True
        return False

    def is_system_write(self, path: str, cwd: str = "") -> bool:
        resolved = self.resolve(path, cwd)
        for prefix in self.system_write_paths:
            if resolved == prefix or resolved.startswith(prefix.rstrip("/") + "/"):
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


# -- shell parsing -------------------------------------------------------------

# With ``punctuation_chars`` the lexer emits these as their own tokens and
# groups runs, so "&&", "||" and ">>" arrive whole.
_SEPARATORS = {";", "&", "&&", "|", "||", "(", ")", "\n"}
_REDIRECTIONS = {">", ">>", "<", "<<", ">&", "&>", "<<<"}
_WRITE_REDIRECTIONS = {">", ">>", ">&", "&>"}
# Characters that let a command hide work from static inspection. Their
# presence means the validator cannot fully account for the command, so it is
# never plain-allowed (block rules still apply). Note: "*"/"?" globs are not
# here — they do not hide the command, and read-only globs are common.
_UNVALIDATABLE_CHARS = ("$", "`", "\\")

_POWER = ("shutdown", "reboot", "poweroff", "halt")
_SHELLS = ("sh", "bash", "zsh", "dash", "ksh", "fish")
_DOWNLOADERS = ("curl", "wget", "fetch")
_PKG_MANAGERS = ("apt", "apt-get", "dnf", "yum", "pacman", "pip", "pip3",
                 "npm", "pnpm", "yarn", "brew", "snap", "gem", "cargo")


def _lex(command: str) -> list:
    """Operator-aware tokenisation. Raises ValueError on unbalanced quotes."""
    lexer = shlex.shlex(command, posix=True, punctuation_chars=True)
    lexer.whitespace_split = True
    return list(lexer)


def _segmentize(tokens: list) -> tuple:
    """Split tokens into sub-command word lists, redirections, and whether a
    pipe is present. Operators are consumed, not kept as words."""
    segments: list = []
    redirs: list = []
    current: list = []
    pipe = False
    i = 0
    while i < len(tokens):
        tok = tokens[i]
        if tok in ("|", "||"):
            pipe = True
        if tok in _SEPARATORS:
            if current:
                segments.append(current)
                current = []
            i += 1
            continue
        if tok in _REDIRECTIONS:
            target = tokens[i + 1] if i + 1 < len(tokens) else ""
            redirs.append((tok, target))
            i += 2
            continue
        current.append(tok)
        i += 1
    if current:
        segments.append(current)
    return segments, redirs, pipe


def _basename(token: str) -> str:
    return os.path.basename(token.strip("'\""))


def _looks_like_path(token: str) -> bool:
    cand = token.strip("'\"")
    return cand.startswith(("/", "~", "./", "../")) or "/" in cand


def _classify_segment(policy: Policy, tokens: list, cwd: str) -> RuleDecision:
    """Decide one sub-command (already split on operators) in isolation."""
    if not tokens:
        return RuleDecision(policy.default_shell_action, "empty sub-command")
    words = [_basename(t) for t in tokens]
    first = words[0]

    if first in _POWER:
        return RuleDecision(BLOCK, f"{first} is blocked")

    if any(w == "mkfs" or w.startswith("mkfs.") for w in words):
        return RuleDecision(BLOCK, "mkfs is blocked")

    if "dd" in words:
        for token in tokens:
            if token.strip("'\"").startswith("of=/dev/"):
                return RuleDecision(BLOCK, "dd writing to a device is blocked")

    # Any argument that resolves to a sensitive path blocks the command.
    for token in tokens[1:]:
        if _looks_like_path(token) and policy.is_sensitive(token.strip("'\""), cwd):
            return RuleDecision(BLOCK, f"command touches sensitive path {token}")

    if "rm" in words:
        idx = words.index("rm")
        args = tokens[idx + 1:]
        short_flags = "".join(a[1:] for a in args if a.startswith("-") and not a.startswith("--"))
        long_flags = [a for a in args if a.startswith("--")]
        targets = [a for a in args if not a.startswith("-")]
        recursive = "r" in short_flags or "R" in short_flags or "--recursive" in long_flags
        if recursive:
            home = os.path.normpath(policy.home)
            for raw_target in targets:
                target = policy.resolve(raw_target.strip("'\""), cwd)
                if (target in ("/", "/*") or target == home
                        or home.startswith(target.rstrip("/") + "/")):
                    return RuleDecision(BLOCK, f"recursive delete of {raw_target} is blocked")
        return RuleDecision(CONFIRM, "delete is irreversible and a reversible alternative "
                                     "(move to trash) may exist; confirmation required")

    if "mv" in words or "rmdir" in words:
        return RuleDecision(CONFIRM, "move/remove is hard to undo; confirmation required")

    if first in _PKG_MANAGERS and ("install" in words or (first in ("npm", "pnpm", "yarn") and "i" in words)):
        return RuleDecision(CONFIRM, "package installation changes the system; "
                                     "confirmation required")

    if first == "systemctl":
        sub = words[1] if len(words) > 1 else ""
        if sub in policy.readonly_systemctl:
            return RuleDecision(ALLOW, f"read-only command: systemctl {sub}")
        return RuleDecision(CONFIRM, f"systemctl {sub} may change the system; confirmation required")

    if first in policy.readonly_commands:
        return RuleDecision(ALLOW, f"read-only command: {first}")

    return RuleDecision(policy.default_shell_action,
                        "shell command is not on the read-only allowlist")


def _fully_validatable(command: str, segments: list, redirs: list, pipe: bool) -> bool:
    """Whether the validator can account for the *whole* command line.

    Only a single simple sub-command with no operators, redirections,
    pipes or substitutions is eligible for ``allow``; everything else is at
    most ``confirm``. This is what keeps the guard from failing open.
    """
    if len(segments) != 1 or redirs or pipe:
        return False
    if any(ch in command for ch in _UNVALIDATABLE_CHARS):
        return False
    return True


def _check_system_run(policy: Policy, command: str, cwd: str) -> RuleDecision:
    cmd = command or ""

    # Cap the input *before* any tokenising or matching.
    if len(cmd) > policy.max_command_chars:
        return RuleDecision(CONFIRM, "command is longer than the guard inspects; "
                                     "confirmation required")

    # User-supplied literal block list (raw-substring, by design).
    for needle in policy.extra_block_substrings:
        if needle and needle in cmd:
            return RuleDecision(BLOCK, f"command matches blocked substring {needle!r} (policy)")

    try:
        tokens = _lex(cmd)
    except ValueError:
        # Unbalanced quotes etc.: we could not parse it, so we cannot vouch
        # for it — confirm rather than allow.
        return RuleDecision(CONFIRM, "command could not be parsed (unbalanced quotes); "
                                     "confirmation required")
    if not tokens:
        return RuleDecision(policy.default_shell_action, "empty command")

    segments, redirs, pipe = _segmentize(tokens)
    all_words = [_basename(t) for seg in segments for t in seg]

    if policy.block_sudo and "sudo" in all_words:
        return RuleDecision(BLOCK, "sudo is blocked by the default policy")

    # Piping a remote download straight into a shell interpreter.
    if pipe:
        firsts = [_basename(seg[0]) for seg in segments if seg]
        if any(f in _SHELLS for f in firsts) and any(f in _DOWNLOADERS for f in firsts):
            return RuleDecision(BLOCK, "piping a remote download into a shell is blocked")

    # Redirections that write into a sensitive or system path.
    for op, target in redirs:
        if op in _WRITE_REDIRECTIONS and target:
            clean = target.strip("'\"")
            if policy.is_sensitive(clean, cwd):
                return RuleDecision(BLOCK, f"redirection writes to sensitive path {target}")
            if policy.is_system_write(clean, cwd):
                return RuleDecision(BLOCK, f"redirection writes to system path {target}")

    # Worst sub-command decision wins.
    action, reason = ALLOW, "read-only command"
    for seg in segments:
        decision = _classify_segment(policy, seg, cwd)
        if _SEVERITY[decision.action] > _SEVERITY[action]:
            action, reason = decision.action, decision.reason

    # No fail-open: only a fully accountable command line may be allowed.
    if action == ALLOW and not _fully_validatable(cmd, segments, redirs, pipe):
        return RuleDecision(CONFIRM, "command combines shell features the guard cannot "
                                     "fully validate; confirmation required")
    return RuleDecision(action, reason)


def _system_run_cwd(policy: Policy, params: dict) -> str:
    raw = params.get("cwd")
    if isinstance(raw, str) and raw.strip():
        return policy.resolve(raw, cwd=policy.home)
    return os.path.normpath(policy.home)


def evaluate(policy: Policy, command: str, params: dict) -> RuleDecision:
    """Layer 1 decision for one gadget command."""
    params = params or {}
    if command == "device.health":
        return RuleDecision(ALLOW, "device.health is always allowed")
    if command == "system.run":
        cwd = _system_run_cwd(policy, params)
        return _check_system_run(policy, str(params.get("command") or ""), cwd)
    if command in ("file.read", "file.write"):
        raw = str(params.get("path") or "")
        if not raw:
            return RuleDecision(ALLOW, "no path; the executor will reject the call")
        # file.read / file.write take an absolute path, but resolve defensively
        # against the account home so a relative path or ".." cannot dodge the
        # sensitive/system checks.
        resolved = policy.resolve(raw, cwd=policy.home)
        if policy.is_sensitive(resolved):
            return RuleDecision(BLOCK, f"{command} on sensitive path {raw} is blocked")
        if command == "file.write":
            if policy.is_system_write(resolved):
                return RuleDecision(BLOCK, f"write to system path {raw} is blocked")
            home = os.path.normpath(policy.home)
            if resolved == home or resolved.startswith(home.rstrip("/") + "/"):
                return RuleDecision(ALLOW, "single file write inside the account home")
            return RuleDecision(CONFIRM, f"write outside the account home ({raw}); "
                                         "confirmation required")
        return RuleDecision(ALLOW, "file read on a non-sensitive path")
    return RuleDecision(ALLOW, "unknown command; the executor will reject it")
