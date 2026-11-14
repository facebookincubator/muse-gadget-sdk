# Software echo cancellation for ESP32-S3 live calls

`CONFIG_MUSE_VOICE_SOFTWARE_AEC` is an opt-in way to keep a gadget's microphone
available while Muse speaks. It uses Espressif ESP-SR's full-duplex AEC at
16 kHz. The normal half-duplex mic gate remains the fallback if AEC cannot
initialize. This does not change held-button voice notes.

The call loop pairs each 20 ms mono microphone chunk with the PCM most
recently written to the speaker. A configurable delay ring aligns that
reference with the speaker echo, and a frame adapter packs both streams into
the 512-sample blocks expected by ESP-SR. The cleaned microphone stream goes
to the existing Muse live-call transport, which already handles server-side
voice activity and interruption. The watch does not run a second speech
recognizer or send a new kind of request.

## Configure a board

The board needs a microphone, speaker, PSRAM, and enough contiguous internal
memory after its network and audio tasks start. Enable `MUSE_REALTIME_VOICE`
and `MUSE_VOICE_SOFTWARE_AEC` in `idf.py menuconfig`. The latter also moves the
avatar and audio static buffers to PSRAM. Keep it disabled until the board's
speaker reference delay and self-interruption behavior are measured.

`MUSE_VOICE_AEC_REF_DELAY_CHUNKS` is the delay in 20 ms chunks between a
speaker write and the corresponding echo arriving in the mic. The Waveshare
ESP32-S3-Touch-AMOLED-2.06 measured about 50 ms; two chunks (40 ms) worked
in testing. Different codecs, DMA
queues, speaker placement, and mic placement can require another value.

During playback, frames whose cleaned level is below -55 dBFS are sent as
silence to the voice detector. Above that level, the cleaned samples are sent
so the user can interrupt Muse without touching the gadget. If the board's
AEC fails to start, the existing half-duplex behavior resumes for that call.

## Validation before enabling by default

1. Record the free internal heap and largest block during a live call. Check
   that AEC starts, the call stays connected, and no watchdog or panic occurs.
2. Ask Muse for a long reply and stay silent. It must finish without hearing
   its own speaker as a new user turn.
3. Speak over Muse twice in the same call. Both interruptions should reach
   Muse, and a full reply should still play afterward.
4. Check Speaker off, reconnect, long idle, battery use, and varying speaker
   volume. Keep the feature opt-in until these pass on that board.

## Bench results (one watch)

The first physical bench was on a Waveshare 2.06 watch using a local
development build before this upstream branch was rebased. Moving its static
avatar/audio buffers to PSRAM raised the largest free internal block from
about 7 KB to 23 KB at idle. The AEC created successfully; after its
dependency linked, it used about 8.5 KB additional internal RAM in the call.
In sampled silent-user playback windows, raw microphone levels of about
-32 to -48 dBFS fell to about -81 to -100 dBFS after cancellation; near-end
speech measured about -29 to -31 dBFS. These sparse samples are not a
controlled ERLE or speech-quality benchmark. A silent-listener call completed
normally, and repeated voice interruptions worked in one call. On the final
local development image, two interruptions in the same call worked without
Muse cutting itself off. The cleaned upstream branch builds but has not been
flashed. Initialization fallback was exercised in the host harness, not forced
on the watch. These results do not establish performance on other ESP32-S3
layouts or long-term battery behavior.
