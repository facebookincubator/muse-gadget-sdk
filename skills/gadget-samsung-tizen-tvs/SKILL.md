---
name: gadget-samsung-tizen-tvs
description: >-
  Control compatible Samsung Tizen TVs through their paired local WebSocket interface using
  Muse Home Link. Use for requested remote or app actions, and Frame Art operations only when
  the model and Art API support them.
---

# Samsung Tizen TVs

Use this skill when a confirmed Samsung Tizen TV exposes a compatible local remote WebSocket service.

## Identify the Device

- Read the TV's REST device information first: `GET http://<tv-ip>:8001/api/v2/` needs no pairing. Its `device` object carries `modelName`, `FrameTVSupport` and, on some models, `PowerState`. Keep `wifiMac` and the other identifiers it returns private.
- Confirm model, firmware and the actual remote endpoint. A Frame model's Art API is separate from the general remote interface.
- Use the returned device information and installed client's compatibility notes; do not infer Art support from the Samsung brand alone. `FrameTVSupport` must read `true` before any Art operation.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- A compatible local WebSocket client routed through Home Link, with the TV's key pinned (see TLS Pinning). Pairing tokens belong to the TV's TLS port, `wss://<tv-ip>:8002/api/v2/channels/<channel>` (the client library sends the token only there); the channel is `samsung.remote.control` for the remote and `com.samsung.art-app` for Frame Art.
- The TV's Access Notification setting must let a new client ask: Settings > General > External Device Manager > Device Connection Manager > Access Notification. On a 2019 The Frame set to Off, a new client was refused at once with `ms.channel.unauthorized` and no prompt appeared. Set to Always On, the TV showed an Allow prompt for the art channel, and after the user allowed it the saved token reconnected later without another prompt. The client library and Home Assistant suggest First Time Only. A client the TV refused before stays listed under Device Connection Manager > Device List, where the user can remove it.
- User approval of the normal TV pairing prompt. Before a new client connects, ask the user to watch the TV and choose Allow: an unanswered prompt likely ends in a client timeout rather than a refusal.
- Secure storage/reuse of the returned token: keep it in the platform's secret storage and pass it to the client from there, never through a plain token file, the workspace, logs or chat. samsungtvws logs a new token at INFO and token-bearing URLs and responses at DEBUG, so keep its logging at WARNING or quieter, leave WebSocket tracing off, and remove the token from any error before showing it.
- Permission for the requested screen/audio/app action. Frame art changes require permission for the particular content and operation.

## TLS Pinning

Stock samsungtvws (these steps were checked against version 3.0.7) turns certificate checks off on port 8002 (`cert_reqs: CERT_NONE`), on the connection that carries the pairing token, so nothing confirms that the other end is the TV. Pin the TV's public key instead:

1. First pairing, or a TV with a saved token but no saved pin: there is usually no independent way to verify the TV's key, so explain trust on first use on the user's home network. Capture the leaf certificate's public key from port 8002 over a TLS connection that sends nothing else, for example with `set -o pipefail; openssl s_client -connect <tv-ip>:8002 </dev/null | openssl x509 -pubkey -noout | openssl pkey -pubin -outform der | openssl dgst -sha256 -binary | base64`. Check that every step succeeded and that a public key was extracted before saving anything; on any error, stop without saving or replacing a pin. Save the resulting SHA-256 hash of the public key (SPKI) per TV in the platform's secret storage, next to the token. Seeing the Allow prompt on the TV is not certificate verification.
2. Enforce the pin on every connection that carries the token. Use samsungtvws's synchronous classes, `SamsungTVWS` and `SamsungTVArt`, created with the TV's IP address as `host`, `port=8002` (their default, 8001, is an unencrypted WebSocket) and a finite `timeout` such as 30 seconds (the default, `None`, can wait forever, while Prerequisites tells the user to expect a timeout). Their `open()` calls websocket-client's `create_connection` (`websocket.create_connection`, as `samsungtvws.connection` uses it) with fixed TLS options, and `create_connection` uses an already-connected TLS socket passed as `socket` as it is. In the process that runs samsungtvws, replace that call with one that, for `wss://<approved-tv-ip>:8002` only, opens the TLS connection itself (CA and hostname checks off, since the pin replaces them; the client's timeout set on the socket), reads the peer certificate with `getpeercert(binary_form=True)`, computes the base64 SHA-256 of its public key (SPKI, for example with the `cryptography` package) so it has the same form as the saved pin, compares the two, and only then passes the socket to the original `create_connection` with `redirect_limit=0`, so a redirect can't carry the token to another connection. Refuse any other `wss://` URL instead of passing it on: a host name, or a new address the TV got from DHCP, would otherwise send the token without the check; ask the user to confirm the TV's new address, then connect there with the same check against the saved pin. The TV's key doesn't change with its address, so a different key at the new address is a mismatch (step 4), never a new pin. Pass only `ws://` URLs, which samsungtvws builds without the token, to the original function unchanged. This covers the pairing connection too, which returns the token.
3. Other samsungtvws paths don't go through that wrapper: the async classes open their connection with the `websockets` library, and the REST calls use `requests` with certificate checks off. Don't use the async classes for anything that carries the token, and never use `send_password` (`rest_ime_input`): it puts the token in a REST URL on port 8002. Type text with `send_text`, which goes over the pinned WebSocket, and make REST reads only on `http://<tv-ip>:8001` (Identify the Device), which needs no token: a client created with `port=8002` sends its REST helpers (`supported()`, `rest_device_info()` and the others) to port 8002 with certificate checks off, so don't call them on it.
4. On a mismatch, close the socket before anything is sent, stop and tell the user. Do not replace the pin or re-pair automatically, never fall back to a connection without the check, and keep connections on the approved TV's address.
5. The Frame Art upload's second connection (Art API versions other than 0.97) carries the picture and a per-upload key from the TV, not the pairing token. Stock samsungtvws connects to the announced address inside `upload()`, so enforce Workflow step 5's address rule by subclassing `SamsungTVArt` and overriding `_open_d2d_socket` to compare `conn_info["ip"]` with the approved TV's address before connecting, raising otherwise. Construct that subclass directly: `SamsungTVWS.art()` returns the stock `SamsungTVArt`, which skips the override.

## Workflow

1. Answer a status question (is the TV on?) from the REST device information, using `PowerState` where present. Do not open a WebSocket just to answer it: on some TVs, opening one turns the TV on. If `PowerState` is missing, tell the user the TV is reachable but its power state can't be read without opening a connection that may turn it on, and ask before opening one.
2. Establish the documented local remote connection with a recognizable client name and valid saved token, or complete normal user-approved pairing.
3. Read available device, power, app or Art status using interfaces documented for this model. Do not substitute a cloud API when a local read is unavailable.
4. For remote control, send only the requested supported key or application operation; resolve real app identifiers rather than guessing them.
5. On an Art-capable Frame with a supported Art API version (`api_version`), use its documented status/mode, available-art selection or requested upload operation. The art channel pairs like the remote channel. Transfer only user-approved media through the documented local client protocol, and authorize any additional destination it requires. How the file travels depends on the Art API version: with 0.97, samsungtvws sends it as a binary frame on the art WebSocket; with other versions, it opens a second TCP connection to an address and port the TV announces on the art channel, routed through Home Link like the WebSocket. That announced address must be the TV's own IP; only the port may differ. If it is any other address, stop and tell the user. An upload only adds the picture to the TV's art collection; showing it (`select_image`) is a separate step the user must have asked for.
6. Check the relevant response and available state after each action. Do not use a key sequence as an unverified substitute for an unavailable direct operation.

## Verify the Result

- A WebSocket acknowledgment or accepted key does not prove the desired on-screen outcome; use state read-back or user confirmation.
- Check Art API version/errors separately from remote pairing. Remote success does not establish Art support.
- After showing art, read `get_current_artwork` (`get_current()` in samsungtvws) and ask the user to confirm the screen. On the tested Frame, a 3840x2160 JPEG uploaded, displayed and read back this way. Pressing the TV's power button left Art Mode; return with `set_artmode(True)` in samsungtvws (the Art API's `set_artmode_status`) only when the user asks; on the tested Frame it returned to Art Mode.
- Inspect state before retrying power or other toggle-like commands.

## Limits

- No cloud alternatives, UDP wake or device-to-agent media fetch without an existing reachable source.
- No universal Frame Art coverage, app installation, unrequested content replacement/deletion or firmware changes.
- A sleeping TV may not expose its API; do not promise wake from network presence alone.
- The Frame Art upload's second connection is not authenticated (plain TCP, or TLS with certificate checks off), so someone on the network path between Home Link and the TV can read the picture. Upload only pictures the user is comfortable sending that way.
- Pinning has not been tested on hardware: the tested Frame was paired and used with stock samsungtvws, without a certificate check. The TLS Pinning steps follow the Insignia Fire TV skill's pattern and the client libraries' source.

## Sources

- [Samsung TV local WebSocket and Art API client](https://github.com/xchwarze/samsung-tv-ws-api), version 3.0.7: README (Access Notification and Device List), [`connection.py`](https://github.com/xchwarze/samsung-tv-ws-api/blob/v3.0.7/samsungtvws/connection.py) (the WebSocket URL carries the token only on port 8002, where certificate checks are off; `create_connection` with fixed TLS options), [`remote.py`](https://github.com/xchwarze/samsung-tv-ws-api/blob/v3.0.7/samsungtvws/remote.py) (`send_text` over the WebSocket; `send_password` sends the token through REST; `art()` returns the stock `SamsungTVArt`), [`rest.py`](https://github.com/xchwarze/samsung-tv-ws-api/blob/v3.0.7/samsungtvws/rest.py) (`requests` with `verify=False`), [`async_connection.py`](https://github.com/xchwarze/samsung-tv-ws-api/blob/v3.0.7/samsungtvws/async_connection.py) (the async classes use the `websockets` library) and [`art/art.py`](https://github.com/xchwarze/samsung-tv-ws-api/blob/v3.0.7/samsungtvws/art/art.py) (art channel, upload as a WebSocket binary frame for Art API 0.97 or over a second socket otherwise, opened in `_open_d2d_socket` with certificate checks off where it uses TLS, `select_image`, `get_current_artwork`, `set_artmode` / `set_artmode_status`).
- [websocket-client `_http.py`](https://github.com/websocket-client/websocket-client/blob/b6cff278b0bfa515884da4037a175e2fe9318c7e/websocket/_http.py): `connect()` uses a caller-supplied TLS socket as it is, which is where the wrapper checks the pin. [`_core.py`](https://github.com/websocket-client/websocket-client/blob/b6cff278b0bfa515884da4037a175e2fe9318c7e/websocket/_core.py): `redirect_limit` caps the redirects the handshake follows.
- [libcurl `CURLOPT_PINNEDPUBLICKEY`](https://curl.se/libcurl/c/CURLOPT_PINNEDPUBLICKEY.html): the `openssl` steps that turn a server certificate into the base64 SHA-256 hash of its public key.
- [Home Assistant Samsung Smart TV integration](https://www.home-assistant.io/integrations/samsungtv/): troubleshooting for the Access notification prompt.
- [Home Assistant's Samsung TV bridge](https://github.com/home-assistant/core/blob/dev/homeassistant/components/samsungtv/bridge.py): reads `PowerState` from device information first, because opening a WebSocket turns some TVs on; checks `FrameTVSupport`.
