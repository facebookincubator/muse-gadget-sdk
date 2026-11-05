# Streaming reply speech

Enable `CONFIG_MUSE_TTS_STREAMING` to speak finished reply messages through a
provider adapter. No provider, endpoint, credentials, or voice/model settings are
bundled. Without an adapter, or with the speaker off when a message starts,
replies retain reading-paced captions. Typed console replies remain unspoken.

## Supplying a provider

Implement `muse_tts_get_provider()` from `muse_tts_provider.h` in an application
or board component linked into the firmware. Its weak default returns `nullptr`.
A strong definition returns your synthesis function, or `nullptr` if the adapter
is unavailable (for example, missing credentials). This selection hook runs on
the session task and must not block. Compile provider code into the same Muse
component, or ensure its component is linked so the strong definition is included.

The synthesis function receives owned text and a sink. It runs synchronously on
one worker and must:

1. Declare `muse_tts_format::mp3` through `sink.format` before writing. Other
   formats are rejected; adding a codec requires a corresponding decoder path.
2. Parse its transport response and write elementary MP3 bytes through
   `sink.write`. Transport framing, authentication, request schema, and response
   validation belong in the adapter. Network chunks need not align to MP3 frames.
3. Respect the sink's backpressure and stop when a sink operation returns false.
   Poll `sink.cancelled` around transport operations, use bounded network waits,
   and exclude time spent waiting on the sink from network inactivity timeouts.
4. Return success only on clean EOF, otherwise failure or cancellation. Do not
   retry after starting an audio write: part of that write may already be audible.
   Do not retain the text/sink after returning or access chat state on the worker.

The engine copies request text, assigns an identity, and passes audio through a
32 KiB PSRAM queue to the existing incremental MP3 decoder and resampler. The
session task owns decoding, captions, and speaker output; network maintenance
and cancellation remain responsive. A worker must acknowledge cancellation by
returning before its queue is reused. Stale reads/results are rejected by ID.
The worker's stack is 16 KiB in PSRAM.

## Reply behavior and limits

TTS text is stored independently of captions: at most 32 KiB including the
terminator per message and 128 KiB per turn. Authoritative final text replaces
accumulated deltas. Allocation or limit failures skip synthesis and mark the
reply incomplete rather than speaking a prefix. Caption storage and Watcher
review behavior are unchanged. Adapter/model request-size limits still apply.

Clean EOF drains compressed audio and queued PCM. A failure before audio arrives
falls back to captions; an interrupted stream is reported as an incomplete reply
without automatically replaying speech. Push-to-talk and connection teardown
cancel the request. A 30-second playback-progress watchdog detects stalls, and
total active speech is limited to ten minutes per turn. The regular turn deadline
is suspended while streaming speech is active.

Turning the speaker off during an active reply mutes PCM while audio and captions
keep progressing. Turning it on plays the remaining audio; the muted portion is
not replayed. A reply that finishes while muted ends normally.

## Validation

Host tests compile the production streaming worker against a bounded threaded
RTOS fake, feed real MP3 data to the production decoder at arbitrary chunk
boundaries, and exercise retained text and speaker toggles. Run:

```sh
python3 -m unittest discover -s tests -p 'test_muse_tts*.py'
python3 -m unittest discover -s tests -p 'test_muse_chat_session.py'
```

Hardware testing of the shared pipeline with an out-of-tree adapter received
1,465,826 MP3 bytes and played 91.61 seconds of audio, exceeding the former
512 KiB limit. Push-to-talk cancellation, mute/unmute, and recovery on a new
request after Wi-Fi interruption were confirmed. This does not exercise every
possible provider or certify its transport behavior.

For a new adapter, capture serial logs and verify:

| Test | Pass condition |
| --- | --- |
| Short spoken reply | Complete speech with no replay or reset. |
| Single 250–300 word reply with a recognizable closing sentence | Hear the closing sentence. `TTS stream ended` reports more than 524,288 MP3 bytes and `complete`. |
| Download/playback overlap | `reply audio` appears before `TTS stream ended` for the long reply. |
| Push-to-talk during speech | Old audio stops and only the new reply plays. |
| Mute for 5–10 seconds, then unmute | Captions progress silently, then the remaining speech becomes audible without an incomplete-reply error. |
| Leave muted through EOF | Reply ends normally without replaying missed speech. |
| Interrupt Wi-Fi, restore it, submit a new request | Failed request does not replay; a new request works after reconnection. |
| Repeated long replies | Endings remain intact with no allocation failure, watchdog, or reset. |

Full firmware builds require the repository's supported ESP-IDF v6.0.3 toolchain.

## Reply progress ring

The rim (or bottom bar on rectangular displays) follows the caption/playback
cursor for the current reply message. Its initial duration estimate uses the
full synthesis text, rather than the clipped caption prefix. Received MP3 refines
that estimate and decoding supplies the final audio duration.

The ring moves forward only within a message, resetting for the next one.
Unfinished progress is capped at 99%; if a duration estimate was too short, it
can hold there until playback completes. Completion is based on consumed PCM,
not download completion or decoded-ahead audio. Caption-only playback includes
the final-page hold before successful completion. Muting keeps this clock
advancing; network buffering does not. Recording, errors, and interruption clear
the reply progress. Link-only caption paging and real-time calls are unchanged.

This is approximate presentation progress, not word-level speech alignment.

For device verification, the voice task logs `reply progress: message N, P%`
at approximately 10% intervals, at the unfinished 99% cap, and at completion.
These values are published to the same state read by the UI ring. A monotonic
sequence verifies the software's progress value; visually inspect the ring too.
A short hold at 99% is expected if the initial duration estimate was too short.
