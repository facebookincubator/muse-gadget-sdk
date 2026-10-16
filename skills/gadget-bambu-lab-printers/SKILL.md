---
name: gadget-bambu-lab-printers
description: >-
  Read Bambu Lab printer status (job, progress, temperatures, AMS filament) through the
  owner's Bambu Cloud account or the local network, and — only on printers where the owner
  has enabled Developer Mode — dispatch prints over the LAN. This is a closed ecosystem:
  cloud print dispatch is unavailable to third parties, and Developer Mode disables Bambu
  Handy and cloud features while it is on. Never enable Developer Mode on the user's behalf.
---

# Bambu Lab Printers — Cloud Status and Developer-Mode LAN Dispatch

Use this skill when the home has a Bambu Lab printer (X1, P, A, or H series) bound to the
owner's account and the user asks about print status, filament, or sending a print. For
Klipper-based printers use the Moonraker skill instead — that ecosystem is open, this one
is not, and the differences below are the substance of this skill.

## Identify the Device

- Confirm the exact printer (model and serial) from the account's bound-printer list or
  LAN discovery. Homes often have more than one; confirm which one a question is about.
- Establish which access paths exist for it: cloud account (basic status), LAN with the
  printer's access code (rich status), Developer Mode (third-party dispatch).

## Guided Setup Check

Walk through these in order; for a missing layer, give the user the step instead of
improvising around it.

1. **Account binding.** The printer must be bound to the owner's Bambu Lab account.
   Cloud status reads use the account session held by the host's tooling; credentials
   and tokens are never pasted into chat.
2. **LAN access code.** For local status, read the access code from the printer's
   network settings screen and store it with the host's other device credentials.
   LAN status is richer than cloud status (layers, temperatures, AMS detail).
3. **Developer Mode — the owner's decision, made knowingly.** LAN print dispatch by
   third-party tools requires LAN Only mode plus Developer Mode, enabled by the owner
   on the printer's touchscreen. Before they decide, state the trade plainly: while
   Developer Mode is on, Bambu Handy, cloud dispatch, cloud print history, and OTA
   firmware updates stop working; Bambu Studio on the same network still prints, and
   switching the mode off restores everything — except that prints made in LAN mode
   are never added to the cloud history. Never enable it on the user's behalf, and
   never treat a question about printing as consent to switch modes.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- The guided setup check passes for the path being used (cloud status, LAN status, or
  Developer-Mode dispatch).
- For dispatch: a file already sliced for the exact printer model and the user's
  explicit approval of this specific print.

## Workflow

1. Status first: printer state, job name, progress %, remaining time, current/total
   layers, nozzle and bed temperatures. Report AMS trays — filament type, color, and
   remaining amount — and warn when a running job's filament looks short.
2. Prefer LAN status when available: cloud status omits temperatures and layers and
   depends on Bambu's servers and the printer's cloud connection being healthy.
3. Dispatch (Developer Mode only): upload the sliced file over the local file channel
   and start it over the local control channel only after confirming the printer is
   idle, the loaded AMS filament matches the file's requirements, and the user has
   approved this exact print. If any check is unclear, do a dry run and report instead
   of starting.
4. Report dispatch with the slicer's own estimates (filament, time) and the exact file
   and printer used — not guesses.

## Verify the Result

- After a start, re-read status within a few minutes: state should show printing with
  the expected job name and progressing layers. If the printer stayed idle, say so
  rather than retrying blind.
- After completion, confirm the final state and remaining filament before answering
  follow-up questions about the next print.

## Limits

- No cloud print dispatch: Bambu signs cloud jobs with a private key, so third parties
  cannot send prints through Bambu Cloud at all. Do not route around that
  authorization; the LAN Developer-Mode path is the supported alternative.
- Pause, stop, or resume only on the owner's explicit request for that job —
  intervening in someone else's print wastes filament and can ruin the part.
- No camera: there is no documented third-party live-view path outside Bambu's own
  apps, so do not promise camera checks or timelapse review.
- Developer Mode deliberately relaxes Bambu's LAN authorization; note that it belongs
  on a trusted home network only.
- Community pattern for a closed ecosystem: Bambu can change the account API,
  firmware authorization, or LAN behavior at any time, and a firmware update may
  close paths this skill relies on.

## Sources

- [Bambu Lab Wiki — LAN Only Mode and Developer Mode](https://wiki.bambulab.com/)
- [Bambu Lab](https://bambulab.com/)
- Contrast: the open [Moonraker](../gadget-moonraker-3d-printers/SKILL.md) skill in this catalog.
