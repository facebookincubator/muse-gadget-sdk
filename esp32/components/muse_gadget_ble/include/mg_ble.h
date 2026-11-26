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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mg_battery.h"
#include "mg_link.h"
#include "mg_press_src.h"
#include "mg_session.h"
#include "mg_subs.h"
#include "mg_token_proof.h"
#include "mg_video.h"

struct muse_update_req;   /* muse_local_press.h: an update change, as the request front read it */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * musegadgets BLE server (CONFIG_MUSE_GADGET_BLE_AUDIO).
 *
 * It doesn't own the BLE stack: Home Link's server (main/ble_server.c) does,
 * and this registers with it as a companion, the way Muse's phone setup
 * service does. Its services join the shared GATT database, it sees every GAP
 * event, and Link's server advertises its UUID for it (taking turns with
 * Link's own setup advertising while both want to be found). One central at
 * a time: while an mg client is connected nothing else can connect.
 *
 * Services: musegadgets (Control, Data), Battery, Device Information, Nordic
 * UART (log mirror, CONFIG_MUSE_GADGET_BLE_NUS_LOG_TAGS).
 */

/* What the board supplies. Any callback may be NULL. */
typedef struct {
    const char *(*model)(void);          /* DIS model number: the board's name */
    bool safe_mode;                      /* SAFE mode: DIS firmware revision ends ".safe" / "+safe" */
    /* In SAFE: the user entered it with the key (muse_boot_why_user), so a Bluetooth update needs no press there
     * (mg_smp_gate.h); false: a SAFE the guard entered itself, which can't ask for the press (rc 11). */
    bool safe_updates_open;
    /* An unproven link's update change asks for ONE confirm press on the gadget (muse_local_press_ask's
     * muse_update_answer_t: 0 confirmed or armed, 1 asking (rc 10), 2 refused (rc 11)), the change as mg_ble.c's
     * request front read it (muse_local_press.h, mg_smp_front.h). NULL: rc 11. */
    uint8_t (*local_press_ask)(const struct muse_update_req *req);
    /* Declare a recovery mode in request_status and serve only its
     * command set (mg_proto_config_t). 0: normal. */
    uint8_t recovery_mode, recovery_reason, recovery_flags;
    void (*bench_silence)(void);         /* the console's !SILENCE (the stay-reachable check), may be NULL */
    void (*bench_safe_identity)(void);   /* the console's !SAFEID (the next boot only), may be NULL */
    void (*bench_setup_adv)(uint32_t secs);   /* the console's !SETUP ADV (Link setup advertised), may be NULL */
    /* >ble.pause=SECS[,ADDR] / !IN ble.pause=...: the connection dropped,
     * then for secs no advertising (only_addr NULL) or advertising
     * connectable only from only_addr (6 bytes, most significant first),
     * then as before (mg_ble_bench_pause). May be NULL. */
    void (*bench_pause)(uint32_t secs, const uint8_t *only_addr);
    /* Advertise fast for 30 s (a talk press with no app connected;
     * ble_server_adv_fast). May be NULL. */
    void (*adv_fast)(const char *why);
    /* The console's !IN <word>: a bench press of the device's own
     * screen or buttons, by the platform's allowlist. Called on the NimBLE
     * host: queue it, never run it there. 1 queued, 0 not a bench word, -1
     * busy. May be NULL. */
    int (*bench_input)(char *line);
    /* The token proof's bench switch is on for the connected central (either console's
     * bench.out=1): the platform's visible "Bench" mark on while true (the
     * home's top row on a screen board, the status light's cue on one with
     * none). Called on each change, from the task that made it (never under
     * mg.ble's lock): set a flag, no waiting. May be NULL. */
    void (*bench_mark)(bool on);
    /*
     * The token proof (mg_command_token_proof). proof_key copies the proof
     * key K that Link setup stored; false if there's none (not set up).
     * proof_clear erases the tokens and K and returns the board to Link
     * setup, without blocking (it may restart the board). Once the erase
     * has ended (before any restart), the platform calls
     * mg_ble_proof_clear_done() with its result: the sealed clear's answer.
     * Both NULL: the proof isn't offered.
     */
    bool (*proof_key)(uint8_t k[MG_TOKEN_PROOF_KEY_LEN]);
    /* Link setup's marker says the gadget is set up. With no proof key that's no owner at all: set up counts
     * either way (fail closed: owner-only commands and Bluetooth updates refused until a new setup). NULL:
     * set up is having the key. */
    bool (*set_up)(void);
    void (*proof_clear)(void);
    int (*battery_pct)(void);            /* 0-100, or -1 without a battery */
    /* set_mic_gain's level, 1-100, for this connection's captures only (never
     * stored, the user's setting untouched); 0 when the connection ends: back
     * to the device's own gain. Called with the protocol locked, from the
     * NimBLE host or the worker: don't block. */
    void (*mic_gain)(uint8_t gain);
    void (*attention)(void);             /* wake the screen: a capture request or playback */
    void (*refresh_advertising)(void);   /* the host server re-reads mg_ble_wants_advertising() */
    /*
     * Plays 16 kHz mono PCM, blocking at the speaker's pace (the board's
     * volume and mute settings apply). NULL: no speaker, so stream_audio
     * isn't offered.
     */
    bool (*speaker_write)(const int16_t *pcm, size_t frames);
    /*
     * The audio the output path holds after speaker_write returns (the I2S
     * DMA's buffers), in microseconds, for buffer_update's queued ms (A/V
     * sync). NULL: unknown (queued ms 0xffff).
     */
    uint32_t (*speaker_depth_us)(void);
    /*
     * The barge-in echo rule (a talk press holds no reply echo): at a talk press's
     * touch-down during a reply, silences what speaker_write left in the
     * output path (the I2S DMA's ~30 ms) at once, with no click and the mic's
     * clock running, and copies the samples it held unplayed into keep,
     * oldest first (at most max); returns how many. The playback task writes
     * them first when a tap resumes the reply (no sample lost), and drops
     * them when a hold claims the turn. NULL, or 0: nothing cleared (the DMA
     * plays out, as before).
     */
    size_t (*speaker_clear)(int16_t *keep, size_t max);
    /*
     * The call video's picture on the panel now (muse_video_shown32): its
     * frame number extended to 32 bits as subtitles count it; false while
     * none is showing. Set: the device lists
     * mg_stream_audio_capability_video_sync and holds a reply's first sample
     * to its mg_stream_audio_video_anchor. NULL: no video, not listed.
     */
    bool (*video_shown32)(uint32_t *frame);
    /*
     * The speaker level (mg_setting_parameter_speaker_volume): 0-100, 0 when
     * the speaker is off. set turns it off at 0, else on at that volume, and
     * persists it. Both NULL: the setting is unsupported.
     */
    int (*speaker_volume)(void);
    void (*set_speaker_volume)(uint8_t volume);
    /*
     * A quiet speaker: the most makeup gain stream_audio gets, in tenths of
     * a dB, at speaker_volume 100 (none up to 50, rising to it), with a peak
     * limiter after it (mg_boost_t). NULL or 0: none.
     */
    int (*speaker_boost_db10)(void);
    /*
     * A board's speech chain for the speaker (the ESP-Mosaico's high-pass,
     * compressor and look-ahead limiter, muse_spk_dsp.h): run runs 16 kHz
     * mono through it in place (after the boost), reset empties it (its
     * look-ahead's samples dropped). The player decides when (mg_chain_t,
     * mg_play.h): every reply chunk; silence while the chain still holds
     * audio, so a reply's end plays out; never while a barge-in pause holds
     * the reply (its state waits with it, so a tap's resume goes on from the
     * same sample: the rewind speaker_clear handed back is post-chain and is
     * written as it is); emptied when the speaker already went quiet on a
     * reply (a hold's claim, a flush in the pause) and at a stream's start.
     * Both NULL: none.
     */
    void (*speaker_chain)(int16_t *pcm, size_t n);
    void (*speaker_chain_reset)(void);
    /*
     * A vibration motor: plays the push-to-talk activation buzz, at the hold
     * threshold (the StopWatch's). NULL (no motor):
     * haptics_enabled and ptt_buzz_* are unsupported and there's no buzz.
     */
    void (*vibrate)(uint16_t freq_hz, uint16_t ms, uint8_t volume_pct);
    /*
     * Haptics (the haptics core, haptics/): the actuator's setting 15
     * (kind, flags, min_on_ms, strength steps) and its drive: on (with the
     * longest it may stay on, a safety stop) or off at once. Both or NULL:
     * then only the talk buzz (vibrate) and no haptic_play.
     */
    const uint8_t *haptic_actuator;
    void (*haptic_motor)(bool on, uint16_t max_ms);
    /* Whether the device's own haptics setting allows its cues (no client). */
    bool (*haptics_on)(void);
    /*
     * A display or status light that follows the client's assistant_state
     * (mg_assistant_state_t; idle when the client disconnects). NULL: the
     * command isn't offered. Called from the BLE worker: don't block.
     */
    void (*assistant_state)(uint8_t state);
    /*
     * Video streaming (mg_command_stream_video, CONFIG_MUSE_GADGET_BLE_VIDEO).
     * video_caps NULL: not offered. video_begin: a stream starts (the board
     * starts taking frames from mg_ble_vstream()); false refuses it.
     * video_end: it ended. Both are called on the BLE host or worker task,
     * with the protocol locked: don't block.
     */
    const mg_video_caps_t *video_caps;
    bool (*video_begin)(const mg_video_params_t *vp);
    void (*video_end)(void);
    /*
     * Live call (mg_command_live_call): the call's state as the client
     * reports it (mg_live_call_state_t; idle when the client disconnects).
     * NULL: not offered. Called from the BLE worker with the protocol locked:
     * don't block.
     */
    void (*live_call)(uint8_t state);
    /*
     * Live call subtitles (mg_command_subtitle):
     * what the board draws, and each message from the phone (BLE worker,
     * protocol locked: copy what you keep, don't block long). NULL: not offered.
     */
    const mg_subs_caps_t *(*subtitle_caps)(void);
    void (*subtitle)(const mg_subs_event_t *ev);
    /* Whether the board can draw subtitles now (their buffers allocated):
     * false leaves mg_command_subtitle unlisted and refused.
     * NULL: always, once subtitle_caps gave caps. */
    bool (*subtitle_ready)(void);
    /*
     * The board's clock (device_action set/get_unix_time, mgcommands.h,
     * Clock): set from the phone's time, read back for report_unix_time.
     * store: the offset and flags may be kept across restarts (else RAM
     * only: an unproven central on a set-up gadget). NULL: no clock
     * (answered unsupported).
     */
    void (*clock_set)(int64_t unix_ms, bool has_offset, int16_t offset_min, bool has_flags, uint8_t flags, bool store);
    void (*clock_get)(int64_t *unix_ms, int16_t *offset_min, uint8_t *flags);
    /*
     * Battery and power (device_action request/report_battery_status 8/9
     * and power_off 10). battery_status fills the
     * latest reading (false: none yet); NULL: no battery, not offered.
     * power_off starts the board's power-off and returns at once (it runs
     * from the BLE worker); NULL: the board can't power off from software.
     */
    bool (*battery_status)(mg_battery_status_t *out);
    void (*power_off)(void);
    bool (*power_off_ready)(void);   /* NULL: power_off always works */
    /*
     * A touchscreen of touch_w x touch_h pixels (0: none): mg_command_ui_input
     * touch events, and with CONFIG_MUSE_GADGET_BLE_HID the HID over GATT
     * service. The board feeds its touches with mg_ble_touch().
     */
    uint16_t touch_w, touch_h;
    /* The touchscreen is round (its edge ring is a ring; else a band along the edges). */
    bool touch_round;
    /* The gadget's own screen holds the touches now (a passcode keypad or any
     * other passcode entry on it): no touch event and no HID report carries
     * one (mg_touch_feed_held). NULL: never. */
    bool (*touch_private)(void);
    /* A Nordic UART line mg doesn't know: the board's bench console, for
     * the lines it allows over BLE (true if taken). NULL: none. */
    bool (*console)(char *line);
    /*
     * Link setup is under way on the connection (a pairing session): the
     * link stays active for its whole duration (link modes). NULL:
     * only while the gadget isn't set up. Polled from the worker; cheap.
     */
    bool (*setup_active)(void);
    /*
     * A user's key or touch on the gadget (mg_ble_link_user, any task,
     * before the link checks; not the screen lit alone): the device session's active task
     * (about 2 min after it, the session may open and is
     * kept awake). Must not block. NULL: none.
     */
    void (*user_action)(void);
    /*
     * The boot's healthy mark for firmware updates over BLE: an
     * updated image is kept once it gets there. NULL: the boot guard's
     * (muse_bootguard_is_healthy(): reachable and 30 s without a reset). A
     * board without the boot guard (the ESP-Mosaico) gives its own. Polled
     * from the worker; must be cheap.
     */
    bool (*boot_healthy)(void);
    /* SAFE on the public address: the musegadgets service refuses
     * every write (a phone's saved record reconnecting by address finds no
     * Muse to talk to). NULL: never. */
    bool (*muse_gatt_refused)(void);
    /*
     * The device session (mg_command_session, CONFIG_MUSE_GADGET_BLE_SESSION;
     * needs the token proof): the gadget's own VM session over BLE.
     * session_credentials fills the credential Link setup stored (false: no
     * token, and the stored access_expires_at, 0 if none); session_store_tokens
     * stores a refreshed pair and its access_expires_at (0: none) in one commit
     * (false: not saved). session_state: the client reported the VM session
     * (mg_session_state_t; open starts a new one with that epoch, anything
     * else ends it); session_message: a whole message from the VM waits for
     * mg_ble_session_take(). The last two run on the BLE host or worker task
     * with the protocol locked: don't block. NULL: not offered.
     */
    bool (*session_credentials)(mg_session_creds_t *c);
    bool (*session_store_tokens)(const char *access_token, const char *refresh_token, uint64_t access_expires_at);
    /* The credentials can't be handed out now (a re-setup is committing):
     * hellos are refused busy or wait. NULL: never. */
    bool (*session_busy)(void);
    /* The gadget holds no Wi-Fi credentials (the client refreshes its token
     * pair: caps client_refreshes, asked at each request_status). NULL: as
     * the build is (BLE-only builds hold none). */
    bool (*client_refreshes)(void);
    void (*session_state)(uint8_t state, uint16_t epoch);
    void (*session_message)(void);
} mg_ble_platform_t;

struct ble_gatt_svc_def;
struct ble_gap_event;

/* ---- hooks for the host BLE server (set before it starts) ---- */
const struct ble_gatt_svc_def *mg_ble_services(void);
int mg_ble_gap_event(struct ble_gap_event *event);
/* The musegadgets service UUID, little-endian, for advertising data. */
const uint8_t *mg_ble_adv_uuid128(void);
/* Whether it wants to be advertised now (initialized, nobody connected). */
bool mg_ble_wants_advertising(void);
/* The worker missed its 5 s heartbeat (the Muse protocol is stalled):
 * the BLE server stops advertising the Muse identity until it beats again. */
bool mg_ble_stalled(void);
/* Before the host starts (the companion's configure_host): with HID, the security manager and bonds. */
void mg_ble_configure_host(void);
/* With HID: the 16-bit service UUID and GAP appearance to advertise beside ours (0: none). */
uint16_t mg_ble_adv_uuid16(void);
uint16_t mg_ble_adv_appearance(void);

/* After NVS is up, before the host server starts. */
void mg_ble_init(const mg_ble_platform_t *platform);

/* Link setup stored, replaced or erased the proof key: re-reads whether the
 * gadget is set up, and drops a token proof made with the old key. */
void mg_ble_setup_changed(void);

/* Erases the settings kept on the device (7, 13, 14, the buzz, the queue):
 * defaults from the next boot (a factory reset; the caller restarts). */
void mg_ble_settings_reset(void);

/* The pairing changed hands (a setup reset, or a new setup's commit): the
 * offline clips go, so the next owner can't read the last one's, and a clip
 * recorded before this call is never saved after it. true once none are left
 * in flash (or there is no queue); false if the erase failed (the queue is
 * off until the next boot). */
bool mg_ble_queue_forget(const char *why);

/* An mg client is connected (not necessarily ready for audio). */
bool mg_ble_connected(void);
/* ...and ready for audio, with push-to-talk on: a press goes to it (mg_ble_link() is MG_LINK_READY). */
bool mg_ble_ptt_ready(void);
/* Where the client is, for the screen: kept current on every BLE event and command. */
mg_link_t mg_ble_link(void);
/* Who is connected, for the Status page: this
 * connection's client_app (setting 19, "" unset) and the central's GAP Device
 * Name, read once over GATT after connect (CONFIG_MUSE_GADGET_BLE_PEER_NAME;
 * "" none, not yet, or the read failed): the page shows that name, else
 * "phone" (client_name, 18, is reserved). Neither is ever
 * logged. All "" with no connection. Any task. */
typedef struct {
    char gap_name[MG_CLIENT_NAME_MAX + 1];
    char app_name[MG_CLIENT_APP_FIELD_MAX + 1];
    char app_version[MG_CLIENT_APP_FIELD_MAX + 1];
} mg_ble_peer_t;
void mg_ble_peer(mg_ble_peer_t *out);
/* GAP Device Name reads that settled with no name (an ATT error, nothing
 * there, not text, or never started), since boot; also on the console's
 * status line (peer_name_fails=N; never the name). 0 without the read. Any task. */
uint32_t mg_ble_peer_name_failures(void);

/* A live call's stats overlay: what the link and
 * the reply playback already keep, read under the protocol's lock. link:
 * a central is connected (itvl: its connection interval in 1.25 ms units, 0
 * not reported yet; mode: "sleep" / "ready" / "active"). play: the board has
 * a speaker (stream: bumped by each reply stream's start; underruns: that
 * stream's; depth_ms: reply audio queued now). Any task. */
typedef struct {
    bool link;
    uint16_t itvl;
    const char *mode;
    bool play;
    uint32_t stream, underruns, depth_ms;
    uint32_t silence_ms;   /* that stream's silence for underruns and prebuffering */
} mg_ble_call_stats_t;
void mg_ble_call_stats(mg_ble_call_stats_t *out);

/* Control's queue counts (no silent drops), as on the BLE
 * console's status line: replies dropped (no room in the outbox), held for
 * the MTU exchange, notifications refused for length, commands past the
 * command queue, capture controls past the capture outbox, device session
 * answers dropped, touch events refused; replies waiting now and the most
 * ever. Counts only. As "k=v ..." text, or (json) an object. Any task. */
int mg_ble_reply_stats(bool json, char *out, size_t n);

/* ---- video (CONFIG_MUSE_GADGET_BLE_VIDEO) ---- */
/* The receive ring the decoder pops frames from, or NULL. */
muse_vstream_t *mg_ble_vstream(void);
/*
 * The audio clock for the call picture (lip sync; mg_play_av_pos): true
 * while a reply's audio plays steadily after a resolved A/V anchor, with
 * *frame the anchor's picture and *pos_us where the sound heard now stands
 * from that anchor's sample (us of the reply's content). The playback task
 * publishes it after each speaker write (lock-free: a sequence count, no BLE
 * lock), and a read carries it forward to now on the clock; one older than
 * 60 ms, or none, is false. Any task; quick.
 */
bool mg_ble_av_clock(uint32_t *frame, int32_t *pos_us);
/* The user picked (true) or left (false) the video view: request_streaming. */
void mg_ble_video_request(bool want);
/* The decoder needs a keyframe. */
void mg_ble_video_keyframe(void);
/* The decoder took frames out: send credits soon. Also its numbers for buffer_update. */
void mg_ble_video_freed(uint16_t shown, uint16_t dropped, uint8_t decode_ms);
/* Ends a stream from the device's side. */
void mg_ble_video_stop(void);
/* Bench tuning: the connection interval (1.25 ms units) and event length
 * (0.625 ms units, 0 = the controller's) a stream asks for. */
void mg_ble_video_tune(uint16_t itvl, uint16_t ce);
/* A client that can stream video is connected and ready. */
bool mg_ble_video_client(void);
/* The display finished drawing frame number, age_ms ago; shown pictures so far (buffer_update's echo). */
void mg_ble_video_shown(uint16_t number, uint32_t age_ms, uint16_t shown);
/* Bench tuning: the peripheral latency a still UI stream's link takes (0: never relaxed). */
void mg_ble_ui_tune(uint16_t latency);

/* ---- UI input (a touchscreen: mg_ble_platform_t.touch_w) ---- */
/* A client wants the panel's touches (touch events on, or a HID mode set). */
bool mg_ble_touch_wanted(void);
/* One touch sample, about every 10 ms while wanted: pressed, and the point in panel pixels. */
void mg_ble_touch(bool pressed, uint16_t x, uint16_t y);
/* A UI stream's client takes control events (its menu, pan): mg_ui_control_t. */
bool mg_ble_ui_controls(void);
/* The menu button: opens or closes the UI stream's menu (drawn by the client). False if not sent. */
bool mg_ble_ui_menu_toggle(void);
/* Bench: a consumer-control key (0x224 AC Back, 0x223 AC Home) on the HID. False: no HID host. */
bool mg_ble_hid_key(uint16_t usage);
/* Bench: fn(arg) on the musegadgets worker, where a client's commands run
 * (its stack has room for what a console word mustn't do on the console's,
 * such as the subtitles' layout). fn owns arg. False: not queued (the worker
 * isn't up, or its queue is full), fn isn't called and arg is the caller's. */
bool mg_ble_run_on_worker(void (*fn)(void *arg), void *arg);
/* Bench: forgets every bonded phone (a HID host that keeps reconnecting) and drops the connection. */
void mg_ble_forget_bonds(void);
/* HID off (saved) and its service hidden, until a client's hid_map turns it
 * on: the console's hid.off. Does nothing on builds without HID. */
void mg_ble_hid_off(void);
/* Logs one line about firmware updates on mg.smp (the Nordic UART mirror
 * shows it): running and other slot, their states, the bootloader and
 * whether this unit can roll back. The console's read-only "ota?". */
void mg_ble_ota_log(void);

/* A firmware update's state for the screen: 0 none, 1 writing (*pct
 * 0-100), 2 installing (checked, boots at the reset), 3 stopped (aborted or
 * refused; the running image stays) until mg_ble_update_ui_clear(), and
 * MG_BLE_UPDATE_UI_ASK while an update asks for its confirm press
 * (muse_local_press_prompting). Always 0 without BLE updates. From any
 * task. */
#define MG_BLE_UPDATE_UI_ASK 4
uint8_t mg_ble_update_ui(uint8_t *pct);
void mg_ble_update_ui_clear(void);
/* "Update stopped"'s second line: "Nothing was installed"
 * before the commit, else the image's state ("Installed; restart to
 * finish"). */
const char *mg_ble_update_stop_text(void);
/* The screen has drawn state: an update's restart waits (500 ms at
 * most) for "Installing" to be on screen. Boards with a screen call it. */
void mg_ble_update_ui_shown(uint8_t state);

/* ---- device session (mg_command_session) ---- */
/* The client reports the gadget's VM session open. */
bool mg_ble_session_open(void);
/* The open session's id, 0 if none: a new one with every session. */
uint32_t mg_ble_session_id(void);
/* Hellos answered and credentials_updates saved so far: when either
 * changes, the re-ask series (open_request [1] after an unasked state)
 * starts over. */
void mg_ble_session_reask_marks(uint32_t *hellos, uint32_t *saved);
/* The connection's device session is sealed (a sealed hello): its data is
 * sealed too, both ways (mg_session.c). */
bool mg_ble_session_sealed(void);

/* Queues one message to the VM (a ServiceRequestEnvelope, copied) in
 * session (its id); false if that session isn't the open one, the client
 * doesn't take one that large, or there's no room for it now
 * (mg_ble_session_room()). */
bool mg_ble_session_send(uint32_t session, const uint8_t *msg, size_t len);
/* Bytes mg_ble_session_send() takes now, and the largest message the client takes. */
size_t mg_ble_session_room(void);
uint32_t mg_ble_session_client_max(void);
/* The oldest whole message from the VM (a ServiceResponseEnvelope) in
 * session, copied into out; its credit goes back to the client. 0 if none
 * waits, or session isn't the open one. */
size_t mg_ble_session_take(uint32_t session, uint8_t *out, size_t cap);
/* Asks the client to open (true) or close (false) the session. */
void mg_ble_session_request(bool open);
/* The largest message the gadget takes (its hello's max_message), or 0 without the session. */
size_t mg_ble_session_max_message(void);

/* ---- live call (mg_command_live_call) ---- */
/* The user started (true) or ended (false) a live call on the device. */
void mg_ble_live_call(bool start);
/* Why the call the client just reported failed (mg_live_call_failure_reason_t,
 * setup_required 1), 0 when it gave none. Any task. */
uint8_t mg_ble_live_call_failure_reason(void);
/* The call state the client just reported came from a central without this connection's token proof, and
 * wasn't the answer to the gadget's own pending start. Any task. */
bool mg_ble_live_call_unproven(void);
/* The UI holds the phone's call behind the passcode keypad: its reply audio waits for the release. */
void mg_ble_live_call_held(bool held);
/* The device's user changed a setting (mgcommands.h, Settings reports): the
 * client hears [get_settings, param, value]. */
void mg_ble_report_setting(uint8_t param);
/* Charging started or stopped: the client hears report_battery_status. */
void mg_ble_report_battery(void);
/* A press the device itself detected (mg_press_src.h: a physical key, the panel, its wake event): it opens a
 * client's start_mic for MG_PRESS_WINDOW_MS on a release build. Any other source counts for nothing. Any task. */
void mg_ble_press(uint8_t src);
/* The build offers Live call at all (a board that can draw it: PSRAM).
 * The UI shows its Live call row only then. */
bool mg_ble_live_call_offered(void);
/* The device's talk press (at touch-down): activity for link modes (active).
 * Any task. */
void mg_ble_link_activity(void);
/* The gadget's own turn (mgcommands.h): its talk press reached the
 * client (button_down sent). Activity until its reply has played, done or
 * error, or 20 s with no reply (mg_linkpol_turn). Any task. */
void mg_ble_link_turn(void);
/* Other activity of the device's own (why: a static string for the log,
 * e.g. "push-to-talk capture"): active now, and back to the app's mode 5 s
 * after the last. A touch, a key or a menu isn't activity. Any task. */
void mg_ble_link_action(const char *why);
/* A user's wake or touch on the gadget (why: "key", "touch", "wake"): any key
 * press, a touch, the screen lit by the user, and the bench's injected input
 * alike (automation behaves like a user). Link modes: sleep -> ready, never
 * active, and the 10 min in ready restarted; a sleep the app wrote is ready
 * until 10 min after the last one (mgcommands.h "Link modes"). Any task. */
void mg_ble_link_user(const char *why);
/* Bench (>ble.pause=SECS[,ADDR], !IN ble.pause=... on the Nordic UART):
 * drop the BLE connection, then for secs (1-600) either stop all advertising
 * (only_addr NULL) or accept a connection only from only_addr (6 bytes, most
 * significant first; public or random static), then resume as before.
 * Nothing else changes (no unpair, no NVS write; a restart ends it). False:
 * out of range or no way to pause on this build. Logged on mg.ble. Any task. */
bool mg_ble_bench_pause(uint32_t secs, const uint8_t *only_addr);
/* The same from a console word's text, "SECS" or "SECS,AA:BB:CC:DD:EE:FF"
 * (mg_unlock_pause_parse); logs a refusal. */
bool mg_ble_bench_pause_text(const char *text);
/* The bench pairing auto-confirm from the USB console (>pair.auto,
 * >pair.auto=0), as the Nordic UART's !PAIR AUTO. Logged on mg.ble. False
 * before mg_ble is up. */
bool mg_ble_pair_auto_arm(bool on);
/* Bench (link.wake=0|1, link.wake?; the USB console's >link.wake..., a plain
 * Nordic UART line): listening at every connection event during a link-mode
 * switch, CONFIG_MUSE_GADGET_BLE_LINK_WAKE's at boot (off by default), RAM only
 * (kept across connections; a restart puts the option's value back). link.wake=0 closes an open window. Fills out with the
 * answer (the switch, the controller mechanism, the window's state) and logs
 * it on mg.ble; false when line isn't the word. Any task. */
bool mg_ble_link_wake_word(const char *line, char *out, size_t n);
/* Bench (link.sleep=ITVL_MIN,ITVL_MAX,LAT, link.sleep=0, link.sleep?; the
 * USB console's >link.sleep..., a plain Nordic UART line): the sleep mode's
 * request for the sleep-parameter A/B (mg_linkpol_sleep_word), RAM only,
 * kept across connections, a restart restores the defaults. A set the BLE
 * spec refuses gets the usage line. Fills out with the answer (the set in
 * effect and the last sleep grant) and logs it on mg.ble; false when line
 * isn't the word. Any task. */
bool mg_ble_link_sleep_word(const char *line, char *out, size_t n);
/* The token proof's bench switch (bench.out=0|1, bench.out?) from the USB console
 * (>bench.out...): mic output to a central without this connection's token
 * proof, for a Mac bench client that holds no proof key. Automation, RAM
 * only, off at every boot, never stored, kept across connections; on
 * releases a capture held for the proof at once (mg_proto_set_output_bench).
 * The Nordic UART takes the same words for its own connection only (it
 * ends at the disconnect, logged on and off with the
 * connection, the "Bench" mark shown while on; nus_command). Fills out with
 * the answer (the switch and where output may go: open, bench, proven,
 * unproven) and logs a change on mg.ble; false when line isn't the word.
 * Any task. */
bool mg_ble_bench_out_word(const char *line, char *out, size_t n);
/* A central is connected and output may go to it (it passed this
 * connection's token proof, or the bench switch is on). Camera output over
 * the device session checks it (main/app.c camera.capture). Any task. */
bool mg_ble_output_ok(void);
/* The platform's proof_clear erase has ended (erased or not), before the restart it may schedule: a sealed
 * clear waiting for it is answered [1] or [0]. Any task. */
void mg_ble_proof_clear_done(bool erased);
/* The sealed clear's answer isn't out yet: the platform's restart waits for it, bounded. Any task. */
bool mg_ble_proof_clear_owed(void);

/* A client that can take a live call is connected and ready. */
bool mg_ble_live_call_client(void);
/* Bench: the link goes to full speed at a live call's start (on, the
 * default) or only at its video stream's start, as before (off). */
void mg_ble_call_fastlink(bool on);

/* Bench A/B (console credit.coalesce=1|0, RAM only, off at boot): a request_buffer_update during a live-depth
 * reply answered by the stream tick's next update (<= 10 ms) instead of at once (the default). */
void mg_ble_credit_coalesce(bool on);

/* Framed Data (mgcommands.h mg_data_type_framed): on/off and the counters since connect, one log line
 * ("framed: on; since connect: N selects, writes video V audio A, D discarded; plain change_data_type S").
 * The console's framed?; also at each video stream's end. */
void mg_ble_framed_log(void);


#if CONFIG_MUSE_GADGET_BLE_VIDEO
/* Bench: the frame rate stream_video's capabilities suggest from now on (RAM only). */
void mg_ble_video_suggest_fps(uint8_t fps);
#endif

#ifdef __cplusplus
}
#endif
