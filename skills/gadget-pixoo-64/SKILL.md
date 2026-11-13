---
name: gadget-pixoo-64
description: Drive a Divoom Pixoo-64 LED pixel display (64x64) on the local network through Muse Home Link — read its state, push still frames and text — using the device's local HTTP API.
---

# Divoom Pixoo-64

The Pixoo-64 is a 64×64 LED pixel display with an unauthenticated local HTTP
API. It works with Muse Home Link when it is on the same subnet as the Link:
find it with Link discovery, then send its API calls through the Link as
documented HTTP requests. No cloud account or Divoom app is needed for the
local API.

Verified working with Home Link on 2026-11-12 (a `Channel/GetIndex` call
through the Link returned the device's current channel).

## The API

Every command is a JSON `POST` to a single endpoint on port 80:

```
POST http://<device-ip>/post
Content-Type: application/json

{"Command": "<command>", ...parameters}
```

Every response is JSON and includes `error_code` — `0` means success. Check
it on every call; the device accepts some malformed requests silently.

### Read state (safe, changes nothing)

- `{"Command":"Channel/GetIndex"}` — returns `SelectIndex`, the current
  channel. Channel `3` is the custom channel, where frames you push are shown.
- `{"Command":"Channel/GetAllConf"}` — returns the device's channel
  configuration. The field set varies by firmware; use `Channel/GetIndex`
  when you only need the current channel.

### Show something

Switch to the custom channel before pushing frames:

```json
{"Command":"Channel/SetIndex","SelectIndex":3}
```

Push one still frame. `PicData` is **raw RGB pixel bytes, base64-encoded** —
64 × 64 × 3 = 12,288 bytes for a full frame, row by row, one byte each for R,
G, B per pixel. It is not a PNG or GIF file; render the frame to raw pixels
first.

```json
{"Command":"Draw/ResetHttpGifId"}
{"Command":"Draw/SendHttpGif","PicNum":1,"PicWidth":64,"PicOffset":0,"PicID":0,"PicSpeed":100,"PicData":"<base64 raw RGB>"}
```

Always send `Draw/ResetHttpGifId` before a frame or frame sequence — without
it, the device silently ignores the frames.

Draw text over a frame you have pushed:

```json
{"Command":"Draw/SendHttpText","TextId":1,"x":0,"y":0,"dir":0,"font":8,"TextWidth":64,"TextHeight":16,"speed":50,"color":"#FFFFFF","TextString":"HELLO"}
```

## Limits

- **Stills first.** Pushing multi-frame GIF animations can crash and reboot
  the device (observed with a 16-frame push on current firmware); community
  experience caps animations at roughly 40 frames even when they work. Prefer
  single still frames. To animate, re-push stills yourself at a human pace
  instead of handing the device an animation.
- **Pace your requests.** The device handles rapid-fire requests badly —
  under ~100 ms spacing is unreliable. Keep at least ~350 ms between POSTs in
  a sequence.
- **`Draw/CommandList` cannot batch animation frames.** Send one
  `Draw/SendHttpGif` per frame.
- **Text only overlays frames sent via `Draw/SendHttpGif`** — it does not draw
  over gallery or SD-card content.
- **The local API has no authentication.** Anyone on the subnet can drive the
  display. Keep the device on a trusted LAN segment and never port-forward it.
- Play a GIF from a URL only at 16×16, 32×32, or 64×64 (`Device/PlayTFGif`);
  other sizes can crash the device, per Divoom's own documentation.

## Sources

- Official Divoom local API documentation:
  http://doc.divoom-gz.com/web/#/12?page_id=196
- Community command references are linked from the official doc index above;
  prefer the official docs when they disagree.

