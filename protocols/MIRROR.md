<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Cable mirror protocol

The console reader in `muse_input.c` owns the session. Integers are little
endian. The cable carries screen rectangles, touch, button and text input, and
sound on a native USB console or a UART at 460800 baud or more, with ESP_LOG
lines between frames. There is one viewer per console.

## Session

Open the board's console at its configured baud rate (normally 115200 for
UART) without a reset: the viewer sets DTR before opening, as POSIX raises it
first, then clears both lines. Send four `FF` bytes, wait
500 ms, then send `\n>mirror\n`. The FF bytes end an old session; they are
not an electrical BREAK. The leading `>` is required by Muse's console.
Bare `mirror` would invoke the console's single-character commands.

`>mirror=N` asks a UART console to run the session at N baud: 230400,
460800 or 921600. The board writes the same reply at the console's own
speed, changes its speed once the reply has left, and waits 150 ms before
it sends anything else; the viewer changes its side when it has read the
reply. The console returns to its own speed when the session ends. A
native USB console and any other N keep the speed and still start the
session.

The board replies with `00 4d 49 52 52 4f 52 31 0a` (`\0MIRROR1\n`).
The viewer waits up to five seconds. From that point board-to-host messages
have this envelope:

| Field | Bytes |
|---|---|
| Sync | `A5 5A` |
| Length | u32, payload size |
| Payload | length bytes |
| CRC32 | u32, over length bytes followed by payload |

CRC32 matches `zlib.crc32(length_bytes + payload)` and chained
`esp_rom_crc32_le` with seed zero. A payload of `R` gives
`a55a010000005275ee277e`. The viewer accepts 1..65536 payload bytes and
checks CRC before applying them. Corrupt lengths, CRC failures and a frame
incomplete after three seconds cause resynchronization and an `R` refresh.

Host-to-board messages use `length:u32 + payload`, without sync or CRC.
The limit is 1024 bytes. Apart from `S`, the firmware drains and ignores
payloads larger than 64 bytes. Header completion and payload reads each
have a two-second deadline. Six seconds without a complete host message
ends the session; the viewer sends `02` after about five idle seconds.

The mirror task owns input processing and sends. Its RX queue holds 32
messages. Repeated pointer motion may be dropped when full; edges and key
messages wait up to three seconds. Disconnect releases held keys and
queues a pointer release. The console reader waits for the final queued
BYE to be consumed, drains trailing input, then restores its log hook and
normal command processing. Normal end sends `E`; failed TX may prevent it.

Each frame and formatted ESP_LOG record shares a TX mutex. Writes use at
most 128 bytes and advance by the accepted count. Logging waits at most
50 ms for that mutex and has a separate 50 ms TX budget. Dropped log records
are counted. ROM output and direct `printf` bypass the hook and can corrupt
a frame. The viewer prints timestamped console lines and can append them
with `--log FILE`.

The native USB driver has timed writes. Muse's UART adapter has no TX ring
or hardware flow control; each write waits for wire time. At 115200 baud a
32 KiB frame, the largest, takes about 2.85 seconds before other overhead,
within the three seconds a frame may take to send.

A viewer that sends `Z` takes deflated messages. The board then sends a
screen message of 256 bytes or more as `Z` followed by the raw deflate
stream (RFC 1951, no zlib header) of the whole message, its `F` included,
when that is shorter. The viewer inflates at most 65536 bytes and handles
the result as the message it is. Each `Z` stands alone: no dictionary is
carried between messages, so a lost one costs nothing after it. The board
uses the compressor in the chip's ROM.

## Board metadata

Admission sends `B` followed by UTF-8 JSON without a trailing NUL:

```json
{"name":"AIPI Lite","w":128,"h":128,"round":false,"diag":1.44,"touch":false,
 "keyboard":false,"audio":true,
 "buttons":[{"role":2,"name":"BOOT","x":-1,"y":-1}]}
```

Geometry comes from the LVGL display; other fields come from `muse_board`.
`diag` is the diameter in inches for a round screen, otherwise its diagonal.
`(-1,-1)` means no button position hint. `keyboard` describes navigation
keys, not text insertion. `audio` is decided for each session; see Sound.

| Key | Muse role |
|---|---|
| 1 | Talk |
| 2 | Auxiliary |
| 3 | Power |
| 4 | Speaker |
| 5..10 | Up, Down, Left, Right, Enter, Escape |

Roles describe input events; the physical board's UI decides their action.
Navigation roles are press events. Talk, auxiliary, power and speaker
support separate press and release edges.

## Board payloads

`F` carries `state:u8, rects:u16, width:u16, height:u16`, then rectangles.
State is 0 bright or 2 dark; the decoder also accepts 1 dim. Bit 7 set
says the rest of the same screen follows in the next `F`: one update too big
for a message goes out in several, all taken from one snapshot, and the
viewer shows them when the last one, without bit 7, arrives. Empty F
heartbeats carry state each second. `E` has no body and ends the session.
`O open:u8` reports whether a visible text field is focused (0 or 1), only
after the viewer opts in with `O`. It is sent initially and when the state
changes. `A source:u8` and, on a UART, `a source:u8` carry sound, see Sound.
A viewer ignores a board payload it does not know; `MIRROR1` changes only
with the envelope.

Each rectangle is `x:u16,y:u16,w:u16,h:u16,coding:u8,reserved=0:u8,length:u32`,
then its payload. The nonempty rectangle must fit inside the screen.
RGB565 uses R bits 15..11, G bits 10..5 and B bits 4..0.

| Coding | Payload |
|---|---|
| 0 | Raw RGB565 u16 in row order |
| 1 | `(count:u16,pixel:u16)` runs in row order |
| 2 | The same runs in column order |

Counts are nonzero and sum to w*h; runs may cross traversal line boundaries.
The decoder must consume exactly w*h pixels and all payload bytes. Every
rectangle is absolute: none depends on an earlier message.

On `R`, the board marks the whole screen dirty and sends B before more F
messages. The viewer resets its coverage on B. This is the boundary after
any old in-flight frames. Unsent bands precede new dirty rectangles. There is no frame sequence or full-screen marker; the viewer
tracks coverage before saving a screenshot.

Capture copies LVGL flushes only while connected. A new session waits for
a full LVGL redraw before sending pixels. LVGL remains running for the
viewer while the physical display is asleep. Rendering remains at the
board's normal rate; encoding is limited to one pass per 40 ms and cable
bandwidth can make delivery slower.

## Host payloads

| Bytes | Action |
|---|---|
| `00 x:u16 y:u16` | Pointer release |
| `01 x:u16 y:u16` | Pointer press or move |
| `02` | Keepalive |
| `03 key:u8` | Tap, released after about 150 ms |
| `06 key:u8 down:u8` | Hold/release, down is 0 or 1 |
| `R` | Refresh the full screen |
| `O` | Opt into text-field state reports |
| `I utf8:1..63 bytes` | Insert UTF-8 text into the focused field |
| `K edit:u8` | 1 Backspace, 2 Delete, 3 Left, 4 Right, 5 Enter |
| `Z` | Opt into deflated screen messages |
| `07 source:u8` | Listen: 0 stop, 1 the microphone, 2 the speaker |
| `09 mode:u8` | Speaker: 0 plays on the board, 1 for the viewer only |
| `0A mode:u8` | Microphone: 0 the board's, 1 the board's and the viewer's, 2 the viewer's |
| `S 02 pcm` | The viewer's microphone, 20 ms of PCM16 in 640 bytes, on the chip's own USB |
| `S 03 adpcm` | The viewer's microphone as one 164-byte IMA ADPCM chunk, on a UART |

Out-of-range coordinates, unknown roles and unknown payloads do nothing.
Pointer press, first movement and release are separate LVGL samples;
later movements may coalesce. A held key remains down while either the
held-key bit or its pending tap is active.

## Sound

Sound is PCM16 mono at 16 kHz, little endian, the format of `muse_audio`.
A board with a microphone or a speaker offers it (`audio` true) when its
console is the chip's own USB, or a UART in a session at 460800 baud or
more. A slower session reports it false and ignores `07`, `09` and `0A`.

A UART session sends the board's sound as IMA ADPCM, 8 KB/s instead of 32:
`a source:u8`, then `predictor:i16, index:u8, 0`, then 160 bytes with two
4-bit codes each, the first sample in the low half. The predictor and step
index at the head of every message are the encoder's state before its
first sample, so a message decodes without the ones before it.

`07` picks what the viewer hears. The board then sends `A source:u8`, the
current `07` source, followed by 640 bytes, 20 ms (on a UART the `a` message
above), up to 16 messages between two screen passes. Samples wait in a 16 KiB ring; a viewer more than the ring behind
continues from its newest half. The speaker source is what Muse plays,
before the speaker-off setting and `09` silence it on the board. The
microphone source is what Muse records, after its high-pass filter and
after `0A` mixing. The board captures only while Muse records, so the
microphone source is silent between recordings.

`09 01` keeps the board's speaker silent while playback continues at its
normal pace. `0A` feeds the viewer's microphone to Muse: mode 1 adds it to
the board's samples with clipping, mode 2 replaces them, with silence where
the viewer sent nothing. On the chip's own USB the viewer sends `S 02` and 20 ms
of PCM, 640 bytes; on a UART `S 03` and one ADPCM chunk in the layout above,
20 ms in 164 bytes. The samples wait in a 128 ms ring
that keeps the newest 100 ms when full, and are dropped while the mode is
0. `S` goes from the console's task straight to that ring, not through the
message queue. Other formats and `S` payloads of another length are
ignored.

Changing the source or the microphone mode discards what the ring held.
Disconnect stops listening and restores the board's speaker and microphone.

## Text input

The viewer sends `O` after each board description and clears its field-open
state until the board answers.

Insert chunks end on a UTF-8 character boundary and contain no NUL bytes.
The firmware rejects malformed, overlong, surrogate and out-of-range UTF-8,
empty inserts, inserts over 63 bytes, and unknown edit codes. Text received
with no open field is discarded. At most 32 edits wait for the LVGL task;
a send waits up to three seconds for room, then drops the viewer if the
queue remains full. Disconnect and admission of a new viewer clear queued
edits and reset report negotiation.

The LVGL task applies at most eight edits per tick, rechecking the focused
visible field for each edit. A field in the top layer takes priority over
the active screen. Native textarea character and length constraints apply.
Muse's compact keypad commits any pending multi-tap character before an
external edit; Enter sends its normal Ready event. A standard LVGL keyboard
receives Ready for Enter; without a keyboard, one-line fields receive Ready
and multiline fields receive a newline. Text is not logged by the mirror.
