/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * mgcommands.h: the Muse Gadgets BLE protocol between a gadget and its
 * client app (commands, wire formats, and what each side must do). Values
 * it doesn't define are reserved.
 */

#ifndef MGCOMMANDS_H
#define MGCOMMANDS_H

#include <stdint.h>

#define MG_SPEC_VERSION 1

#define MG_SERVICE_UUID "65E6635D-174A-4AF0-805E-D1EDED7C2A63"
#define MG_CONTROL_UUID "732CE4AB-9F36-4F11-9147-0BAC4FCE97C4"
#define MG_DATA_UUID    "5B84AFDF-3B31-4FEF-89E6-514A4A97ABB3"

// The same UUIDs as little-endian byte lists, the order they take in
// advertising data and in most embedded BLE stacks (e.g. NimBLE's
// BLE_UUID128_INIT).
#define MG_SERVICE_UUID_LE_BYTES                                             \
  0x63, 0x2a, 0x7c, 0xed, 0xed, 0xd1, 0x5e, 0x80, 0xf0, 0x4a, 0x4a, 0x17,   \
      0x5d, 0x63, 0xe6, 0x65
#define MG_CONTROL_UUID_LE_BYTES                                             \
  0xc4, 0x97, 0xce, 0x4f, 0xac, 0x0b, 0x47, 0x91, 0x11, 0x4f, 0x36, 0x9f,   \
      0xab, 0xe4, 0x2c, 0x73
#define MG_DATA_UUID_LE_BYTES                                                \
  0xb3, 0xab, 0x97, 0x4a, 0x4a, 0x51, 0xe6, 0x89, 0xef, 0x4f, 0x31, 0x3b,   \
      0xdf, 0xaf, 0x84, 0x5b

// Standard services every musegadgets device also exposes.
#define MG_BAS_SERVICE_UUID16           0x180F
#define MG_BAS_BATTERY_LEVEL_UUID16     0x2A19
#define MG_DIS_SERVICE_UUID16           0x180A
#define MG_DIS_MANUFACTURER_NAME_UUID16 0x2A29
#define MG_DIS_MODEL_NUMBER_UUID16      0x2A24
#define MG_DIS_SERIAL_NUMBER_UUID16     0x2A25
#define MG_DIS_HARDWARE_REVISION_UUID16 0x2A27
#define MG_DIS_FIRMWARE_REVISION_UUID16 0x2A26
#define MG_DIS_SOFTWARE_REVISION_UUID16 0x2A28

#define MG_NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define MG_NUS_RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define MG_NUS_TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
#define MG_NUS_SERVICE_UUID_LE_BYTES                                         \
  0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5,   \
      0x01, 0x00, 0x40, 0x6e
#define MG_NUS_RX_UUID_LE_BYTES                                              \
  0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5,   \
      0x02, 0x00, 0x40, 0x6e
#define MG_NUS_TX_UUID_LE_BYTES                                              \
  0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5,   \
      0x03, 0x00, 0x40, 0x6e

// Clients negotiate at least this ATT MTU; audio frames fit comfortably.
#define MG_MIN_ATT_MTU 100

// The Core spec caps an attribute value at 512 octets whatever the MTU, so
// every write and notification is at most min(ATT_MTU - 3, MG_MAX_ATT_VALUE)
// bytes; "ATT_MTU - 3" in this file always means that. (Android refuses a
// longer write; CoreBluetooth's maximumWriteValueLength is at most 512.)
#define MG_MAX_ATT_VALUE 512

// Advertised name prefix clients may use as a fallback filter: the SDK's
// boards are MuseGadget-XXXXXX, products in the Muse Gadget catalog
// MuseGadget-<PRODUCT>-XXXXXX (PRODUCT: the catalog model code; XXXXXX: the
// last three bytes of the BLE address, uppercase hex).
#define MG_DEVICE_NAME_PREFIX "MuseGadget"

// Audio defaults when change_data_type carries no format parameters.
#define MG_AUDIO_DEFAULT_SAMPLE_RATE_HZ 16000
#define MG_AUDIO_DEFAULT_CHANNELS       1
#define MG_LC3_DEFAULT_FRAME_US         10000
#define MG_LC3_DEFAULT_FRAME_BYTES      40
// The higher LC3 rate a device may also offer, for playback and capture,
// only when a client asks for it (see Audio formats, Capture formats and
// Audio playback). Recommended there: 32 kb/s, MG_LC3_DEFAULT_FRAME_BYTES
// per 10 ms, the same airtime as at 16 kHz.
#define MG_LC3_HIGH_SAMPLE_RATE_HZ      24000
// LC3 frame size limits (liblc3): 16-320 kb/s at 10 ms.
#define MG_LC3_MIN_FRAME_BYTES          20
#define MG_LC3_MAX_FRAME_BYTES          400
// A capture's bitrate when nothing asked for another (40 B per 10 ms in LC3).
#define MG_CAPTURE_DEFAULT_BITRATE_KBPS 32

/*
 * =============================================================================
 * musegadgets BLE Protocol
 * =============================================================================
 *
 * A small command protocol for BLE gadgets that stream push-to-talk audio to
 * a client (phone app, browser, or gateway). The gadget is the GATT server.
 * Multi-byte integers are little-endian.
 *
 * =============================================================================
 * Advertising
 * =============================================================================
 *
 *   Advertising data:  Flags, and the Complete List of 128-bit Service UUIDs
 *                      containing MG_SERVICE_UUID. Clients scan by this UUID.
 *   Scan response:     Complete Local Name (the GAP Device Name), which
 *                      should start with MG_DEVICE_NAME_PREFIX, e.g.
 *                      "MuseGadget-1A2B3C", or for a catalog product its
 *                      model code too, e.g. "MuseGadget-XN54-1A2B3C".
 *                      Appearance and the 16-bit BAS UUID may be added if
 *                      they fit.
 *   Muse Link setup:   a gadget is set up over the Link setup service, which
 *                      it advertises from the same address, taking turns,
 *                      until it is set up; its manufacturer data flags byte
 *                      has bit 1 set (Gadget setup v2).
 *
 * =============================================================================
 * GATT Service Definition
 * =============================================================================
 *
 * Primary Service: musegadgets
 *   Service UUID: 65E6635D-174A-4AF0-805E-D1EDED7C2A63
 *
 *   Characteristic 1: Control
 *     UUID: 732CE4AB-9F36-4F11-9147-0BAC4FCE97C4
 *     Properties: Write Without Response, Notify
 *     Permissions: Write
 *     CCC Descriptor: Read, Write
 *     Description:
 *       Command channel. The client writes mg_command_t commands here (first
 *       byte = command ID, remaining bytes = parameters). The device notifies
 *       responses, status updates, and data type changes. Each Control write
 *       or notification is exactly one whole message, never split across
 *       several: a device sizes its notifications from the MTU the link
 *       actually agreed (the answer to any Exchange MTU, either side's), and
 *       a message that doesn't fit isn't sent.
 *
 *   Characteristic 2: Data
 *     UUID: 5B84AFDF-3B31-4FEF-89E6-514A4A97ABB3
 *     Properties: Write Without Response, Notify
 *     Permissions: Write
 *     CCC Descriptor: Read, Write
 *     Description:
 *       Bulk data channel. The device notifies payloads (audio, offline clips)
 *       whose type is set by the most recent mg_command_change_data_type
 *       notification on Control. Payloads are chunked to ATT_MTU - 3 bytes.
 *       For live audio, a notification carries whole codec frames only, so a
 *       client can decode each notification on its own.
 *       During audio playback the client writes the encoded audio here (see
 *       Audio playback).
 *
 * Primary Service: Battery Service (standard BLE BAS), required
 *   Service UUID: 0x180F
 *   Characteristic: Battery Level (0x2A19), Read + Notify, uint8_t 0-100.
 *   Devices without a battery report 100.
 *
 * Primary Service: Device Information Service (standard BLE DIS), required
 *   Service UUID: 0x180A
 *   Characteristics (Read, UTF-8, no terminator):
 *     Manufacturer Name String (0x2A29)  required, e.g. "Seeed Studio"
 *     Model Number String      (0x2A24)  required, board name,
 *                                        e.g. "XIAO nRF54L15"
 *     Firmware Revision String (0x2A26)  required, firmware version,
 *                                        e.g. "1.2.0"
 *     Software Revision String (0x2A28)  optional, "mg<MG_SPEC_VERSION>"
 *     Hardware Revision String (0x2A27)  optional
 *     Serial Number String     (0x2A25)  optional
 *   The user-facing device name is the GAP Device Name (0x2A00).
 *
 * Primary Service: Nordic UART Service (NUS), required
 *   Service UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *   RX (6E400002-...): Write, Write Without Response. Client -> device text.
 *   TX (6E400003-...): Notify. Device -> client text.
 *   A line-oriented debug console: the device mirrors its log on TX and may
 *   accept console commands on RX. Clients must not depend on its contents.
 *   On a release build of a device that holds a token the log mirror runs
 *   only for a connection whose token proof matched, or in the local window
 *   (see Token proof).
 *   BAS, DIS and NUS must never carry audio, keys, or user content.
 *
 * =============================================================================
 * Protocol Overview
 * =============================================================================
 *
 * Command format (client write):     [mg_command_t, parameters...]
 * Notification format (device):      [mg_command_t, response parameters...]
 *
 * Values not defined in an enum are reserved. In mg_command_t, 0x00-0x7F
 * keep the numbering of the earlier Meta gadget protocol mgcommands grew
 * from (the legacy protocol below) and the gaps are held so future
 * commands can return with the same numbers; 0x80-0xFD are
 * musegadgets-specific. Receivers ignore trailing parameter bytes they do not
 * understand, so optional parameters can be added later.
 *
 * Connection flow:
 *   1. Client negotiates ATT MTU >= MG_MIN_ATT_MTU and subscribes to the
 *      Control and Data CCC descriptors.
 *   2. Client writes mg_command_request_status on Control; the device
 *      notifies change_data_type and supported_features. The session is
 *      ready. A device treats the client as ready once both CCCDs are
 *      subscribed and request_status has arrived, in either order: a client
 *      may write request_status right after the Control subscription, before
 *      its Data subscription completes (iOS does). A device in a recovery
 *      mode declares it here, before its command list (see Recovery mode),
 *      and the client goes no further.
 *   3. If supported_features lists mg_command_token_proof and the client
 *      holds a proof key for this device, it runs the token proof.
 *   4. If supported_features lists mg_command_session and the proof matched,
 *      the client sends hello and opens the device's VM session (see Device
 *      session), unless the device's session caps (the typed list
 *      [supported_features, sub_feature, mg_command_session, caps]) declare
 *      mg_session_cap_credentials_only. The Muse apps say hello only to a
 *      device that declares mg_session_cap_sealed, and in parallel with the
 *      rest of connect setup.
 *
 * Push-to-talk:
 *   - The client enables device-side push-to-talk with set_settings
 *     [mg_setting_parameter_push_to_talk_enabled, 1].
 *   - Button down: device notifies [gesture, button_down], plays the
 *     activation buzz (if it lists and has haptics enabled; see Haptics),
 *     notifies [change_data_type, <audio type>, <format...>], then streams
 *     audio on Data.
 *   - Button up: device notifies [gesture, button_up], stops capture, and
 *     notifies [stop_mic] to mark the end of the utterance.
 *   - Ordering: a live turn's own frames (its change_data_type, Data and
 *     stop_mic) may pass Control replies the device has already queued,
 *     unless a queued gesture or another change_data_type governs them;
 *     replies keep their own order and are never dropped. button_down and
 *     button_up go out at once when no gesture is queued. A client
 *     therefore never assumes a reply queued before a turn arrives before
 *     that turn's audio.
 *   - Device-initiated capture (push-to-talk turns) uses
 *     mg_setting_parameter_audio_codec (persisted, see set_settings; SBC on
 *     a fresh device), whatever codec a start_mic chose for the client's own
 *     captures. A client sends the codec it decodes on a turn at every
 *     connect (LC3 if it decodes LC3, else SBC), since the device remembers
 *     the last value any client set. Its rate and bitrate are
 *     mg_setting_parameter_capture_sample_rate and
 *     mg_setting_parameter_capture_bitrate_kbps where the codec takes them
 *     (see Capture formats).
 *   - With push-to-talk disabled, the device only reports gestures; the
 *     client may drive capture itself with start_mic and stop_mic (on a
 *     release build, within 5 s of a press: see start_mic).
 *   - Pre-roll (optional): a device may capture from the button's
 *     touch-down, before its hold threshold. Once the hold threshold passes
 *     it notifies [gesture, button_down] first (clients drop mic Data that
 *     comes before it), then change_data_type, then sends up to 1 s of that
 *     backlog as a burst at the start of the turn's Data, then live audio.
 *     A press released before the threshold is a tap: the device discards
 *     the backlog and sends no audio. The mic ring shows whenever the device
 *     captures, pre-roll included.
 *
 * Settings reports:
 *   When the device's user changes a setting on the device (the speaker
 *   volume from its buttons, a slider, a mute), the device notifies it
 *   unsolicited in get_settings' answer shape: [get_settings, parameter,
 *   value], e.g. [get_settings, speaker_volume (10), v] (0 = speaker off).
 *   A change the client made itself with set_settings is not reported back.
 *   Clients apply it as an answer to get_settings.
 *   During a live call, a speaker_volume report re-picks subtitles or reply
 *   audio (see Live call subtitles).
 *   - The device's speaker_volume is the source of truth. Clients read it
 *     with get_settings at connect and follow its reports.
 *     They read it again at a call's start.
 *     A client's volume control writes it with set_settings; a client never
 *     keeps a copy of its own as the level, and never pushes a stored value
 *     to the device on connect.
 *
 * Assistant state:
 *   A device with a display or status light lists mg_command_assistant_state
 *   and shows the same sequence as a Wi-Fi Muse gadget: listening, thinking,
 *   responding, then its finished animation, then idle.
 *   - The device shows listening while it captures, and thinking by itself
 *     once an utterance ends (stop_mic), since the utterance is now with the
 *     assistant. It never shows its finished animation at stop_mic, even
 *     from a client that has sent no assistant_state yet: clients need not
 *     send thinking.
 *   - The client then reports the turn with assistant_state: responding when
 *     the reply starts (its text streaming, or its audio playing on the
 *     device), done once the reply has fully arrived and, if the device plays
 *     it, once playback has ended (stop_streaming keep acknowledged), and
 *     error if the turn fails. idle cancels the turn.
 *   - While it plays stream_audio the device shows responding whatever the
 *     last assistant_state was, and returns to that state after playback.
 *     A client that plays the reply on the device therefore sends done after
 *     the device acknowledges stop_streaming keep, not when the text ends.
 *   - A device left in thinking or responding for 120 s without another
 *     assistant_state, or whose client disconnects, returns to idle (120 s
 *     covers an agent's tool gap; the client sends thinking during it).
 *   - Any assistant_state takes over from the device's own thinking, and
 *     marks the client as one that reports the turn for the rest of the
 *     connection.
 *   - Older clients never send assistant_state. While the client has sent
 *     none on this connection, the device ends the turn by itself, playing
 *     its finished animation and going idle instead of staying in (or
 *     returning to) thinking: a device that plays the reply does so when
 *     playback ends; a device without stream_audio does so about 15 s after
 *     stop_mic if no assistant_state has arrived.
 *
 * Audio formats (Data payloads for each mg_data_type_t):
 *   - audio_sbc:  standard SBC frames (syncword 0x9C), self-describing.
 *                 Devices use mono, 16 blocks, 8 subbands, loudness
 *                 allocation, not mSBC, at 16 kHz unless both sides chose
 *                 32000, 44100 or 48000 (SBC has no 8000 or 24000). This is
 *                 the default codec.
 *   - audio_lc3:  LC3 frames (ETSI TS 103 634) of a fixed size, back to back.
 *                 The sample rate, frame duration and size come from
 *                 change_data_type, defaulting to 16000,
 *                 MG_LC3_DEFAULT_FRAME_US and MG_LC3_DEFAULT_FRAME_BYTES.
 *                 Rates 8000, 16000, 24000, 32000, 44100 and 48000, each
 *                 only where both sides chose it. 44100 runs the 48 kHz
 *                 configuration, as liblc3 does: the codec is set up at
 *                 48000 and fed 44.1 kHz samples, so a 10 ms frame holds 480
 *                 samples and lasts 10,884 us; its label says rate 44100 and
 *                 duration 10000 (the configuration's, the dt_us liblc3
 *                 takes). Frame bytes MG_LC3_MIN_FRAME_BYTES to
 *                 MG_LC3_MAX_FRAME_BYTES: a bitrate in kb/s takes
 *                 floor(kb/s * duration_us / 8000) bytes (at 10 ms, kb/s *
 *                 1.25: 16 kb/s = 20 B, 32 kb/s = 40 B, 64 kb/s = 80 B), with
 *                 the configuration's duration at 44100 too. A live frame
 *                 travels in one notification, so the frame must fit
 *                 ATT_MTU - 3: at MG_MIN_ATT_MTU that is 78 kb/s (97 B at
 *                 10 ms), and a client that wants more (96 kb/s at 24000 is
 *                 120 B) negotiates an ATT_MTU of at least frame bytes + 3
 *                 first; a device clamps to what the agreed MTU takes.
 *                 Senders keep
 *                 LTPF analysis off (liblc3: lc3_encoder_disable_ltpf, or
 *                 LC3_ENC_LTPF=0); a receiver may omit the LTPF postfilter.
 *   - audio_opus: raw Opus packets, one per notification.
 *   - audio_pcm:  signed 16-bit little-endian samples.
 *
 * Capture formats:
 *   A client's own captures (start_mic) are 16 kHz, LC3 at
 *   MG_CAPTURE_DEFAULT_BITRATE_KBPS, unless it asks for another rate or
 *   bitrate. Device-initiated captures follow settings 13 and 14 (below).
 *   - A device that captures at more than 16000, or at a choice of
 *     bitrates, lists what it captures per codec: [supported_features,
 *     sub_feature, change_data_type, (codec, count, count x (uint16_t rate
 *     in Hz, uint16_t min kb/s, uint16_t max kb/s))...], 16000 first in each
 *     codec, e.g. 10 fe 08 01 01 80 3e 3c 00 3c 00 13 02 80 3e 10 00 40 00
 *     c0 5d 10 00 60 00 (SBC at 16000, 60 kb/s fixed; LC3 at 16000 with
 *     16-64 kb/s and at 24000 with 16-96 kb/s). min == max: that bitrate
 *     only (an SBC bitpool is fixed). At 44100 the kb/s are the 48 kHz
 *     configuration's (frame bytes * 0.8). Absent: 16000 at the default
 *     bitrate for every codec in the start_mic list. A device lists only
 *     what it implements; the list fits one notification at MG_MIN_ATT_MTU.
 *   - start_mic's params 2 and 3 ask for a rate and a bitrate. The device
 *     captures at the rate if it lists it for the codec, else at 16000, and
 *     clamps the bitrate into that rate's range; it never refuses for
 *     either. A listed top bitrate may need more than one notification's
 *     room at the agreed MTU (24000 at 96 kb/s is 120 B against 97 at
 *     MG_MIN_ATT_MTU): the client picks a bitrate whose frame fits
 *     ATT_MTU - 3, or negotiates a larger MTU first, and the device clamps
 *     to what fits.
 *   - mg_setting_parameter_capture_sample_rate and
 *     mg_setting_parameter_capture_bitrate_kbps set the rate and bitrate of
 *     device-initiated capture (push-to-talk turns and their pre-roll); the
 *     device persists them (see set_settings) and clients send theirs at
 *     every connect (see each setting).
 *     Talk holds in a call are device-initiated capture too.
 *     The rate applies where the capture's codec lists it, else 16000; the
 *     bitrate is clamped into that codec and rate's range (a fixed-bitrate
 *     codec keeps its own). A press that adopts a client's start_mic capture
 *     keeps that capture's format.
 *   - change_data_type always labels the rate (param 2) and, for LC3, the
 *     frame bytes and duration (params 4 and 5); the client decodes what the
 *     label says. Offline clips carry the same fields in clip_info.
 *
 * Audio playback (client -> device):
 *   A device with a speaker lists mg_command_stream_audio in
 *   supported_features. The flow mirrors the legacy protocol's stream_audio:
 *   1. The client sends request_capabilities; the device answers
 *      report_capabilities (codecs, buffer size, sample rates, maximum
 *      bitrate and channels). The sample rates list 16000 first (an omitted
 *      start_streaming parameter takes the first value), then any other
 *      rate the device plays in some codec (Audio formats has each codec's
 *      rates). Every listed codec plays at 16000; a codec and rate pair the
 *      device doesn't play gets invalid_value, and the client retries at
 *      16000.
 *   2. The client sends start_streaming with its parameters, naming the
 *      sample rate it chose (a listed one, in a codec that has it) and, for
 *      LC3, the frame size its bitrate takes (within max_bitrate). The
 *      device answers buffer_update if it accepts them, or error with
 *      mg_error_code_unsupported / mg_error_code_invalid_value if not, or
 *      mg_error_code_busy while its own use has the speaker (media, a bench
 *      test); never because of push-to-talk (below).
 *      A hands-free call is such a use too.
 *   3. The client writes the encoded audio on Data with Write Without
 *      Response: whole codec frames, in order, no header, in the codec from
 *      start_streaming. It keeps the bytes in flight (bytes written minus the
 *      total received from the latest buffer_update) below the available
 *      bytes that update reported. The device sends buffer_update in reply to
 *      request_buffer_update and on its own at least every 250 ms while
 *      streaming. The total received restarts at 0 at every start_streaming
 *      (audio and video alike) and counts every byte received on Data for
 *      that stream, including a write the device dropped whole (a write that
 *      isn't whole frames or doesn't fit), so the client's in-flight count
 *      drains; bytes written outside a stream don't count. The client counts
 *      its written bytes from the same start. Data past the credit the device
 *      granted (past the received total plus the available bytes of its
 *      latest buffer_update; before the first, the one early keyframe of
 *      Video streaming) is dropped and counted in the received total, never
 *      buffered, even where the buffer has room: an audio write whole; a
 *      video write together with the frame it belongs to, and the device
 *      sends request_keyframe.
 *      A video keyframe written early behind start_streaming (below) counts
 *      as written from the start, and the device's received total includes
 *      it once read; a client never re-bases its count on one buffer_update.
 *   4. stop_streaming with keep: the device plays what it holds, then
 *      notifies stop_streaming keep. With drop: it discards its buffer at
 *      once and notifies stop_streaming drop. Every stop_streaming the client
 *      sends is answered exactly once (with nothing playing, at once). A
 *      start_streaming that arrives while a stream still plays, or plays out
 *      after keep, replaces it: the device first notifies that stream's stop
 *      (keep if it had played out, else drop), then answers the new stream's
 *      buffer_update. So a stop_streaming notification always belongs to the
 *      stream before the latest buffer_update that followed a start.
 *   The device notifies stop_streaming drop on its own when it must end
 *   playback, e.g. when push-to-talk takes over the audio path; the client
 *   stops writing.
 *   In a Live call's one reply stream (Live reply audio below) a talk press
 *   instead discards what the device holds, as a flush does, and keeps the
 *   stream open; the device ends a call's stream only when it can't go on
 *   (an error).
 *   A client whose stream the device ended opens a new one at the next
 *   reply.
 *   A user cancel: a device-ended stop_streaming drop of a push-to-talk
 *   answer that no [gesture, button_down] follows within 300 ms, once that
 *   answer has started playing on the device, means the user cancelled it
 *   there (a short press); a drop before playback started is not a cancel.
 *   This holds outside a Live call only.
 *   The client sees playback start only by what playback alone causes,
 *   between two buffer_updates of the stream with no flush between: queued
 *   ms falling by 20 ms or more, or available bytes rising by at least one
 *   encoded frame (bytes received count Data not yet in the buffer).
 *   The client then cancels the answer too: no further audio, follow-up
 *   answer or late text for that turn, and on the chat path no phone
 *   playback.
 *   On a voice session that includes cancelling the response at the VM.
 *   It shows no failure and settles the turn. A device sends this
 *   drop only to clients that act on it (by client_app version); to other
 *   clients it stops locally and answers keep.
 *   During a push-to-talk turn:
 *   - A start_streaming that arrives during the turn is accepted. The device
 *     buffers the stream under credits (buffer_update as usual) and plays it
 *     only after the turn and its 120 ms tail have ended: never the speaker
 *     into the turn's mic.
 *   - So a client must stop an in-flight or pending reply (stop_streaming
 *     drop) when it sees a new [gesture, button_down]: a barge-in. Otherwise
 *     turn N's reply plays after turn N+1.
 *   - A client must tolerate a reply stream that gets no credits for as long
 *     as a turn lasts (up to about 60 s), without timing out.
 *     In a call it doesn't fail the call for it either.
 *   Devices start playing once they hold their start threshold, and fill
 *   underruns with concealment or silence.
 *   In a call that threshold is at most 40 ms (Live reply audio below).
 *   Playback follows the device's speaker setting
 *   (mg_setting_parameter_speaker_volume): with the speaker off, the stream
 *   is accepted and paced but plays silence.
 *
 * Video streaming (client -> device):
 *   A device with a display that can decode video lists mg_command_stream_video
 *   in supported_features. The flow mirrors Audio playback, on the same Data
 *   characteristic, and shares its flow control:
 *   1. The client sends request_capabilities; the device answers
 *      report_capabilities (codecs, its display, the largest picture and
 *      frame rate it decodes, its receive buffer, and the stream it suggests).
 *   2. The client sends start_streaming with the stream's parameters. The
 *      device answers buffer_update if it takes them, or error with
 *      mg_error_code_unsupported / invalid_value / busy if not. busy covers
 *      both a passing state (a stream just stopped and still freeing) and a
 *      picture size the device has no room for right now, so a client
 *      retries busy a few times, spaced (about 250, 500 and 1000 ms), then
 *      steps down to the next smaller size its capabilities allow before it
 *      gives up the stream.
 *      In a call, that gives up video for the call.
 *      A stream leaves audio playback on (see below for both at once).
 *      If the device's capabilities carry
 *      mg_stream_video_capability_flag_early_keyframe, the client may
 *      write the stream's first frame, its keyframe, right behind
 *      start_streaming without waiting for that answer (the per-frame
 *      limit, half of report_capabilities' buffer_size less 8, bounds it),
 *      then waits for the first buffer_update before writing more: one frame
 *      is what every device can take before it has started the stream. The
 *      device accepts Data for a stream it is starting. That first
 *      buffer_update may be sent before the early frame is read, so its
 *      received total may not count it yet: bytes in flight are the bytes
 *      written since start_streaming (the early frame included) less the
 *      latest received total, never re-based on one update. If it answers error
 *      instead (busy, say), the stream was never open and it discards the
 *      Data written for it; the client starts again with start_streaming
 *      and a keyframe when it retries.
 *   3. The client writes the stream on Data with Write Without Response,
 *      split at any byte into writes of up to ATT_MTU - 3 bytes (or on the
 *      L2CAP channel report_capabilities may offer, never on both). The stream
 *      is a sequence of frames, each a frame header (MG_VIDEO_FRAME_HEADER_LEN
 *      bytes; its layout is beside that define) followed by
 *      one access unit in Annex B form (start codes; SPS and PPS before every
 *      keyframe). Bytes in flight stay below the available bytes of the
 *      latest buffer_update, exactly as for audio playback. buffer_update may
 *      carry, after its two counters, uint16_t frames shown, uint16_t frames
 *      dropped and uint8_t average decode time in ms since start_streaming,
 *      so the client can lower its frame rate or bitrate.
 *   4. The device sends request_keyframe when it can't decode until the next
 *      keyframe (a frame lost or damaged, or decoding fell behind); the client
 *      makes its next frame a keyframe. A device may drop frames it can't
 *      decode in time, never part of one.
 *   5. stop_streaming (either side, payload mg_stream_audio_buffer_drop)
 *      ends the stream; the device stops decoding at once and the client
 *      stops writing. The device does not answer the client's stop_streaming
 *      (an answer could arrive after the client's next start_streaming and
 *      read as the new stream's end); it notifies stop_streaming only when it
 *      ends a stream itself (its user closed the view, an error). A
 *      start_streaming while a stream is on replaces it, also silently.
 *   Video and audio playback can run at once on the one Data characteristic,
 *   each with its own start_streaming, credits (buffer_update on its own
 *   command) and stop. While both are on, the client tells the device which
 *   one its Data writes belong to by writing [change_data_type, <type>] on
 *   Control before them: the video stream's start codec (H.264, or a still
 *   stream's image codec, e.g. image_jpeg), or the playback's
 *   audio codec; a device takes any type that isn't the audio stream's as
 *   the video stream's. For example: change_data_type video, five video writes,
 *   change_data_type sbc, two audio writes, change_data_type video, ...
 *   The device applies change_data_type in order with the Data writes around
 *   it. Audio writes stay whole frames as for playback alone; video may
 *   split anywhere. change_data_type only selects how the Data writes after
 *   it are interpreted: it always switches and is never answered (no error),
 *   whatever is on. Data written while the selected type has no open stream
 *   (a playback the device has just ended, say) is discarded silently (no
 *   error: a client reads an error naming change_data_type as its stream
 *   refused), never handed to another stream. With one stream on, its writes need no
 *   change_data_type; when a second starts, the client sends one before
 *   writing it (until then Data stays with the first). When the client ends
 *   either, Data selected for it goes to the other. When the device ends one
 *   (its own stop_streaming notification), Data selected for it is discarded
 *   until the client's next change_data_type, since writes already in flight
 *   were meant for the ended stream; the client sends change_data_type
 *   before its next write to the other. A device that drops or can't decode
 *   video sends request_keyframe at once.
 *   A/V sync: when reply audio and video come from one source, the speaker
 *   and the panel must agree within audio 45 ms ahead of the picture to
 *   125 ms behind it (broadcast lip-sync tolerance).
 *   In a Live call that source is the call's video of an avatar speaking.
 *   The device shows each video frame as soon as it is decoded
 *   and plays audio in order from its buffer; it reports the numbers and the
 *   client does the aligning, since only the client holds frames cheaply:
 *   - The device reports its audio delay: buffer_update for stream_audio
 *     carries queued ms (the audio ahead of its speaker, output path
 *     included). The video delay is the latency from the last-shown frame in
 *     stream_video's buffer_update.
 *   - The client holds each video frame before encoding it by the audio path's
 *     delay minus the video path's: queued ms + the link's one-way latency
 *     (about half the credit round trip) − the video's measured
 *     capture-to-shown latency, never below 0. It adds audio written but not
 *     yet credited only while it is backlogged against credit (otherwise that
 *     count also holds audio that arrived after the report was made, and the
 *     estimate runs ~15 ms long). The echoed video latency is noisy (±25 ms a
 *     sample): the client takes a median of the last ~10. Within a reply it
 *     moves the hold by at most one frame per second, so the picture never
 *     jumps, and sets it straight to the target at each reply start (nothing
 *     is spoken between replies, so the step isn't seen).
 *     It resets the hold at each call start.
 *     Through a reply's first 500 ms, while the device's buffer fills, the
 *     hold follows the target at once, and estimates taken before any of the
 *     reply's audio was written are ignored.
 *   - A device that doesn't send queued ms is aligned by the client's
 *     estimate from buffer_size − available at the stream's byte rate.
 *   Live reply audio: reply audio a client paces live
 *   (a call's reply audio always is)
 *   plays with at most ~100 ms of total depth on the device (queued ms,
 *   output path included), so it feels live:
 *   - The device starts playing once it holds its start threshold (at most
 *     40 ms; while a video stream runs, at most 100 ms, since video makes
 *     reply writes burstier and the picture is the later path anyway) and
 *     never re-buffers to a larger one after a gap: it conceals a short gap
 *     (LC3's packet loss concealment, or a faded repeat) and resumes as soon
 *     as data arrives.
 *   - The client sends a start burst of at most 60 ms, then forwards audio
 *     as it arrives in writes of at most 40 ms, with no further lead. A client
 *     that paces this way says so with mg_stream_audio_parameter_live_depth_ms
 *     in start_streaming, and only then does the device cap its credit to
 *     that depth.
 *     The Muse apps always send it for a call's reply stream, with a depth of
 *     at most 100 ms (a device whose listed floor is over 100 ms gets none
 *     and plays from its whole buffer).
 *     Without it (older clients) the device grants its whole buffer and
 *     starts at its normal threshold, so a client that doesn't pace live never
 *     underruns against a cap it didn't ask for.
 *   - With so little depth the client lives on fresh credit: while it
 *     honors a live depth the device sends buffer_update at least every
 *     20 ms of playout (not the 250 ms floor above), so a credit-paced
 *     client never runs it dry waiting for room. On the whole-buffer path
 *     it may update on change instead. Under an honored live depth the
 *     client relies on those periodic reports and sends no
 *     request_buffer_update for that stream (re-asking on every answer
 *     floods the link: ~85 requests/s at a 100 ms depth, each answered);
 *     it asks only when no report arrived for 500 ms (its stuck check).
 *   One stream per Live call: the client opens one reply-audio stream when
 *   the call goes live (or at its first reply) and keeps it open until the
 *   call ends, writing each reply's audio as it arrives and nothing between
 *   replies. It never stops and restarts the stream per reply, since each
 *   restart waits on the old stream's play-out and the new start's answer
 *   while live audio piles up.
 *   - Between replies the device's buffer runs empty: it plays silence (a
 *     short concealment fade first), keeps the stream open however long the
 *     gap (a whole turn, minutes), and plays the next reply's first bytes
 *     at once, never re-buffering to a larger threshold. "Reply playback"
 *     (panel hold, the speaking face) means audio is playing, not that the
 *     stream is open.
 *   - Barge-in: the client sends mg_stream_audio_flush to a device that
 *     lists it, discarding what the device holds without closing the
 *     stream; to one that doesn't, stop_streaming drop and a new start.
 *   - The push-to-talk rules above still apply: audio arriving during a turn
 *     is held and plays after the turn and its tail.
 *   The device may ask for a stream: request_streaming [1] when its user
 *   picks the camera (or other video) on the device, request_streaming [0]
 *   when they leave it. A client that can stream answers 1 with
 *   start_streaming and 0 by stopping. The device repeats a pending
 *   request_streaming [1] when a client becomes ready (request_status).
 *
 * Live call:
 *   The client is the Muse app; the SDK's reference apps don't implement it.
 *   A device that can start a live call with the Muse lists
 *   mg_command_live_call in supported_features. In a live call the client
 *   holds an open voice conversation with the Muse: push-to-talk utterances
 *   go into the call as the user's audio, the Muse's spoken replies come back
 *   as audio playback, and the client may stream the Muse's picture as video.
 *   - Device -> client: [live_call, mg_live_call_request_t] when the user
 *     starts or ends the call on the device (a menu item or a gesture).
 *     The device repeats a pending start when a client becomes ready
 *     (request_status), as for request_streaming.
 *   - Client -> device: [live_call, mg_live_call_state_t] whenever the call
 *     changes state, whichever side started it, so the device can show it.
 *     A device shows no call after idle, ended or failed, or when the client
 *     disconnects. A device that has shown connecting for 60 s with no newer
 *     state from the client ends the call itself: it sends
 *     [live_call, end] as for the user's end, shows its "Couldn't start the
 *     call" banner and leaves the calling screen. It is a backstop for a
 *     silent client: a client fails its own join sooner (the Muse apps at
 *     45 s), with a reason. The client cancels the connect on that end as on
 *     the user's and answers ended or failed. A client whose connect
 *     legitimately takes longer re-sends connecting, which restarts the 60 s.
 *     With failed, a client adds why
 *     ([live_call, failed, mg_live_call_failure_reason_t]) to a device that
 *     lists the failure_reason live-call feature; others get failed alone.
 *     setup_required means the phone needs the user first (the Muse app's
 *     voice intro, the microphone permission): the device shows "Open the
 *     Muse app to finish setting up calls" instead of "Couldn't start the
 *     call", and the client prompts on the phone.
 *   - From a central whose token proof hasn't matched on this connection, a
 *     set-up gadget takes live_call states without a proof gate, within
 *     limits. connecting, failed and ended that answer this connection's own
 *     pending start (the gadget asked this link to start a call) drive that
 *     start screen as usual: connecting shows, failed or ended closes it (the
 *     Muse apps answer a gadget's start before the proof; live comes only
 *     after it). Any other state may show the call screen only on an unlocked,
 *     awake gadget. For every call that isn't this connection's own pending
 *     start (another link's call, or a proven link's live call), such a
 *     state never wakes the screen, never draws over or delays the passcode
 *     lock (auto-lock runs as usual), and never cancels or replaces the
 *     owner's pending or live call (one this gadget started or accepted).
 *   Push-to-talk, assistant_state and audio playback work as outside a call.
 *   - Hands-free: a device that keeps its mic open for the whole call lists
 *     [supported_features, sub_feature, live_call, hands_free, ...] (bytes
 *     10 fe 85 01). The client then sends start_mic when the call goes live
 *     and stop_mic only at the call's end. Without it, the client sends no
 *     start_mic for the call: push-to-talk carries the call, and the mic
 *     opens only while the user holds talk. A talk hold in a call captures
 *     in LC3 where the device has it, whatever audio_codec says, at
 *     capture_sample_rate and capture_bitrate_kbps (16 kHz and 32 kb/s
 *     unless the client set others), labeled with its frame bytes.
 *   - Walkie-talkie: a device without hands_free opens the mic for a call
 *     only while the user holds talk (button or screen), from the call's
 *     start until it ends, with the mic ring on while it captures. It
 *     ignores a client start_mic during a call and for 3 s after it ends (no
 *     capture, no ring, no answer); clients send none. A capture a client's
 *     start_mic began before the call ends when the call starts, and a
 *     stop_mic for it is always taken. The press that
 *     starts a call is never a talk hold, even if it stays down after the
 *     call starts: a talk hold is a new press on the talk control. A hold
 *     during connecting is captured as a push-to-talk turn would be; the client
 *     takes its audio as the call's joining speech (not a question), buffers
 *     it until the call has joined and trims leading silence.
 * Live call subtitles (live calls only; test-vectors/
 * mg-subtitle-v1.json and mg_subtitle_ref.py are the reference):
 *   A device without a speaker, or whose speaker is silent, can show the
 *   Muse's words instead of playing them, in step with the avatar's mouth.
 *   The client decides, for a device that lists mg_command_subtitle: it
 *   sends subtitles rather than reply audio when the device has no speaker
 *   or its speaker_volume is 0, and subtitles together with reply audio when
 *   the user chose "always" for this device ("never" = reply audio even at
 *   0). The device's speaker_volume is the source of truth (see Settings
 *   reports). Under "always" the client sends subtitle state 1 before the
 *   reply's first audio, to every device with a speaker whatever
 *   wants_audio_too says. When subtitles replace the audio, the client opens
 *   no stream_audio for replies (unless the device sets wants_audio_too), so
 *   the link carries video alone, and it sends assistant_state done when the
 *   reply's text ends. A mute or unmute in the
 *   middle of a Live call switches at once both ways: on a mute the client
 *   stops writing the reply's audio and flushes what the device holds
 *   (mg_stream_audio_flush, where listed), keeping the call's stream open,
 *   and the rest of the reply shows as subtitles; on an unmute it resumes
 *   writing the reply's audio from the current moment, so the voice comes
 *   back at once at the current word. Flush serves this switch and barge-in. All integers are
 *   little-endian; every message is [subtitle, mg_subtitle_command_t, ...].
 *   - request_capabilities / capabilities, as the other families do. The
 *     client asks once per connection; a device answers version 1 and the
 *     flags, lines and characters per line it renders and the largest words
 *     payload it takes (max_words_bytes, counted from the reply id on, after
 *     the command and sub-command bytes).
 *   - A words message (header included) never exceeds the link's ATT payload
 *     (ATT_MTU - 3). max_words_bytes is an upper bound the device sets from
 *     its MTU: it counts from the reply id, so a whole words write is
 *     max_words_bytes + 2 bytes, and the device reports no more than
 *     ATT_MTU - 5. If the MTU changes after it answered, the device sends
 *     capabilities again, unsolicited, with the new bound; the client uses
 *     the latest. A client must also cap each words write at its own maximum
 *     write-without-response length and split a long burst into several
 *     words messages.
 *   - state says whether this call is in subtitle mode and which timing
 *     domain the cues use. state off, like clear, drops everything queued.
 *     Only state 1 starts subtitle mode: words, reply_end and clock received
 *     outside it are ignored (a device may count them), so a device never
 *     shows text the client did not turn on.
 *   - On a locked gadget (its passcode lock screen up) subtitles never draw
 *     over the lock screen, and no subtitle message, state 1 included, wakes
 *     or unlocks it. The device keeps the reply's words and shows them, as
 *     they come due, once the user unlocks; a reply whose end (reply_end's
 *     moment) comes while it is locked is dropped.
 *   - From a central whose token proof hasn't matched on a set-up device,
 *     subtitles show only on a screen that is already on: they never wake
 *     it or keep it lit (see Token proof). While the device shows a prompt
 *     that asks for the user's press or code (pairing, an update's confirm,
 *     the passcode keypad), no subtitle draws over it.
 *   - words carries cues: one word each (the word with the whitespace after
 *     it, UTF-8, 1..255 bytes) and the moment to show it.
 *     - Frame timing (the default, while video streams): the moment is a
 *       video frame number, so the word appears with the mouth that speaks
 *       it and no clock needs syncing. It is the video frame header's 16-bit
 *       frame number extended to 32 bits: the device counts wraps over every
 *       frame header it receives (decoded or dropped) since start_streaming,
 *       and both sides start again from the header's number at each
 *       start_streaming. The device reveals a word when it shows a frame
 *       whose extended number has reached the cue's (serial comparison: due
 *       when (int32_t)(shown - show_at) >= 0).
 *     - Milliseconds timing (no video): the moment is milliseconds from the
 *       reply's start, and the client sends clock about once a second with
 *       how much of the reply has played; the device runs its own clock from
 *       the latest one.
 *     - MG_SUBTITLE_SHOW_NOW shows a word at once, in either domain; a cue
 *       whose moment has already passed is shown at once too.
 *     The device keeps one reply. A words message for another reply id drops
 *     the old one first. offset is the first word's UTF-8 byte offset in the
 *     reply's text, and each further word follows the one before. replace
 *     drops every word at or after offset before adding these (the Muse
 *     revised its text); final says the reply's text up to the end of this
 *     message won't change. Revealed text is the longest run of words from
 *     the reply's start that are each due; a revealed word stays revealed
 *     until a replace or clear removes it.
 *   - The client sends each cue at least ~300 ms before its moment, packs as
 *     many whole cues as fit max_words_bytes (at most 255) into a message,
 *     with replace on the first message of a run only and final on the last
 *     only, and cuts a word longer than max(4, min(255, max_words_bytes - 11))
 *     bytes into pieces at code point boundaries, never inside one.
 *   - reply_end closes a reply at a moment in the same domain; the device keeps
 *     the last lines briefly (its choice) and then clears them. clear drops
 *     everything for that reply id (0: any reply): barge-in, the end of the
 *     call, subtitle mode going off, or a new reply. A device ignores clock,
 *     reply_end and clear for a reply id it doesn't hold (other than 0).
 *   - A device that renders Latin-1 only says so (latin1_only); the client
 *     then sends no other characters. A device shows what it can of anything
 *     else (a replacement glyph), never rejects a message for it.
 * UI streaming (client -> device):
 *   A mostly static picture (the phone's screen, a web page) at the panel's
 *   own size, as opposed to camera video at the smaller size the device
 *   suggests. A device that takes it lists mg_stream_video_capability_ui in
 *   report_capabilities: the size (normally its panel's, shown one to one),
 *   the largest frame it buffers, and the most bands at that size. The
 *   client starts it with start_streaming and mg_stream_video_parameter_content
 *   = mg_video_content_ui; everything else is Video streaming above, with
 *   these differences:
 *   - The picture may be up to the ui capability's size (not max_size), and
 *     every frame must fit its max frame bytes (header excluded).
 *   - Frames come only when the picture changes (and a few more right after
 *     a change, which sharpen it), at any interval: the device shows every
 *     frame it decodes, keeps the last one on screen for as long as the
 *     stream is on, and never treats a quiet stream as stalled.
 *   - H.264: one reference frame, no B frames, a keyframe at the start and
 *     on request_keyframe only (no periodic keyframes or intra refresh are
 *     needed: Data is reliable). The playout delay is refused.
 *   - While no frame has arrived for a second, the device may ask for a
 *     slower connection (more peripheral latency); its next Data write puts
 *     the fast link back. The client needs to do nothing about it.
 *
 * Still images (client -> device):
 *   Photos the phone compresses (JPEG) and the device decodes and
 *   shows, each one whole, as fast as the link and the decoder go. A device
 *   that takes them lists mg_stream_video_capability_still in
 *   report_capabilities: the largest picture, the largest file and the image
 *   codecs. The client starts it with
 *   start_streaming and mg_stream_video_parameter_content =
 *   mg_video_content_still (codec: the first image's; width and height: the
 *   largest picture it will send); everything else is Video streaming above,
 *   with these differences:
 *   - Each frame is one complete image file (keyframe flag set: every frame
 *     stands alone) in any codec the still capability lists, whatever the
 *     start's codec: the device tells them apart by their signatures (JPEG
 *     FF D8 FF).
 *     Every file fits the capability's max bytes and half the buffer less 8.
 *   - The device decodes every image in order and shows it centered (it may
 *     draw the top while it decodes the rest), keeps the last one on screen
 *     while the stream is on, and never treats a quiet stream as stalled.
 *     A client may write the next image while the device decodes the last
 *     (credits as for video): that overlaps the link and the decoder.
 *   - For each image the device sends image_report (its status, codec, size
 *     and where the time went: receive, queue, decode, draw), once it is
 *     shown or refused. A damaged or truncated file is reported damaged,
 *     never crashes the device, and the stream goes on with the next one.
 *   - The playout delay and bands are refused. mg_stream_video_parameter_overlay
 *     asks the device to show the image's size and decode time over it.
 *
 * UI input (device -> client):
 *   A device with a touchscreen lists mg_command_ui_input, and its typed
 *   list [supported_features, sub_feature, ui_input, <mg_ui_input_feature_t>...]
 *   says what it offers. Touch events: the client sends [ui_input,
 *   touch_enable, 1]; the device then notifies [ui_input, touch, ...] for
 *   each touch down, move (at most every 16 ms, and only when the point
 *   moved) and up, in its panel's pixels, and its own UI ignores the touches
 *   while a UI stream is on. touch_enable 0 (or a new connection) stops it.
 *   While the device's own screen shows a passcode keypad or any other
 *   passcode entry, no touch reaches the client or the HID host: no touch
 *   event and no HID report carries it. A touch under way when one appears
 *   ends there (an up at its last point before it), and touches go out
 *   again from the first touch down after it closes.
 *   HID: a device that lists hid_digitizer, hid_mouse_abs or hid_mouse_rel
 *   also exposes a HID over GATT service (HOGP, 0x1812) whose reports need an
 *   encrypted, bonded link; the phone's OS (not the client) is the HID host.
 *   The client tells the device where the streamed square sits on the phone's
 *   screen with hid_map; the device then sends each touch through the
 *   inverse of the square-to-disc warp (when the map says the warp is on)
 *   into that square and on to the HID host as an absolute touch, an
 *   absolute pointer, or (iOS, which takes only pointers, relative) a
 *   relative pointer that is first parked in the screen's top-left corner.
 *   The device answers hid_map, and notifies on any change, with hid_status.
 *   Controls (a device that lists the controls feature): a UI stream has a
 *   few settings either side may change, each [ui_input, control, id, value]
 *   (mg_ui_control_t). The client says what each is at the start of a UI
 *   stream and on every change; the device sends one when its user changes
 *   it (from its own buttons, or by tapping the menu): the client applies it
 *   and echoes the new value. menu: a device button opened (1) or closed (0)
 *   the stream's menu, which the client draws into the picture; while it is
 *   open every touch goes to the client as a touch event, never to the HID.
 *   pan: 1 while every drag moves the phone-screen window instead of
 *   reaching the phone; edge_pan: 1 while a drag that starts in the panel's
 *   outer edge ring (beyond 88% of its radius; on a panel that isn't round,
 *   the outer 10% of its width or height along each edge) pans the window
 *   instead. Such a touch (and in pan mode, any touch) is held back until
 *   it's decided: moved more than 12 px, it's a pan, sent as touch events
 *   with MG_TOUCH_ID_PAN in the id and none to the HID; lifted before that,
 *   or held still for 350 ms, it's an ordinary touch (a tap at its down
 *   point, a long press) for the phone and the HID.
 *
 * Token proof:
 *   A gadget gets its device token pair through Muse Link setup, not over
 *   this protocol. Setup leaves the device
 *   and the app each holding a proof key
 *     K = HKDF-SHA256(salt "mg token proof v1", IKM access token,
 *                     info node_id, 32 bytes)
 *   and on every connection the client checks that both sides agree on it
 *   without sending the token:
 *     client -> challenge [client_nonce 16]
 *     device -> response  [device_nonce 16,
 *                          HMAC-SHA256(K, "mg token proof v1 device" ||
 *                                      client_nonce || device_nonce)]
 *     client -> confirm   [HMAC-SHA256(K, "mg token proof v1 client" ||
 *                                      client_nonce || device_nonce)]
 *     device -> result    [1 match, 0 mismatch]
 *   A device without a token answers challenge with error not_found. A new
 *   challenge restarts the exchange; confirm without a pending response is
 *   invalid_value. The result lasts for the connection. On a device that
 *   holds a token, these wait for a match on this connection and are
 *   refused with error proof_required before it: mic and camera output
 *   (held, see start_mic), start_mic itself unless a challenge is
 *   outstanding, offline clip reads and queue erase, power off, audio and
 *   video streams to it (start_streaming), clear and the session hello. On a
 *   release build of a device that holds a token (automation builds are
 *   exempt), these wait for the match too: ui_input hid_key, a legacy skill
 *   invoke (see Skills; list_request and cancel stay open) and haptic_play
 *   (haptic_stop is always taken). A device that doesn't implement one
 *   answers unsupported first, before any proof check: the gate covers only
 *   what a device does. assistant_state and the throughput test stay open,
 *   with no proof.
 *   So do subtitles and the Live call screen, within limits: from a
 *   central whose proof hasn't matched, subtitles show only on a screen
 *   that is already on (they never wake it, keep it lit or hold off its
 *   sleep or its passcode lock), nothing of them or of a call's controls
 *   draws over a prompt that asks for the user's press or code (pairing,
 *   an update's confirm, the passcode keypad), and such a call state
 *   remaps none of the device's keys.
 *   Reads of the owner's content: on a release build of a device that holds
 *   a token, reads that return the owner's content answer only a connection
 *   whose proof matched, or in the local window a key hold arms:
 *   get_settings' client_name and client_app (skipped from the answer, then
 *   proof_required once), get_unix_time (proof_required: local time and
 *   offset) and the NUS log mirror (silent until then); offline clips' reads
 *   already wait (see Offline clips). Identity and status reads stay open:
 *   supported_features and the typed lists, request_status, get_settings'
 *   other fields (spec_version, codec, rates, volumes, link mode and
 *   parameters, ...), battery, capabilities, stream credit and status, the
 *   skills document, DIS and BAS; so do touch events (never while a
 *   passcode entry is on the device's screen: see UI input). A device with
 *   no local window answers the owner's content only to the proof. Test, bench
 *   and automation builds are unchanged.
 *   Link setup's confirm prompt follows README.md (Gadget setup v2: The
 *   press, The re-setup prompt's rate): one press of the confirm key or one
 *   tap on the Confirm disc confirms; on a set-up gadget's release build a
 *   re-setup never by the talk key, whose press only dismisses its prompt,
 *   and a gadget whose only key is its talk key takes a re-setup only in its
 *   setup mode (its button held at boot into its normal firmware, never a
 *   SAFE), answering a hello outside it error_pairing_unavailable. A hello
 *   on the prompt's own connection takes the prompt over, one from another
 *   connection gets error_operation_in_progress while it is up, and on a
 *   release build a new prompt within 10 s after a dismiss on the same
 *   connection is refused the same way.
 *   Bluetooth firmware updates over SMP need no proof and no phone. On a
 *   release build of a gadget that holds a device token (set up), an update
 *   change (image upload, image state write, erase, reset) from a link whose
 *   proof hasn't matched needs one physical press on the gadget, any key, the
 *   talk key included (one press, no hold; injected input, a console or
 *   Nordic UART word or a scripted touch, never counts): the first such write
 *   gets rc 10 (EBUSY: confirm on the gadget) while the gadget shows "Press
 *   to confirm update" (on its screen, or by LED on a screenless board), the
 *   client retries about once a second, and the first write after the press
 *   gets rc 0. A key or touch that went down before the update prompt showed
 *   never confirms it, not even at its release; the next press does. It's an
 *   order check with no timing. Such an upload's first chunk must name its length and
 *   whole-image SHA-256: one that names none gets rc 11 with no prompt, and a
 *   chunk with no upload started on its link gets rc 11. The press confirms
 *   the upload whose write started the prompt and binds to that upload's
 *   length and whole-image SHA-256 for 600 s, across link drops; it isn't
 *   bound to the link. What it allows, on any link within the 600 s, is that
 *   upload: its first chunk (the same length and SHA-256), started again or
 *   resumed on a new link with no new prompt, its later chunks, the state
 *   write that stages it, and an erase or the reset on the link that carries
 *   it. A transfer under way when the 600 s end finishes on its link, but
 *   starting it again, on that link or another, asks again, as does any other
 *   upload. The state write carries the image's own hash, not the whole-image
 *   SHA-256 (MCUboot's covers the image's header and body, not its trailing
 *   TLVs), so the device checks the bytes it holds instead: a write past the
 *   bound length, a last chunk after which the slot's bytes don't hash to the
 *   bound SHA-256, and a state write while they don't get rc 11 and end the
 *   binding. A press on a prompt that an erase, a reset or a state write
 *   started covers only that change (the same request body) on its link, for
 *   600 s. Forget ends the link's proof and any binding. The console, Link
 *   setup and device actions never see this window. The prompt times out
 *   after 30 s, and on a release build a new one shows at most once per 10 s
 *   per connection (a sooner write that would show one gets rc 10 with no new
 *   prompt); a cancel or a timeout answers the next write rc 11
 *   (EACCESSDENIED) once and ends that prompt; it binds nothing, so the next
 *   write of the same upload is a new request (rc 10 and a new prompt, at
 *   that rate). A matched proof, the wired console's arm, a recovery the
 *   user's button entered and bench or automation builds need no press; a
 *   release gadget that holds no device token (not set up, or after Forget)
 *   asks for the same one press. A recovery the gadget entered by itself
 *   refuses update writes with rc 11 (see Recovery mode). A gadget locked
 *   with a passcode asks for it first, then for the press. Reads stay open.
 *   Test vectors: test-vectors/mg-update-v2.json.
 *   A match tells the app that the gadget holds the token
 *   this phone set up, not that the link is secure (sealing does that).
 *   A matched token proof lasts for the BLE connection it was made on (Forget
 *   ends it sooner), and
 *   the link is neither bonded nor encrypted. A relay in radio range that
 *   forwards the proof exchange between the owner's phone and the gadget can
 *   use that connection's proof-gated commands until it ends: read or erase
 *   offline clips, power the gadget off, change stored settings, stream
 *   video or play audio on it, press its HID keys, run its legacy skills,
 *   play haptics and update its firmware with no press. Release firmware
 *   opens the microphone for a client only after a recent press, gesture or
 *   wake word on the gadget (see start_mic).
 *   Where images are signed, they are signed with the repository's
 *   development key, which is public, so a valid signature doesn't show who
 *   built the image. Firmware the relay installs can therefore open the
 *   microphone whenever it likes.
 *   During a call a device without hands_free (see Live call) ignores
 *   start_mic altogether; only the user's own talk hold opens its
 *   microphone then. On a device that lists hands_free, the press, gesture
 *   or wake word that started the call (or the press that accepted it)
 *   grants start_mic for that call, so while it is connecting or live a
 *   relay holding the proof can open the microphone too.
 *   The re-setup prompt is limited per connection, so a nearby device can
 *   raise it again by reconnecting; the talk key never confirms it, so the
 *   owner can only dismiss it.
 *   Without a matched proof, a central can't stream or play audio on a set-up
 *   gadget, and a firmware update needs one physical press on the gadget,
 *   set up or not; anyone in range can install firmware with the owner's
 *   press. Where images are signed, they are signed with the
 *   repository's development key, which is public, so a valid signature
 *   doesn't show who built the image. A nearby device can start an update,
 *   and the owner's next press on the gadget, a talk press included, installs
 *   it. Credentials never leave the device in the clear (the sealed device
 *   session). This protocol is for community gadgets; link encryption and a
 *   sealed command protocol (these commands inside the sealed session, bound
 *   to their sub-command) come in a later revision.
 *   The device answers a challenge promptly, ahead of replies it has queued;
 *   one it can't answer within about 1 s gets error busy, never silence. A
 *   client treats busy (challenge or confirm) as "ask again": it restarts the
 *   exchange on the same link about 200 ms later with a new challenge, and
 *   never counts busy as a refusal. Its 5 s timeout is for silence: each
 *   busy answer restarts it, up to about 20 s from the
 *   first challenge, since a device may answer busy for several seconds
 *   after it boots (e.g. until its random source is seeded).
 *   clear (after result 1 on this connection) erases the device's tokens and
 *   K and returns it to setup; the device answers result 1 first. Before a
 *   match it is error proof_required. A device that declares
 *   mg_session_cap_sealed_clear refuses this unsealed clear with error
 *   [token_proof, insecure] and erases nothing (in the hello's order: no
 *   token is not_found, before a match proof_required, then insecure): it takes the clear only as
 *   the sealed mg_session_clear (a relayed proof must not let a proxy erase
 *   it), so a client sends clear that way to such a device and token_proof
 *   clear only to older firmware.
 *
 * Recovery mode:
 *   A gadget running a recovery or SAFE mode (a boot-loop escape, a crash
 *   guard, its Muse guard off, SAFE at the user's request) declares it in its
 *   request_status answer, after change_data_type and before its command
 *   list: [supported_features, sub_feature, request_status, mode, reason,
 *   flags] (mg_device_mode_t, mg_recovery_reason_t, mg_recovery_flag_t),
 *   e.g. 10 fe 0a 01 01 01. A device in its normal mode never sends it. The
 *   declaration holds for the whole boot: the device leaves recovery only by
 *   restarting into its normal firmware, never on a live connection.
 *   - Device: it lists only what it serves in recovery: request_status,
 *     get_settings (reads) if it serves it, and device_action
 *     (request_battery_status, and reboot with mg_recovery_flag_restart). It
 *     never lists token_proof. It answers everything else with
 *     mg_error_code_recovery (a rescue image may answer unsupported for a
 *     command it doesn't know): it never answers a token proof challenge with
 *     a response, never opens a device session, and never completes Link
 *     setup (its setup service is absent, or refuses before committing
 *     credentials). Its MCUmgr SMP service takes image updates as Token
 *     proof's update rule says: a recovery the user's button entered (on an
 *     automation build a console word counts as that button: see
 *     mg_recovery_reason_user) takes them with no further press; on a
 *     set-up gadget's release build, one the device entered by itself (boot
 *     loop, crash guard) refuses image writes with SMP rc 11 and never
 *     prompts (reads stay open; the way back is then that button route into
 *     recovery, or USB), and so does one on a gadget that holds no device
 *     token. A
 *     button-entered recovery declares reason user, so a client tells the two
 *     apart. On a set-up gadget a bench build's scripted SAFE (reason bench)
 *     takes image updates as a user-entered SAFE does, in bench builds only.
 *   - Client: it trusts nothing from a declared device and runs no token
 *     proof with it: no push-to-talk, audio, video, offline clips, settings
 *     writes, device session or skills.
 *     No Live call or subtitles either.
 *     It keeps the gadget's saved setup, shows that the gadget is in recovery
 *     and why, and offers only an MCUmgr update, Forget (on the phone) and, with
 *     mg_recovery_flag_restart, restart normally (device_action reboot). A
 *     declaration never stands in for the proof: the gadget is trusted again
 *     only when a later connection, with no declaration, proves its token. A
 *     mode the client doesn't know is a recovery mode.
 *   - A recovery mode keeps a distinct identity (another name or address,
 *     never the Muse one), so a client that doesn't handle the declaration
 *     never meets recovery under the gadget's normal identity. A client that
 *     handles it treats a device that does advertise its normal identity and
 *     declares as above.
 *
 * Throughput test (a bench command):
 *   mg_command_device_action with mg_device_action_throughput_test measures
 *   the link, numbered as in the legacy protocol (which defines the device->client
 *   direction; mg adds client->device). The test is optional: a device that
 *   lists device_action accepts mg_command_device_action and answers
 *   mg_error_code_unsupported for any action it doesn't implement, this one
 *   included. A device refuses it with mg_error_code_busy while audio is
 *   live, recording or playing, or a test is already running.
 *   - [device_action, throughput_test, 1, duration_s (uint16)]: send. The
 *     device notifies change_data_type throughput_test, then streams Data
 *     notifications as fast as the link takes them, each MTU-3 bytes: a
 *     uint32 sequence number from 0, then the pattern byte (sequence + i) &
 *     0xFF for byte i of the rest. It stops after duration_s seconds (0:
 *     until stop) and notifies the report below on its own.
 *   - [device_action, throughput_test, 2, duration_s (uint16)]: receive. The
 *     client writes change_data_type throughput_test on Control, then Data
 *     frames in the same format with Write Without Response, honouring its
 *     stack's back-pressure. The device counts them; after duration_s (0:
 *     until stop) it reports and ignores further test data.
 *   - [device_action, throughput_test, 0]: stop. The device answers with the
 *     report for whichever direction ran (a report of zeros if none did):
 *     [device_action, throughput_test, 0, bytes (uint32), packets (uint32),
 *     ms (uint32), sequence gaps (uint32)], counted from the first test
 *     packet to the last. For send, what it sent; for receive, what it got,
 *     and gaps counts sequence numbers missing from what arrived.
 *   While a receive test runs the device keeps its connection latency off,
 *   as for any client->device transfer.
 *
 * Device session (the device's own VM session over BLE):
 *   A BLE gadget reaches its owner's VM with the Noise session a Wi-Fi
 *   gadget uses (esp32/main/noise_control.cpp), with the phone as its
 *   network, but on other routes: a Wi-Fi gadget's Link control, tunnel and
 *   agent name use the VM's Gadget API (/api/gadget/control,
 *   /api/gadget/tunnel, /api/gadget/me), and a BLE gadget's session uses the
 *   VM's daemon (POST /link-control). The
 *   device keeps its own credential, as a Wi-Fi gadget does, and hands it
 *   to the app for the session; the app runs TLS, the WebSocket and Noise to
 *   the VM as the gadget's own node, and the device sends and receives that
 *   session's messages over mg_command_session. A message is one Noise
 *   transport message as the transport framer reassembles it: device ->
 *   client messages are ServiceRequestEnvelopes, client -> device messages
 *   ServiceResponseEnvelopes (esp32/components/noise_core, ServiceCodec.h).
 *   The app never decodes them. So a device's commands are its own
 *   link.register entries on its stream to the daemon's POST /link-control,
 *   Muse runs them
 *   with link.invoke, and its other requests (GET /fs/raw, POST /fs/upload,
 *   /chat/..., /voice/...) are streams of its own; the VM checks what the
 *   gadget role may do.
 *
 *   Opening: after a matching token proof on this connection, the client
 *   sends hello [version, max_message (uint32), window (uint32), hello_flags
 *   (mg_session_hello_flag_t, optional)] and the device answers hello with
 *   its own (echoing mg_session_hello_flag_sealed when it accepts it). version is MG_SESSION_VERSION; a device
 *   that doesn't speak the client's version answers error invalid_value.
 *   max_message is the largest message the sender can take,
 *   MG_SESSION_MIN_MESSAGE..MG_SESSION_MAX_MESSAGE; window is the message bytes it
 *   can hold before it gives credit back, 1..max_message (at least 4 in a
 *   sealed hello: a sealed hello with a smaller window gets invalid_value,
 *   since a sealed message can't start with less). A sealed hello on a link
 *   whose ATT_MTU is below MG_MIN_ATT_MTU gets invalid_value too: the sealed
 *   session's one-write messages (such as credentials_saved) need it,
 *   and a device that can't reach it doesn't declare mg_session_cap_sealed.
 *   A device answers hello with error not_found if it holds no token,
 *   proof_required before a match, and insecure to any hello without
 *   mg_session_hello_flag_sealed, on any link, encrypted or not: link
 *   encryption never stands in for sealing. A hello with
 *   mg_session_hello_flag_sealed to a device that declares
 *   mg_session_cap_sealed is never refused as insecure; that
 *   session, credentials included, is sealed (see Sealed credentials). One
 *   sealed session per token proof: once the device has answered a sealed
 *   hello, that proof's nonces are consumed, and a second sealed hello
 *   before a new matching proof on the connection is refused with
 *   proof_required. A device may end the sealed session on any refusal, so
 *   a client whose sealed message gets an error, or whose answer never comes
 *   (no credentials_saved in 35 s, a dropped link), stops using that session
 *   and sends its next sealed message only after a new proof and one sealed
 *   hello; it never seals twice under a counter it has sent. A device in a
 *   recovery mode answers recovery. A new
 *   hello resets the session as state closed does.
 *
 *   Credentials: right after its hello the device sends credentials
 *   [flags, total_len (uint32, first fragment only), bytes], fragmented as
 *   data is (see Messages) but outside credit, one UTF-8 JSON object of at
 *   most MG_SESSION_MAX_CREDENTIALS bytes, written without spaces:
 *     {"node_id":"...","access_token":"...","refresh_token":"...",
 *      "device_key":"<base64, 32 bytes>","key_id":"<base64, 16 bytes>",
 *      "vm_id":"...","vm_kind":"svm"}
 *   in that order, the keys in standard base64 with = padding. device_key and
 *   key_id are absent on a device set up without a device key; vm_id (the
 *   fetch_vms entry the key is bound to, printable ASCII, at most 64 bytes)
 *   and vm_kind ("svm" or "cvm") are there only when setup stored them with
 *   the key (Gadget setup over Muse Link). The device
 *   passes all of these through exactly as setup stored them and keeps them
 *   across credentials_update (a refreshed pair keeps the binding). The
 *   client opens the session with them as a Wi-Fi gadget would (fetch_vms,
 *   the device-key handshake, the token as the bearer): to the fetch_vms
 *   entry with that vm_id (else the default VM), with the VM's attestation
 *   checked for vm_kind "cvm" and never the token-only handshake there. A
 *   client that can't do the device-key handshake ignores the key fields
 *   and opens no session to a "cvm" binding. It holds them in memory for
 *   this connection only and forgets them when it ends. A new setup's
 *   commit, or a reset, ends the session (as state closed does) and the
 *   hello with it.
 *   When the client refreshes the token pair it sends credentials_update
 *   [flags, total_len, bytes], fragmented as credentials is, with {"access_token":"...","refresh_token":
 *   "...","access_expires_at":<unix seconds>}; the device stores the pair
 *   (and access_expires_at, if it can, to hand back in its next
 *   credentials) in one commit, then answers credentials_saved [1 saved, 0
 *   not saved]. In a sealed session whose client hello carried
 *   mg_session_hello_flag_sealed_saved (the device echoes it), that answer
 *   is sealed like any payload message and framed as credentials (flags
 *   first|last|sealed, total_len, the next device-to-client counter; one
 *   write); otherwise the device answers with the one-byte legacy unsealed
 *   form, which uses no counter. The sealed answer's plaintext is 9 bytes:
 *   the result, then the
 *   first MG_SESSION_SAVED_HASH_LEN bytes of SHA-256 over that update's UTF-8
 *   refresh_token (zeros when not saved). A device that gives up on a
 *   deferred write-back answers sealed [0] while the link is up. The client
 *   counts only a sealed answer whose hash matches the pair it sent as
 *   saved; a sealed [0] or another hash means not saved; an unsealed [1] in
 *   a sealed session confirms nothing (it uses no counter), and with no
 *   sealed answer within 35 s while the link stays up the client assumes
 *   saved (older firmware answers unsealed). Test vectors:
 *   mg-session-seal-v1.json sealed_credentials_saved. A client treats
 *   credentials without access_expires_at as expired and refreshes them if
 *   the device sets
 *   mg_session_cap_client_refreshes. A client whose update wasn't
 *   saved keeps using the new pair for this connection and sends it again on
 *   the next. A credentials_update that isn't such an object (its first
 *   byte is '{', no leading space) gets error
 *   [session, invalid_value] and changes nothing, and so does one whose pair
 *   would push the device's whole stored document - node_id, the pair, the
 *   key fields, any binding and access_expires_at, the bytes its next
 *   credentials sends - past MG_SESSION_MAX_CREDENTIALS: the device checks
 *   that document's size before it stores anything.
 *
 *   Session state: the client reports the VM session with state [state,
 *   epoch (uint16)] (mg_session_state_t): connecting, open, or closed with the
 *   reason it isn't open. open starts a new session with a new epoch: the
 *   device opens its streams from the start (POST /link-control, then
 *   link.register). Any other state ends the session: the device drops
 *   every stream, any partial message and every pending invocation, and
 *   never replays a request in the next session. The device may ask with
 *   open_request [1 open, 0 close]; the client answers with a state when it
 *   can. Data flows only while the state is open. The client never closes an
 *   open session for idleness and never reopens one on its own; it only asks
 *   the device to open one with wake when the VM asks it to. A device
 *   opens and keeps a session only for an active task (a user action in
 *   the last ~2 min, traffic it owes, a wait on the VM for something, or an
 *   open request of its own); registered commands alone don't make it stay
 *   reachable, and an agent's invoke while the session is closed gets
 *   node_offline, unless the device's link.register carried
 *   "is_wakeup_supported": true (top level). Then the VM asks the phone that
 *   holds the device (its gadget.wake command), the client sends wake over
 *   BLE, the device opens its session as for a task of its own, and the VM
 *   sends the waiting invoke on the new session (node_offline if the device
 *   isn't back within 20 s). A device sets is_wakeup_supported only on
 *   builds that handle wake and keep BLE up while idle.
 *   While a task is active and the session is open, the device
 *   sends an empty frame on its /link-control stream at least every 20 s;
 *   with none it sends nothing and lets the session close, which is normal.
 *   After a state it didn't ask for, a device with an active task asks
 *   again with open_request [1] at once, then with backoff while the task
 *   lasts and it stays connected (with none it asks nothing; a task starting
 *   asks then): after closed 1, 2, 4 ... capped
 *   at 60 s; after no_network (the phone's network, or an OS that killed the
 *   app's socket) its own series, 1, 2, 4 ... capped at 15 s; neither series
 *   restarts the other, and an open restarts a series only after a session
 *   that stayed open 60 s (after a shorter one the same kind of end
 *   continues it, so a server that keeps ending at once is never asked
 *   faster); never after its own
 *   open_request [0], not_enrolled, auth_failed or too_large (until its
 *   credentials or the cause change; a credentials_update that saves while
 *   the session is closed asks again at once and starts a new series, since
 *   new credentials may fix it). A request the device makes while the
 *   session isn't open waits for open (about 10 s at most) rather than
 *   being dropped: a phone app that iOS suspends ends its
 *   sessions at each wake, so a gap of a fraction of a second is normal.
 *   Its first request after a wake can land on the connection the OS just
 *   aborted: that request is lost with the session (requests are never
 *   replayed into the next one), so a device re-sends a request whose
 *   session the client closed unasked (closed or no_network, the BLE link
 *   still up) before any response, once the session reopens (once, within
 *   the request's ~10 s). Only GET and HEAD are re-sent that way: a POST
 *   (or any other method) in flight fails with the session and its caller
 *   retries, reading back what landed or starting again under a new id;
 *   nothing whose effect must happen at most once is re-sent blind, by
 *   either side.
 *   After a link loss the request fails and the next
 *   connect starts clean.
 *
 *   Messages: data [flags, total_len (uint32, first fragment only), bytes]
 *   carries one message, one at a time in each direction. The first fragment
 *   has mg_session_flag_first and total_len, the last mg_session_flag_last (one
 *   fragment has both), every fragment carries at least one byte and the
 *   bytes add up to total_len exactly. Every write and notification is at
 *   most min(ATT_MTU - 3, MG_MAX_ATT_VALUE) bytes, headers included
 *   (MG_SESSION_FIRST_HEADER, MG_SESSION_DATA_HEADER). A message larger than the receiver's max_message
 *   is never sent: a client whose VM sends one closes the session with state
 *   too_large, as a Wi-Fi device ends a session on a frame it can't hold.
 *   abort drops the message being received. A fragment that breaks these
 *   rules (a continuation with no message open, a first fragment while one
 *   is open, total_len 0 or over max_message, bytes past total_len) makes
 *   the device answer error [session, invalid_value] and drop the message; the
 *   client closes the session.
 *
 *   Credit: a sender may have at most the receiver's window of message bytes
 *   (fragment payloads, not headers) not yet given back; credit [bytes
 *   (uint32)] gives back bytes the receiver has consumed. A receiver gives
 *   back every byte of a message it has handled, and of one dropped by abort
 *   or an error; one whose window is smaller than its max_message also gives
 *   back the bytes of a message still arriving once they fill its window
 *   (they are in its message buffer), so a message larger than the window
 *   completes; the session's end (state other than open, or a new hello)
 *   resets credit to the window on both sides. credit [0] is valid and
 *   changes nothing: it is the keepalive. While a session is open a device
 *   may nudge the phone so it keeps reading (every 20 s while a screen of
 *   its waits on the session, say): it sends the credit it owes (the bytes
 *   of messages it has fully taken and not yet given back; a message still
 *   arriving earns none), or credit [0] when it owes none; a receiver takes
 *   credit [0] and gives nothing back. A device never buffers past the
 *   credit it granted: a client fragment that would take the client past it
 *   (the device's window, less the bytes it hasn't given back) is dropped
 *   and counted with the message it belongs to, answered [session,
 *   invalid_value], and its bytes given back as for any message dropped by
 *   an error. Credit and state go out ahead of media and session data, and
 *   the client keeps session data from starving media.
 *
 *   Sealed credentials: a device that lists [supported_features,
 *   sub_feature, mg_command_session, caps] with mg_session_cap_sealed
 *   seals credentials and credentials_update when the hello carried
 *   mg_session_hello_flag_sealed, so no BLE bond is needed. Such a message
 *   carries mg_session_flag_sealed on every fragment (a later fragment of a
 *   sealed credentials_update without it drops the update: error [session,
 *   invalid_value], nothing stored, its counter used), and total_len counts
 *   the sealed bytes: counter (uint32) || ciphertext || tag (16 bytes). Keys
 *   come from the
 *   token proof key K and the nonces of the latest matching proof on this
 *   connection: enc_key || mac_key = HKDF-SHA256(salt "mg session seal v1",
 *   IKM K, info client_nonce || device_nonce || node_id, 64). Keystream
 *   block i = HMAC-SHA256(enc_key, "mg session seal v1 stream" || dir ||
 *   counter || i); tag = HMAC-SHA256(mac_key, "mg session seal v1 tag" ||
 *   dir || counter || len(ciphertext) || ciphertext), first 16 bytes; dir 1
 *   device -> client, 2 client -> device; counter, i and lengths uint32
 *   big-endian. Each direction's counter starts at 0 after every sealed
 *   hello and goes up by one per sealed message, and never wraps or repeats:
 *   a receiver that has taken 0xffffffff takes no other counter in that
 *   direction (one that would wrap to 0 is used), and a sender whose next
 *   counter would pass 0xffffffff ends the sealed session instead. A client
 *   then starts a new one (a new proof and sealed hello: fresh keys, counters
 *   from 0); a device seals nothing more in it and refuses each sealed
 *   message from the client with invalid_value. A receiver takes the next
 *   counter at a message's first fragment; if that message's tag then fails,
 *   its counter is given back: the next counter again, or, if a later one was
 *   taken meanwhile (a credentials_update or clear arriving between data
 *   fragments, so two sealed messages can be in flight), a hole it takes
 *   once (at most 255 back). Any other counter gets invalid_value. The keys depend only on K and the proof's nonces, so a
 *   second sealed session under the same proof would reuse the keystream:
 *   the client re-proves (a new challenge, fresh nonces from both sides)
 *   before any new sealed hello on the connection, and both sides pick
 *   every proof nonce fresh from a cryptographic random source. A sealed credentials_update with a wrong tag or counter gets
 *   error [session, invalid_value] and changes nothing. The plaintext is
 *   the JSON above, at most MG_SESSION_MAX_CREDENTIALS bytes; the device
 *   may add "access_expires_at" (unix seconds). A device sets
 *   mg_session_cap_client_refreshes when it holds no Wi-Fi credentials and
 *   so can't refresh its own token pair: the client refreshes it and sends
 *   it back with credentials_update; a device without it refreshes its own
 *   pair and the client never does.
 *   Sealed data: in a sealed session every payload-carrying message is
 *   sealed the same way, data included, both directions: one counter per
 *   direction shared by credentials, credentials_update, credentials_saved
 *   and data, restarting
 *   only at a sealed hello (not at state open or a new epoch). The whole
 *   message is sealed, then fragmented: total_len counts the sealed bytes,
 *   the plaintext is at most max_message - 20, and windows and credit count
 *   sealed bytes. A sealed message's first fragment carries at least the
 *   whole 4-byte counter: a sender that holds less than 4 bytes of credit
 *   waits for more before starting a sealed message (a sealed hello's window
 *   is at least 4), and a receiver may refuse a shorter sealed first
 *   fragment with invalid_length. The receiver takes the counter from the
 *   first fragment (the next one, or the one hole) and checks the tag at the last; a message aborted
 *   after its first fragment has used its counter on both sides. In a sealed
 *   session, unsealed data or credentials_update gets error [session,
 *   insecure] and changes nothing; a wrong tag or counter on data gets
 *   [session, invalid_value], the message is dropped and its credit
 *   returned, and so does client data whose plaintext doesn't decode as a
 *   ServiceResponseEnvelope (another sealed message relabeled as data, say),
 *   its counter used. state, open_request, wake,
 *   credit and abort are never sealed.
 *   A new proof, and every accepted sealed hello, drop each sealed message
 *   part way in or out on both sides, the taken-but-unanswered ones included,
 *   and restart the counters: nothing from the old session completes in the
 *   new one, and no counter of the new session is ever given back for it. A
 *   refused hello changes nothing: the session goes on as it was.
 *   test-vectors/mg-session-seal-v1.json holds sealed messages
 *   (sealed_data included), from mg_session_seal_ref.py.
 *   Known limits of seal v1, for a later version: the tag covers dir,
 *   counter and ciphertext but not the sub-command byte, so a party on the
 *   link can relabel a sealed message, and the plaintext often parses under
 *   the new label: one byte 0x00 is a valid clear (a credentials_update must
 *   start with '{', so a clear's short plaintext can't become one).
 *   A party that both picks a plaintext (the VM, through the data the client
 *   relays) and sits on the link can turn one message into another, so
 *   neither side treats a sealed message's sub-command as authenticated.
 *   state, open_request, wake, credit and abort carry no tag either, so such
 *   a party can also end a session or forge an epoch, and a sealed message
 *   held back lands in that forged epoch. The hello is not authenticated, so
 *   credentials_saved can be withheld until the client's 35 s assume-saved
 *   rule applies. A receiver takes a counter at a message's first fragment,
 *   before any tag checks, so such a party can also hold a client message
 *   back, open a hole at its counter with a first fragment of its own and a
 *   bad last one, and have the held message taken after later ones (within
 *   255), or take counters with first fragments that never complete (a
 *   denial of service). K is derived once, at setup, and not again at a
 *   credentials_update, so whoever learns the token it came from can open
 *   every later session captured off the link (no forward secrecy). Each of
 *   these is for seal v2, which also binds the sub-command and the session
 *   epoch into the tag; test-vectors/mg-session-seal-v1.json relabelled
 *   shows what v1 accepts.
 *
 *   Data, credit, abort and credentials_update before a successful hello, or
 *   data while the state isn't open, get error [session, invalid_value]. The
 *   hello's checks (token proof, sealing) guard the credentials too: they
 *   are the owner's VM credential for this gadget. A client -> device
 *   use of a device -> client sub-command gets unsupported.
 *   test-vectors/mg-session-v1.json holds exchanges byte for byte,
 *   from mg_session_ref.py, a reference device in Python.
 *
 * Skills (superseded by the device session):
 *   A device's commands are its own link.register entries on its VM
 *   session (see Device session), which Muse runs directly. New firmware lists
 *   mg_command_session and not mg_command_skill; this section stays for
 *   devices that shipped it.
 *   A skill is something the device can do for Muse beyond the base
 *   protocol, called by name with JSON parameters (e.g. device.led with r, g
 *   and b). A device that has at least one skill or a skills manifest lists
 *   mg_command_skill in supported_features; any other device answers it
 *   unsupported. The phone app adds base skills of its own (device.status,
 *   device.ota, ...) built from the rest of the protocol; firmware never
 *   lists those names.
 *
 *   Skills document (UTF-8 JSON, written without spaces):
 *     {"v":1,"skills":[<skill>...],"manifest":{"url":"...","revision":"..."}}
 *   skills and manifest are each optional, at least one present. A skill:
 *     {"name":"device.led","description":"...",
 *      "required":{"r":{"type":"integer","description":"Red 0-255"},...},
 *      "optional":{...},"timeout_ms":5000}
 *   the same shape as a Wi-Fi gadget's link.register command entry, plus its
 *   name. type is string, integer, number, boolean, object or array. Names
 *   are lowercase dotted <domain>.<verb>, ASCII, at most MG_SKILL_NAME_MAX
 *   bytes; descriptions at most MG_SKILL_DESCRIPTION_MAX bytes; timeout_ms
 *   defaults to MG_SKILL_DEFAULT_TIMEOUT_MS. The document changes only with
 *   the firmware (or a runtime change that also bumps manifest.revision).
 *
 *   Reading it: the client sends list_request [offset] from 0; the device
 *   answers one list_chunk [offset, total_len, bytes] per request, as many
 *   bytes as fit (all of the rest if they fit). The client asks again at
 *   offset + len until offset + len == total_len. A total_len that changes
 *   between chunks means the document changed: start again at 0. An offset
 *   past total_len is error invalid_value; offset == total_len gets an empty
 *   chunk.
 *
 *   Running one: the client sends the request
 *     {"name":"device.led","params":{"r":255,"g":0,"b":0}}
 *   (params may be absent: {}) in invoke chunks [request_id, offset,
 *   total_len, bytes], in order, each filled but the last. total_len is
 *   1..MG_SKILL_MAX_REQUEST. Once offset + len == total_len the device runs
 *   the skill and sends the result JSON in result chunks [request_id,
 *   offset, total_len, bytes], back to back, each filled but the last:
 *     {"ok":true,"payload":{...}}
 *     {"ok":false,"error":{"code":"<code>","message":"..."}}
 *   code is unknown_skill, invalid_params, busy, unsupported, failed or
 *   timeout; payload is always an object. A result is at most
 *   MG_SKILL_MAX_RESULT bytes. Clients use a new request_id for each invoke
 *   (counting up, wrapping at 255) and wait up to the skill's timeout_ms.
 *
 *   Chunk size: every write and notification is at most ATT_MTU - 3 bytes,
 *   so a chunk carries at most ATT_MTU - 3 - MG_SKILL_LIST_CHUNK_HEADER
 *   bytes of the document (list_chunk) or ATT_MTU - 3 - MG_SKILL_INVOKE_HEADER
 *   bytes of JSON (invoke, result); the headers count every byte before the
 *   payload, the command byte included. Receivers accept shorter chunks.
 *
 *   Rules:
 *   - One invoke at a time per connection. It is in progress from its first
 *     chunk until its result has been sent. A first chunk (offset 0) with
 *     another request_id meanwhile gets a result
 *     {"ok":false,"error":{"code":"busy","message":"another skill is running"}}
 *     for that request_id at once; its later chunks are ignored. A first
 *     chunk with the request_id being received starts that request again.
 *   - A later chunk (offset > 0) of the request being received must start
 *     where the last one ended, keep its total_len, be nonempty and end
 *     within total_len; a first chunk must be nonempty, within total_len,
 *     and total_len 1..MG_SKILL_MAX_REQUEST. Otherwise the device answers
 *     error [mg_command_skill, invalid_value] and discards the request.
 *     Later chunks of a request the device isn't receiving (discarded,
 *     cancelled or refused) are ignored.
 *   - On a release build of a device that holds a token, an invoke's first
 *     chunk before this connection's token proof matched gets error
 *     [mg_command_skill, proof_required] and the request is refused (its
 *     later chunks are ignored); list_request and cancel stay open (see
 *     Token proof).
 *   - cancel [request_id] drops the request if the device is receiving it
 *     (or hasn't started its result); the device sends nothing. A cancel
 *     for any other request_id is ignored. A disconnect drops it too.
 *   - The device checks the request before it runs anything, in this order,
 *     answering a result (not mg_command_error):
 *       not one JSON object        invalid_params "request is not a JSON object"
 *       name not a string          invalid_params "name must be a string"
 *       no skill of that name      unknown_skill  "no skill named <name>"
 *       params not an object       invalid_params "params must be an object"
 *       each declared parameter, required ones first, in schema order:
 *         required and absent      invalid_params "missing parameter <key>"
 *         of the wrong type        invalid_params "parameter <key> must be
 *                                  a string|an integer|a number|a boolean|
 *                                  an object|an array"
 *     Parameters the skill doesn't declare are ignored. An integer is a JSON
 *     number with no fraction or exponent; send integers that way (a device
 *     may refuse 1.0 or 1e0). The skill then checks ranges itself
 *     (invalid_params, the message naming the parameter).
 *   - Skills run off the BLE callback context and never block audio: a
 *     long one answers at once with a payload such as {"status":"started"}.
 *   - No skill is privileged: a client that may invoke at all (a proven
 *     one on a release build of a device that holds a token, as above; any
 *     client on other builds or a device that isn't set up) may run any
 *     listed skill, so firmware lists only skills that are safe to run (no
 *     erase, unpair or update skills; updates are the app's device.ota over
 *     SMP).
 *   - test-vectors/mg-skills-v2.json holds exchanges byte for
 *     byte (list and invoke at ATT MTU 23 and 247, busy, cancel and the
 *     errors above), from mg_skills_ref.py, a reference device in Python.
 *
 * Clock (device actions, numbered as in the legacy protocol):
 *   A device with a clock keeps wall time for its own display; the phone sets it.
 *   - [device_action, set_unix_time, unix_time_ms (int64)
 *     [, utc_offset_minutes (int16) [, flags (uint8)]]]: client -> device. The
 *     client sends it on every connection to a device that lists device_action,
 *     and again whenever the phone's time, timezone or 12/24-hour setting
 *     changes. Local time is UTC plus utc_offset_minutes; flags bit0 is
 *     mg_clock_flag_24_hour (0 = 12-hour), other bits 0. Clients should always
 *     send both: a device keeps the last offset and flags it received when they
 *     are omitted, and before it has received any, its offset and flags are
 *     device-defined. INT64_MIN gets mg_error_code_invalid_value; a device
 *     may also answer invalid_value for a time before 2024-01-01T00:00:00Z
 *     (1704067200000 ms, the floor that tells a set clock from an unset one)
 *     and keeps its clock as it was; every later time up to
 *     9999-12-31T23:59:59Z (253402300799999 ms) is taken; a later one gets
 *     invalid_value.
 *     Otherwise there is no reply. A device without a clock answers
 *     mg_error_code_unsupported. After the first valid set the device is synced
 *     and keeps time across disconnects. It takes the time from any central,
 *     but on a device that holds a token a set from a central without this
 *     connection's token proof changes RAM only (no flash write); the device
 *     may persist offset and flags from a proven connection (including ones
 *     this connection set before its proof matched, once it matches), a
 *     wired console or its own change. A device that holds no token
 *     persists a changed offset or flags at most once per 10 min, when setup
 *     completes, or once at a planned power-off, as for set_settings.
 *   - [device_action, get_unix_time]: client -> device. The device answers
 *     [device_action, report_unix_time, unix_time_ms (int64),
 *     utc_offset_minutes (int16), flags (uint8)], unix_time_ms INT64_MIN if it
 *     was never synced. Its clock may count whole seconds only. The local
 *     time and offset are the owner's: on a release build of a device that
 *     holds a token, get_unix_time gets error proof_required before this
 *     connection's token proof matched (or outside the local window).
 *
 * Haptics (device actions, numbered as in the legacy protocol):
 *   A device with a vibration actuator lists haptics_enabled and
 *   haptics_actuator in its typed set_settings list. haptics_actuator
 *   (read-only) gives the actuator's kind (ERM or LRA), whether it takes a
 *   strength and timing lists, its shortest felt pulse and its strength
 *   steps. Such a device implements haptic_play and haptic_stop. A device
 *   that lists haptics_enabled without haptics_actuator (older firmware) only
 *   buzzes at the talk threshold: clients send it neither action.
 *   - [device_action, haptic_play, motor, 0, id, flags, strength, repeat,
 *     kind, ...]: client -> device. motor is the legacy protocol's (0 = the
 *     first actuator, 0xFF = all; a device with one takes both). The 0 is
 *     the legacy effect list, empty, so the request is also a valid, silent
 *     legacy haptic_play; a request with library effect ids there (1-123)
 *     gets unsupported from a device without a waveform library. id is the
 *     client's and comes back in the status. flags: mg_haptic_flag_t.
 *     strength: 0 = the device's own (ptt_buzz_volume_percent), 1-100
 *     percent; ignored without mg_haptic_actuator_flag_strength. repeat:
 *     passes, 1-255. kind, then its bytes:
 *     - mg_haptic_kind_preset: uint8_t mg_haptic_preset_t.
 *     - mg_haptic_kind_timing (only with mg_haptic_actuator_flag_timing):
 *       uint8_t count (1..MG_HAPTIC_MAX_STEPS), then count uint16_t step
 *       lengths in ms, on and off in turn, on first; an even count's last
 *       step is the off gap before the next pass.
 *     Between passes of a pattern that ends on an on step the device waits
 *     MG_HAPTIC_REPEAT_GAP_MS. The device answers with a status,
 *     [device_action, haptic_play, id, mg_haptic_status_t,
 *     mg_haptic_reason_t]: playing at once, or refused with the reason
 *     (nothing plays); later done, stopped or interrupted, once, when it
 *     ends. A haptic_play while one of the client's patterns plays replaces
 *     it: the old one's stopped (replaced) comes first.
 *     Errors, as mg_command_error for device_action: invalid_length for a
 *     truncated request or a count its bytes don't match; invalid_value for a
 *     motor other than 0 or 0xFF, repeat 0, strength over 100, a step of 0,
 *     an on step over MG_HAPTIC_MAX_ON_MS, a pass over MG_HAPTIC_MAX_PATTERN_MS
 *     or all passes over MG_HAPTIC_MAX_REQUEST_MS; unsupported for an unknown
 *     kind or preset, or a timing list without the timing flag.
 *     On a release build of a device that holds a token, haptic_play before
 *     this connection's token proof matched gets error [device_action,
 *     proof_required] and plays nothing (see Token proof); a device without
 *     haptics answers unsupported first.
 *   - [device_action, haptic_stop, motor]: client -> device. Stops the
 *     client's pattern at once (its status: stopped, reason none); never a
 *     cue the device plays on its own. No other answer. Always taken, proof
 *     or not.
 *   - Rules. The device refuses a pattern (refused) or cuts it short
 *     (interrupted), with the reason: while a firmware update runs (an MCUmgr
 *     upload from its first write until it ends, aborts or its resume hold
 *     expires; a vendor OTA alike) [updating]; on battery at 10 % or less or
 *     under the board's low-battery voltage [low_battery]; when the
 *     pattern's on time doesn't fit the duty budgets (below), refused at
 *     once [duty_limit]; during a push-to-talk turn or while it plays a cue
 *     of its own [in_use]; when haptics_enabled is 0, unless flags has bench
 *     [disabled]; and, if its sleep can't time a pattern without lighting the
 *     screen, while it sleeps [asleep]. bench is for test tools: it skips
 *     haptics_enabled and nothing else. Patterns run from a timer, never
 *     block the BLE or audio path, and stop when the link drops.
 *   - Duty budgets: a client's pattern plays only if its whole on time
 *     (steps shorter than the actuator's shortest pulse counted at that
 *     pulse), added to the actuator's recent use, stays within
 *     MG_HAPTIC_DUTY_MAX_ON_MS for the MG_HAPTIC_DUTY_WINDOW_MS window and
 *     MG_HAPTIC_MINUTE_MAX_ON_MS for the minute. The device counts on time in
 *     5 s buckets of its clock (bucket n holds what ended in [5000 n,
 *     5000 n + 5000) ms), and a window's use is the on time in every bucket
 *     it reaches back into, the current one included: 3 buckets for the 10 s
 *     window, 13 for the minute, plus the step on now. That counts up to one
 *     bucket more than the window, never less than its true use.
 *   - The device's own cues (the talk-threshold buzz with the ptt_buzz_*
 *     settings, the setup confirmation) follow the same rules and
 *     haptics_enabled, and take the actuator from a client pattern
 *     (interrupted, in_use).
 *
 * Link modes:
 *   Several gadgets share one phone's radio, and the central serves every
 *   link at its interval whatever the peripheral latency, so only the gadget
 *   in use should hold a short interval. A device that lists
 *   mg_setting_parameter_link_mode keeps one of three modes and requests the
 *   matching connection parameters (interval min/max in 1.25 ms units,
 *   latency, timeout in 10 ms units; all three meet Apple's accessory rules):
 *   sleep 100/160/4/600, ready 24/36/5/400, active 12/12/0/400 (sleep's
 *   6 s timeout leaves 6 listens at its slowest grant; a 4 s timeout drops
 *   links).
 *   - At connect the device settles to ready (its request goes after the
 *     central's own first update, or about 1 s after connect if none came).
 *   - The device goes active on its own, never slower than without this
 *     setting: at its own talk press (at touch-down),
 *     at a call's start,
 *     when the client starts a stream to it (start_video, start_streaming,
 *     start_mic), and for Link setup and updates for their whole duration.
 *     A talk press's turn stays activity until its reply has played (or
 *     assistant_state done arrives), or 20 s pass with no reply, so the reply
 *     never starts on a slower link.
 *   - 5 s after its last activity ends it returns to the client's mode
 *     (ready unless the client wrote sleep). With no activity for 10 minutes
 *     in ready it goes to sleep by itself. A user's wake or touch on the device
 *     (a press, a touch, the screen lit by the user) returns it from sleep to
 *     ready, not active, and restarts the 10 minutes: whatever the user
 *     starts from sleep would otherwise wait seconds for the central to
 *     schedule the update.
 *   - A client write of active makes it active ahead of a stream; with no
 *     activity for 30 s it returns to ready, so a client that disappears
 *     can't pin it active. A client write of ready or sleep takes effect when
 *     the device's own activity has ended. A write held that way is not
 *     visible in a read (reads return the effective mode): the client sees
 *     it in the notification sent when the activity ends.
 *   - Every effective change notifies [settings, link_mode, mode], so the
 *     client sees a gadget go active on its own. link_params reports what the
 *     controller granted.
 *   - A refused request (L2CAP reject, or an update that fails) leaves the
 *     link as it is; the device logs it, retries once after 5 s, then waits
 *     for the next mode change. A refusal for a procedure collision (LL
 *     error 0x2A, Different Transaction Collision) is retried after about
 *     300 ms instead, if the link isn't already at the target. It accepts any update the central makes by
 *     itself, and never re-requests the set it is in (to force an update,
 *     e.g. at call start,
 *     it goes through the other mode first).
 *   - Clients keep at most one gadget active per phone: when one goes active,
 *     they write ready to every other gadget that is, and write sleep to a
 *     gadget left in ready with no activity for a while.
 *
 * Offline clips:
 *   A device with storage may record push-to-talk utterances while no client
 *   is connected, and hand them over later with mg_command_audio_queue. The
 *   client lists clips with status and clip_info, reads each with read_clip,
 *   and erases the queue with clear once every clip is safe. Live
 *   push-to-talk wins over a download: the device finishes or abandons the
 *   clip transfer before streaming live audio, and the client stops issuing
 *   read_clip while a live utterance is open. Each clip carries its own
 *   format in clip_info (see mg_audio_queue_command_clip_info).
 *   Clips are the owner's voice: on a set-up device, clip_info and read_clip
 *   answer error proof_required and status reports zero clips and zero used
 *   bytes until this connection's token proof matched. At an ownership change
 *   (Forget's clear, a re-setup's commit) the device erases the whole used
 *   clip region, in the background after the commit, keeping an erase mark
 *   until it's done and resuming at boot, so no earlier owner's audio stays
 *   readable on the flash.
 *
 * =============================================================================
 */

#ifdef __OBJC__
#import <Foundation/Foundation.h>
#define MG_ENUM(_type, _name) typedef NS_ENUM(_type, _name)
#else
#define MG_ENUM(_type, _name)                                                \
  typedef _type _name;                                                      \
  enum _name##_Constants
#endif

MG_ENUM(uint8_t, mg_command_t) {
  mg_command_reserved = 0,
  // param 1 (optional): mg_data_type_t audio codec (default: device choice)
  // param 2 (optional): uint16_t sample rate in Hz (default 16000; captured
  //         only if the device lists it for the codec, else 16000)
  // param 3 (optional, after param 2): uint16_t bitrate in kb/s (default
  //         MG_CAPTURE_DEFAULT_BITRATE_KBPS; clamped into the device's range
  //         for the codec and rate). See Capture formats.
  // param 4..N: reserved, ignored by devices
  // The device notifies change_data_type, then streams audio on Data.
  // On a release build of a set-up device, start_mic needs, besides the
  // token proof, a press, gesture or wake word the device itself detected
  // within the last 5 s (console words, protocol commands such as
  // mg_session_wake, and a charger plug-in never count); otherwise it gets
  // error press_required, with no capture and no mic ring. A capture a
  // console word starts over a wireless link follows the same rule (a wired
  // console is the owner's action); a local mic test started on the device
  // that sends nothing off it doesn't. Automation builds need the token
  // proof alone. A release build of a device that isn't set up holds no
  // token, so no proof can match: it refuses a client's start_mic with
  // error proof_required (bench, automation and test builds don't).
  // During a call, and for 3 s after it ends, a device without the Live
  // call hands_free feature ignores start_mic (no capture, no answer) and
  // the stop_mic that follows it; a stop_mic for a capture taken before the
  // call is honored, and a call's start ends that capture. A device that
  // lists hands_free also takes start_mic while a call it started is
  // connecting or live (started by a real press, gesture or wake word, or
  // accepted by a real press; a call a console word or protocol command
  // started opens none), on a new link too once its token proof matched.
  mg_command_start_mic = 1,
  // Client -> device: stop capture.
  // Device -> client: capture has ended. Sent whenever capture ends,
  // whichever side started it.
  mg_command_stop_mic = 2,
  // 3-5 reserved
  // 6 reserved for a future security extension
  // 7 reserved
  // param 1: mg_data_type_t
  // Audio types may add, in order, each optional:
  //   param 2: uint16_t sample rate in Hz (default 16000)
  //   param 3: uint8_t channel count (default 1)
  //   param 4: uint16_t encoded frame size in bytes (0 = variable)
  //   param 5: uint16_t frame duration in microseconds
  mg_command_change_data_type = 8,
  // param 1: uint8_t mic gain level, 1-100 (0 or above 100: error
  // invalid_value). A level, not a register value or dB: 50 = the device's
  // own tuned gain; the device maps 1-50 monotonically onto its lowest usable
  // gain up to that tuned gain, and 50-100 onto the tuned gain up to the
  // highest gain at which normal speech about 30 cm away doesn't clip (equal
  // to the tuned gain if it has no headroom), rounding to the nearest step
  // its codec has, and applies it at once, a running capture included, for
  // every capture format. Until a client sends it on the connection the
  // device uses its tuned gain; a client without a user's choice doesn't
  // send it.
  mg_command_set_mic_gain = 9,
  // No params. Response: change_data_type (current type), supported_features.
  mg_command_request_status = 10,
  // param 1: mg_gesture_t, param 2 (optional): button index
  mg_command_gesture = 11,
  // 12-15 reserved
  // Response payloads:
  //   Command list (commands the device accepts from the client; commands it
  //   only notifies, such as gesture or error, are not listed):
  //     [mg_command_supported_features, <mg_command_t>...]
  //   Typed sub-feature list for namespaces under a command:
  //     [mg_command_supported_features, mg_command_sub_feature,
  //      <owning mg_command_t>, <namespace-specific values...>]
  // Defined typed lists:
  //   [supported_features, sub_feature, mg_command_start_mic,
  //    <mg_data_type_t>...]   audio codecs the device can capture with
  //   [supported_features, sub_feature, mg_command_change_data_type,
  //    (mg_data_type_t, uint8_t count, count x (uint16_t Hz, uint16_t min
  //    kb/s, uint16_t max kb/s))...]   the rates and bitrates the device
  //    captures at, per codec, 16000 first (see Capture formats; absent:
  //    16000 at the default bitrate only)
  //   [supported_features, sub_feature, mg_command_set_settings,
  //    <mg_setting_parameter_t>...]   every setting the device accepts in
  //    get_settings and set_settings. Clients offer only these (e.g. no
  //    vibration controls on a device that doesn't list haptics_enabled)
  //    and never send others.
  //   [supported_features, sub_feature, mg_command_live_call,
  //    <mg_live_call_feature_t>...]   live call features the device has
  //    (any number; clients ignore values they don't know; absent: none; a
  //    later list replaces an earlier one on that connection)
  //   [supported_features, sub_feature, mg_command_request_status,
  //    mg_device_mode_t, mg_recovery_reason_t, mg_recovery_flag_t]   the
  //    device is in a recovery mode (see Recovery mode); sent before the
  //    command list, never by a device in its normal mode
  //   [supported_features, sub_feature, mg_command_session,
  //    mg_session_cap_t caps]   what the device session can do (see Device
  //    session); after the command list. Absent, or caps without
  //    mg_session_cap_sealed: the device doesn't support sealed credentials
  mg_command_supported_features = 16,
  // 17-24 reserved
  // param 1: mg_command_t that errored, param 2: array of mg_error_data_t
  mg_command_error = 25,
  // 26-28 reserved
  // param 1: mg_stream_audio_command_t, additional params depend on
  // sub-command. See Audio playback.
  mg_command_stream_audio = 29,
  // param 1: mg_device_action_t, additional params depend on the action.
  // See Clock, Haptics, Recovery mode and Throughput test.
  mg_command_device_action = 30,
  // 31-33 reserved
  // param 1: mg_token_proof_command_t, additional params depend on
  // sub-command. See Token proof.
  mg_command_token_proof = 34,
  // 35-62 reserved
  // param 1: array of mg_setting_parameter_t to fetch
  // Response: array of (mg_setting_parameter_t, value) pairs; each value's
  //         byte width is determined by its parameter (see mg_setting_parameter_t)
  // The device also sends that shape unsolicited when its user changes a
  // setting on the device (see Settings reports). On a release build of a
  // device that holds a token, client_name and client_app are the owner's
  // content: before this connection's token proof matched (and outside the
  // local window) the answer skips them, then the device notifies
  // proof_required once; the other fields always answer (see Token proof).
  mg_command_get_settings = 63,
  // 64 reserved
  // param 1: array of (mg_setting_parameter_t, value) pairs; each value's
  //         byte width is determined by its parameter (see mg_setting_parameter_t)
  // No response on success; read values back with get_settings.
  // A device that meets an unknown parameter stops parsing and notifies
  // mg_error_code_unsupported.
  // On a device that holds a token, a change to a persisted setting from a
  // central without a matching token proof on this connection is skipped
  // and changes nothing: the device applies the write's other pairs, then
  // notifies mg_error_code_proof_required once. A device that holds no token
  // takes it, but writes such changes to flash at most once per 10 min,
  // when setup completes, or once at a planned power-off (never a flash
  // write per change a central makes). Per-connection settings
  // (push_to_talk_enabled, haptics_enabled, link_mode, client_app) are never
  // refused for this, nor is the time (set_unix_time).
  mg_command_set_settings = 65,
  // 66-75 reserved
  // param 1: mg_audio_queue_command_t, additional params depend on sub-command.
  // See Offline clips.
  mg_command_audio_queue = 76,
  // 77-0x7F reserved
  // 0x80-0x82 reserved for a future security extension
  // Client -> device. param 1: mg_assistant_state_t. No reply.
  // See Assistant state.
  mg_command_assistant_state = 0x83,
  // param 1: mg_stream_video_command_t, additional params depend on
  // sub-command. See Video streaming.
  mg_command_stream_video = 0x84,
  // Both directions. param 1: mg_live_call_request_t (device -> client) or
  // mg_live_call_state_t (client -> device). See Live call.
  mg_command_live_call = 0x85,
  // param 1: mg_ui_input_command_t, additional params depend on
  // sub-command. See UI input.
  mg_command_ui_input = 0x86,
  // Both directions. param 1: mg_subtitle_command_t. See Live call subtitles.
  mg_command_subtitle = 0x87,
  // param 1: mg_skill_command_t, additional params depend on sub-command.
  // See Skills. Superseded by mg_command_session.
  mg_command_skill = 0x88,
  // Both directions. param 1: mg_session_command_t, additional params depend
  // on sub-command. The device's own VM session over BLE; see Device session.
  mg_command_session = 0x89,
  // 0x8A-0xFD reserved
  // Reserved for typed mg_command_supported_features payloads only.
  // This is not a standalone top-level command.
  mg_command_sub_feature = 0xFE,
  // Reserved for future extended command protocol
  mg_command_extended = 0xFF,
};

// Other values reserved.
MG_ENUM(uint8_t, mg_data_type_t) {
  mg_data_type_unknown = 0,
  mg_data_type_audio_sbc = 1,
  mg_data_type_audio_adpcm = 2,
  mg_data_type_audio_opus = 3,
  mg_data_type_audio_pcm = 6,
  mg_data_type_audio_lc3 = 19,
  // Throughput test data (see Throughput test), the same number as in the
  // legacy protocol.
  mg_data_type_throughput_test = 20,
  // Compressed bytes of one offline clip. The clip's codec and byte count come
  // from mg_audio_queue_command_clip_info; the type stays set for the whole
  // download.
  mg_data_type_audio_queue = 23,
  // H.264 constrained baseline video (see Video streaming): CAVLC, no
  // B-frames, one or more slices, 4:2:0, 8 bits.
  mg_data_type_video_h264 = 24,
  // VP8 video (RFC 6386, see Video streaming): each frame is one VP8 frame
  // as libvpx writes it (no IVF or RTP header), 4:2:0, 8 bits. Any version
  // (profile) decodes; version 1 (bilinear prediction) with the loop filter
  // off is the cheapest for a gadget to decode.
  mg_data_type_video_vp8 = 25,
  // One baseline JPEG file (JFIF/EXIF, Huffman, 8 bits) per frame of a
  // still-image stream.
  mg_data_type_image_jpeg = 27,
  // Framed Data, for a device that lists
  // mg_stream_video_capability_flag_framed while a video stream and audio
  // playback share the Data characteristic. The client selects it once with
  // change_data_type; from then on every Data write starts with one
  // mg_data_type_t byte naming the stream the rest of the write belongs to
  // (the video stream's codec, or the playback's audio codec), and the
  // client sends no more change_data_type while it stays framed.
  // - The type byte is not stream data: credits, buffer_update counts and
  //   frame sizes exclude it. A write carries at most (max write - 1) bytes
  //   of stream data.
  // - Audio writes still carry whole frames; video still splits anywhere.
  //   flush and video_anchor apply in order with the Data around them.
  // - A write whose type byte names no open stream (or a stream the device
  //   ended) is discarded and counted, as plain Data for a closed stream
  //   is; no "discard until the next change_data_type" rule applies. So is
  //   a write with no stream data (the type byte alone, or an empty write):
  //   it adds nothing to any stream's counts and changes no state.
  //   Test vectors: test-vectors/mg-framed-v1.json.
  // - A repeated change_data_type(framed) while already framed is accepted
  //   as a no-op (a client re-selects framed whenever both streams open
  //   again, e.g. a video restart or a new per-reply playback stream).
  // - Framed mode ends with a change_data_type to any other type, when both
  //   streams have ended, or at disconnect. A device that lists the flag
  //   still takes plain change_data_type, so older clients keep working.
  // - The client uses framed only while both streams are open; with one
  //   stream it writes plain Data, and when one stream ends while
  //   the other stays open it first sends change_data_type to the remaining
  //   stream's type (until then the device stays framed). A client that keeps
  //   one playback stream open across replies stays framed meanwhile; with a
  //   playback stream per reply it re-enters framed per reply.
  //   A Live call keeps one reply stream open for the whole call, so it stays
  //   framed for the call.
  mg_data_type_framed = 30,
};

// Other values reserved.
MG_ENUM(uint8_t, mg_gesture_t) {
  mg_gesture_button_down = 0,
  mg_gesture_button_up = 1,
};

MG_ENUM(uint8_t, mg_error_data_t) {
  // param 1: mg_error_code_t (uint16)
  mg_error_data_code = 1,
  // param 1: size (uint16), param 2: string
  mg_error_data_log = 2,
};

// 0x0100-0x01FF are reserved for a future security extension.
MG_ENUM(uint16_t, mg_error_code_t) {
  // Unknown command, sub-command, or setting parameter
  mg_error_code_unsupported = 1,
  mg_error_code_invalid_length = 2,
  mg_error_code_invalid_value = 3,
  // The device cannot do this right now (e.g. read_clip during live audio).
  mg_error_code_busy = 4,
  // The thing asked for doesn't exist (e.g. a token proof challenge on a
  // device that holds no token).
  mg_error_code_not_found = 5,
  // A command that waits for this connection's token proof (see Token
  // proof), before the proof matched.
  mg_error_code_proof_required = 6,
  // Not sealed where only sealed is taken: a session hello without
  // mg_session_hello_flag_sealed (on any link), an unsealed data,
  // credentials_update or clear message, and an unsealed token_proof clear
  // to a device that declares mg_session_cap_sealed_clear.
  mg_error_code_insecure = 7,
  // The device is in a recovery mode and won't do this until it restarts
  // normally (see Recovery mode).
  mg_error_code_recovery = 8,
  // start_mic on a release build with no recent press on the device (see
  // start_mic).
  mg_error_code_press_required = 9,
};

// Other values reserved.
MG_ENUM(uint8_t, mg_setting_parameter_t) {
  // value: uint8_t MG_SPEC_VERSION (read-only)
  mg_setting_parameter_spec_version = 0,
  // Per connection, not persisted: 0 at every new connection.
  // value: uint8_t bool (0=disabled, 1=enabled)
  mg_setting_parameter_push_to_talk_enabled = 2,
  // Enables the device's vibration: the push-to-talk activation buzz, its
  // other cues and haptic_play (only a bench test skips it; see Haptics).
  // Only devices with a vibration motor list this; the ptt_buzz_* settings are
  // listed only where the device honors them.
  // Per connection, not persisted: 0 at every new connection.
  // value: uint8_t bool (0=disabled, 1=enabled)
  mg_setting_parameter_haptics_enabled = 3,
  // Push-to-talk activation buzz frequency. Persisted on device.
  // value: uint16_t freq_hz (20-20000 Hz). Default 210.
  mg_setting_parameter_ptt_buzz_freq_hz = 4,
  // Push-to-talk activation buzz duration, played for at most
  // MG_HAPTIC_MAX_ON_MS. Persisted on device.
  // value: uint16_t duration_ms (1-5000 ms). Default 100.
  mg_setting_parameter_ptt_buzz_duration_ms = 5,
  // Push-to-talk activation buzz volume: on an actuator with
  // mg_haptic_actuator_flag_strength, the strength of every cue the device
  // plays on its own and of haptic_play's strength 0 (0 = its weakest);
  // ignored otherwise. Persisted on device.
  // value: uint8_t volume percent (0-100). Default 80.
  mg_setting_parameter_ptt_buzz_volume_percent = 6,
  // Codec for device-initiated capture (push-to-talk turns). Persisted on
  // device: remembered across connections and reboots; SBC on a fresh device
  // or after a factory reset. Clients still send their chosen codec at every
  // connect, and get_settings answers the remembered value.
  // value: uint8_t mg_data_type_t. Default mg_data_type_audio_sbc.
  mg_setting_parameter_audio_codec = 7,
  // Record push-to-talk utterances to the offline queue while no client is
  // connected. Persisted on device.
  // value: uint8_t bool (0=disabled, 1=enabled)
  mg_setting_parameter_audio_queue_enabled = 8,
  // 9 reserved
  // Speaker volume for playback (see Audio playback) and device sounds.
  // Persisted on device. 0 turns the speaker off; any other value turns it on
  // at that volume. Only devices with a speaker accept it.
  // value: uint8_t volume percent (0-100)
  mg_setting_parameter_speaker_volume = 10,
  // Brightness of the device's status light. Persisted on device. Only
  // devices with a dimmable status light list it. 0 turns the light off
  // except for what needs the user's attention (mic capture, setup
  // confirmation, errors).
  // value: uint8_t percent (0-100)
  mg_setting_parameter_led_brightness = 11,
  // Capture with a lower-power microphone path (e.g. one microphone instead
  // of a beamformed array) at some cost in quality. Persisted on device.
  // Only devices with such a path list it.
  // value: uint8_t bool (0=best quality, 1=low power)
  mg_setting_parameter_mic_low_power = 12,
  // Sample rate for device-initiated capture (push-to-talk turns), where the
  // capture's codec takes it, else 16000.
  // Talk holds in a call are device-initiated capture too.
  // Persisted on device: remembered across connections and reboots; 16000 on
  // a fresh device or after a factory reset. Clients still send their chosen rate
  // at every connect, and get_settings answers the remembered value. Only
  // devices that capture at more than one rate list it; a rate the device
  // doesn't list for any codec gets invalid_value and changes nothing. See
  // Capture formats.
  // value: uint16_t sample rate in Hz. Default 16000.
  mg_setting_parameter_capture_sample_rate = 13,
  // Bitrate for device-initiated capture where the capture's codec takes a
  // choice of bitrates (LC3), clamped into the device's range for its codec
  // and rate. Persisted on device: remembered across connections and
  // reboots; MG_CAPTURE_DEFAULT_BITRATE_KBPS on a fresh device or after a
  // factory reset. Clients still send their chosen bitrate at every connect,
  // and get_settings answers the remembered value. Only devices that
  // capture at a choice of bitrates list it; a value outside every listed
  // range gets invalid_value and changes nothing. See Capture formats.
  // value: uint16_t kb/s. Default 32.
  mg_setting_parameter_capture_bitrate_kbps = 14,
  // The vibration actuator (read-only: set_settings refuses it with
  // invalid_value). Listed with haptics_enabled by devices that implement
  // haptic_play and haptic_stop (see Haptics).
  // value: 4 bytes: uint8_t mg_haptic_actuator_t kind, uint8_t
  // mg_haptic_actuator_flag_t flags, uint8_t min_on_ms (the shortest on step
  // it renders as a felt pulse; shorter on steps are lengthened to it),
  // uint8_t strength steps (how many strengths it renders; 0 without the
  // strength flag).
  mg_setting_parameter_haptics_actuator = 15,
  // The link mode (see Link modes). Per connection, ready (1) at every
  // connect. A client write is the mode it wants for this gadget; a read
  // returns the effective mode, and the device notifies the setting whenever
  // its effective mode changes (its own activity starting or ending).
  // value: uint8_t mg_link_mode_t.
  mg_setting_parameter_link_mode = 16,
  // The connection parameters the controller last reported (read-only:
  // set_settings refuses it with invalid_value). value: 6 bytes: uint16_t
  // interval (1.25 ms units), uint16_t latency (events), uint16_t
  // supervision timeout (10 ms units), little-endian; all 0 when the
  // controller hasn't reported any yet.
  mg_setting_parameter_link_params = 17,
  // Who is connected, for the gadget's status page. The
  // device shows the connected device's name as it reads it itself: the
  // central's GAP Device Name (0x2A00) over GATT, or a classic link's remote
  // name. The name is the user's: a device never prints it on a console the
  // app relays into its logs (NUS); its screen and a USB console may show it.
  // client_name: reserved; clients don't write it.
  // A device need not list it; one that does sees nothing written.
  // value: uint8_t length (0-MG_CLIENT_NAME_MAX), then UTF-8.
  mg_setting_parameter_client_name = 18,
  // client_app: per connection, not persisted, empty at every new
  // connection. A device that takes it lists it in its set_settings typed
  // list (clients send only listed settings); a client writes it once per
  // connection after a matching token proof (after request_status if the
  // device lists no token_proof; a device may refuse it before the proof)
  // and ignores unsupported. value: uint8_t length, UTF-8 app name
  // ("Muse app"), uint8_t length, UTF-8 version ("16.0.0.86"), each
  // 0-MG_CLIENT_APP_FIELD_MAX bytes. Shown as "<app name>: <version>" when
  // the status row is tapped. client_name and client_app are the owner's
  // content for get_settings (see mg_command_get_settings).
  mg_setting_parameter_client_app = 19,
};

#define MG_CLIENT_NAME_MAX 32
#define MG_CLIENT_APP_FIELD_MAX 24

// mg_setting_parameter_link_mode's values.
MG_ENUM(uint8_t, mg_link_mode_t) {
  // Long interval, for a gadget nobody is using: 100/160/4/600
  // (125-200 ms, latency 4, 6 s).
  mg_link_mode_sleep = 0,
  // The default when connected and not active: 24/36/5/400 (30-45 ms,
  // latency 5, 4 s).
  mg_link_mode_ready = 1,
  // Streams, push-to-talk capture, reply playback, setup and updates:
  // 12/12/0/400 (15 ms, latency 0, 4 s).
  // Calls too.
  mg_link_mode_active = 2,
};

MG_ENUM(uint8_t, mg_assistant_state_t) {
  // No turn in progress.
  mg_assistant_state_idle = 0,
  // The assistant is working on the utterance. Devices show it on their own
  // after stop_mic; clients send it after a client-driven capture.
  mg_assistant_state_thinking = 2,
  // The reply is arriving or playing.
  mg_assistant_state_responding = 3,
  // The reply is complete; the device plays its finished animation, then
  // goes idle.
  mg_assistant_state_done = 4,
  // The turn failed; the device shows its error state, then goes idle.
  mg_assistant_state_error = 5,
};

// Flags in report_battery_status (same bits as the legacy protocol).
MG_ENUM(uint8_t, mg_battery_status_flag_t) {
  mg_battery_status_flag_charging = 1 << 0,
  mg_battery_status_flag_present = 1 << 1,
  mg_battery_status_flag_current_measured = 1 << 2,
  mg_battery_status_flag_current_estimated = 1 << 3,
};

// Actions under mg_command_device_action, numbered as the legacy protocol's
// device actions so the two never diverge: values mg doesn't define are
// reserved for their legacy meanings.
MG_ENUM(uint8_t, mg_device_action_t) {
  // Client -> device. No params. The device restarts into its normal
  // firmware (the legacy reboot). Only a device in a recovery mode that sets
  // mg_recovery_flag_restart accepts it; any other answers
  // mg_error_code_unsupported. See Recovery mode.
  mg_device_action_reboot = 0,
  // 1-7 reserved (legacy device actions)
  // Client -> device. No params; answered with report_battery_status.
  mg_device_action_request_battery_status = 8,
  // Device -> client (an answer, or unsolicited when charging starts/stops).
  // param 1: uint16_t voltage_mv, param 2: int16_t current_100ua (negative =
  // discharging), param 3: uint8_t soc_percent, param 4: int16_t
  // temperature_c, param 5: uint16_t capacity_mah, param 6: uint16_t
  // full_capacity_mah, param 7: uint32_t timestamp (device ms), param 8:
  // uint8_t mg_battery_status_flag_t. Unknown values are 0 (flags say which
  // are measured). soc_percent is 0-100: a client ignores a larger value
  // (keeps the last level, never clamps). Same layout as the legacy protocol.
  mg_device_action_report_battery_status = 9,
  // Client -> device. No params. The device powers off (a key or the charger
  // wakes it). Devices that can't power off don't list it.
  mg_device_action_power_off = 10,
  // param 1: uint8_t 0 = stop (and report), 1 = send (device -> client),
  //          2 = receive (client -> device; an mg extension)
  // param 2: uint16_t duration in seconds (0 = until stop), when starting
  // See Throughput test.
  mg_device_action_throughput_test = 11,
  // 12-14 reserved (legacy device actions)
  // Client -> device. param 1: uint8_t motor (0, or MG_HAPTIC_MOTOR_ALL),
  // param 2: the legacy effect list, empty (one 0), then uint8_t id, uint8_t
  // mg_haptic_flag_t flags, uint8_t strength (0 = the device's own, 1-100),
  // uint8_t repeat (1-255), uint8_t mg_haptic_kind_t, then the kind's bytes.
  // Device -> client (status): uint8_t id, uint8_t mg_haptic_status_t,
  // uint8_t mg_haptic_reason_t. See Haptics.
  mg_device_action_haptic_play = 15,
  // 16 reserved (the legacy haptic_repeat; mg repeats with haptic_play's
  // repeat)
  // Client -> device. param 1: uint8_t motor (0, or MG_HAPTIC_MOTOR_ALL).
  // Stops the client's pattern. See Haptics.
  mg_device_action_haptic_stop = 17,
  // Client -> device. param 1: int64_t unix_time_ms (UTC; INT64_MIN refused,
  // and a device may refuse a time before 2024-01-01T00:00:00Z), param 2
  // (optional): int16_t utc_offset_minutes, param 3 (optional): uint8_t
  // mg_clock_flags_t. No reply. See Clock.
  mg_device_action_set_unix_time = 18,
  // Client -> device. No params; answered with report_unix_time (the owner's
  // content: proof_required before the proof on a set-up release build). See
  // Clock.
  mg_device_action_get_unix_time = 19,
  // Device -> client. param 1: int64_t unix_time_ms (INT64_MIN if never synced),
  // param 2: int16_t utc_offset_minutes, param 3: uint8_t mg_clock_flags_t.
  mg_device_action_report_unix_time = 20,
  // 21-0xFF reserved (legacy device actions)
};

// Live call features a device lists as
// [supported_features, sub_feature, mg_command_live_call, features...].
MG_ENUM(uint8_t, mg_live_call_feature_t) {
  // The device captures the user for the whole call, hands-free: the client's
  // start_mic at live carries the call until it ends. Without it, the client
  // opens no call capture: push-to-talk carries the call.
  mg_live_call_feature_hands_free = 1,
  // The device takes a mg_live_call_failure_reason_t after a failed state
  // and shows it.
  mg_live_call_feature_failure_reason = 2,
};

// Flags of set_unix_time / report_unix_time (param 3).
MG_ENUM(uint8_t, mg_clock_flags_t) {
  // Show a 24-hour clock (else 12-hour).
  mg_clock_flag_24_hour = 1,
};

// Throughput test directions, param 1 of mg_device_action_throughput_test.
MG_ENUM(uint8_t, mg_throughput_test_t) {
  mg_throughput_test_stop = 0,
  mg_throughput_test_send = 1,
  mg_throughput_test_receive = 2,
};

// Haptics (see Haptics). haptic_play's motor byte for every actuator.
#define MG_HAPTIC_MOTOR_ALL 0xFF
// Most steps in a timing list.
#define MG_HAPTIC_MAX_STEPS 16
// Longest on step, and longest the actuator stays on without a break.
#define MG_HAPTIC_MAX_ON_MS 500
// Longest pass of a pattern, steps added up.
#define MG_HAPTIC_MAX_PATTERN_MS 5000
// Longest haptic_play, all passes and the gaps between them.
#define MG_HAPTIC_MAX_REQUEST_MS 10000
// Off gap between passes of a pattern that ends on an on step.
#define MG_HAPTIC_REPEAT_GAP_MS 300
// Duty limits: on time in any window of these lengths.
#define MG_HAPTIC_DUTY_WINDOW_MS 10000
#define MG_HAPTIC_DUTY_MAX_ON_MS 5000
#define MG_HAPTIC_MINUTE_MAX_ON_MS 20000

// mg_setting_parameter_haptics_actuator's kind.
MG_ENUM(uint8_t, mg_haptic_actuator_t) {
  // An eccentric rotating mass motor.
  mg_haptic_actuator_erm = 1,
  // A linear resonant actuator.
  mg_haptic_actuator_lra = 2,
};

// mg_setting_parameter_haptics_actuator's flags.
MG_ENUM(uint8_t, mg_haptic_actuator_flag_t) {
  // It renders a strength (PWM duty or drive amplitude); without it every
  // pattern plays at full drive.
  mg_haptic_actuator_flag_strength = 0x01,
  // It takes timing lists (mg_haptic_kind_timing); presets always work.
  mg_haptic_actuator_flag_timing = 0x02,
};

// haptic_play's flags.
MG_ENUM(uint8_t, mg_haptic_flag_t) {
  // A bench or developer test: plays even with haptics_enabled 0. Every other
  // rule still applies. Test tools only, never a product path.
  mg_haptic_flag_bench = 0x01,
};

// haptic_play's kind.
MG_ENUM(uint8_t, mg_haptic_kind_t) {
  // Then uint8_t mg_haptic_preset_t.
  mg_haptic_kind_preset = 1,
  // Then uint8_t count, count uint16_t ms: on, off, on, ...
  mg_haptic_kind_timing = 2,
};

// Presets, each with the steps an on/off actuator plays (ms, on first). An
// LRA or a driver with its own effects may play its nearest effect instead,
// at half to twice the length.
MG_ENUM(uint8_t, mg_haptic_preset_t) {
  mg_haptic_preset_tap = 1,       // 50
  mg_haptic_preset_double = 2,    // 50, 120, 50
  mg_haptic_preset_long = 3,      // 400
  mg_haptic_preset_ring = 4,      // 400, 200, 400, 1000 (one ring cycle)
  mg_haptic_preset_success = 5,   // 40, 100, 120
  mg_haptic_preset_error = 6,     // 120, 80, 120, 80, 120
};

// haptic_play's status.
MG_ENUM(uint8_t, mg_haptic_status_t) {
  // Accepted and started.
  mg_haptic_status_playing = 0,
  // Played to the end.
  mg_haptic_status_done = 1,
  // Ended by the client: haptic_stop (reason none) or its next haptic_play
  // (reason replaced).
  mg_haptic_status_stopped = 2,
  // Cut short by the device; the reason says why.
  mg_haptic_status_interrupted = 3,
  // Never started; the reason says why.
  mg_haptic_status_refused = 4,
};

// Why a pattern was refused, interrupted or stopped.
MG_ENUM(uint8_t, mg_haptic_reason_t) {
  mg_haptic_reason_none = 0,
  // haptics_enabled is 0 (and the request isn't a bench test).
  mg_haptic_reason_disabled = 1,
  // A firmware update is running.
  mg_haptic_reason_updating = 2,
  mg_haptic_reason_low_battery = 3,
  // The device sleeps and won't light its screen for this.
  mg_haptic_reason_asleep = 4,
  // The duty budget is spent; retry later.
  mg_haptic_reason_duty_limit = 5,
  // A push-to-talk turn, or a cue the device plays on its own, has the
  // actuator.
  mg_haptic_reason_in_use = 6,
  // The client's next haptic_play.
  mg_haptic_reason_replaced = 7,
};

// The mode in the request_status typed list (see Recovery mode).
MG_ENUM(uint8_t, mg_device_mode_t) {
  // A recovery or SAFE mode: Muse isn't served and the token can't be proved.
  mg_device_mode_recovery = 1,
};

// Why the device is in a recovery mode.
MG_ENUM(uint8_t, mg_recovery_reason_t) {
  mg_recovery_reason_unspecified = 0,
  // The boot-loop escape: too many failed boots in a row.
  mg_recovery_reason_boot_loop = 1,
  // A crash guard: the last run ended in a crash or a watchdog reset.
  mg_recovery_reason_crash_guard = 2,
  // The Muse guard turned Muse off after repeated Muse failures, until it is
  // turned back on.
  mg_recovery_reason_muse_guard = 3,
  // The user's button route asked for SAFE mode (open to image updates); a
  // console word counts only where that word is open (automation builds, or
  // a proven link / local window on release builds).
  mg_recovery_reason_user = 4,
  // A bench build's scripted SAFE request; never in a release image. A client
  // treats an unknown reason as a plain SAFE.
  mg_recovery_reason_bench = 5,
};

// Flags in the recovery declaration.
MG_ENUM(uint8_t, mg_recovery_flag_t) {
  // device_action reboot restarts the device into its normal firmware.
  mg_recovery_flag_restart = 0x01,
};

#define MG_TOKEN_PROOF_NONCE_LEN 16
#define MG_TOKEN_PROOF_MAC_LEN 32

MG_ENUM(uint8_t, mg_token_proof_command_t) {
  // Client -> device. Payload: uint8_t client_nonce[16]
  mg_token_proof_challenge = 0,
  // Device -> client. Payload: uint8_t device_nonce[16], uint8_t mac[32]
  mg_token_proof_response = 1,
  // Client -> device. Payload: uint8_t mac[32]
  mg_token_proof_confirm = 2,
  // Device -> client. Payload: uint8_t matched (1 or 0)
  mg_token_proof_result = 3,
  // Client -> device. No payload. Only after a match on this connection,
  // and only to a device without mg_session_cap_sealed_clear (one with it
  // answers insecure and takes mg_session_clear instead).
  mg_token_proof_clear = 4,
};

#define MG_SKILL_MAX_REQUEST         1024
#define MG_SKILL_MAX_RESULT          2048
#define MG_SKILL_NAME_MAX            48
#define MG_SKILL_DESCRIPTION_MAX     300
#define MG_SKILL_DEFAULT_TIMEOUT_MS  10000
// Bytes before the payload, the command byte included (see Skills).
#define MG_SKILL_LIST_CHUNK_HEADER   6
#define MG_SKILL_INVOKE_HEADER       7

MG_ENUM(uint8_t, mg_skill_command_t) {
  // Client -> device. Payload: uint16_t offset into the skills document.
  mg_skill_list_request = 0,
  // Device -> client. Payload: uint16_t offset, uint16_t total_len, then
  // bytes of the skills document from offset.
  mg_skill_list_chunk = 1,
  // Client -> device. Payload: uint8_t request_id, uint16_t offset,
  // uint16_t total_len, then bytes of the request JSON from offset.
  mg_skill_invoke = 2,
  // Device -> client. Payload: uint8_t request_id, uint16_t offset,
  // uint16_t total_len, then bytes of the result JSON from offset.
  mg_skill_result = 3,
  // Client -> device. Payload: uint8_t request_id. No reply.
  mg_skill_cancel = 4,
};

#define MG_SESSION_VERSION      1
#define MG_SESSION_MIN_MESSAGE  4096u
#define MG_SESSION_MAX_MESSAGE  (16u * 1024u * 1024u)
#define MG_SESSION_MAX_CREDENTIALS 2048u
// Bytes of SHA-256(refresh_token) in a sealed credentials_saved.
#define MG_SESSION_SAVED_HASH_LEN 8
// Bytes before the message bytes, the command byte included (see Device
// session). Credentials fragments use the same headers.
#define MG_SESSION_FIRST_HEADER 7
#define MG_SESSION_DATA_HEADER  3

MG_ENUM(uint8_t, mg_session_command_t) {
  // Both directions, client first. Payload: uint8_t version,
  // uint32_t max_message, uint32_t window, then optionally uint8_t
  // mg_session_hello_flag_t hello_flags.
  mg_session_hello = 0,
  // Client -> device. Payload: uint8_t mg_session_state_t, uint16_t epoch.
  mg_session_state = 1,
  // Device -> client. Payload: uint8_t open (1 open, 0 close).
  mg_session_open_request = 2,
  // Both directions. Payload: uint8_t mg_session_data_flag_t flags, then
  // uint32_t total_len if flags has mg_session_flag_first, then message bytes.
  mg_session_data = 3,
  // Both directions. Payload: uint32_t bytes of credit given back.
  mg_session_credit = 4,
  // Both directions. No payload. Drops the message being received.
  mg_session_abort = 5,
  // Device -> client, right after its hello. Fragmented as data: the
  // credentials JSON.
  mg_session_credentials = 6,
  // Client -> device. Fragmented as data, outside credit: the refreshed
  // token pair JSON. Sealed, total_len is 21..MG_SESSION_MAX_CREDENTIALS + 20;
  // any other, or a pair whose stored document wouldn't fit
  // MG_SESSION_MAX_CREDENTIALS, gets error [session, invalid_value].
  mg_session_credentials_update = 7,
  // Device -> client. Payload: uint8_t saved (1 saved, 0 not saved); in a
  // sealed session, sealed and fragmented as data (one write) with a 9-byte
  // plaintext: saved, then MG_SESSION_SAVED_HASH_LEN bytes of SHA-256 over
  // the update's refresh_token (zeros when not saved).
  mg_session_credentials_saved = 8,
  // 9 and 10 reserved.
  // Client -> device, never sealed. No payload; 2 bytes on the wire
  // (command, sub-command). "Open your Muse session now": sent when the VM
  // asks the client to wake this device (gadget.wake) because an agent's
  // invoke is waiting for it. Honored only on a connection whose token
  // proof matched and whose hello is done; otherwise ignored. The device
  // treats it as a task of its own: it sends open_request [1] under the
  // usual series rules and, once open, link.register; the VM then sends the
  // invoke. With nothing else to do the session closes as usual after the
  // task. Only for a device that declares mg_session_cap_wake; a device
  // without it doesn't set is_wakeup_supported either. Vector:
  // test-vectors/mg-session-v1.json, "wake at MTU 247".
  mg_session_wake = 11,
  // Both directions, sealed only. Client -> device: inside a sealed session
  // (after a sealed hello on this connection's proof), on the next
  // client-to-device counter, outside data credit and sent between data
  // messages like credentials_update, fragmented the same way. Plaintext:
  // uint8_t 0 (any other value: error [session, invalid_value]), so total_len
  // is exactly 21 (counter 4 + 1 + tag 16; any other: invalid_value). The device
  // erases its tokens and K as soon as the tag checks and returns to setup
  // (what token_proof clear does), then answers with its own
  // mg_session_clear, sealed on its
  // next device-to-client counter: plaintext [1] once erased, [0] if the
  // erase failed; then the session ends. Only the answer waits for a device
  // message already part way out; the session's derived keys stay in RAM
  // just to seal it: from the erase on, a credentials_update, data or any
  // other sealed client message gets [session, invalid_value] (data its
  // credit back) and changes nothing, so [1] still means erased, and a
  // device message not yet started (the credentials still waiting for room,
  // say) never goes out and its counter is never used: the answer is sealed
  // on that counter, the next one the client takes (a receiver takes only
  // the next counter, see Sealed credentials). Vectors:
  // test-vectors/mg-session-v1.json, "sealed clear while the credentials
  // wait at MTU 247" and "... while a device message is reserved but not
  // started ...".
  // Unsealed gets [session, insecure];
  // a wrong tag or counter gets [session, invalid_value]; neither erases
  // anything. A link lost after the tag checked still erases. A clear whose
  // first fragment arrives while a credentials_update is still arriving, or
  // that the device has nowhere to hold, gets [session, busy]; like any
  // sealed message whose first fragment arrived, that clear has used its
  // counter on both sides, so the client retries (about 200 ms later,
  // bounded by its answer time) sealed on its next counter. Only for a
  // device that declares mg_session_cap_sealed_clear.
  mg_session_clear = 12,
};

MG_ENUM(uint8_t, mg_session_state_t) {
  // No session, and none being opened.
  mg_session_state_closed = 0,
  mg_session_state_connecting = 1,
  mg_session_state_open = 2,
  // Closed: the phone has no network.
  mg_session_state_no_network = 3,
  // Closed: the VM refused the gadget's credential or key.
  mg_session_state_auth_failed = 4,
  // Closed: the phone holds no credential for this gadget, or the VM its key
  // is bound to (vm_id) isn't among the account's VMs.
  mg_session_state_not_enrolled = 5,
  // Closed: the VM sent a message larger than the device's max_message.
  mg_session_state_too_large = 6,
};

MG_ENUM(uint8_t, mg_session_data_flag_t) {
  mg_session_flag_first = 1 << 0,
  mg_session_flag_last = 1 << 1,
  // credentials, credentials_update and data: the bytes are sealed (see
  // Sealed credentials; in a sealed session every message with a payload is
  // sealed).
  mg_session_flag_sealed = 1 << 2,
};

// What a device session can do, listed as
// [supported_features, sub_feature, mg_command_session, caps].
MG_ENUM(uint8_t, mg_session_cap_t) {
  // Seals credentials and credentials_update under the token proof (see
  // Sealed credentials) when the hello asks for it.
  mg_session_cap_sealed = 1 << 0,
  // Holds no Wi-Fi credentials, so the client refreshes the token pair and
  // writes it back with credentials_update.
  mg_session_cap_client_refreshes = 1 << 1,
  // The device session carries credentials only (no VM session): the device
  // never sends open_request and ignores state (or answers it with
  // unsupported). A client
  // that sees it says hello for the credentials and never opens a VM session
  // for this device or sends state. Older credentials-only firmware lacks
  // the bit, so a client also takes a state answered unsupported as "no VM
  // session on this device" for the rest of the connection: it closes the
  // VM connection it opened, never sends state again on that connection,
  // and never counts that error against the token handoff.
  mg_session_cap_credentials_only = 1 << 2,
  // Speaker gadget: after this connection's token proof matched, the
  // device asks for LE Secure Connections bonding (an SMP Security Request)
  // and derives a classic BR/EDR key from it (cross-transport key
  // derivation), then connects to the phone as an A2DP sink. A client that
  // sees it lets the phone's pairing prompt run (Android may call createBond
  // itself) and treats the bonded device like any other (proof, hello and
  // session unchanged). Only devices that list it ever ask for a bond; every
  // other Muse link stays unbonded.
  mg_session_cap_speaker_bond = 1 << 3,
  // The device handles mg_session_wake. A client sends wake only to a
  // device that declares it (and answers the VM's gadget.wake with an error
  // otherwise).
  mg_session_cap_wake = 1 << 4,
  // The device takes mg_session_clear and refuses an unsealed token_proof
  // clear (insecure, nothing erased). A client sends Forget's clear as
  // mg_session_clear to a device that declares it, and token_proof clear
  // only to firmware without it.
  mg_session_cap_sealed_clear = 1 << 5,
};

// hello_flags, the client hello's optional last byte. A device ignores
// bits it doesn't know, so a client may set a newer flag for any device.
MG_ENUM(uint8_t, mg_session_hello_flag_t) {
  // Asks for sealed credentials; a device that seals echoes it in its hello.
  mg_session_hello_flag_sealed = 1 << 0,
  // The client counts a sealed credentials_saved on the device-to-client
  // counter. Only then does the device seal that answer (and echo this
  // flag); without it the device answers with the legacy one-byte unsealed
  // form, which uses no counter, so an older client's counter never falls
  // behind the device's.
  mg_session_hello_flag_sealed_saved = 1 << 1,
  // 1 << 2 reserved.
};

MG_ENUM(uint8_t, mg_stream_audio_command_t) {
  // Client -> device. No payload.
  mg_stream_audio_request_capabilities = 0,
  // Device -> client. Payload: uint8_t count, then count entries, each an
  // mg_stream_audio_capability_t followed by its value.
  mg_stream_audio_report_capabilities = 1,
  // Client -> device. Payload: uint8_t count, then count entries, each an
  // mg_stream_audio_parameter_t followed by its value. Codec is required;
  // omitted parameters take the device's first reported value. Entries carry
  // no length, so a device can't skip one it doesn't know: it stops at the
  // first unknown id and uses what it parsed (older firmware refuses with
  // unsupported instead). So a client puts parameters in increasing id order
  // and sends a newer one only to a device that lists it. A device that
  // holds a token takes it only after this connection's token proof
  // matched (error proof_required before it), or on an automation build on
  // a connection that turned its bench override on. If that proof ends while
  // the link stays up, the device ends the open stream (its own
  // stop_streaming).
  mg_stream_audio_start_streaming = 2,
  // Both directions. Payload: mg_stream_audio_buffer_action_t.
  // Client -> device: end playback. Device -> client: playback has ended.
  mg_stream_audio_stop_streaming = 3,
  // Client -> device. No payload.
  mg_stream_audio_request_buffer_update = 4,
  // Device -> client. Payload: uint32_t available buffer bytes,
  // uint32_t total bytes received since start_streaming, then optionally
  // uint16_t queued ms: how much audio the device holds ahead of its speaker
  // at this moment (buffer, decoder and output path; 0xffff unknown), which a
  // client uses to align video with it (A/V sync).
  mg_stream_audio_buffer_update = 5,
  // Client -> device. No payload. Only to a device that lists
  // mg_stream_audio_capability_flush. Discards everything the device holds
  // for the open stream (buffer, decoder and as much of the output path as
  // it can) at once and keeps the stream open: its counters keep running and
  // the device answers with a buffer_update showing the emptied buffer.
  // Applied in order with the Data writes around it, like change_data_type:
  // Data written before it is discarded, Data after it plays.
  mg_stream_audio_flush = 6,
  // Payload: uint32_t frame, a video frame number extended to 32 bits: the
  // video frame header's 16-bit frame number, its wraps counted over every
  // frame header received since start_streaming.
  // Subtitle frame timing extends it the same way.
  // Client -> device, only to a device that lists
  // mg_stream_audio_capability_video_sync, and only while a video stream runs.
  // In a Live call that is the call's video stream.
  // Applied in order with the Data writes around it,
  // like flush: the first sample of the next Data written after it belongs
  // with picture `frame` (the picture the client would show as that sample
  // is heard). An anchor that arrives while its reply's first audio (the
  // first Data after a gap) is still unplayed names that first sample
  // instead, so a late anchor still starts the reply on its picture. The
  // device starts that sample when it shows a picture whose
  // extended number has reached `frame` (due when (int32_t)(shown - frame)
  // >= 0), playing silence meanwhile for at most 500 ms; if that picture is
  // already shown it plays at once. It only ever delays audio that starts
  // after a gap (a reply's first sample): an anchor that lands while audio
  // plays never pauses it and is only measured (the A/V offset on the
  // video stats overlay). A flush, or the video stream stopping or restarting,
  // drops a pending anchor and the audio plays at once. It never skips
  // audio. While it holds, the device grants no more credit than its live
  // depth, so the rest waits on the client. The Muse apps send one with
  // each reply's first audio write when the device lists the capability
  // and video runs; without one the device keeps its own A/V delay (its
  // measured video latency plus a per-board allowance for the phone's
  // encoder and BLE video path).
  mg_stream_audio_video_anchor = 8,
};

MG_ENUM(uint8_t, mg_stream_audio_capability_t) {
  // value: uint8_t count, then count mg_data_type_t (supported codecs)
  mg_stream_audio_capability_codecs = 1,
  // value: uint32_t playback buffer size in bytes
  mg_stream_audio_capability_buffer_size = 2,
  // value: uint8_t count, then count uint32_t sample rates in Hz
  mg_stream_audio_capability_sample_rates = 3,
  // value: uint16_t maximum bitrate in kb/s
  mg_stream_audio_capability_max_bitrate = 4,
  // value: uint8_t maximum channel count
  mg_stream_audio_capability_max_channels = 5,
  // value: uint8_t 1: the device takes mg_stream_audio_flush.
  mg_stream_audio_capability_flush = 6,
  // value: uint16_t the smallest live depth in ms the device can hold (its
  // start threshold, decoder and output path). The device takes
  // mg_stream_audio_parameter_live_depth_ms and holds max(asked, this). A
  // device that lists it reports queued ms in every buffer_update (0xffff
  // only before playback starts).
  mg_stream_audio_capability_live_depth = 7,
  // value: uint8_t 1: the device takes mg_stream_audio_video_anchor.
  mg_stream_audio_capability_video_sync = 8,
};

MG_ENUM(uint8_t, mg_stream_audio_parameter_t) {
  // value: mg_data_type_t codec
  mg_stream_audio_parameter_codec = 1,
  // value: uint32_t sample rate in Hz
  mg_stream_audio_parameter_sample_rate = 2,
  // value: uint16_t bitrate in kb/s
  mg_stream_audio_parameter_bitrate = 3,
  // value: uint8_t channel count
  mg_stream_audio_parameter_channels = 4,
  // value: uint16_t encoded frame size in bytes (0 = variable)
  mg_stream_audio_parameter_frame_size = 5,
  // value: uint16_t live depth in ms. Sent only to a device that lists
  // mg_stream_audio_capability_live_depth (a device that doesn't may refuse
  // an unknown parameter). The client paces this stream live (Live reply
  // audio: a start burst of at most 60 ms, writes of at most 40 ms, reply
  // audio ahead of queued video) and asks the device to keep the audio it
  // holds (queued ms: buffer, decoder, output path) at about this many ms,
  // or at the device's listed floor if that is higher. Audio in flight on
  // the link doesn't count against it: the device's credit covers depth -
  // queued plus what it sees in flight, grows that in-flight allowance after
  // an underrun (about 20 ms each, to at most 40 ms more) and shrinks it
  // slowly while clean. So a client fed live reads queued ms near the asked
  // depth (not depth minus what is in flight). Absent: the device grants its
  // whole buffer, as for a client that bursts or paces at 1x.
  mg_stream_audio_parameter_live_depth_ms = 6,
};

MG_ENUM(uint8_t, mg_stream_audio_buffer_action_t) {
  // Play what is buffered, then stop.
  mg_stream_audio_buffer_keep = 0,
  // Discard what is buffered and stop now.
  mg_stream_audio_buffer_drop = 1,
};

MG_ENUM(uint8_t, mg_live_call_request_t) {
  // The user ended the call on the device.
  mg_live_call_request_end = 0,
  // The user started the call on the device.
  mg_live_call_request_start = 1,
};

MG_ENUM(uint8_t, mg_live_call_state_t) {
  // No call.
  mg_live_call_state_idle = 0,
  // The client is connecting the call.
  mg_live_call_state_connecting = 1,
  // The call is live.
  mg_live_call_state_live = 2,
  // The call ended normally.
  mg_live_call_state_ended = 3,
  // The call failed to start or dropped.
  mg_live_call_state_failed = 4,
};

// Why a call failed (after mg_live_call_state_failed, to a device that lists
// mg_live_call_feature_failure_reason).
MG_ENUM(uint8_t, mg_live_call_failure_reason_t) {
  // No reason given: the device shows its usual failure text.
  mg_live_call_failure_unspecified = 0,
  // The phone needs the user before calls can start (the Muse app's voice
  // intro, the microphone permission). The device shows "Open the Muse app
  // to finish setting up calls"; the client prompts on the phone.
  mg_live_call_failure_setup_required = 1,
};

// mg_subtitle_capabilities' version byte.
#define MG_SUBTITLE_CAPS_VERSION 1

MG_ENUM(uint8_t, mg_subtitle_command_t) {
  // Client -> device. No payload.
  mg_subtitle_request_capabilities = 0,
  // Device -> client. Payload: uint8_t version (MG_SUBTITLE_CAPS_VERSION),
  // uint8_t flags
  // (mg_subtitle_capability_flag_t), uint8_t text lines the device renders,
  // uint8_t characters a line holds at its font, uint16_t max_words_bytes
  // (the largest words payload it accepts, counted from the reply id; never
  // more than ATT_MTU - 5, so a whole words write fits one ATT payload; sent
  // again unsolicited when the MTU changes).
  mg_subtitle_capabilities = 1,
  // Client -> device. Payload: uint8_t 1 while this call shows subtitles,
  // 0 to stop (drops everything), uint8_t mg_subtitle_timing_t. Any other
  // first byte is invalid (the device answers invalid_value).
  mg_subtitle_state = 2,
  // Client -> device. Payload: uint16_t reply id, uint8_t flags
  // (mg_subtitle_words_flag_t), uint16_t the first word's UTF-8 byte offset
  // in the reply, uint8_t count (1..255), then count words, each uint32_t
  // when to show it (see Live call subtitles; MG_SUBTITLE_SHOW_NOW for at
  // once), uint8_t length (1..255), and that many UTF-8 bytes.
  mg_subtitle_words = 3,
  // Client -> device. Payload: uint16_t reply id, uint32_t when the last
  // word's cue ends (same domain as the words).
  mg_subtitle_reply_end = 4,
  // Client -> device. Payload: uint16_t reply id (0 = every reply),
  // uint8_t mg_subtitle_clear_reason_t.
  mg_subtitle_clear = 5,
  // Client -> device, milliseconds timing only, about once a second.
  // Payload: uint16_t reply id, uint32_t milliseconds played of the reply.
  mg_subtitle_clock = 6,
};

MG_ENUM(uint8_t, mg_subtitle_capability_flag_t) {
  // The device has no speaker, so a client with subtitles always uses them.
  mg_subtitle_capability_no_speaker = 0x01,
  // The device takes reply audio and subtitles together.
  mg_subtitle_capability_wants_audio_too = 0x02,
  // The device takes cues timed by video frame number (it decodes video).
  mg_subtitle_capability_frame_timing = 0x04,
  // The device takes cues timed in milliseconds.
  mg_subtitle_capability_ms_timing = 0x08,
  // The device reveals each word as its cue comes due, not a line at a time.
  mg_subtitle_capability_word_reveal = 0x10,
  // The device renders Latin-1 only; a client sends no other text.
  mg_subtitle_capability_latin1_only = 0x20,
};

MG_ENUM(uint8_t, mg_subtitle_timing_t) {
  // show_at is a video frame number, extended to 32 bits.
  mg_subtitle_timing_frame = 0,
  // show_at is milliseconds from the reply's start.
  mg_subtitle_timing_ms = 1,
};

MG_ENUM(uint8_t, mg_subtitle_words_flag_t) {
  // The reply's text up to the end of this message will not change.
  mg_subtitle_words_final = 0x01,
  // The words replace everything from their offset on.
  mg_subtitle_words_replace = 0x02,
};

MG_ENUM(uint8_t, mg_subtitle_clear_reason_t) {
  mg_subtitle_clear_call_ended = 0,
  // The user spoke over the Muse, or pressed to talk.
  mg_subtitle_clear_interrupted = 1,
  mg_subtitle_clear_mode_off = 2,
  mg_subtitle_clear_new_reply = 3,
};

// A cue with this moment is shown as soon as it arrives.
#define MG_SUBTITLE_SHOW_NOW 0xFFFFFFFFu
// Bytes before the first word of a words payload, and before each word's text.
#define MG_SUBTITLE_WORDS_HEADER 6
#define MG_SUBTITLE_WORD_HEADER 5
#define MG_SUBTITLE_MAX_WORD_BYTES 255

MG_ENUM(uint8_t, mg_stream_video_command_t) {
  // Client -> device. No payload.
  mg_stream_video_request_capabilities = 0,
  // Device -> client. Payload: uint8_t count, then count entries, each an
  // mg_stream_video_capability_t followed by its value.
  mg_stream_video_report_capabilities = 1,
  // Client -> device. Payload: uint8_t count, then count entries, each an
  // mg_stream_video_parameter_t followed by its value. Codec, width and
  // height are required. A device that holds a token takes it only after
  // this connection's token proof matched (error proof_required before it),
  // or on an automation build on a connection that turned its bench
  // override on. If that proof ends while the link stays up, the device ends
  // the open stream (its own stop_streaming).
  mg_stream_video_start_streaming = 2,
  // Both directions. Payload: mg_stream_audio_buffer_action_t (drop). The
  // device never answers the client's; it sends one when it ends a stream itself.
  mg_stream_video_stop_streaming = 3,
  // Client -> device. No payload.
  mg_stream_video_request_buffer_update = 4,
  // Device -> client. Payload: uint32_t available buffer bytes, uint32_t total
  // bytes received since start_streaming, then optionally uint16_t frames
  // shown, uint16_t frames dropped, uint8_t average decode ms, then
  // optionally uint16_t the frame number (from its frame header) of the last
  // frame the display finished drawing and uint16_t how many ms ago
  // (0xffff: none yet), from which the client measures capture-to-shown
  // latency.
  mg_stream_video_buffer_update = 5,
  // Device -> client. No payload: make the next frame a keyframe.
  mg_stream_video_request_keyframe = 6,
  // Device -> client. Payload: uint8_t 1 = the user wants a stream, 0 = no
  // longer.
  mg_stream_video_request_streaming = 7,
  // Device -> client, one per image of a still-image stream, once it is shown
  // or refused. Payload (37 bytes; 39 with the command bytes): uint16_t frame number, uint8_t
  // mg_image_status_t, uint8_t mg_data_type_t codec (0: not recognized),
  // uint32_t file bytes, uint16_t width, uint16_t height, then microseconds
  // (uint32_t each, 0xffffffff: unknown): receive (the frame's first byte in
  // to its last), queue (last byte in to decode start), decode (decode start
  // to the last row decoded), draw (last row decoded to the last row on the
  // panel), total (first byte in to shown); uint32_t decoder working memory
  // bytes, uint8_t mg_image_report_flag_t bits; then (41 bytes with the
  // command bytes; older devices send 39) uint16_t hold_ms: how long the
  // device keeps showing this image (and a demo sequence's card after it)
  // before it is ready for the next one. A client pacing a slideshow waits
  // that long after the report before writing the next image.
  mg_stream_video_image_report = 8,
  // 9, 10 and 11 reserved.
};

// image_report status.
MG_ENUM(uint8_t, mg_image_status_t) {
  mg_image_status_shown = 0,
  // A decode error (corrupt or truncated data): what decoded may be shown.
  mg_image_status_damaged = 1,
  // Not a codec, tool, size or format the device decodes: nothing shown.
  mg_image_status_unsupported = 2,
  // No memory for the decoder or the picture: nothing shown.
  mg_image_status_no_memory = 3,
};

// image_report flags.
MG_ENUM(uint8_t, mg_image_report_flag_t) {
  mg_image_report_flag_internal_ram = 0x01,   // the decoder's memory was in internal RAM (not PSRAM)
  mg_image_report_flag_progressive = 0x02,    // rows went to the panel while the rest decoded
};

// Entries carry no length up to 16, so a client parses in order and stops
// at an id it doesn't know. From id 17 on,
// every value starts with uint8_t length (the bytes that follow), so a
// client skips an unknown id >= 17 and goes on.
MG_ENUM(uint8_t, mg_stream_video_capability_t) {
  // value: uint8_t count, then count mg_data_type_t (supported codecs)
  mg_stream_video_capability_codecs = 1,
  // value: uint16_t width, uint16_t height, uint8_t round (1 = circular):
  // the panel as the user sees it (upright)
  mg_stream_video_capability_display = 2,
  // value: uint16_t width, uint16_t height: the largest picture it decodes
  mg_stream_video_capability_max_size = 3,
  // value: uint8_t frames per second at the suggested size
  mg_stream_video_capability_max_fps = 4,
  // value: uint32_t receive buffer size in bytes
  mg_stream_video_capability_buffer_size = 5,
  // value: uint16_t width, uint16_t height, uint8_t fps, uint16_t kb/s: the
  // stream the device suggests for its display (it may upscale it)
  mg_stream_video_capability_suggested = 6,
  // value: uint16_t LE L2CAP PSM: the device also takes the stream on an
  // L2CAP connection-oriented channel on this PSM (no security required),
  // byte for byte as on Data; credits still come as buffer_update. The
  // channel carries video only (it isn't ordered with Control, so it can't
  // be interleaved with audio playback by change_data_type). On
  // Android a channel moves several times what one GATT write at a time does.
  mg_stream_video_capability_l2cap_psm = 7,
  // value: uint16_t LE milliseconds: the longest playout delay the device
  // takes (mg_stream_video_parameter_playout_delay). Absent: it shows each
  // frame as soon as it is decoded, and refuses the parameter.
  mg_stream_video_capability_playout_delay = 8,
  // value: uint8_t the most bands (mg_stream_video_parameter_tiles) the
  // device decodes in parallel, one per core. Absent: one.
  mg_stream_video_capability_tiles = 9,
  // value: uint8_t percent: with two bands, the share of the picture's
  // height the device wants in the first (top) band, which the client rounds
  // to a multiple of 16 lines; the bands are then of unequal height (each
  // band's SPS gives its own). For a device whose cores aren't equally free
  // (one also drives the display). Absent: equal bands.
  mg_stream_video_capability_tile_split = 10,
  // value: uint16_t width, uint16_t height, uint8_t max fps, uint32_t max
  // frame bytes, uint8_t most bands (mg_stream_video_parameter_tiles) at
  // this size: the device takes UI streams (mg_video_content_ui, see UI
  // streaming) of up to this picture, normally its panel's own size, shown
  // one to one. Absent: no UI streams.
  mg_stream_video_capability_ui = 11,
  // value: uint8_t still patch formats, a bit (1 << mg_video_still_format_t)
  // each: the device takes lossless still patches in a UI stream (frames
  // with mg_video_frame_still, see UI streaming). Absent: none. Comes after
  // ui (a client that doesn't know one stops parsing there).
  mg_stream_video_capability_ui_still = 12,
  // value: uint8_t mg_stream_video_capability_flag_t bits. The last entry
  // but still (a client that doesn't know one stops parsing there, so
  // still, which clients before it don't know, comes after it). Absent:
  // none of them.
  mg_stream_video_capability_flags = 13,
  // value: uint16_t width, uint16_t height: the largest still image;
  // uint32_t max image file bytes; uint16_t reserved (0); uint16_t reserved
  // (0); uint8_t count, then count mg_data_type_t image codecs. The device
  // takes still-image streams
  // (mg_video_content_still, see Still images). Absent: none. Comes after
  // flags.
  mg_stream_video_capability_still = 14,
  // value: uint16_t width, uint16_t height, uint8_t fps, uint16_t kb/s,
  // uint8_t turn: the device also takes a stream whose pictures are turned
  // a quarter turn from the upright view (turn 1 = 90 degrees clockwise,
  // 3 = counter-clockwise; other values: not offered), coded as width x
  // height (the turned size, e.g. 240 x 296 for a 296 x 240 landscape panel
  // scanned in portrait). It shows them upright 1:1 without holding a
  // second whole picture, so this stream may be larger than suggested and
  // max_size, which still govern upright streams. A client that codes
  // turned pictures sends
  // mg_stream_video_parameter_rotation in start_streaming, with width and
  // height the coded size; everything else (frame numbers, keyframes,
  // credit, the A/V anchor) is unchanged.
  // Subtitle cues are unchanged too.
  // A size change
  // (stop + start) may switch between turned and upright. Comes after still
  // (a client that doesn't know one stops parsing there).
  mg_stream_video_capability_rotated = 16,
  // 17 reserved.
};

// mg_stream_video_capability_flags.
MG_ENUM(uint8_t, mg_stream_video_capability_flag_t) {
  // The device accepts a stream's first keyframe right behind
  // start_streaming, before its answer (see Video streaming, step 2).
  // Without it the client waits for the first buffer_update: devices that
  // don't set it may lose Data that arrives before they've started the
  // stream.
  mg_stream_video_capability_flag_early_keyframe = 0x01,
  // The device takes framed Data (mg_data_type_framed) while video and
  // audio playback share Data.
  mg_stream_video_capability_flag_framed = 0x02,
};

// A still patch's pixels (mg_video_frame_still).
MG_ENUM(uint8_t, mg_video_still_format_t) {
  // RGB565 big-endian (the panel's own order), row by row, as one zlib
  // stream (RFC 1950) of exactly w * h * 2 bytes.
  mg_video_still_rgb565_zlib = 1,
};

// What a stream carries (mg_stream_video_parameter_content).
MG_ENUM(uint8_t, mg_video_content_t) {
  // Live or pre-recorded video, at up to max_size (the default).
  mg_video_content_video = 0,
  // A mostly static picture at up to the ui capability's size, frames only
  // on change (see UI streaming).
  mg_video_content_ui = 1,
  // Still images, each frame one whole image file (see Still images).
  mg_video_content_still = 2,
  // 3 reserved.
};

MG_ENUM(uint8_t, mg_stream_video_parameter_t) {
  // value: mg_data_type_t codec
  mg_stream_video_parameter_codec = 1,
  // value: uint16_t picture width in pixels (after cropping)
  mg_stream_video_parameter_width = 2,
  // value: uint16_t picture height in pixels (after cropping)
  mg_stream_video_parameter_height = 3,
  // value: uint8_t frames per second (nominal)
  mg_stream_video_parameter_fps = 4,
  // value: uint16_t bitrate in kb/s
  mg_stream_video_parameter_bitrate = 5,
  // value: uint16_t milliseconds (at most the device's
  // mg_stream_video_capability_playout_delay). Nonzero: the device shows the
  // frames at a steady rate (the fps parameter, by frame number) this long
  // after the first one arrives, so frames that arrive unevenly (a keyframe
  // that takes several frame times on the link) still show evenly. A frame
  // that arrives later than its time shows at once and moves the schedule
  // later. 0 or absent: each frame as soon as it is decoded, for live
  // sources where latency matters more. For pre-recorded clips.
  mg_stream_video_parameter_playout_delay = 6,
  // value: uint8_t bands (at most mg_stream_video_capability_tiles; absent or
  // 1: one). The picture is that many horizontal bands, each a multiple of
  // 16 lines, top first: of equal height, or with two bands and
  // mg_stream_video_capability_tile_split offered, the first that share of
  // the height. Each band is its own H.264 stream of the picture's width
  // and the band's height, with its own SPS and PPS, coded independently, so the
  // device decodes them at once on separate cores. Each frame carries one
  // access unit per band: every band but the last as a uint32_t LE length
  // and its access unit, then the last band's access unit to the end of the
  // frame. A keyframe frame has every band a keyframe (all bands start at
  // the same frame, and request_keyframe asks for all of them).
  mg_stream_video_parameter_tiles = 7,
  // value: mg_video_content_t (absent: video). ui only to a device that
  // lists mg_stream_video_capability_ui, still only to one that lists
  // mg_stream_video_capability_still.
  mg_stream_video_parameter_content = 8,
  // value: uint8_t seconds (still images only; 0 or absent: off): the device
  // shows a line with each image's codec, size and decode time over it for
  // that long.
  mg_stream_video_parameter_overlay = 9,
  // value: uint8_t (still images only; 0 or absent: off). 1 = the demo
  // sequence, for showing the codecs side by side: per image a black
  // downloading screen (codec, bytes in of the file's, a bar) while it
  // transfers, nothing of the image until it has decoded, then the whole
  // image at once with a large overlay (codec, size, link, decode and display
  // times) for 3 s, then black. The report's hold_ms says when the device is
  // ready for the next image.
  mg_stream_video_parameter_still_demo = 10,
  // value: uint8_t turn (0, or the turn mg_stream_video_capability_rotated
  // lists): the stream's pictures are coded turned; width and height are
  // the coded size. A device answers invalid_value to a turn it doesn't
  // list.
  mg_stream_video_parameter_rotation = 11,
  // 12 and 13 reserved.
};

// UI input: touch events and the gadget's HID (see UI input).
MG_ENUM(uint8_t, mg_ui_input_command_t) {
  // Client -> device. Payload: uint8_t 1 = notify touch events, 0 = stop.
  // Per connection: off at every new connection.
  mg_ui_input_touch_enable = 0,
  // Device -> client. Payload: uint8_t mg_touch_action_t, uint16_t x,
  // uint16_t y (the panel's pixels from its top-left corner, 0..width-1 and
  // 0..height-1), uint16_t time in ms (the device's clock, wrapping),
  // uint8_t pointer id (0 on single-touch panels).
  mg_ui_input_touch = 1,
  // Client -> device. Payload: uint8_t mg_hid_mode_t, then for every mode but
  // off: uint16_t x, uint16_t y, uint16_t w, uint16_t h: the streamed square
  // on the phone's screen, in 1/65535 of the screen's width (x, w) and height
  // (y, h) from its top-left corner (in the orientation the host reports
  // touches in); uint8_t flags (mg_hid_map_flags_t); then for mouse_rel only:
  // uint16_t pointer counts across the screen's width, uint16_t counts down
  // its height (the calibration: how far the pointer moves, at the host's
  // tracking speed, in steps of at most 8 counts). Answer: hid_status.
  mg_ui_input_hid_map = 2,
  // Device -> client. Payload: uint8_t mg_hid_mode_t in use, uint8_t
  // mg_hid_status_flags_t. Answers hid_map and is notified on any change.
  mg_ui_input_hid_status = 3,
  // Both directions. Payload: uint8_t mg_ui_control_t, uint8_t value. See
  // UI input, controls.
  mg_ui_input_control = 4,
  // Client -> device. Payload: uint16_t usage on the HID Consumer page
  // (0x0224 AC Back, 0x0223 AC Home): the gadget's HID presses and releases
  // that key, for a phone whose screen is driven through the HID. Answered
  // with an error when no HID host is connected. On a release build of a
  // device that holds a token, error proof_required until this connection's
  // token proof matched (see Token proof).
  mg_ui_input_hid_key = 5,
};

// A touch event's pointer id byte: the low 7 bits are the pointer, and this
// bit marks a gesture that pans the screen mirror (edge ring or pan mode,
// decided once it moved; see UI input, controls): the client pans, and
// never passes it to the phone.
#define MG_TOUCH_ID_PAN 0x80

MG_ENUM(uint8_t, mg_ui_control_t) {
  mg_ui_control_fisheye = 1,   // 1: the square is warped onto the round panel
  mg_ui_control_pan = 2,       // 1: drags move the phone-screen window (mirror only)
  mg_ui_control_quality = 3,   // 0: fast, 1: sharp (finer still pictures)
  mg_ui_control_stop = 4,      // device -> client, 1: end the UI stream
  mg_ui_control_menu = 5,      // 1: the stream's menu is open (drawn by the client)
  mg_ui_control_edge_pan = 6,  // client -> device, 1: edge-ring drags pan (mirror)
  mg_ui_control_overscroll = 7,  // how far the mirror window pans past the screen: 0 none, 1 top and bottom, 2 all edges
};

// Typed list for [supported_features, sub_feature, mg_command_ui_input, ...].
MG_ENUM(uint8_t, mg_ui_input_feature_t) {
  mg_ui_input_feature_touch = 1,          // touch events (touch_enable)
  mg_ui_input_feature_hid_digitizer = 2,  // HOGP absolute touch (Android)
  mg_ui_input_feature_hid_mouse_abs = 3,  // HOGP absolute pointer
  mg_ui_input_feature_hid_mouse_rel = 4,  // HOGP relative pointer (iOS)
  mg_ui_input_feature_controls = 5,       // control events and a menu button
};

MG_ENUM(uint8_t, mg_touch_action_t) {
  mg_touch_down = 0,
  mg_touch_move = 1,
  mg_touch_up = 2,
};

MG_ENUM(uint8_t, mg_hid_mode_t) {
  mg_hid_mode_off = 0,
  mg_hid_mode_digitizer = 1,   // a one-finger touchscreen: tip switch + X/Y
  mg_hid_mode_mouse_abs = 2,   // a pointer with absolute X/Y and button 1
  mg_hid_mode_mouse_rel = 3,   // a relative pointer with button 1, homed top-left
};

MG_ENUM(uint8_t, mg_hid_map_flags_t) {
  // The streamed square is warped onto the round panel (square-to-disc):
  // touches go through the inverse warp first.
  mg_hid_map_fisheye = 1 << 0,
  // The square may reach past the screen (a mirror window panned past its
  // edges): x and y are (fraction + 0.5) / 2 of 65535, so -0.5 to 1.5 of
  // the screen. A touch that goes down outside the screen is dropped (the
  // window shows black there); one that moves out stays on the edge.
  mg_hid_map_offset = 1 << 1,
};

MG_ENUM(uint8_t, mg_hid_status_flags_t) {
  mg_hid_status_bonded = 1 << 0,     // the link is encrypted with a bond
  mg_hid_status_host = 1 << 1,       // a HID host enabled input report notifications
};

// Precedes each access unit on Data during a video stream.
#define MG_VIDEO_FRAME_MAGIC 0x56   // 'V'
#define MG_VIDEO_FRAME_HEADER_LEN 8
MG_ENUM(uint8_t, mg_video_frame_flags_t) {
  mg_video_frame_keyframe = 1 << 0,
  // A UI stream's still patch, not the codec's: uint16_t LE x, y, w, h (even,
  // inside the picture), uint8_t mg_video_still_format_t, then the pixels.
  // The device draws them exactly over that part of the picture, where they
  // stay until the decoded picture changes there. Only to a device that
  // lists mg_stream_video_capability_ui_still with that format.
  mg_video_frame_still = 1 << 1,
};
// Byte layout (little-endian), not a C struct to keep it packing-free:
//   uint8_t  magic (MG_VIDEO_FRAME_MAGIC)
//   uint8_t  flags (mg_video_frame_flags_t)
//   uint16_t frame number, from 0 at start_streaming, wrapping
//   uint32_t access unit length in bytes (not counting this header)

MG_ENUM(uint8_t, mg_audio_queue_command_t) {
  // Request: no payload.
  // Response: [mg_command_audio_queue, status, uint8_t enabled,
  //            uint32_t used_bytes, uint32_t capacity_bytes,
  //            uint16_t clip_count]
  mg_audio_queue_command_status = 1,
  // Request: no payload. Erases every clip, then notifies status.
  mg_audio_queue_command_clear = 3,
  // Metadata for one complete clip by zero-based queue index.
  // Request: uint16_t clip_index.
  // Response: [mg_command_audio_queue, clip_info, uint16_t clip_index,
  //            mg_data_type_t codec, uint32_t start_timestamp_ms,
  //            uint32_t compressed_bytes, then the clip's format as
  //            change_data_type's params 2-5: uint16_t sample rate in Hz,
  //            uint8_t channels, uint16_t frame bytes (0 = variable),
  //            uint16_t frame duration in us, then optionally uint64_t
  //            start_unix_ms]
  // The client decodes each clip by its own format. A clip_info without the
  // format (older firmware) means 16000, mono, and the codec's defaults.
  // start_unix_ms is the wall-clock time the clip started, in ms since the
  // Unix epoch, from the device's clock (see Clock); 0 when the clock wasn't
  // set. Absent: unknown. It comes only after the format fields.
  // A client that doesn't decode clips by this format gets clips it can play
  // as an older client would: the device keeps them SBC at 16000. It picks
  // another codec, rate or bitrate (e.g. LC3 at 16 kb/s, 20 B per 10 ms,
  // where storage is tight) only for a client that declares it decodes each
  // clip by its own format; this revision defines no such declaration.
  // start_timestamp_ms is device uptime at the start of the clip. It resets
  // on reboot, so use it for ordering within one boot only; clips are
  // queued oldest first.
  mg_audio_queue_command_clip_info = 4,
  // Request: uint16_t clip_index.
  // The device notifies [change_data_type, mg_data_type_audio_queue], then
  // streams the clip's compressed_bytes on Data, chunked to the MTU. The
  // client detects completion by byte count. Codec frames are stored whole
  // and back to back, in the format described under Audio formats.
  mg_audio_queue_command_read_clip = 5,
};

#endif
