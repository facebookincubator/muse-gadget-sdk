# Powering a board from a Nordic PPK2

Read this before anyone, an agent or a person, opens a PPK2, enables its
source output, or asks someone to unplug a board. It covers host setup, the
safety gates, the single continuous source owner, the control handshake, and
how to tell a usable capture from a bad one. The tools in `tools/power/` are
board-agnostic; the SenseCAP Watcher is the worked example
([Watcher procedure](sensecap-watcher.md)), and the
[porting recipe](README.md) covers other boards and devices. Commands below
run from `esp32/`.

## Authorization comes first

- **No hardware without the board owner's explicit go-ahead for that action**:
  flashing, opening serial ports, enabling PPK2 output, or asking someone to
  unplug or power-cycle a board. A document, a previous session, or an earlier
  approval for something else is not authorization. Host tests, `--help` and
  the synthetic benchmark need no hardware; prefer them, and emulation, until
  the owner says otherwise.
- If serial access is denied, get access the owner authorizes. Never work
  around a sandbox or an access policy.
- Do not kill other PPK2 owners, acquisitions or keepers, or the Nordic
  desktop tools. Report the conflict and arrange ownership with whoever runs
  them.

## The fixture is the operator's assertion

Software cannot see the wiring. Before source output is enabled, get the
owner's actual confirmation, for this fixture and this session, of each of
these, and write down the basis of each answer:

- The battery is physically disconnected or isolated. Never put the PPK2
  source in parallel with a cell.
- Source polarity, grounds, connector pinout, current limit and peak demand
  are verified. The PPK2 source is good to roughly 1 A; it is unsuitable for a
  load approaching or exceeding that, and its ceiling is not an overload test
  target. Investigate brownouts or overrange instead of averaging them away.
- USB/source overlap is suitable for the **actual** power path: the charger,
  the battery-detect and thermistor state, and any onboard blocking. Where the
  path needs it, use verified external blocking (a diode or ideal diode) or
  charger isolation. A charger connected from VBUS to VBAT charges into the
  PPK2 while USB is present, so battery removal and stock firmware do not
  prove overlap is safe, and neither does a promise to unplug USB later.
- The source setpoint is chosen explicitly in **integer millivolts, 800–5000**,
  inside the battery input's and PMU's range. `3872` is a Li-ion example,
  never a default.
- DUT USB is still connected during setup, and stays connected until the
  owner process reports READY. After READY, USB comes out only through the
  agreed procedure.

Record whether each basis is an operator assertion or measured or
manufacturer-backed evidence. Never infer verified wiring from a previous
session, a photograph, or the acknowledgment flags, and never claim
protection that is not fitted. If the wiring is unknown, stop before output
ON. The measurements characterize USB-free states, not the charger or the
handoff itself.

A blocking element changes the DUT voltage: a 3872 mV source ahead of a diode
is not 3872 mV at VBAT. Measure the DUT voltage under load if you want DUT
energy. `--dut-voltage-mv` records that measured constant for separately
named DUT-energy estimates; it does not establish wiring safety or correct
for conversion efficiency. Never compensate a drop by guessing a higher
setpoint.

## One continuous owner

One process acquires one exclusive PPK2 connection **before DUT USB
removal**, powers the source, drains measurement samples continuously through
every state, and keeps the same connection until the owner confirms DUT USB
is reconnected.

- **Never stop, close, reopen, retune or power-cycle the PPK2 while it is the
  board's only supply**, and never start acquisition again between states.
  Never kill a keeper process to launch a separate measurement process.
- Never unplug the PPK2 or terminate its process while it is the sole supply.
  The safe order is always **source on before USB comes out, USB back in
  before the source goes off**.
- `end`, a timed state finishing, Ctrl-C, SIGTERM, collection faults and an
  exhausted output budget **do not authorize source shutdown**. They leave the
  owner alive. Reconnect DUT USB through the verified wiring, then confirm
  `finish`.
- An unexpected physical disconnect, host crash, forced termination or power
  loss cannot be repaired in software. Invalidate the capture and restore
  power safely with the board owner; do not assume continuity. Where
  uninterrupted power is a safety requirement, use a hardware fixture that
  provides it.
- Do not restart source mode, auto-reset hardware, or erase device storage as
  generic recovery.

## Install and identify

```sh
python3 -m venv tools/power/.venv
tools/power/.venv/bin/python -m pip install -r tools/power/requirements.txt
tools/power/.venv/bin/python tools/power/ppk2_profile.py --help
```

Dependencies are pinned (`ppk2-api==0.9.2`, `pyserial==3.5`); reuse an
existing environment only when its versions match. No vendor code is copied
into this repository.

With permission to inspect USB descriptors:

```sh
tools/power/.venv/bin/python tools/power/ppk2_profile.py discover
```

Discovery lists descriptors only: it opens no serial port and changes no
source. A PPK2 may expose two CDC interfaces. Select its control/measurement
interface explicitly with `--port "$PPK_PORT"`, never by sorted name, suffix
or first match. Only that interface is opened; the tool never toggles every
attached PPK2. A bounded stale-stream drain
and valid calibration metadata are required before source mode, voltage or
ON, so a wrong interface fails safe. The tool uses pyserial exclusivity plus
a per-device advisory host lock; unknown USB identities and unrelated tools
cannot get absolute cross-interface exclusion, so verify ownership yourself.

## Start the owner once

Only after every fixture condition has actually been confirmed, start a
long-lived owner in a persistent terminal or a managed background task:

```sh
tools/power/.venv/bin/python tools/power/ppk2_profile.py hold \
  --port "$PPK_PORT" --voltage-mv 3872 --receiver-max-bytes 16777216 \
  --ack-source-wiring --ack-battery-isolated --ack-no-charging-backfeed \
  --ack-usb-connected-before-start \
  --output "$RUN/ppk.jsonl" --control-dir "$RUN/control"
```

`record` takes the same arguments as `hold`, and either can keep a
per-sample file with `--raw "$RUN/samples.bin"`. Neither finishes on a
timer.

- All output paths and the control directory must be new; the parent
  directory must exist.
- In a terminal, missing acknowledgments are prompted before the PPK2 is
  opened. A noninteractive run must pass all four. They record a verified
  setup; they do not make unknown wiring safe.
- READY is a JSON line on stderr, issued **only after the background reader
  decodes a finite current sample** (two-second default deadline). It carries
  the finite-sample count and first/last finite-data host offsets.
  `last_data_host_offset_s` means decoded finite data; raw byte activity is
  `last_receive_host_offset_s`.
- Empty, invalid or failed transport during this gate stops, switches OFF and
  closes before READY, while DUT USB is still attached. Failed emergency OFF
  commands are kept and retried during cleanup; a startup cleanup error
  warns **SOURCE STATE UNKNOWN; OFF NOT VERIFIED**.
- If the reader cannot join, no READY is issued, and the source owner, signal
  handlers and FIFO are kept. Keep USB connected, inspect `status`, and
  confirm `finish` only when safe. Never kill a stuck reader to make the error
  go away.
- READY and a fault-free `status` prove software ownership, not correct
  electrical wiring. Recheck readiness, data recency and faults before arming
  a sweep.

## Preflight before USB comes out

The PPK2 streams 400 kB/s, and a busy host (parallel compilers, endpoint
agents) can stall the consumer. Pause other heavy work on the capture host,
start the owner with `--receiver-max-bytes 16777216`, and label a short idle
window while USB is still connected:

```sh
C="tools/power/.venv/bin/python tools/power/ppk2_profile.py control --control-dir $RUN/control"
$C begin --label preflight-idle --duration-s 10
$C status
```

Remove USB only when that window and a fresh `status` show coverage within
99–101%, zero loss counters (decoded missing, invalid, late, receiver
overflow/read errors), and receive backlog plus consumer delay under 0.5 s.
A failed preflight stays in the capture as evidence but is not test data and
does not excuse loss later. While USB is out, check health only through the
control FIFO.

## Label states through the handshake

Interactive commands, typed into the owner's terminal after the agreed USB
removal:

```text
begin idle 10
status
begin display 5
status
finish USB_RECONNECTED
```

A timed `begin` ends only that label; source and drain stay active. Use `end`
before another label if the current one has no duration. Change the device
state, wait well past its settling time, then `begin`; `end` before changing
it again.

A runner or agent sends blocking commands to the same process through the
control FIFO:

```sh
$C begin --label power-sweep
$C end
$C status
# ONLY after USB reconnection has actually been confirmed:
$C finish --usb-reconnected
```

- Control clients never open serial. Each command waits for its
  acknowledgment with an OS event wait, not a polling loop.
- **A timeout means the outcome is unknown.** Ask `status` before retrying
  any state-changing command, and never re-arm or re-send blindly. Nothing
  state-changing is retried silently.
- The control directory is private (0700) and carries a lifetime token, so a
  stale session is never reused blindly; no stale directory or competing
  process is reclaimed automatically.
- Oversized status replies set `control_details_truncated`, omit
  `last_record` and shorten free-form details, while keeping owner, counters
  and transport state.
- Never send or type `finish` until DUT USB reconnection has actually been
  confirmed.

Each label ends with an `end` record carrying `mean_uA`, `min_uA`, `max_uA`,
`p50_uA`…`p99_uA`, `sample_count`, `coverage_ratio_estimate`, sampled charge,
source-setpoint energy and loss counters.

### Python API

With `esp32/` on `sys.path`:

```python
from tools.power.ppk2_control import send_command
status = send_command(control_dir, {"op": "status"}, timeout_s=10)
send_command(control_dir, {"op": "begin", "label": "idle", "duration_s": 10})
send_command(control_dir, {"op": "end"})
# Only after confirmed USB reconnection:
send_command(control_dir, {"op": "finish", "usb_reconnected": True})
```

A standalone runner may import `PPK2Session` and `SafetyAcknowledgment` from
`tools.power`: `start()`, `begin(label, duration_s=None)`, `end()`,
`status()`, `interrupt()` and `finish(usb_reconnected=True)`. Entering the
context starts ownership, but leaving it **never closes a live source**: a
normal exit without a confirmed finish raises `USBReconnectRequired`, and an
exception keeps the owner. Keep the session reference, arrange USB
reconnection, then finish. Normal use needs the default background drain;
`background=False` does not guarantee transport READY and is only for fakes
or runners that call `pump()` continuously. Never treat its return alone as
permission to remove USB.

## Raw receiver

The same exclusive backend starts one fast raw thread at initial acquisition.
It does bounded serial reads, arrival stamps and queue counters, never
decoding, statistics or file writes. Consumer batches are at most 64 KiB.

- The queue defaults to 1 MiB (`--receiver-max-bytes`, 1024–16777216). Its
  entry cap is `max(4096, ceil(capacity_bytes / 1024))`, bounded at 16384:
  4096 entries for 1 MiB, 16384 for 16 MiB, so roughly 1 KiB reads can use the
  whole byte budget. Tiny reads can still hit the entry cap first; explicit
  entry caps stay within 1–16384.
- Overflow or a read error latches an **INVALID transport fault**, invalidates
  queued continuation and counts discarded bytes while the raw drain and the
  source continue. There is no silent drop-and-restart. Stop/join happens only
  at confirmed finish or pre-READY startup cleanup; a stuck raw reader keeps
  serial and control ownership.
- The standalone CLI sets a 0.0005 s GIL switch interval
  (`--gil-switch-interval-s`, 0.0001–0.005) while active and restores it after
  shutdown. Library callers never have their GIL policy changed. This is
  mitigation, not real-time proof.
- Producer arrival is the host's queue-commit time, not a sample timestamp.
  `status.receiver` keeps peak queue, OS and combined backlog and dequeue
  delay even when empty; `max_consumer_delay_s` includes processing delay.
  Windows use producer arrivals, never delayed consumer time.
- An atomic `begin` host/producer prefix keeps queued pre-begin data out of
  label statistics while keeping it in global and raw counts.
  `window_sample_index_start` estimates the first eligible valid ordinal
  (exact only with no pending or new invalid samples); legacy
  `first_valid_received_index` is the processed count. Every window's index
  span equals its valid sample count.
- `begin` freezes `decoded_missing_frame_count_at_begin`,
  `total_invalid_samples_at_begin` and `late_unassigned_sample_count_at_begin`.
  The late count is lifetime-global in status, windows and `end`; per-label
  deltas are `label_late_unassigned_sample_count`. Reject new invalid,
  missing or late counts and excessive label or global delay; pre-begin
  exclusions are not late errors.

## Timing and statistics

- PPK2 API current samples are **µA** at a nominal **100,000 samples/s**. The
  adapter keeps incomplete four-byte frames; it never invents partial samples.
- Aggregate windows default to 0.1 host seconds (`--window-s`, 0.01–1.0).
  Windows and state commands have receive-side boundaries, not hardware
  sample timestamps.
- `sample_index_start/end`, `time_s` and `nominal_sample_end_time_s` are a
  **valid received ordinal / Fs** axis; unknown sample loss compresses it.
  `host_start/end_offset_s` are separate monotonic receive-side offsets, and
  `host_monotonic_origin_s` is comparable only within one host clock epoch.
- Count, mean, min/max and sampled integrals are exact over received finite
  values. Quantiles are exact only up to the bounded reservoir (4096 by
  default); larger sets report uniform-reservoir estimates. Window
  percentiles cannot be recombined into state percentiles: use the optional
  raw file and a bounded offline method for exact state quantiles, or omit
  them.
- Coverage is valid samples / (host seconds × 100000), an **estimate**, not
  proof of where loss happened. Receive buffering can exceed 100%; values are
  not clamped. Empty windows report zero coverage and null statistics. An
  empty capture is not zero current.
- `sampled_charge_uC = sum(current_uA) / 100000`; mAh divides µC by 3,600,000.
  `sampled_energy_uJ = charge_uC × source_mV / 1000`. Missing intervals are
  never filled and no extrapolated total energy is asserted. DUT energy is
  named separately when a measured DUT voltage was supplied.
- Raw records are little-endian `<Qf`: valid received ordinal and float32 µA,
  the upstream calibrated/filtered current rather than ADC words. The JSONL
  documents the schema and voltage basis. Default caps are 16 MiB aggregate
  and 64 MiB raw, at most 512 MiB each. Raw exhaustion sets `raw_truncated`;
  aggregate or write faults pause recording, keep ownership, and invalidate
  completeness claims.

## Accepting a capture

Reject a timing-sensitive capture with a fault, truncated required raw data,
unknown interruptions, excessive `max_observed_backlog_bytes`, or
sample-versus-host drift large enough to cross a state guard. Exclude
boundary windows conservatively. Require included-state coverage of at least
99%, but do not treat that alone as proof that no loss was misplaced. Record
the alignment, guard and drift assumptions with the results.

`power_sweep.py analyze --pre-fault-prefix` is a narrow, offline exception for
whole rows recorded before a final raw receiver fault; see
[Optional pre-fault prefix](sensecap-watcher.md#optional-pre-fault-prefix-offline-only).
It is not capture recovery and never permission to restart or shut down the
source.

Scrub local USB identifiers, serial numbers, boot/run IDs, host paths,
network names and credentials before publishing anything. Keep raw captures
local unless publishing them is explicitly appropriate.

## Host-only checks

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_ppk2*.py'
tools/power/.venv/bin/python tools/power/bench_ppk2.py --seconds 5
```

The benchmark uses the real pinned decoder, fake serial memory, both bounded
reservoirs and temporary JSON/raw output. It never enumerates or opens
hardware. Require throughput headroom above 100k samples/s; this does not
validate wiring, USB transport reliability or device behavior.
