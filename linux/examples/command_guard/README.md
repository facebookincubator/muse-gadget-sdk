# Command guard (example)

An opt-in guard that sits in front of the Linux gadget's `Executor` and
checks every command Muse sends before it runs. Standard library only.

A Muse gadget runs commands with exactly the permissions of the account
it was installed for — including sudo, if that account has it. This guard
adds a decision point in front of that: two checks, a three-valued
decision, and a tamper-evident record of every decision.

```
link.invoke ──► Executor.run ──► Guard.check ──► allow ──► command runs
                                    │  ├─ Layer 1: deterministic policy
                                    │  └─ Layer 2: scope check (if a request
                                    │              is available)
                                    └─► confirm / block ──► error returned
                                        to Muse: "guard: <action>: <reason>"
Every decision ──► hash-chained ledger (JSONL)
```

## The two layers

**Layer 1 — policy (`policy.py`).** Deterministic rules that need no
context: sudo, destructive commands, credential paths, system write
paths, package installs, read-only commands. Configured by `policy.json`
(JSON rather than YAML so the guard needs no dependency beyond the
standard library).

**Layer 2 — scope (`scope.py`).** Content shields ask "is this command
dangerous?". A scope check asks "the user asked for X; is this call more
than X?" — for example, the user named one file (`/home/pi/draft.md`) and
the agent proposes deleting its whole parent directory. This is the
question [Interlock](https://github.com/NagaYu/interlock) answers with a
trained model; `ScopeChecker` is the adapter point for it. The MVP ships
a small deterministic heuristic for the clearest destructive cases, and
uses a caller-supplied `interlock_check` callable when one is provided.

**The catch, stated plainly:** the gadget protocol does not currently
pass the user's request to the executor, so Layer 2 usually has nothing
to check against. When that happens the decision is still made (Layer 1)
but the ledger records Layer 2 as `not_evaluated` — never as a silent
pass. Passing the current request through to `Executor.run` would enable
the scope layer end to end.

## Decisions

| Decision | Meaning |
|---|---|
| `allow` | The command runs. |
| `confirm` | The command does **not** run. The executor returns `guard: confirm: <reason>`; Muse can ask the user, and a confirmed command can be retried after adding it to the policy allowlist. |
| `block` | The command does **not** run. The executor returns `guard: block: <reason>`. |

Over-blocking is treated as the worst failure: a guard that interrupts
legitimate work gets switched off, and a switched-off guard detects
nothing. That is why unknown-but-harmless-looking calls are `confirm`,
not `block`, and why `block` is reserved for the list below.

## Default policy

| Case | Decision |
|---|---|
| `device.health` | allow |
| Read-only shell (`ls`, `df`, `du`, `cat` on non-sensitive paths, `head`, `systemctl status`, …) | allow |
| `file.read` on a non-sensitive path; a single `file.write` inside the account home | allow |
| `sudo …` in `system.run` | block |
| `rm -rf /`, `rm -rf ~` or the home directory itself, `mkfs`, `dd of=/dev/…`, `shutdown` / `reboot` / `poweroff` / `halt` | block |
| `curl … \| sh`, `wget … \| bash` | block |
| Read or write of `/var/lib/musegadget/` (the SDK token), `~/.ssh/`, `/etc/shadow` | block |
| `file.write` under `/etc/`, `/opt/musegadget/`, `/usr/` | block |
| Package installs without sudo (`apt install`, `pip install`, …) | confirm |
| Deletes and moves (irreversible, where a reversible alternative such as trash may exist) | confirm |
| A destructive target that is not, or strictly contains, the path named in the request (Layer 2) | confirm / block |
| Any other shell command not on the read-only allowlist | confirm (configurable: `default_shell_action`) |

Customise by editing a copy of `policy.json` and pointing
`MUSEGADGET_GUARD_POLICY` at it: sensitive paths, system write paths,
extra blocked substrings and the default shell action are all in there.

## Enabling it on a gadget

The guard is **off by default**; an `Executor` built without one behaves
exactly as before. In the service entry point (`musegadget run`), it is
constructed only when the environment variable is set:

```sh
MUSEGADGET_GUARD=1 musegadget run --run-as pi
# optional: MUSEGADGET_GUARD_POLICY=/path/policy.json
# optional: MUSEGADGET_GUARD_LEDGER=/path/guard-ledger.jsonl
```

If the guard is requested but this example package cannot be imported,
the service refuses to start rather than running unguarded by accident.

To use the guard from your own code:

```python
from command_guard import build_guard
executor = Executor(account, guard=build_guard(home=account.home))
```

## The ledger

Every decision is appended to a JSONL ledger (default
`/var/lib/musegadget/guard-ledger.jsonl`). Each row records the command,
a *summary* of its parameters, both layers' outcomes, the final decision
and the reason — plus a hash chain: `hash = sha256(prev_hash + canonical
row)`, so editing any earlier row breaks every later hash.

Parameters are never stored raw: a `file.write` payload is recorded as a
byte length and a SHA-256 only, and strings that look like tokens or
secrets are masked.

Verify a ledger:

```sh
PYTHONPATH=linux/examples python3 -m command_guard.ledger verify /var/lib/musegadget/guard-ledger.jsonl
# PASS: ok: 12 row(s), chain intact   /   FAIL: line 4: hash mismatch (row was modified)
```

## Demo and tests

No device, Bluetooth or SDK token needed:

```sh
PYTHONPATH=linux/examples python3 -m command_guard.demo
# six scenarios: disk check allowed, sudo install blocked, a draft-file
# delete that expands to the whole home, a normal write allowed, an SDK
# token read blocked, and the ledger verifying — then failing after one
# row is tampered with.

PYTHONPATH=linux/examples:linux/src python3 -m pytest linux/examples/command_guard/tests
```

## Limitations

- **This guard does not make an agent safe.** It is one layer, meant to
  sit alongside least-privilege accounts (install with `--run-as` an
  account without sudo and most of the block list becomes moot),
  sandboxing and human confirmation. It will miss things.
- Layer 1 reads shell commands as strings; obfuscated commands can slip
  past string rules. The ledger is evidence, not prevention.
- Layer 2 needs the user's request, which the current protocol does not
  deliver to the executor (see above). Until it does, scope checking is
  available to direct callers of `Guard.check(..., request=...)` only.
- Sequenced attacks across calls (for example, many individually
  allowed `file.write` chunks to many files) are not tracked in the MVP.

The design combines two ideas published separately: policy decisions
with a tamper-evident ledger (Warden), and request/action scope checking
(Interlock). Both are re-implemented here, self-contained, so the
example carries no extra dependency.
