---
name: gadget-amazon-echo
description: >-
  Control Amazon Echo speakers and Alexa-linked smart home devices through
  Muse Home Link using the community alexa-remote-control script. Covers
  TTS announcements, volume, media transport, TuneIn/music playback, Alexa
  routines, and smart-home switching via Alexa text commands. Cloud, not
  local: needs a Linux Home Link with internet access, the user's own
  Amazon login, and Amazon's unofficial Alexa web API.
---

# Amazon Echo: Voice Control via CLI

Use this skill when fresh discovery identifies Amazon Echo devices on the
user's Alexa account and the user requests spoken announcements, volume or
playback control, routine triggers, or smart-home device switching.

Tested with: Echo Dot (2nd generation), Echo Dot (5th generation), Amazon
Smart Plug, on amazon.in, 2026-10-04.

## Limitations up front

- This drives Amazon's **unofficial** cloud device APIs via a community
  script, not a local protocol. It needs a Linux Home Link (the script is
  a shell program; an ESP32 Home Link cannot run it) that can reach
  Amazon over the internet, and it acts as the user's Amazon account.
- Amazon changes authentication without notice; when it breaks, re-auth is
  the fix, not a bug in these steps.
- The script renews its session cookies from the refresh token, but
  Amazon can stop accepting the token. Expect the user to re-run the
  login tool periodically.
- No voice purchasing, no Drop In, no multi-room changes unless the user
  explicitly asks.

## Identify the Device

- `-a` prints friendly names only, and the script matches `-d` against its
  cached device list by substring, using the first match. When `-d` is
  omitted it picks the first Echo on the account. Names alone are not a
  safe target — select by the full device-list line (see Workflow).
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

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

On a Linux Home Link (the Muse Linux client):

- `alexa-remote-control.sh` installed and executable (a single shell
  script), plus `curl` and `jq`. Commands run with a minimal `PATH`
  (`/usr/local/bin`, `/usr/bin`, `/bin` and the `sbin` directories), so
  install it there or call it by absolute path.
- Each command runs in a fresh non-interactive shell; exports from an
  earlier command do not persist. Keep the settings below as `export`
  lines in a private env file (mode 0600, owned by the Home Link account)
  and source it in the same command as every call, e.g.
  `. ~/.config/alexa-remote-control.env && alexa-remote-control.sh ...`.
- Region: set `AMAZON` and `ALEXA` for the user's store — e.g.
  `amazon.com`/`alexa.amazon.com` (US), `amazon.in`/`alexa.amazon.in`
  (India), `amazon.de`/`alexa.amazon.de` (Germany),
  `amazon.co.uk`/`alexa.amazon.co.uk` (UK) — and `TTS_LOCALE` for the
  user's language (e.g. `en-US`, `en-IN`). The script's built-in defaults
  are German (`amazon.de`, `de-DE`); do not rely on them.
- Authentication: `REFRESH_TOKEN` (obtained via alexa-cookie-cli) is
  the script's only working login, and it avoids storing the Amazon
  password. The user performs the login step themselves and puts the
  token in the env file. Never place the password, OTP, or refresh token
  in a command line, log, or chat message.
- Point `TMP` at a private directory (mode 0700) owned by the Home Link
  account rather than the shared `/tmp`: the script keeps its cookie jar,
  device list and volume cache there. The same OS account must run the
  script every time, or cookie/temp file ownership breaks.

## Workflow

1. Discover fresh: the script reuses its cached device list until it is
   deleted, so run `rm -f "$TMP"/.alexa.devicelist.*` then
   `alexa-remote-control.sh -a`. Read `"$TMP/.alexa.devicelist.txt"`:
   one line per device, `<name>=<deviceType>=<serial>=<family>`. Match the
   user's device by name **and** serial; if a device is missing or two
   lines could be it, stop and ask — do not guess.
2. Select: use that whole line as the selector, e.g.
   `SEL='Kitchen Dot=<deviceType>=<serial>=ECHO'`. Before acting, check
   `grep -c -- "$SEL" "$TMP/.alexa.devicelist.txt"` prints `1`. Stop if
   it does not, or if the line contains `*`, `?`, `[`, `\`, `%20` or
   repeated spaces (the script globs and regex-matches the selector);
   ask the user to rename the device in the Alexa app. Multi-room groups
   (family `WHA`) are out of scope for this selector.
3. Pass `-d "$SEL"` on every device action below and on its readback.
   Never omit `-d` (the script falls back to the first Echo) and never use
   `-d ALL` unless the user asked for every device. The script echoes
   `serial:<serial>` for the device it used; confirm it is the selected
   one.
4. TTS: `-d "$SEL" -e speak:'<text>'`. SSML is accepted. Keep the first
   test under ten words. `SPEAKVOL=<level>` raises the volume for the
   announcement, then restores the level it read beforehand — or
   `NORMALVOL` (default 10) if it could not read one — so read the volume
   fresh afterwards when you set it.
5. Volume: `-d "$SEL" -e vol:<0-100>`; verify with a fresh read (see
   Verify the Result).
6. Media: `-d "$SEL" -e play` (or `pause`, `next`, `prev`, `shuffle`,
   `repeat`). TuneIn: `-d "$SEL" -r "<station>"` or
   `-d "$SEL" -e playmusic:TUNEIN:'<station>'`. Amazon Music:
   `-d "$SEL" -e playmusic:AMAZON_MUSIC:'<music>'`.
7. Smart-home switching: `-d "$SEL" -e textcommand:'turn on <friendly name>'`
   (or `turn off`); Alexa answers on the selected Echo. This is a
   state-changing action — see Load safety below. Never toggle as a
   retry; use explicit on/off.
8. Routines: `-d "$SEL" -e automation:'<routine name>'` runs an Alexa
   routine by its exact name, with the selected Echo as its device.
9. Bluetooth: `-d "$SEL" -b list` shows paired devices;
   `-d "$SEL" -b <MAC>` connects.
10. Diagnostics: `-d "$SEL" -q` (queue), `-d "$SEL" -n`
    (notifications/alarms); `-lastalexa` and `-lastcommand` are
    account-wide.

Pass text, names and stations as one single-quoted argument (write an
embedded `'` as `'\''`). The script inserts text into JSON without full
escaping and turns `"` into `'`, so keep spoken and command text to plain
words and punctuation.

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

- The script's exit code does not show whether Amazon accepted a command;
  treat any `ERROR` line or error body in its output as a failure.
- After TTS, confirm audibly or ask the user — the API returns success
  before the speaker finishes talking.
- After volume, wait a few seconds, then read it fresh:
  `rm -f "$TMP/.alexa.volume.<serial>"` and run `-d "$SEL" -z`. The
  script writes the requested level to that cache file before sending the
  command, and `-z` returns the cache for a minute, so without the `rm` it
  can confirm a change Amazon rejected. No number in the output means the
  read failed and the change is unverified.
- After plug switching, verify via the Alexa app or by asking the user;
  the script's output alone does not prove the relay changed state.
- On any auth failure, re-run the login flow before retrying. Do not
  retry a failed switch blindly.

## Limits

- Do not create, modify, or delete Alexa routines, alarms, or schedules
  under a control request — those need their own requested scope.
- Do not enable voice purchasing or change payment settings, ever.
- `textcommand` and `automation` can trigger anything Alexa can do. Use
  them only for the device control or query the user asked for — never
  for purchases, calls, messages, Drop In or account settings.
- The temperature and motion sensors are readable only through Alexa
  responses and routine triggers, not as direct API values — query via
  `textcommand` (e.g. "what is the room temperature") and treat the
  spoken answer as the reading.

## Sources

- [alexa-remote-control](https://github.com/thorsten-gehrig/alexa-remote-control) —
  command-line control, device list, speak/volume/textcommand reference
- [alexa-remote-control README](https://github.com/thorsten-gehrig/alexa-remote-control/blob/HEAD/README.md) —
  environment variables (AMAZON, ALEXA, SPEAKVOL, REFRESH_TOKEN)
- [alexa-cookie-cli](https://github.com/adn77/alexa-cookie-cli) — the
  login tool that produces the refresh token
- [alexa-cookie](https://github.com/Apollon77/alexa-cookie) — cookie/refresh-token
  generation when Amazon's login checks block plain auth
