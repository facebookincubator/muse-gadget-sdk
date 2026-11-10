---
name: gadget-bambu-lab-printers
description: >-
  Read Bambu Lab printer status (job, progress, temperatures, filament) over the local
  network with the printer's access code, and, only on printers where the owner has enabled
  LAN Only mode and Developer Mode, start an owner-approved print. Not Bambu Cloud
  sign-in, arbitrary G-code, firmware or configuration changes. Never enable Developer Mode
  on the user's behalf.
---

# Bambu Lab Printers

Use this skill when the home has a Bambu Lab printer (X1, P, A or H series) on the local
network and the user asks about print status, filament or sending a print. For
Klipper-based printers, use the Moonraker skill instead.

## Identify the Device

- Confirm the exact printer (model and serial) from LAN discovery or a user-confirmed
  address. Homes often have more than one; confirm which one a question is about.
- Establish which local paths exist for it: LAN status with the access code, and whether
  the owner has enabled Developer Mode for third-party print starts.

## Guided Setup Check

Walk through these in order. For a missing layer, give the user the step instead of
improvising around it.

1. **LAN access code.** The owner reads the access code from the printer's network
   settings screen and stores it with the host's other device credentials. Credentials
   are never pasted into chat, logged or passed in a command-line URL. Validate the
   printer's TLS identity with a configured or pinned trust path before sending the
   code; do not disable certificate validation.
2. **Developer Mode is the owner's decision, made knowingly.** Starting prints from
   third-party tools requires LAN Only mode plus Developer Mode, enabled by the owner on
   the printer's touchscreen. Before they decide, state the trade plainly: while it is on,
   Bambu Handy, cloud printing, cloud print history and online firmware updates stop
   working; Bambu Studio on the same network still prints, and switching the mode off
   restores cloud features, though prints made in LAN mode are not added to the cloud
   history. Never enable it on the user's behalf, and never treat a question about
   printing as consent to switch modes.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- The guided setup check passes for the path being used (LAN status, or Developer-Mode
  print start).
- For a print start: a file the user explicitly selected, already sliced for this exact
  printer model, plus the user's confirmation that the build plate is clear and the
  printer is ready. Do not substitute another file.

## Workflow

1. Status first: printer state, job name, progress %, remaining time, current/total
   layers, nozzle and bed temperatures, and any error. Report the filament source:
   AMS trays (type, color, remaining amount) when an AMS is present, or the external
   spool otherwise. Treat a disconnected printer as unavailable, not idle.
2. Print start (Developer Mode only), each step from a fresh status read:
   - Confirm the printer is idle with no active or paused job and no error.
   - Confirm the loaded filament (AMS tray or external spool) matches the file's
     material. If the printer cannot report it, as with an external spool, ask the user
     to confirm the loaded filament instead of assuming it.
   - Confirm with the user this exact file, this exact printer and a clear build plate.
   - Upload the selected file over the local file channel, then send one documented
     start request for it. If any check fails or is unclear, stop and report.
3. Report the start with the slicer's own estimates (filament, time) and the exact file
   and printer used, not guesses.

## Verify the Result

- A successful request is not proof that printing started. Re-read status: it should
  show printing with the expected job name and progressing layers.
- If the printer stayed idle or the result is uncertain, report it and inspect the
  current job. Never resend a start automatically, and never restart a running print.
- After a requested pause, stop or resume, re-read status and report it done only when
  the state has changed.

## Limits

- No Bambu Cloud sign-in or cloud print dispatch. Bambu authorizes cloud jobs itself;
  do not route around that authorization.
- No arbitrary or raw G-code, firmware or configuration changes, calibration or homing
  moves, or bypassing thermal and motion protections.
- Pause, stop or resume only on the user's explicit request for that job. Intervening
  in someone else's print wastes filament and can ruin the part. The one exception is the
  Bambu Lab P2S skill's failure pause under criteria the user agreed in advance.
- Camera live view and timelapse review are out of scope for this skill; do not promise
  them. For P2S chamber-camera snapshots and print monitoring, see the Bambu Lab P2S skill.
- Developer Mode relaxes Bambu's LAN authorization; it belongs on a trusted home network
  only.
- Bambu can change firmware authorization or LAN behavior at any time; an update may close
  paths this skill relies on.

## Sources

- [Bambu Lab Wiki](https://wiki.bambulab.com/)
- [Bambu Lab](https://bambulab.com/)
