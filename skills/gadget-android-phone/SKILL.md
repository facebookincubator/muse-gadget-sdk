---
name: gadget-android-phone
description: >-
  Use the user's own Android phone as a Muse surface through an owner-built
  companion app: the app reports observed device state on a fixed loop with a
  heartbeat, and accepts commands only as allowlisted templates over a relay
  channel the owner controls. A command is successful only when a later report
  shows the observed state matching the desired state (verified acks) — a
  dispatched command proves nothing by itself. The core app stays slim and
  hosts device plugins by namespace; actuation beyond observation is gated
  in-app by the owner and ships disarmed.
---

# Android Phone (owner-built companion app)

Use this skill when Muse needs the user's phone as a first-class device
surface: reading trustworthy phone state (mode, battery, network, screen,
connectivity lanes), launching apps or navigation, controlling media and
Do Not Disturb, or driving phone-attached peripherals — where the evidence
standard is "observed state changed", not "command was sent".

## Identify the Device

- The surface is not a stock Android API. It is a **companion app the device
  owner builds and installs themselves** on their own phone (framework-only
  Android is sufficient; no vendor partnership or system privileges are
  assumed, because consumer devices gate those surfaces to
  signature/system apps).
- Confirm the app is installed, its foreground reporting service is running,
  and a fresh report exists before treating any phone state as known.
  Absence of a report means state is *unknown*, never "probably unchanged".

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- The companion app, built and side-loaded by the owner, with the grants the
  owner chooses to give it (e.g. notification access for state observation,
  accessibility where a feature needs it, a settings-write grant for
  self-healing a debug lane). Every grant is the owner's on-device decision
  and is revocable there.
- A **relay channel under the owner's control** (for example a small worker
  or server the owner operates): the app polls it for desired state and
  commands, and posts reports to it. The relay's secret is entered on-device
  in the app and stored only in the app's private preferences — it is never
  hardcoded in source, shipped in the binary, or written to logs.
- Optional: the phone's own wireless debugging, paired once in-app by the
  owner, if the deployment uses the loopback command executor described
  below. Pairing is an owner-present step.

## The Pattern

### Reports and the liveness contract

The app runs a fixed reporting loop (a ~60 s reference cadence): it observes
device state on-device — current device mode and running routines as the
platform itself reports them, battery/charging, power-save, screen on/off,
ringer, network kind, storage, plus the health of its own connectivity lanes
(debug-endpoint discovery, VPN/tunnel state) — and posts a report to the
relay **on any change, and as a heartbeat at least every ~15 minutes** even
when nothing changed. Silence is itself a signal: a report older than the
heartbeat bound with no change report means the loop is dead (service
killed, grant revoked, battery manager intervened) and every state claim
from it is stale. Investigate the loop; do not reason from the last report.

Observation must be defensive: parse the platform's own status output,
and treat a format change as a **loud parse failure** reported as such —
never a silently wrong state.

### Verified acks — a fire is never a success

This is the discipline the whole pattern exists for. The app reconciles
each report's *observed* state against the *desired* state the relay
publishes, and emits a verdict (`in_sync` / `drift` / `unknown`) with an
`ack` that is true **only when observed matches desired**. A command being
accepted, a macro firing, or an intent dispatching proves nothing: macros
and intents report success unconditionally by construction. Consumers of
this skill must:

- Claim success only from a subsequent report whose observed state matches,
  or an equivalent independent read-back.
- Surface `drift` with the reconciler's plan (the corrective action it
  proposes) instead of retrying blindly; a failed fire is never auto-retried
  for the same request.
- Treat `unknown` (no desired state, no trustworthy observation, or a stale
  report) as a health problem to diagnose, not a state to guess from.

### Command channel — allowlisted templates, no free-form shell

Commands travel over the same relay: the server enqueues, the app fetches
and executes, results post back. Execution is **allowlisted templates
only** — each template is a fixed command shape with typed, validated
argument slots (charset, range, and count checked before anything runs).
There is no free-form shell, no arbitrary intent construction, and no
raw command string ever reaches the executor. A reference template set:

- Reads: battery dump, device model, settings get, standby/role-holder
  state, recent log tail, crash-record dump, foreground app/activity.
- App and media: launch an app by package, open a navigation intent to a
  single-token destination, media play/pause/next/previous, media volume
  raise/lower/get and a bounded set within the stream's range.
- Device state: Do Not Disturb on/off via the platform command service.
- Gated native operations, executed on-device rather than as shell text,
  each behind its own in-app enable switch: flashlight on/off with state
  read back from the system's torch callback, and starting/stopping an
  assistant voice session.

Two operational rules: enqueue commands **singly** when the executor
processes one per fetch (a batch leaves the rest queued until expiry),
and verify any set-style command with its get-style counterpart — a
running routine can hold the same setting and override the fire.

### Plugin host — slim core, device namespaces

The companion app is a **host**, kept slim: state reporting with verified
acks, the command channel, and the safety gates are the core. Device-specific
support is never baked in. A plugin registers a **command namespace** with
the host, declares its transport endpoint, its status read-back (so the
verified-ack discipline applies to it too), and its session needs
(permissions, service shape). The host routes by namespace and never
interprets device semantics; each plugin owns its device session lifecycle
and its quirks stay inside the plugin.

Worked example: a `buds` plugin for a pair of Bluetooth earbuds registers
verbs for status/battery, volume, noise-control mode, ambient level,
equalizer preset, and touch lock, speaking the earbuds' vendor Bluetooth
protocol from its own app, while the host contributes only routing and
gates. A verb the plugin cannot execute (earbuds disconnected) returns a
typed refusal, not a fake success — and until a plugin's transport has
been exercised live against the real device, its verbs are documented as
built-but-unverified.

## Workflow

1. Read the latest report via the relay; check its age against the
   heartbeat bound before using any field.
2. Decide from the verdict: `in_sync` — nothing to do; `drift` — read the
   plan; `unknown` — diagnose the loop first (service, grants, relay).
3. Enqueue a single allowlisted template with validated arguments.
4. Wait for the result post-back **and** a report (or independent read-back)
   showing the resulting observed state. That read-back is the success
   claim; the enqueue is not.
5. For a peripheral, route to the plugin's namespace and apply the same
   read-back standard to its typed results.

## Verify the Result

- The post-action report's observed state matches the desired state and
  `ack` is true.
- Independent read-backs agree where they exist (platform settings values,
  service logs, focus dumps) — the reference deployment live-verified each
  template class this way before relying on it.
- The report loop stays fresh after the action; an action that kills the
  reporting loop is a failure even if the action itself landed.

## Safety Bounds

- **Actuation is owner-gated and ships disarmed.** Any capability that
  makes the phone *do* something beyond observing and reconciling (for
  example firing device routines) sits behind an in-app arm switch that
  defaults OFF, with scope limits (a single mode, a fresh verified
  observation, fire at most once per request). Arming, linking targets,
  and the first test fire are the owner's in-app actions only — never
  performed by Muse driving the UI or a debug lane.
- **Secrets live on-device only.** Relay credentials and any stored
  launch intents are kept in the app's private storage, never in source,
  logs, reports, or the relay's payloads beyond authentication.
- **No silent state invention.** Stale, missing, or unparsable state is
  reported as unknown. This pattern exists because inferred state caused
  real-world mistakes; a confident wrong answer is the failure mode.
- Outward, effectively irreversible phone actions (sending messages,
  placing calls) are not part of this surface; they keep whatever
  approval rules their own lanes already have.

## Limits

- Platform gating is real: consumer Android reserves the clean mode/state
  APIs to signature/system apps, so observation is built from the
  platform's own status output and is inherently format-sensitive — the
  loud-parse-failure rule is load-bearing, not decorative.
- Some state is simply not exposed (for example, a vendor power-saving
  *tier* where only a boolean exists). The report carries what the
  platform exposes and says so; never upgrade a boolean to a tier claim.
- Background execution is at the mercy of the device's battery manager:
  the reporting service needs its exemption, and a force-stop ends the
  loop until the owner relaunches the app. The heartbeat contract exists
  so this is detected in minutes, not discovered mid-task.

## Sources

- [Android foreground services](https://developer.android.com/develop/background-work/services/fgs)
- [Android notification listener services](https://developer.android.com/reference/android/service/notification/NotificationListenerService)
- [Android wireless debugging (ADB over Wi-Fi)](https://developer.android.com/tools/adb)
