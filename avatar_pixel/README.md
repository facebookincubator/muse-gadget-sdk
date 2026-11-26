# Pixel avatar converter and self-hostable API

This is **converter/library and development/self-hosting service code, not a
public hosted deployment**. It implements the frozen
[`../protocols/PIXEL_AVATAR.md`](../protocols/PIXEL_AVATAR.md) v1 contract. No
network image fetching, generative model, training, anatomy inference, face rig,
background removal, device connection, firmware build, or public service is
included. All test art is synthetic robot, animal, plant and geometric logo art
created with Pillow; no default Meta character or private avatar is included.

## Run locally

Python 3.10+ and Pillow are required (validated with Python 3.12 / Pillow 13).
From the SDK root, with a Python environment that already has Pillow:

```sh
python -m avatar_pixel compile --input local.png --output avatar.mspix \
  --profile 96 --colors 24 --preview preview.png --manifest manifest.json
python -m avatar_pixel serve --port 8080
```

`--profile` is 64 (default) or 96; `--colors` is an integer 2..64 (default 24).
`--dither` enables deterministic fixed-point Floyd-Steinberg palette dithering.
`--source-key` accepts exactly 64 hex characters. Without it, the generation
key is SHA-256 of the **original encoded input**, not resized pixels. The key is
opaque caller data, not a credential, path, ownership proof or trusted-source
bypass. A paired-device installer may supply its own generation digest, but
must perform its own pairing/generation validation outside this converter.

CLI input is a bounded regular local file. Output files are **new only** and
created with mode 0600; existing files/symlinks are never overwritten. Failed
multi-file writes roll back only files created by that invocation. Destination
directories must already exist. Optional PNG/JSON files are explicit, private
CLI exports; the HTTP service never creates them.

```python
from avatar_pixel import compile_image, validate_pack

# Caller is responsible for reading a private local input with a bounded read.
result = compile_image(image_bytes, profile=64, colors=24, dither=False)
metadata = validate_pack(result.pack)
# result.pack / result.preview_png / result.manifest stay in caller memory.
# Do not log them or treat source_key as a trusted identity assertion.
```

`AvatarError.code` is a fixed, input-free error code. `Compilation` hides private
bytes and its manifest from `repr`. The compiler performs no file/network I/O.
`validate_pack` checks byte boundaries, dimensions, flags/capabilities, both
CRCs, every palette index (even transparent cells), and nonempty A8 alpha.

## Image processing / generic runtime data

- Only static single-frame PNG, JPEG and WebP; at most 2 MiB encoded, 2048 per
  source edge and 4,194,304 source pixels. Animated/APNG/multipage images,
  malformed decodes, bombs and other decoders are rejected. EXIF orientation
  is respected; EXIF/ICC/text metadata is not copied into the PNG preview.
- Contain-fit into a centered 64x64 or 96x96 transparent canvas, **no crop**.
  Enlargement uses nearest-neighbor; reduction uses Pillow BOX resampling.
  Source backgrounds remain, with no inferred mask. Original A8 transparency
  is resized, not thresholded; black is a normal color. Extra contain-fit
  padding is transparent. Fine detail may disappear during downsampling.
- Fully transparent RGB does not consume the visible palette budget. If all
  distinct visible RGB565 values fit, they are preserved exactly. Otherwise,
  Pillow median-cut selects bounded clusters; unique source RGB565
  representatives prevent palette collapse. Nearest-color mapping has exact
  RGB565 matches and stable ties. Optional dither leaves A8 independent and
  discards color error at fully transparent cells. Lossy quantization can merge
  some colors; distinctions below RGB565 precision cannot be preserved.
- The PNG preview is **the actual packed RGB565 + A8 sprite**, expanded 4x using
  nearest-neighbor (256x256 or 384x384), not a smoothed original approximation.
  Both pack and preview are bounded. No timestamps/random seeds/version fields
  are inserted. Same input/options and supported Pillow build produce identical
  bytes; pin the Pillow version/build for reproducibility across deployments.
- Packs contain generic immutable data only. Runtime breathing, bobbing, tap
  and state feedback are represented by capability mask 15, not executable
  code or per-avatar anatomy. Device rendering and binding are separate work.

## HTTP API

`GET /health` returns `{"status":"ok"}`. `POST /v1/compile` accepts UTF-8 JSON:

```json
{
  "image_base64": "<standard canonical base64 of local PNG/JPEG/WebP bytes>",
  "profile": 64,
  "colors": 24,
  "dither": false,
  "source_key": "<optional 64 hex characters; omit for input SHA-256>"
}
```

Only `image_base64` is required. Omit optional fields for defaults. Explicit
`null`, booleans in numeric fields, floats, nonfinite values, duplicate keys
(including escaped duplicates), unexpected fields and invalid types are
rejected. No `image_url`, URL/data-URI base64, trusted overrides, prompt, file
path, background-removal or arbitrary runtime parameters are accepted.

Response keys:

- `pack_base64`: standard base64 of the exact MSPIX01 binary.
- `preview_png_base64`: standard base64 of the nearest-scaled RGBA PNG.
- `metadata`: bounded numeric/boolean fields, an opaque source-key digest,
  `format`, CRCs, dimensions, palette count, source dimensions, `content_box`
  as `[left, top, right, bottom]` (right/bottom exclusive), requested color
  budget, dither and preview dimensions/size. No image path, source metadata,
  credentials, timestamp or private text is included.

Real local HTTP example (no remote image input):

```python
import base64
import http.client
import json

with open("local.png", "rb") as source:
    encoded = source.read(2 * 1024 * 1024 + 1)
body = json.dumps({"image_base64": base64.b64encode(encoded).decode("ascii"),
                   "profile": 96, "colors": 24, "dither": False})
connection = http.client.HTTPConnection("127.0.0.1", 8080, timeout=15)
try:
    connection.request("POST", "/v1/compile", body,
                       {"Content-Type": "application/json"})
    response = connection.getresponse()
    result = json.loads(response.read(800 * 1024 + 1))
    assert response.status == 200, result.get("error")
    pack = base64.b64decode(result["pack_base64"], validate=True)
finally:
    connection.close()
# Keep result/pack private; do not log or persist HTTP uploads/outputs.
```

Errors are small JSON `{"error":"fixed_code"}` without input/decoder detail.
Invalid JSON/images/options return 400; framing/body overflow 413; media type
or content encoding 415; bearer refusal 401; invalid Host/Origin 403;
unsupported paths 404; unsupported methods 501; Expect 417; read timeout 408;
capacity overflow 503; worker wall timeout 504; worker failure 500/503.
Absolute connection expiry can close the connection before an error is sent.
All normal responses use `Cache-Control: no-store`, `Connection: close` and
`X-Content-Type-Options: nosniff`.

### Privacy, security and operational bounds

- Default bind is `127.0.0.1:8080`. Only literal IPs or `localhost` are accepted
  for bind, with no DNS lookup. Binding any non-loopback IP, including wildcard
  IPv4/IPv6, **requires bearer auth**. Configure a 32..256-character ASCII token
  (`A-Z a-z 0-9 . _ ~ -`) in `AVATAR_PIXEL_BEARER_TOKEN` or an environment
  variable named by `--token-env`; never pass a token on the command line.
  Configured auth protects **both** health and compile routes, even on loopback.
  Send `Authorization: Bearer <token>`. Tokens are compared in constant time.
- No HTTP request/access/exception logs, private bodies, output logs, temp image
  files, caches or persisted uploads. Workers receive no inherited credential
  environment. Each compile runs in a new Python subprocess with in-memory
  stdin/stdout and discarded stderr; timed-out workers are killed and reaped.
  Unix service construction disables parent-process core dumps before accepting
  uploads too; this process-wide privacy setting also applies when embedding
  `make_server` in another application. Only the CLI service startup prints a
  literal bind address/port, never images,
  tokens or generated output. OS swap, tracing and external proxies remain the
  operator's responsibility; use an appropriately private host.
- Bounded JSON body: 2,800,300 bytes (2 MiB canonical base64 plus 4096 bytes of
  JSON overhead). Exact single Content-Length required; duplicates, chunked
  Transfer-Encoding, Expect and content encodings are rejected. No keep-alive.
  Python's HTTP parser also bounds header lines/count. Host must name the
  numeric local socket address (or `localhost` for loopback) and actual port;
  a proxy should rewrite upstream Host accordingly. Origins are prohibited,
  with no CORS headers: this API is not a browser-upload endpoint.
- Default **2 admitted connections**, configurable 1..8, bounded before any
  handler thread/body read; overflow gets 503. Default 3s I/O inactivity,
  15s absolute connection/header/body deadline, 5s compile wall timeout.
  `--max-connections`, `--io-timeout`, `--request-timeout` and
  `--compile-timeout` accept finite bounded settings; compile must be strictly
  shorter than request deadline and at most 30s. Slow headers cannot evade the
  absolute deadline. Each connection has at most one compile worker.
- **Per-platform worker limits:** Linux requires hard/soft `RLIMIT_AS` ceilings
  of 512 MiB, CPU of 30s, regular-file writes of zero and core dumps of zero.
  A limit failure or missing Unix `resource` support fails closed **before
  reading the request**; Linux memory protection is never silently disabled.
  **Darwin/macOS does not set or claim an `RLIMIT_AS` virtual-memory cap**:
  macOS cannot reliably apply that ceiling. Darwin still requires CPU, file
  and core limits, plus parent-process core-dump suppression. Other Unix
  platforms attempt all four limits and fail closed if unsupported; Windows
  without `resource` has no Unix rlimits. Every platform retains bounded image
  decoding, JSON/request/response sizes, admitted connections, absolute I/O
  deadlines and compile wall-timeout kill-and-reap.
  These application bounds are **not a hardened decoder sandbox or a hard
  memory cap on Darwin/Windows**. Production use requires externally isolated
  workers with enforced process/memory budgets (for example a Linux container
  or VM with memory controls), especially on platforms without an address-space
  ceiling. API code makes no external network connection; no hidden public
  egress or default-character fallback exists.
- Pack cap 65,536 bytes; preview cap 512 KiB; complete JSON response cap 800 KiB.
  Compiled profiles fit comfortably under the pack cap (96px/64 palette colors
  = 18,624 bytes). Inputs and outputs never enter a persistent queue.
- **Internet deployment is not provided.** It requires a production reverse
  proxy with TLS, rate limiting, strict upstream Host, private/no-body logging,
  isolated workers, process/memory controls and an operator security review.
  Local bearer HTTP is not TLS. Do not expose this development listener
  publicly as-is. No public service is deployed by these tests or this code.

## Binary layout

Header: `struct.Struct("<8sIHHHHI32sII")`, exactly 64 bytes. Magic
`MSPIX01\0`; exact total file size; width/height; palette count (actual 1..64,
which may be below requested 2..64 for a uniform source); flags 0; capabilities
15; 32-byte generation digest; IEEE payload CRC32; IEEE CRC32 of header bytes
0..59. Payload is **little-endian RGB565 palette**, then row-major one-byte
indices, then row-major A8 alpha. No trailing bytes. Every index is valid and
at least one alpha cell is nonzero. RGB565 is red bits 11..15, green 5..10,
blue 0..4; preview expands by bit replication.

## Tests / integration / extension points

From SDK root (no firmware/device/USB access, network downloads or new packages):

```sh
PYTHONDONTWRITEBYTECODE=1 python -B -m unittest discover \
  -s avatar_pixel/tests -p 'test_*.py' -v
python -m py_compile avatar_pixel/__init__.py avatar_pixel/compiler.py \
  avatar_pixel/protocol.py avatar_pixel/api.py avatar_pixel/_worker.py \
  avatar_pixel/__main__.py
```

Tests cover synthetic diverse shapes; PNG/JPEG/WebP; alpha/background and black;
RGB565 color distinction; deterministic packs/previews; both profiles/color
limits; malformed/bomb/animated inputs; exact header/CRCs and bad transparent
indices; strict JSON types/duplicates/URL refusal; real HTTP compile with fresh
subprocess workers; bearer/Host/Origin/framing guards; bounded connections,
slow input/worker timeouts; terminal EOF/reset closure (not a read timeout);
mocked Darwin valid/invalid requests with unsupported AS limits; Linux AS
fail-closed protection; no private logs/files; private new-only CLI exports;
and a real CLI service in an ephemeral local process/port, torn down afterward.
Test filesystem scratch is confined to new `.test-*` directories within this
package and removed at test completion.

Copy only `avatar_pixel/` into the SDK root to integrate. The converter is
independent of the C reader/ESP32 backend and private paired-device test; it
neither duplicates those components nor changes protocol/firmware files.
Future style algorithms belong behind explicit versioned compiler options,
with security bounds/determinism tests retained. Any future generative model,
remote input, anatomy rig or background-removal behavior would need a separate
reviewed contract, not a silent change to v1.
