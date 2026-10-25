# Muse Gadget + ElevenLabs TTS — Waveshare LCD 1.54

This optional add-on speaks Muse's **finished reply text** without changing
the Muse/Jollybot avatar, faces, UI, captions, pairing or OTA protocol.
It reuses Muse's existing \`minimp3\` decoder, 16 kHz resampler and
\`MUSE_MODE_SPEAKING\` transitions.

- On by configuration, **off by default**. The existing silent caption
  pacing remains the fallback on missing configuration, mute, HTTP errors
  or stalled TTS requests.
- Audio transport is a separate worker so Muse's Noise socket/keepalive
  task is not blocked by ElevenLabs.
- Device → proxy: verified **HTTPS**, POST \`/v1/tts\`, bearer token,
  JSON \`{"text": "..."}\`, streaming \`audio/mpeg\`.
- Proxy → ElevenLabs: \`POST /v1/text-to-speech/{voice_id}/stream\`,
  format \`mp3_22050_32\`, model default \`eleven_flash_v2_5\`.
- The **ElevenLabs API key is only in the proxy**. ESP32 contains an
  *independent* access token for the proxy, not the vendor key.
- Captions track decoded PCM duration. TTS uses the voice selected on
  the proxy, not a new visual character.
- The SDK caps speech to 1023 bytes per finished Muse message; longer
  text would be truncated by upstream caption storage too.

## 1. Run the proxy with Docker

On your home server, clone the repository and switch to
\`feat/elevenlabs-tts-proxy\`. From the repository root:

\`\`\`sh
cd tools/muse/tts-proxy
cp .env.example .env
openssl rand -hex 24
\`\`\`

Edit \`.env\` with these values (never commit it):

\`\`\`dotenv
ELEVENLABS_API_KEY=YOUR_ELEVENLABS_API_KEY
ELEVENLABS_VOICE_ID=YOUR_VOICE_ID
ELEVENLABS_MODEL_ID=eleven_flash_v2_5
TTS_PROXY_TOKEN=LONG_RANDOM_TOKEN_FROM_OPENSSL
\`\`\`

Then run:

\`\`\`sh
docker compose up -d --build
docker compose logs --tail=30 muse-tts
curl -fsS http://127.0.0.1:8765/health
\`\`\`

The Compose port is bound to **127.0.0.1:8765** by default, so it is not
reachable from your ESP32 or the public Internet yet. Publish it through
your reverse proxy / Pangolin / Traefik with valid TLS, forwarding
\`https://tts.example.com/v1/tts\` to the service. If the reverse proxy
runs in another container, put both services on a trusted private Docker
network rather than publishing unauthenticated HTTP. The endpoint still
requires a separate bearer token; never bypass it.

**Important:** no Authentik login redirect, HTTP endpoint, self-signed
certificate, or API key in the device. A valid public CA certificate must
be presented to the ESP32 (ESP-IDF cert bundle).

Optional live smoke test (this consumes ElevenLabs usage):

\`\`\`sh
curl --fail-with-body -X POST http://127.0.0.1:8765/v1/tts \
  -H "Authorization: Bearer $TTS_PROXY_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"text":"Hola, este es Muse hablando con ElevenLabs."}' \
  -o /tmp/muse-tts-test.mp3
file /tmp/muse-tts-test.mp3
\`\`\`

Do not print the API key or the proxy token in logs or screenshots.

## 2. Enable TTS on the Waveshare (ESP-IDF v6.0.1)

On the Mac, in \`esp32/\`, with ESP-IDF activated:

\`\`\`sh
idf.py -B build-muse-waveshare-s3-lcd154 \
  -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-muse-waveshare-s3-lcd154/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-lcd154" menuconfig
\`\`\`

Under **Muse**, enable **Speak Muse replies with an HTTPS TTS proxy**,
then enter:
- URL: \`https://tts.example.com/v1/tts\` (your real HTTPS hostname).
- Proxy token: the exact same \`TTS_PROXY_TOKEN\` configured on the server.

The \`sdkconfig\` file in the ignored local build directory contains the
token and must **never** be committed to GitHub or shared. To check without
printing it:

\`\`\`sh
grep -q '^CONFIG_MUSE_TTS_PROXY=y$' build-muse-waveshare-s3-lcd154/sdkconfig && echo "TTS proxy enabled"
\`\`\`

Compile and install **only after the build succeeds**:

\`\`\`sh
tools/muse/board.sh build lcd154
# Use the verified device /dev/... port; no erase-flash is required.
tools/muse/board.sh flash lcd154 /dev/cu.usbmodemXXXX
\`\`\`

Keep the existing NVS to preserve Muse pairing. For serial logs, check
\`muse_tts_proxy\`, \`muse_chat_session\`, and \`muse_audio\`.

## 3. Check the audio flow

1. Settings → Sound: verify the existing *chirp* and enable Speaker.
2. Ask Muse a short question on the Waveshare.
3. The reply text arrives normally; the TTS proxy sends MP3 pieces.
4. The speaker reads 16 kHz PCM from the original decoder/resampler.
5. Jollybot displays the **original** SPEAKING animation; captions advance
   from played PCM frames.
6. Disconnect the proxy: after a failure/timeout, silent captions still work.

The proxy uses a 25-second stall timeout on the device and limits a single
MP3 reply to 512 KiB. This is a first integration; timing and 2nd-message
behavior still require tests on the actual device.

## 4. Automated unit tests (no ElevenLabs credits)

\`\`\`sh
python3 -m unittest discover -s tools/muse/tts-proxy/tests -v
\`\`\`

This mocks the ElevenLabs HTTPS API and checks authorization,
MP3-byte streaming, rejected inputs and sanitized upstream errors.
A complete ESP-IDF build is still required on the Mac and is **not**
provided by these Python tests.
