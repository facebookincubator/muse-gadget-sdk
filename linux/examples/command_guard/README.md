# Command guard (example)

An opt-in guard that checks every command Muse sends a Linux gadget before
it runs. It is a **`Service` subclass**, `GuardedService`, that overrides
one method — `run_command` — and nothing in the core SDK changes. Standard
library only.

A Muse gadget runs commands with exactly the permissions of the account it
was installed for — including sudo, if that account has it. This guard adds
a decision point in front of that: two checks, a three-valued decision, and
a tamper-evident record of every decision.

```
link.invoke ─► LinkSession ─► GuardedService.run_command ─► Guard.check ─► allow ─► Executor runs it
                                        │                      ├─ Layer 1: deterministic policy
                                        │                      └─ Layer 2: scope check (if a request
                                        │                                  is available)
                                        └─ confirm / block ──► error returned to Muse:
                                                               "guard: <action>: <reason>"
Every decision ──► hash-chained ledger (JSONL)
```

## How it plugs in (no core changes)

The `musegadget` package exposes the seam a gadget needs (see "Building a
gadget" in `linux/AGENTS.md`): subclass `service.Service` and override
`run_command(name, params, timeout_ms)`, which handles each command from
Muse and by default calls the `Executor`. `GuardedService` overrides only
that method:

```python
class GuardedService(Service):
    guard: Guard | None = None

    def run_command(self, name, params, timeout_ms=None):
        if self.guard is None:
            return super().run_command(name, params, timeout_ms)
        decision = self.guard.check(name, params or {})
        if decision.action != "allow":
            return error(f"guard: {decision.action}: {decision.reason}")
        return super().run_command(name, params, timeout_ms)
```

Because the guard lives entirely in this example and plugs in by
subclassing, an installed gadget that does not ship `linux/examples` is
completely unaffected, and the core `Executor`/`cli` are untouched.

> **Concurrency.** `run_command` runs in a worker thread and commands can
> run concurrently, so the ledger append is locked both between threads and
> between processes (see [The ledger](#the-ledger)).

## The two layers

**Layer 1 — policy (`policy.py`).** Deterministic rules that need no
context: sudo, destructive commands, credential paths, system write paths,
package installs, read-only commands. Configured by `policy.json` (JSON
rather than YAML so the guard needs no dependency beyond the standard
library).

The command line is **fully tokenised** with an operator-aware `shlex`
lexer (operators such as `|`, `&&`, `;` and redirections are real tokens),
not matched as a raw string, and every argument of every sub-command is
examined. Every path-looking argument is **resolved against the call's
working directory** (and `~` against the account home) and normalised
before it is matched, so a relative path or a `..` traversal cannot slip a
sensitive target past the rules. The command length is **capped before any
matching runs**. The default is `confirm`, and a shell command is `allow`ed
**only** when the validator can account for the whole command line — one
simple sub-command with no operators, redirections or substitutions, whose
first word is on the read-only allowlist. Anything else is at most
`confirm`; the guard never fails open to `allow`.

**Layer 2 — scope (`scope.py`).** Content shields ask "is this command
dangerous?". A scope check asks "the user asked for X; is this call more
than X?" — for example, the user named one file (`/home/pi/draft.md`) and
the agent proposes deleting its whole parent directory. This is the
question [Interlock](https://github.com/NagaYu/interlock) answers with a
trained model; `ScopeChecker` is the adapter point for it. The MVP ships a
small deterministic heuristic for the clearest destructive cases, and uses
a caller-supplied `interlock_check` callable when one is provided.

**The catch, stated plainly:** the gadget protocol does not currently pass
the user's request to the executor, so Layer 2 usually has nothing to check
against. When that happens the decision is still made (Layer 1) but the
ledger records Layer 2 as `not_evaluated` — never as a silent pass. Passing
the current request through to `run_command` would enable the scope layer
end to end.

## Decisions

| Decision | Meaning |
|---|---|
| `allow` | The command runs. |
| `confirm` | The command does **not** run (see below). The executor returns `guard: confirm: <reason>`. |
| `block` | The command does **not** run. The executor returns `guard: block: <reason>`. |

### What `confirm` means in this example

A true `confirm` would pause and ask the user to approve the command, then
run it on approval. **The gadget protocol has no such user round-trip at
the executor boundary** — `run_command` must return a result synchronously
— so this example does **not** implement a live approval step, and does not
invent an out-of-band one.

**In this example, `confirm` therefore behaves exactly like `block`: the
command does not run.** The two are kept as distinct decisions on purpose:
`confirm` marks "a human could reasonably approve this" (and the ledger
records it as `confirm`), whereas `block` marks "never". A real deployment
can turn a recurring `confirm` into an `allow` by adding the command to the
policy allowlist, or build a genuine approval channel on top of the
`Decision` the guard returns. This choice — document the current
equivalence rather than fake an approval step — is also noted in
`service.py`.

Over-blocking is treated as the worst failure: a guard that interrupts
legitimate work gets switched off, and a switched-off guard detects
nothing. That is why unknown-but-harmless-looking calls are `confirm`, not
`block`, and why `block` is reserved for the list below.

## Default policy

| Case | Decision |
|---|---|
| `device.health` | allow |
| Read-only shell (`ls`, `df`, `du`, `cat` on non-sensitive paths, `head`, `systemctl status`, …), a single simple command only | allow |
| `file.read` on a non-sensitive path; a single `file.write` inside the account home | allow |
| `sudo …` in `system.run` | block |
| `rm -rf /`, `rm -rf ~` or the home directory itself, `mkfs`, `dd of=/dev/…`, `shutdown` / `reboot` / `poweroff` / `halt` | block |
| `curl … \| sh`, `wget … \| bash` | block |
| Read or write of `/var/lib/musegadget/` (the SDK token), `~/.ssh/`, `/etc/shadow` (after resolving the path) | block |
| `file.write` under `/etc/`, `/opt/musegadget/`, `/usr/`; a redirection (`>`) into any of those | block |
| Package installs without sudo (`apt install`, `pip install`, …) | confirm |
| Deletes and moves (irreversible, where a reversible alternative such as trash may exist) | confirm |
| A read-only command combined with operators, redirections or substitutions the guard cannot fully validate | confirm |
| A destructive target that is not, or strictly contains, the path named in the request (Layer 2) | confirm / block |
| Any other shell command not on the read-only allowlist | confirm (configurable: `default_shell_action`) |

Customise by editing a copy of `policy.json` and pointing
`MUSEGADGET_GUARD_POLICY` at it: sensitive paths, system write paths, extra
blocked substrings, the default shell action and `max_command_chars` are
all in there.

## Enabling it on a gadget

Run the service through this example instead of `musegadget run`. It takes
the same `--run-as` account and reads the same SDK token:

```sh
sudo PYTHONPATH=linux/examples \
  MUSEGADGET_GUARD_POLICY=/path/policy.json \
  MUSEGADGET_GUARD_LEDGER=/var/lib/musegadget/guard-ledger.jsonl \
  python3 -m command_guard.service run --run-as pi
```

Both environment paths are optional (defaults: the `policy.json` next to the
package, and `/var/lib/musegadget/guard-ledger.jsonl`). To use the guard
from your own code:

```python
from command_guard.service import GuardedService
from command_guard.guard import build_guard

service = GuardedService(identity=..., executor=..., guard=build_guard(home))
await service.run()
```

`guard=None` makes `GuardedService` behave exactly like the base `Service`.

## The ledger

Every decision is appended to a JSONL ledger (default
`/var/lib/musegadget/guard-ledger.jsonl`). Each row records the command, a
*summary* of its parameters, both layers' outcomes, the final decision and
the reason — plus a hash chain: `hash = sha256(prev_hash + canonical row)`,
so editing any earlier row breaks every later hash.

Reading the previous row's hash and appending the new row is done as one
critical section under both a per-process `threading.Lock` and an
inter-process `fcntl.flock`, because commands are handled concurrently in
worker threads and more than one process may share the ledger. Without that,
two appenders could read the same last hash and fork the chain.

Parameters are never stored raw: a `file.write` payload is recorded as a
byte length and a SHA-256 only, and strings that look like tokens or secrets
are masked.

Verify a ledger:

```sh
PYTHONPATH=linux/examples python3 -m command_guard.ledger verify /var/lib/musegadget/guard-ledger.jsonl
# PASS: ok: 12 row(s), chain intact   /   FAIL: line 4: hash mismatch (row was modified)
```

## Always answers, fails closed

`Guard.check` always returns a `Decision` and never raises: if policy
evaluation, the scope check or the ledger append fails for any reason, the
error is turned into a `block` ("fail closed"), and `GuardedService`
likewise maps any unexpected error to a `guard: block: …` result. So Muse
always receives an answer, never an exception. Failing closed on an internal
error — including a ledger write that fails, since the ledger is the guard's
evidence — is deliberate: a guard that cannot record a decision should not
let the command run. The trade-off is that a persistently failing ledger
(for example a full disk) will refuse commands rather than run them
unrecorded.

## Demo and tests

No device, Bluetooth or SDK token needed:

```sh
PYTHONPATH=linux/examples python3 -m command_guard.demo
# six scenarios: disk check allowed, sudo install blocked, a draft-file
# delete that expands to the whole home, a normal write allowed, an SDK
# token read blocked, and the ledger verifying — then failing after one
# row is tampered with.
```

The tests live in `linux/tests/test_command_guard.py` and are collected by
the main suite:

```sh
cd linux && uv run --with pytest --with . pytest
```

## Limitations

- **This guard does not make an agent safe.** It is one layer, meant to sit
  alongside least-privilege accounts (install with `--run-as` an account
  without sudo and most of the block list becomes moot), sandboxing and
  human confirmation. It will miss things.
- Layer 1 reads shell commands statically; a command with substitutions or
  heavy obfuscation is `confirm`ed rather than trusted, but a determined
  attacker can still arrange side effects the static rules do not model. The
  ledger is evidence, not prevention.
- Layer 2 needs the user's request, which the current protocol does not
  deliver to the executor (see above). Until it does, scope checking is
  available to direct callers of `Guard.check(..., request=...)` only.
- `confirm` does not currently run on approval (see above).
- Sequenced attacks across calls (for example, many individually allowed
  `file.write` chunks to many files) are not tracked in the MVP.

The design combines two ideas published separately: policy decisions with a
tamper-evident ledger (Warden), and request/action scope checking
(Interlock). Both are re-implemented here, self-contained, so the example
carries no extra dependency.
