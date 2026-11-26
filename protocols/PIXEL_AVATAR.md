# Pixel avatar asset contract (v1)

A reusable compiler turns an input image into a small indexed sprite. A shared device renderer supplies generic breathing, bobbing, tap feedback and state indication. No avatar-specific executable code, eye/mouth rig, remote rendering or anatomical assumptions are in v1.

## Binary pack

All integers and RGB565 palette entries are little-endian. CRCs are IEEE CRC32 (the value from Python `zlib.crc32`). Maximum total pack size is 65,536 bytes. Header is exactly 64 bytes:

| Offset | Field | Size | Rules |
| --- | --- | --- | --- |
| 0 | magic | 8 | `MSPIX01\0` |
| 8 | total_size | u32 | Exact file size; no trailing bytes |
| 12 | width | u16 | 16..128 |
| 14 | height | u16 | 16..128 |
| 16 | palette_count | u16 | 1..64 |
| 18 | flags | u16 | 0; unknown flags rejected |
| 20 | capabilities | u32 | 15: generic breathing, bob, tap and state feedback; no face rig |
| 24 | source_key | 32 bytes | Opaque SHA-256 asset-generation key. Default compiler key is SHA-256 of input bytes; SDK installer uses the paired avatar's image-path/revision digest. Never credentials. |
| 56 | payload_crc | u32 | CRC32 of all bytes after offset 64 |
| 60 | header_crc | u32 | CRC32 of bytes 0..59 |

Payload: `palette_count * 2` RGB565 palette bytes; `width * height` one-byte palette indices; `width * height` alpha bytes. Every index must be smaller than palette_count, including fully transparent cells. At least one alpha cell must be nonzero. Black is an ordinary palette color, not a transparency key. Alpha is 0..255 and the runtime composites it onto the selected UI background. Pixels, palette and source_key are immutable. Package boundaries, source key, counts and CRCs are validated before publication.

## Compiler and API

The first implementation is deterministic image processing, not a newly trained generative model. Input supports bounded single-frame PNG/JPEG/WebP only, at most 2 MiB encoded and 2048 pixels per edge / 4,194,304 source pixels. Reject animated/multipage files, decompression bombs and arbitrary URL fetches. Output profiles are 64x64 and 96x96 (contain-fit, no crop), 2..64 requested colors, default 24; optional palette dithering, no smoothing on enlargement. Preserve identity and alpha; do not invent missing limbs, faces or a background removal mask. Preserve the source background by default.

Library/CLI live in root `avatar_pixel/`. `python -m avatar_pixel compile --input INPUT --output OUTPUT [--profile 64|96] [--colors N] [--dither] [--source-key HEX]` writes a private pack and optional private preview/manifest. `python -m avatar_pixel serve` offers `GET /health` and `POST /v1/compile`, binds loopback by default, and is explicitly development/self-hostable service code, not a public deployment. JSON requests accept `image_base64`, `profile`, `colors`, `dither`, optional `source_key` (64 hex chars); reject unexpected fields, duplicate keys and invalid types. JSON response includes base64 pack and PNG preview plus bounded metadata. No uploaded images, credentials or generated outputs are logged or persisted by the HTTP service. Reject unbounded concurrency, oversized request bodies and non-JSON requests. Non-loopback serving requires configured bearer authentication; internet deployment requires a production reverse proxy with TLS, rate limiting and isolated workers. This session publishes no personal assets publicly.

## Device integration

Opt-in `CONFIG_MUSE_PIXEL_AVATAR` requires avatar sync, LVGL UI and PSRAM; default is off. A new installed data pack does not require firmware rebuilding once the renderer is installed. The existing original still and H.264 source remain independent fallbacks. A pixel-appearance selection chooses the installed pack; generic runtime transforms must not imply real audio for text-only replies. No individual anatomy is inferred.

USB maintenance commands are separate from the public API: expose only numeric diagnostics and opaque keys. Export the current canonical personal still with checksummed bounded RGB565 chunks, compile on the host, and install via a bounded serial begin/chunk/commit transaction. Validate exact generation/owner binding, CRCs and bounds before atomic publication; timeout/abort releases staging buffers. Pairing/VM changes make previous private data unavailable. Persist pack safely without formatting existing files data; preserve last valid data on interrupted updates. Cold boot must load a matching local pack independently of cloud connection, with bounded retries after temporary storage/allocation failures. HTTP 502 must not force the unrelated default character once a validated personal pixel pack is available.
