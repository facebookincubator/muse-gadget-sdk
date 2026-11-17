---
name: gadget-music-assistant
description: Control playback and queues and read server information and library results on named Music Assistant players through Muse Home Link when the user has created a long-lived token for the gadget and never to set up providers or change server settings
---

# Music Assistant

Use this skill when the user asks to read or control music on a named Music Assistant player, including checking what is playing, browsing or searching the library, reporting provider status, or running transport, volume, play media, and queue operations on a player they named.

## Identify the Device

Identify the Music Assistant server from the user's own setup rather than assuming an address or port. The server speaks HTTP and WebSocket on port 8095 by default at http://<server-ip>:8095, using that placeholder form until the actual server is confirmed. The port can be changed, so confirm the actual port from the user's setup or the server rather than assuming 8095. Port 8097 is the stream server and is not an API, so never use it for control. Resolve players from the server's own responses, such as its server information and player state, and act only on a player whose identity matches a player the user named. If no response identifies the server or the named player, stop and report what could not be confirmed.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

Authentication is mandatory on current servers. The gadget must use a long-lived token created by the user in the Music Assistant settings under their profile. That token is valid for one year and is revocable. It must be created on the user's own Music Assistant server under their own profile — never use a token issued for another person's server or account, and never share or publish the token. Keep it in user-only approved storage, send it as a Bearer header for HTTP or as the first auth command on a WebSocket session, and never paste it into chat, put it in a URL, or log it. Use HTTPS when the user's server is set up with it; over plain HTTP, keep to the trusted LAN. Short-lived login tokens are for interactive sign-in, not for a gadget, so do not use them for gadget access. The Home Assistant add-on internal webserver shortcut is not usable from a gadget, so a gadget always uses the 8095 server with a token, subject to the confirmed actual port.

There is no stable hand-written REST specification. Pin behavior to the official music-assistant-client library or to the live generated API documentation served at /api-docs on the target server where it provides it, and expect schema drift between server versions. The WebSocket API at /ws is primary and an HTTP JSON-RPC endpoint lives at /api. Describe and resolve operations in terms of the client's player and queue calls checked against the live docs, and do not invent endpoint payloads beyond the transport and authentication facts in this skill.

## Workflow

1. Read state first. Connect to the confirmed server, authenticate, and read server information, the named player's state, its queue and now-playing information, and whether another source is using the attached output before proposing any change.
2. Resolve player IDs from actual responses. Take the player identifier from the server's own player list or state response, match it to the player the user named, and stop if the match is ambiguous or absent.
3. Check group membership before any write. If the named player is synced with others or is part of a group, transport and queue commands act on the whole group: tell the user which players will be affected and go ahead only if they agree.
4. Check for a busy output before playing anything. If the player or its attached output is already busy with something else, stand down and report what it is doing instead of interrupting it.
5. Act on named players only. On a player the user named, the allowed writes are transport controls, volume, play_media, and queue operations on that player's queue. Reads may also include library browse and search and provider status as read-only reporting.
6. Protect shared players. Never leave a test queue or test track on a shared player, clear only a queue you created and only when asked, and never resend a play command automatically.
7. Treat a state question as a question only. A state question is never permission to change anything, and an already-set value counts as success rather than a reason to write again.

## Verify the Result

Verify every write by reading the player and queue state back from the server and comparing it with what the user asked for. Confirm the affected player is the named player, report its transport state, volume, now-playing item, and queue as returned, and note any difference plainly. For reads, report only what the server response showed. Confirm at the end that no test queue or test track was left on a shared player and that no unnamed player was changed.

## Limits

This skill covers named players only and does not cover providers or server configuration. Do not set up providers, enter credentials for streaming services, or change server settings. Do not change multi-room groups, make announcements, or control a server over the open internet. Do not use port 8097 as an API, do not use short-lived login tokens for gadget access, and do not rely on a fixed REST payload where the official client library or the live /api-docs on the target server disagrees.

## Sources

- [Music Assistant documentation](https://www.music-assistant.io/)
- [Music Assistant API](https://www.music-assistant.io/api/)
- [music-assistant-client on PyPI](https://pypi.org/project/music-assistant-client/) — the official client, including long-lived tokens
- The live API docs page at /api-docs on the user's own server
