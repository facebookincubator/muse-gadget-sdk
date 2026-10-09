---
name: ppk2-power-profile
description: Safely profile SenseCAP Watcher source current with one continuous PPK2 owner, bounded host capture, and a blocking control handshake. Use for PPK2 host setup, profiling, and capture validation; never for firmware flashing or implicit hardware authorization.
---

# PPK2 power profiling for SenseCAP Watcher

## Safety is a prerequisite, not a flag

**Do not access hardware without the owner's explicit authorization.** Host tests,
CLI help, and the synthetic benchmark require no hardware. Never infer wiring
verification from a previous session, a photograph, or acknowledgment switches.
Battery removal and a promise to unplug USB later do not characterize the
charger power path. Do not infer generic overlap safety or pretend an external
isolator exists. Record the actual setup and how overlap suitability was
confirmed; the measurements below characterize USB-free states, not the
charger or transition itself.

Before source output is enabled, obtain actual owner confirmation of:

- The battery is physically disconnected/isolated, and source polarity, grounds,
  connector pinout, current limit, and peak demand are verified.
- The USB/source overlap is suitable for the **actual** fixture, considering
  the charger, thermistor/battery-detection state and onboard blocking. Use
  verified external blocking or charger isolation when the power path needs
  it. Stock Watcher firmware and battery removal alone do not prove this:
  its charger is connected from VBUS to VBAT. Record whether the basis is an
  operator assertion or measured/manufacturer-supported evidence; do not
  fabricate installed protection.
- The source setpoint is explicitly chosen in **integer millivolts**, 800–5000.
  `3872` is an example, never an automatic default. PPK2 is unsuitable for a load
  approaching/exceeding its approximately 1 A peak capability.
- DUT USB is still connected during initial setup. Keep it connected until the
  owner reports READY. After READY, remove USB only through the agreed procedure.

Blocking elements change the DUT voltage: a 3872 mV source before a diode is not
3872 mV at VBAT. Measure DUT voltage independently. `--dut-voltage-mv` records
that measured constant for separately named DUT-energy estimates; it does not
establish wiring safety or automatically correct conversion efficiency.

## The non-negotiable owner lifecycle

One process acquires one exclusive PPK2 connection **before DUT USB removal**,
powers the source, continuously drains measurement samples through every state,
and retains the same connection until the owner confirms DUT USB reconnection.

**Never kill a keeper then launch a separate measurement process. Never close,
reopen, stop/start acquisition, or power-cycle between states.** Do not kill other
owners or Nordic GUI processes. Report conflicts and arrange ownership instead.
The tool opens only the explicitly selected PPK2 interface; it never toggles all
attached devices.

`end`, timed-state completion, Ctrl-C, SIGTERM, collection faults, and output
budget exhaustion **do not authorize source shutdown**. They leave the owner
alive. Reconnect DUT USB through the already verified protected wiring, then
confirm `finish`. Unexpected physical disconnect, host crash, forced termination,
or power loss cannot be repaired by software: invalidate the capture and restore
power safely with the owner; do not assume continuity.

## Install and identify

From the repository root:

```sh
python3 -m venv /tmp/ppk-profile-venv
/tmp/ppk-profile-venv/bin/python -m pip install -r esp32/tools/power/requirements.txt
/tmp/ppk-profile-venv/bin/python esp32/tools/power/ppk2_profile.py --help
```

Dependencies are pinned to `ppk2-api==0.9.2`, `pyserial==3.5`. Reuse an existing
venv only when its versions match. No vendor code is copied into this repository.

With permission to inspect USB descriptors:

```sh
/tmp/ppk-profile-venv/bin/python esp32/tools/power/ppk2_profile.py discover
```

Discovery lists descriptors only: no serial opens or source changes. PPK2 may
expose two CDC interfaces. Select its control/measurement interface explicitly
with `--port "$PPK_PORT"`; do not select by sorted name, suffix, or first match.
Only that interface is opened. Bounded stale-stream draining and valid calibration
metadata are required before source mode/voltage/ON; a wrong interface fails safe.
The tool uses pyserial exclusivity plus a per-device advisory host lock. Unknown
USB identities and unrelated tools cannot provide absolute cross-interface
exclusion; verify ownership yourself. Do not bypass the runtime's access policy.

## Start once, control many states

Only after all safety conditions have actually been verified, start a long-lived
owner in a terminal or the existing authorized host execution environment:

```sh
/tmp/ppk-profile-venv/bin/python esp32/tools/power/ppk2_profile.py record \
  --port "$PPK_PORT" --voltage-mv 3872 \
  --ack-source-wiring --ack-battery-isolated --ack-no-charging-backfeed \
  --ack-usb-connected-before-start \
  --output "$CAPTURE_DIR/session.jsonl" \
  --raw "$CAPTURE_DIR/samples.bin" \
  --control-dir "$CAPTURE_DIR/control"
```

All output paths and the control directory must be new. The parent capture
directory must already exist. `hold` uses the same owner/drain and supports the
same recording controls; neither command finishes on a timer. In a terminal,
missing acknowledgments are prompted before opening PPK2. Noninteractive runs
must supply all four verified acknowledgments. READY is a JSON line on stderr,
**only after the background reader successfully decodes a finite current sample**
(default startup deadline: two seconds). READY includes finite-sample count and
first/last finite-data host offsets. Empty, invalid, or failed transport during
this gate stops/OFF/closes before READY, while DUT USB is still attached. Failed
emergency OFF commands are preserved and retried during cleanup; startup cleanup
errors explicitly warn **SOURCE STATE UNKNOWN; OFF NOT VERIFIED**. If the reader
cannot join, no READY is issued and the source owner, signal handlers, and usable
FIFO are retained. Keep USB connected, inspect status, and only confirm finish
when safe; a still-stuck reader must not be killed to make the error disappear.
`last_data_host_offset_s` means finite decoded data; raw byte activity is separate
as `last_receive_host_offset_s`. Recheck readiness, recency, and faults before arm.

Interactive commands, after the agreed USB removal:

```text
begin idle 10
status
begin display 5
status
finish USB_RECONNECTED
```

A timed `begin` ends only that label; the source and drain remain active. Use
`end` before another label if the current one has no duration. Never type finish
until USB reconnection has actually been confirmed.

For an agent/runner, send blocking host FIFO commands to the same process:

```sh
/tmp/ppk-profile-venv/bin/python esp32/tools/power/ppk2_profile.py control \
  --control-dir "$CAPTURE_DIR/control" begin --label watcher-sweep
/tmp/ppk-profile-venv/bin/python esp32/tools/power/ppk2_profile.py control \
  --control-dir "$CAPTURE_DIR/control" end
/tmp/ppk-profile-venv/bin/python esp32/tools/power/ppk2_profile.py control \
  --control-dir "$CAPTURE_DIR/control" status
# ONLY after actual protected USB reconnection:
/tmp/ppk-profile-venv/bin/python esp32/tools/power/ppk2_profile.py control \
  --control-dir "$CAPTURE_DIR/control" finish --usb-reconnected
```

Control processes never open serial. Each command waits for an acknowledgment
using an OS event wait, not polling loops. A timeout means command outcome is
**unknown**; ask `status` before retrying a state-changing command. A private
0700 directory and lifetime token prevent stale sessions being reused blindly.
No stale directory or competing process is automatically reclaimed. Oversized
status replies explicitly set `control_details_truncated`, omit `last_record`,
and shorten free-form details while retaining owner, counters, and transport
state; no state-changing command is silently retried.

## Reusable Python API

With `esp32/` on `sys.path`:

```python
from tools.power.ppk2_control import send_command
status = send_command(control_dir, {"op": "status"}, timeout_s=10)
send_command(control_dir, {"op": "begin", "label": "idle", "duration_s": 10})
send_command(control_dir, {"op": "end"})
# Actual owner confirmation must precede this call:
send_command(control_dir, {"op": "finish", "usb_reconnected": True})
```

A standalone runner may import `PPK2Session` and `SafetyAcknowledgment` from
`tools.power`. Its methods are `start()`, `begin(label, duration_s=None)`, `end()`,
`status()`, `interrupt()`, and `finish(usb_reconnected=True)`. Context entry starts
ownership, but context exit **never implicitly closes a live source**. On normal
exit without confirmed finish it raises `USBReconnectRequired`; after an
exception it retains the owner. Keep the session reference, arrange protected
USB reconnection, then finish. Default background draining is required for normal
use; cooperative `background=False` does not guarantee transport READY and is
for fake tests or runners that continuously call `pump()`. Never use its return
alone as permission to remove USB.

## Independent raw receiver

The same exclusive backend starts one fast raw thread only at initial acquisition.
It performs bounded serial reads, arrival stamps, and queue counters—never decode,
statistics, or file writes. Consumer batches are at most 64 KiB. The queue defaults
to 1 MiB (`--receiver-max-bytes`, integer 1024–16777216) and at most 4096 entries.
Overflow/read errors latch an **INVALID transport fault**, invalidate queued
continuation, and count discarded subsequent bytes while continuing raw drain.
There is no silent-drop/restart policy. Stop/join occurs only at confirmed finish
or pre-READY startup cleanup; a stuck raw reader retains serial/control ownership.

The standalone CLI uses a 0.0005 s GIL quantum (`--gil-switch-interval-s`,
0.0001–0.005) while active and restores it after shutdown. Imports/library callers
never change their process's GIL policy. This is mitigation, not real-time proof.

Producer arrival means host queue-commit observation, not a sample timestamp.
`status.receiver` retains peak queue/OS/combined backlog and dequeue delay even
when empty; `max_consumer_delay_s` includes processing delay. Windows use producer
arrivals, never delayed consumer time. An atomic `begin` host/producer prefix
excludes queued pre-begin data from label stats, retaining it in global/raw counts.
`window_sample_index_start` estimates the first eligible valid ordinal (exact
only with no pending/new invalids); legacy `first_valid_received_index` is the
processed count. Every window index span equals its actual valid sample count.
BEGIN freezes `decoded_missing_frame_count_at_begin`,
`total_invalid_samples_at_begin`, and `late_unassigned_sample_count_at_begin`.
The latter is lifetime-global in status/windows/end; label deltas are named
`label_late_unassigned_sample_count`. Reject new invalid/missing/late counts and
excessive label/global delay; pre-begin exclusions are not late errors.

## Timing, statistics, and capture acceptance

- PPK2 API current samples are **uA**, nominal rate **100,000 samples/s**. The
  adapter retains incomplete four-byte frames; it never invents partial samples.
- Aggregate windows default to 0.1 host seconds (`--window-s`, 0.01–1.0). Windows
  and state commands have receive-side boundaries, not hardware sample timestamps.
- `sample_index_start/end`, `time_s`, and `nominal_sample_end_time_s` describe a
  **valid received ordinal/Fs** axis. Unknown sample loss compresses that axis.
  `host_start/end_offset_s` are separate monotonic receive-side offsets.
  `host_monotonic_origin_s` is comparable only within the same host clock epoch.
- Count, mean, min/max, and sampled integrals are exact over received finite
  values. Quantiles are exact only up to the bounded reservoir capacity (4096 by
  default); larger sets explicitly report uniform-reservoir estimates. Window
  percentiles cannot be recombined into state percentiles. Use optional raw data
  and a bounded offline method for exact state quantiles, otherwise omit them.
- Coverage is valid samples/(host seconds×100000), an **estimate**, not proof of
  loss placement. Receive buffering can exceed 100%; values are not clamped.
  Empty windows report zero coverage and null current statistics.
- `sampled_charge_uC = sum(current_uA)/100000`; mAh divides uC by 3,600,000.
  `sampled_energy_uJ = charge_uC×source_mV/1000`. No missing interval is filled,
  no extrapolated total energy is asserted. DUT energy is separately named when
  an independently measured voltage was supplied.
- Raw records are little-endian `<Qf`: valid received ordinal and float32 uA.
  Raw values are upstream calibrated/filtered current, not raw ADC words. JSONL
  documents the schema and voltage basis. Default caps are 16 MiB aggregate and
  64 MiB raw; configurable byte caps cannot exceed 512 MiB each. Raw exhaustion
  marks `raw_truncated`; aggregate/write faults pause recording, retain ownership,
  and must invalidate completeness claims.

Reject timing-sensitive captures with a fault, truncated required raw data,
unknown interruptions, excessive `max_observed_backlog_bytes`, or sample-versus-
host drift large enough to cross a state guard. Conservatively exclude boundary
windows. Require included-state coverage ≥99%, but do not treat this alone as
proof of no misplaced loss. Record alignment, guard, and drift assumptions.
Scrub local USB identifiers and host paths before publishing reports.

## Host-only verification

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s esp32/tests -p 'test_ppk2*.py'
/tmp/ppk-profile-venv/bin/python esp32/tools/power/bench_ppk2.py --seconds 5
```

The benchmark uses real pinned decoding, fake serial memory, both bounded
reservoirs, and temporary JSON/raw disk output. It never enumerates or opens
hardware. Require throughput headroom above 100k/s; this is not validation of
physical wiring, USB transport reliability, or device behavior.
