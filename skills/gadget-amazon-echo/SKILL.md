---
name: gadget-amazon-echo
description: >-
  Control Amazon Echo speakers and Alexa-linked smart home devices through
  Muse Home Link using the community alexa-remote-control script. Covers
  TTS announcements, volume, media transport, TuneIn/music playback, Alexa
  routines, and smart-home switching via Alexa text commands.
---

# Amazon Echo: Voice Control via CLI

Use this skill when fresh discovery identifies Amazon Echo devices on the
user's Alexa account and the user requests spoken announcements, volume or
playback control, routine triggers, or smart-home device switching.

Tested with: Echo Dot (2nd generation), Echo Dot (5th generation), Amazon
Smart Plug, on amazon.in, 2026-10-04.

## Limitations up front

- This drives Amazon's **unofficial** device APIs via a community script.
  Amazon changes authentication without notice; when it breaks, re-auth is
  the fix, not a bug in these steps.
- Session cookies expire (roughly 14 days). Expect to re-authenticate
  periodically.
- No voice purchasing, no Drop In, no multi-room changes unless the user
  explicitly asks.

## Identify the Device

- Run `alexa-remote-control.sh -a` and match by friendly name **and**
  serial. Names alone are insufficient — duplicate friendly names happen.
- Capability notes by hardware (do not assume; confirm generation from the
  device list or the Alexa app):
  - Echo Dot 5th gen / Echo 4th gen+: temperature sensor and ultrasound
    motion detection usable as routine triggers; tap gestures; Matter
    controller support.
  - Older Dots (e.g. 2nd gen): TTS, volume and transport work; no
    temperature/motion sensors, no Matter.
- Smart-home devices (plugs, lights, switches linked to Alexa) usually do
  **not** appear in `-a`. They are driven through the Alexa voice pipeline
  (`textcommand`), addressed by their Alexa-app friendly name.

## Prerequisites

On the Home Link host (the paired Linux gadget):

- `alexa-remote-control.sh` installed and executable (single bash script),
  plus `jq` for its JSON handling.
- Region: export `AMAZON` and `ALEXA` for the user's store — e.g.
  `amazon.com`/`alexa.amazon.com` (US), `amazon.in`/`alexa.amazon.in`
  (India), `amazon.de`/`alexa.amazon.de` (Germany),
  `amazon.co.uk`/`alexa.amazon.co.uk` (UK). Do not use another region's
  defaults.
- Authentication: preferred method is `REFRESH_TOKEN` (obtained via
  alexa-cookie-cli), which avoids storing the Amazon password. The user
  performs the login step themselves; the resulting cookie jar must be
  mode 0600 and owned by the account that runs the script. Never place the
  password, OTP, or refresh token in a command line, log, or chat message.
- The same OS account must run the script every time, or cookie/temp file
  ownership breaks. Point `TMP` at a private directory owned by that
  account rather than the shared `/tmp`.

## Workflow

1. Discover: `alexa-remote-control.sh -a`. Record exact friendly names. If
   a device is missing, stop — do not guess names.
2. TTS: `-d "<device>" -e speak:'<text>'`. SSML is accepted. `SPEAKVOL=`
   sets the announcement volume without touching the device's normal
   volume. Keep the first test under ten words.
3. Volume: `-d "<device>" -e vol:<0-100>`; read back with `-z`.
4. Media: `-e play`, `-e pause`, `-e next`, `-e prev`, `-e shuffle`,
   `-e repeat`. TuneIn: `-r "<station>"` or
   `-e playmusic:TUNEIN:'<station>'`. Amazon Music:
   `-e playmusic:AMAZON_MUSIC:'<music>'`.
5. Smart-home switching: `-e textcommand:'turn on <friendly name>'` (or
   `turn off`). This is a state-changing action — see Load safety below.
   Never toggle as a retry; use explicit on/off.
6. Routines: `-e automation:'<routine name>'` runs an Alexa routine by its
   exact name.
7. Bluetooth: `-b list` shows paired devices; `-b <MAC>` connects.
8. Diagnostics: `-q` (queue), `-n` (notifications/alarms),
   `-lastalexa`, `-lastcommand`.

## Load safety (smart plugs)

- Before the first switch of any smart plug, identify the attached load
  with the user and record it. Switching cuts power to everything on the
  plug at once — e.g. a TV + streaming box + set-top box on one extension
  all lose power together, and a set-top box can take minutes to recover.
- Loads with motors, heaters, medical devices, or network gear need
  explicit per-action confirmation, not just first-time identification.
- Never exceed the plug's rated amperage; adding new devices to the
  circuit is out of scope for a switching request.
- Abrupt power cuts can corrupt streaming-box storage over time — prefer
  software shutdown where the user asks for a full off.

## Verify the Result

- After TTS, confirm audibly or ask the user — the API returns success
  before the speaker finishes talking.
- After volume, re-read with `-z`.
- After plug switching, verify via the Alexa app or by asking the user;
  the script's exit code alone does not prove the relay changed state.
- On any auth failure, re-run the login flow before retrying. Do not
  retry a failed switch blindly.

## Limits

- Do not create, modify, or delete Alexa routines, alarms, or schedules
  under a control request — those need their own requested scope.
- Do not enable voice purchasing or change payment settings, ever.
- The temperature and motion sensors are readable only through Alexa
  responses and routine triggers, not as direct API values — query via
  `textcommand` (e.g. "what is the room temperature") and treat the
  spoken answer as the reading.

## Sources

- [alexa-remote-control](https://github.com/thorsten-gehrig/alexa-remote-control) —
  command-line control, device list, speak/volume/textcommand reference
- [alexa-remote-control README](https://github.com/thorsten-gehrig/alexa-remote-control/blob/HEAD/README.md) —
  environment variables (AMAZON, ALEXA, SPEAKVOL, REFRESH_TOKEN)
- [alexa-cookie](https://github.com/Apollon77/alexa-cookie) — cookie/refresh-token
  generation when Amazon's login checks block plain auth
