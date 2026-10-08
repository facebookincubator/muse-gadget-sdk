---
name: gadget-logitech-squeezebox
description: >-
  Read player status and control Squeezebox playback or volume through an existing local
  Lyrion/Squeezebox server using Muse Home Link. Use for players already connected to that
  server, not new server deployment.
---

# Logitech Squeezebox via Lyrion

Use this skill when a Squeezebox-compatible player is already connected to a reachable local Lyrion/Squeezebox server.

## Identify the Device

- Identify the existing server's actual HTTP endpoint and server version, then enumerate its connected players.
- Resolve the intended player by returned player ID and name. The speaker's own network port is not necessarily the server's JSON-RPC endpoint.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- An existing configured Lyrion server, any required HTTP authentication, and a player already connected to it.
- A JSON-RPC HTTP client routed through HomeLink. No new server or SlimProto implementation is needed.
- An existing library item or media source reachable and usable by that server/player, with any necessary entitlements.

## Workflow

1. Send HTTP JSON to the server's `/jsonrpc.js` endpoint using method `slim.request`, a request ID and `params: [player_id, command_array]`. Use the empty player ID for documented server-wide reads.
2. Enumerate players with the documented `players` command and query the selected player's `status`. Read power, playback, volume and current playlist before acting.
3. For the user's requested playback, issue supported commands such as `play`, `pause`, `stop`, `playlist` selection or `mixer volume`. Use returned library IDs/URLs and the command's documented argument semantics.
4. Use an explicit desired pause/volume value where supported rather than ambiguous toggles. Do not replace a queue or select a new source merely to inspect status.
5. Poll the selected player's status and correlate JSON-RPC errors/replies to the request.

## Request Shape

A read-only player enumeration request has this shape; send it to the already-authorized server endpoint:

```json
{"id":1,"method":"slim.request","params":["",["players",0,100]]}
```

Follow pagination when the response reports more players. Use the returned player ID for player-specific commands.

## Verify the Result

- Check both JSON-RPC errors and the player's subsequent status, not just HTTP success.
- A queue entry or buffering state is not proof that audio is playing.
- Confirm the intended player was selected before changing a shared server playlist.

## Limits

- No new server deployment, direct SlimProto implementation or automatic access to cloud media services.
- HomeLink is not a media server; agent-local files are not automatically reachable by Lyrion or the player.

## Sources

- [Lyrion JSON-RPC server implementation](https://github.com/LMS-Community/slimserver/blob/HEAD/Slim/Web/JSONRPC.pm)
- [Lyrion documentation](https://lyrion.org/)
