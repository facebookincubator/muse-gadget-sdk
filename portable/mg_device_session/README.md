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

# portable/mg_device_session: sealed device-session credentials

The gadget's side of `mg_command_session`'s credentials (`protocols/mgcommands.h`,
Sealed credentials): after a matching token proof, the device
hands its own device credentials to a Muse app sealed under the proof key K,
and stores a refreshed pair the app writes back. One implementation for every
gadget, vendored unchanged like `portable/mg_link_setup` (whose crypto backend
and token proof it uses).

| Layer | Files | Who uses it |
|---|---|---|
| 1. The seal | `include/mg_session_seal.h`, `src/mg_session_seal.c` | every gadget: the boards with their own session code (ESP32, Zephyr) use only this |
| 2. The device session | `include/mg_device_session.h`, `src/mg_device_session.c` | credentials-only gadgets (a board with no session code of its own): hello, credentials, credentials_update, sealed data's rules, the refusals and deferral, through a port; with `MGDS_DATA=1` (default 0, experimental) also the VM session's data path (e.g. a watch face showing the user's avatar) |

## Layer 1

`mgseal_keys()` derives enc_key and mac_key (HKDF-SHA256, 64 bytes) from K,
the proof's nonces and node_id; `mgseal_seal()` / `mgseal_open()` seal and
open in place (the counter and the tag are checked, in constant time, before
anything is decrypted); `mgseal_xor_at()` gives the keystream at any offset
and `mgseal_tag_stream()` the tag of a plaintext read in pieces, so a device
with no plaintext buffer can stream its credentials (the same bytes).
`mgseal_tag_begin()` / `_update()` / `_check()` make the tag of a message as
its fragments arrive, so a receiver checks a sealed `data` message without a
message buffer (`mgseal_tag_t`, 112 bytes on rv32, is the caller's).
`mgseal_saved_plaintext()` gives a sealed
`credentials_saved`'s 9-byte plaintext: the result, then the first 8 bytes of
SHA-256 over the update's `refresh_token` (zeros when not saved), hashed with
the streaming tag's SHA-256 (no new backend call); the device seals it with
`mgseal_seal()` under its next device-to-client counter
(`MGSEAL_SAVED_SEALED`, 29 bytes, is its `total_len`). The boards with their
own session code (ESP32, Zephyr) use it too.
`mgseal_rx_ok()` / `_take()` / `_unuse()` (inline, no code unless used) are
the receiver's counter rule (two messages in flight): the next
counter or the one hole a message refused at its tag left after a later
counter was taken; layer 2 and the boards with their own session code
(ESP32) run the same one.

The nonces come from the token proof: `mgtp_take_nonces()`
(`portable/mg_link_setup`, `mg_token_proof.h`) hands out the matching
proof's client and device nonces **once** and wipes them; it returns false
before a match, after a take, and after a new challenge or a reset. **One
token proof keys one sealed session**: the keys depend only on K, the nonces
and node_id and the counters restart at every hello, so a second sealed
session under the same proof would reuse the keystream. A board takes the nonces at its sealed hello,
keeps them for that session only, and answers a second sealed hello under the
same proof with `proof_required`. `mgtp_t` keeps its size: the 32 bytes that
held the expected confirm MAC now hold the nonces, and the confirm's MAC is
computed when it arrives.

Cost (rv32, -Os, builtin backend): no RAM of its own; the stack of one
`mglsc_hmac_sha256` call, plus ~0.2 KB for `mgseal_tag_stream()`. The
streaming tag carries a small SHA-256 of its own; `-DMGSEAL_EXT_SHA=1` hashes
with the backend's incremental SHA-256 instead (`mglsc_sha256_init` /
`_update` / `_final`, in `portable/mg_link_setup` from the same commit):
the seal object 2,582 -> 1,616 B on rv32imc_zba_zbb_zbc_zbs (2,800 -> 1,752
on rv32imac) for 84 B of backend wrappers, about 0.9 KB less per board, the
same bytes out (the runner checks the vectors and layer 2 both ways, and on
PSA). A board that links `portable/mg_link_setup`'s backend sets it, so SHA-256 is compiled once; the default (0)
is for ports whose seal backend has only the two primitives (the ESP32's) or whose backend SHA-256 costs more (the
Zephyr port, on PSA). `-DMGSEAL_STREAM=0` leaves the
streaming tag out.

Vectors: `protocols/test-vectors/mg-session-seal-v1.json` and
`mg_session_seal_ref.py`.

## Layer 2

`mgds_rx()` takes every `mg_command_session` write, `mgds_poll()` sends what
is owed and runs the deferrals, `mgds_reset()` ends the session (call it at
every connect and disconnect, every token proof challenge, and every setup
reset or commit; at a commit, before the new set is stored, e.g. in Link
setup's `commit()`: a sealed clear the session took and hasn't run yet runs
in the reset, and must erase the set it was sealed for, not the new one),
`mgds_release_storage()` takes the lent storage back. The
board writes a port (`mgds_port_t`):

| Port call | What the board does |
|---|---|
| `send` | one Control notification: `MGDS_SENT`, `MGDS_AGAIN` (no buffer: offered again from the next poll), < 0 (link gone) |
| `mtu`, `encrypted`, `recovery` | the connection's ATT MTU, a bonded link (unused: every unsealed hello is refused `insecure`), a recovery mode |
| `proven`, `take_nonces` | `mgtp_matched()`, `mgtp_take_nonces()` (or the board's own proof, with the same once-per-proof rule) |
| `get_k`, `node_id` | K (`MGDS_NONE` without a pair or with a placeholder pair) and node_id |
| `token_len`, `token_read` | the stored pair, read in pieces of at most 32 bytes (no plaintext buffer) |
| `busy` | media runs: a hello's answer and an update's commit wait (up to 30 s) |
| `borrow`, `release` | storage for one incoming `credentials_update` (up to 2,068 bytes), or NULL: the update is discarded (sealed: answered `[0]` at its last fragment) |
| `commit` | the new pair in one commit, K and node_id unchanged. A pair whose credentials (with node_id and the expiry) would pass 2,048 bytes isn't committed: answered not saved, the old pair kept |
| `expires`, `commit_expires` | optional, together (NULL: none kept): the stored `access_expires_at` for the credentials, and the commit that keeps the update's with the pair (0: the update carried none). A board that sets `mg_session_cap_client_refreshes` without them is refreshed, and commits, on every connection: the app treats credentials without `access_expires_at` as expired |

Every check runs before the first byte goes out: the credentials are read
once to make the tag and to check every token byte, then each fragment is
read and encrypted again as it goes, so a device needs no output buffer. A
refused hello leaves a session as it was; a second sealed hello under one
proof gets `proof_required`. The device opens no VM session: state is
recorded, client data is dropped and credited back.

Sealed data (mgcommands.h, Sealed data): in a sealed session every
client message with a payload is sealed, `credentials_update` and `data`, with
one client-to-device counter shared by both (from 0 at the sealed hello). The
counter is read from the first fragment and must be the next one (otherwise
`invalid_value`, the counter unused); once taken it is used (an abort, a state
change, an update that finds no storage, a fragment refused before the tag), except by a message refused at its
tag, so a forged message that reaches its tag can't move the counter; one cut off before it (a first fragment
with the next counter, then a malformed one) has used it, as an abort does (the seal reference's
`SealedReceiver` uses it there too; the vectors don't tell the two apart). An unsealed `data` or
`credentials_update` in a sealed session gets `insecure` and changes nothing. A sealed first fragment carries the
whole 4-byte counter (mgcommands.h): a shorter one gets `invalid_length` (its bytes back), and the device
starts a sealed message of its own only with at least 4 bytes of credit.

The sealed clear (`mg_session_clear` in mgcommands.h): the Forget in a sealed session, client -> device,
sealed only, fragmented like a `credentials_update` (outside credit): counter || one byte (0) || tag, 21 bytes;
one write is opened from a copy on the stack, fragments (ATT MTU under 31) go into the port's lent storage (none
now, or a `credentials_update` holding it: `busy` at the first fragment, its counter used (the first
fragment carries the whole counter); the client retries on the next one). It adds no RAM to `mgds_t`.
Unsealed gets `insecure`, a short counter `invalid_length`, a wrong total, a second clear or a counter that isn't
the next one `invalid_value`, a wrong tag `invalid_value` (its counter unused), a plaintext other than 0
`invalid_value` (its counter used); none erases anything. Taken, the port's `clear()` erases the tokens, K and the
setup markers at once; only the answer waits for what goes out before it (a sealed `credentials_saved`, a data
message part-way): then the device answers a sealed `[1]` (`[0]` if the erase failed; sub-command clear) on its
next counter, sealed with the session's kept keys, and the session ends; other writes meanwhile get
`invalid_value`. Credentials not started yet never go out and their counter is never used: the answer is sealed on device counter 0. Two cases erase only
when the answer's turn comes: behind credentials part way out (they are read from the stored pair), and in every case without `MGDS_KEEP_KEYS` (the keys come from K, and a message
part-way is sealed under them). A port whose erase runs elsewhere (a flash erase off this context) returns
`MGDS_CLEAR_PENDING` from `clear()` and calls `mgds_clear_done(t, erased)` when it ends: once what went out before
the clear is out the session holds until then (every session frame `invalid_value`, no data out), then the answer
reports the real result. `mgds_clear_done()` carries no handle (it completes whichever clear is pending now), so
such a port runs one erase at a time, bound to its session: after `mgds_reset()` an erase still running belongs to
no session and isn't reported, a newer `clear()` waits for it, and so does a setup commit (the old erase would wipe
the new pair). At its turn both answers are sealed (without `MGDS_KEEP_KEYS`, before the erase), the
`[0]` tag kept in the spent keys (or nonces), so `mgds_t` keeps its size; only one is ever sent. A link lost while
the erase runs, or after it and before the answer, sends nothing and erases nothing again. A link lost after the
clear's tag checked but before it ran still erases (the Forget was authenticated). Without `port.clear` it is
refused `unsupported`.
`MGDS_CAP_SEALED_CLEAR` (default 0 until both Muse apps send the sealed clear) adds `mg_session_cap_sealed_clear` to
`MGDS_CAPS`. It is one switch with the token proof core's `MGTP_SEALED_CLEAR` (an unsealed token_proof clear then
gets `insecure`): either name sets both, and a build that sets them differently fails. The coupling is in the
preprocessor, so define it for every unit of both libraries (a global define): a define only one library's units see
leaves the other at 0.
A data message's tag is checked as its fragments arrive (`invalid_value` if
it doesn't match; nothing is hashed for one whose keys couldn't be had); either
way the message is dropped and its bytes go back as credit (credit the stack
has no buffer for goes from the next poll). A message refused at its tag doesn't use its counter (a forged message
can't move it). The device sends no data.

Sealed `credentials_saved` (mgcommands.h, Credentials; vectors
`sealed_credentials_saved`): in a sealed session the answer to a
`credentials_update` is sealed under the next device-to-client counter (one
counter: the credentials take 0, the answers 1, 2, ...), framed as
credentials (`[0x89, 8, first|last|sealed, total_len 29, counter || ciphertext
|| tag]`, one 36-byte write whatever `MGDS_FRAME_MAX`: fragmented only to an MTU under 39), sent only after the
commit: `[1 || SHA-256(refresh_token)[0:8]]` stored, `[0 || zeros]` not. Its
plaintext waits in `tag[]` (free once the credentials are out) and is sealed
again for each fragment (`owe0` sits beside `state`, the counter `tx_ctr`
in padding: see `MGDS_TX_CTR_U8`). Every write-back given up on while the link is up is
answered sealed `[0]`, never silence (silence now reads as "assumed saved"):
a commit held past 30 s, a held update dropped by `mgds_release_storage()`, an
update that found no storage or lost it (at its last fragment), an update
refused when the stack had no buffer for its refusal. A held update stays
through a refused first fragment (another update, an unsealed one): only the
new one is refused. A `[0]` owed while the credentials or an earlier
answer go out waits for them (`owe0`, at most 15), a commit waits too, an
answer part-way out at a reset on a live link gets `abort`. Counters never wrap: after the message with its
counter's last value (0xFFFFFFFF, or 255 with `MGDS_TX_CTR_U8=1`) the device seals nothing more and refuses every
sealed client message with `invalid_value` (data: its credit back; an update held for its commit too, nothing
committed), and once the client's 0xFFFFFFFF is taken no
other client counter is (a hole still may be); either way the client re-proves into a new sealed session. A sealed
hello on a link whose ATT_MTU is below `MG_MIN_ATT_MTU` (100) gets `invalid_value` (the one-write messages need it);
a board that can't reach it sets `MGDS_ATT_MTU_MAX` and doesn't declare `mg_session_cap_sealed`. An update whose pair would make a stored
document over `MGDS_MAX_CREDENTIALS` (node_id, the pair and `access_expires_at` where the board keeps one: what the
next credentials sends) is refused `invalid_value` before anything is stored, as one whose JSON doesn't parse.
Refusals (the JSON, the size, `insecure`) stay unsealed error frames; a session whose hello didn't ask for the sealed answer (an older
client) keeps the one-byte `[1]` / `[0]`, and its give-ups stay silent.

### The keepalive (`mgds_nudge()`)

`mgds_nudge(t)` sends one `[session, credit, 0]`, or the credit owed with it, on a session the client reported open
(state open, after the hello and credentials); nothing before that, after any other state, or while a sealed clear is
answered. Credit is never sealed nor forwarded, so the VM sees nothing and both Muse apps take a zero credit silently,
but it is a Bluetooth event: iOS wakes a suspended app for it, which then reads its socket. A screen gadget that shows
a VM conversation over the session calls it every `MGDS_NUDGE_MS` (20 s) while that view is shown and stops when it
closes or the screen sleeps. When the
stack has no buffer it goes from the next `mgds_poll()` (`mgds_busy()` until then), unless the session ends first. It
costs `mgds_t` one bit in existing slack.

### Stall rules (`MGDS_STALL`, default 0, experimental)

Layer 2 keeps the stall rules itself (taken from boards' own stall guards), so a
board needs no stall guard of its own around the session. With
`MGDS_STALL=1` and the port's `end_link` set:

- the 35 s stall clock (`MGDS_STALL_MS`) runs only while the device owes the
  client frames: its hello and credentials going out, a sealed
  `credentials_saved` going out, a give-up `[0]` waiting or a sealed clear's
  answer going out; and while a sealed clear is part way in (its fragments in
  the lent storage). Any client write
  on the link restarts it: a session frame (`mgds_rx()`), or any other write
  the board reports with `mgds_client_frame(t, now)` (Control, Data).
  Owing 35 s with no client write ends the
  session with the link;
- a `credentials_update` part-way (storage lent, or its fragments being
  discarded for want of storage) doesn't start that clock, but no session
  frame for 2 min (`MGDS_UPDATE_IDLE_MS`) while one is ends the session with
  the link too, so a stalled client can't hold a battery gadget awake. Only
  session frames count there: a client busy on Control that never finishes
  the update still reaches it;
- a board that sleeps with the link up calls `mgds_woke(t, now)` at the
  sleep's exit: the clocks run on the `now` it passes and
  `mgds_poll()` doesn't run while it sleeps, so without it the first poll
  after a sleep over 35 s would read as a stall.

Ending with the link, never silently: the session ends
as `mgds_reset(t, true)` ends it (an abort for credentials or an answer
part-way, the storage back wiped, `data_state` closed for a VM session that
was open), then `end_link(ctx)` drops the Bluetooth link; the next connection
re-proves. It's checked in `mgds_poll()`, which a board calls while
`mgds_busy()` (true in both cases). `end_link` NULL: the rules are off.
`MGDS_STALL=0` compiles none of it (the object code is unchanged); the port
then has no `end_link` member. A stall in the VM stream alone (the port's data
waiting for credit) isn't one of these: closing a stalled VM stream stays the
board's job.

| Port call | What the board does |
|---|---|
| `end_link` | `MGDS_STALL` only: drop the Bluetooth link now (the session is already reset); the disconnect then calls `mgds_reset(t, false)` as usual. NULL: no stall rules |

The board also calls `mgds_client_frame()` for every client write that isn't
a session frame, and `mgds_woke()` as it leaves a connected sleep.

### A released update's `[0]` from the poll (`MGDS_GIVE_UP_LATER`, default 0, experimental)

With `MGDS_GIVE_UP_LATER=1`, `mgds_release_storage()` doesn't seal the give-up
`[0]` it owes for a held update on the spot: the answer is owed
(`mgds_busy()` true) and the next `mgds_poll()` seals and sends it under the
next counter, in order with the port's data, as when the stack had no buffer.
For boards that reclaim the storage at the bottom of a deep chain (a media
start from the console or UI): the seal's HMAC (about 640 B of stack on rv32)
moves to the poll (found on a board's deepest main-stack chain).
The wire is the same, one poll later; 0 compiles none of it.

### No ask of layer 2's own without a task (every `MGDS_DATA` build)

A gadget opens and keeps its session only for an active task, and a save
while closed asks at once only with one (`mg-session-v1.json`: device flag
and event `task`). `mgds_ask_quiet(t, true)` (no task) sets a persistent bit (bit 7 of
`ra_steps`; `mgds_t`'s size unchanged): layer 2 then starts no
`open_request [1]` of its own, neither a re-ask after a closed or
no_network it didn't ask for (not scheduled; the series keeps its step) nor
the ask behind a saved credentials_update (`mgds_ask_hold()` can't reach
that one: it goes out inside the same `mgds_rx()`); a waiting re-ask and an
owed `[1]` are dropped silently, the port's `[0]` still goes. The bit
survives a save, a session's end and a hello. `mgds_task_start(t, now)` (a
task starting) clears it and, with the hello done and the VM session down,
asks under the series rules: at once if the ended kind's series hasn't asked
yet, else at that series' next step (an ask of that kind already waiting
stands); `mgds_open_request(t, true)` clears it and asks at once, outside the
series; `mgds_ask_quiet(t, false)` only clears it. The client's
`mg_session_wake` is a task of its own (`mgds_task_start()`) after the
hello and ignored before it. A port with no active task sets the bit at
init and whenever its task ends; one that never sets it always has a task
(as before). `MGDS_ASK_QUIET` is still accepted (it needs `MGDS_DATA`) and
changes nothing. Host: `AskQuiet` in `tests/test_session.py`, and the
vectors' task and wake exchanges.

### The data path (`MGDS_DATA`, default 0, experimental)

For a watch face showing the user's avatar. On
BLE the phone holds the Noise connection to the VM with the gadget's device
token; the gadget exchanges that session's messages as `mg_command_session`
`data` (device -> client ServiceRequestEnvelopes, client -> device
ServiceResponseEnvelopes: noise_core's ServiceCodec wire), sealed both ways in
a sealed session under layer 1's seal. A client message whose plaintext
doesn't decode as a ServiceResponseEnvelope (ServiceCodec's rules: a clear or
`credentials_update` relabeled as data, say) gets
`invalid_value`, its credit back and its counter used, and never reaches the
port (mgcommands.h, Sealed data). No Noise, X25519 or AES-GCM on the
gadget. With `MGDS_DATA=0` none of it is compiled: the object is byte-identical
to the credentials-only layer's (checked for the default and every option set
the boards use, on the host, rv32imc / rv32imac and Cortex-M33 compilers).

Port fields (after the others in `mgds_port_t`, only with `MGDS_DATA`):

| Port field | What the board does |
|---|---|
| `data_state(ctx, state, epoch)` | every state the client reports (`mg_session_state_t`: open with a new epoch starts a session, any other ends it: drop every stream, partial message and pending request, replay nothing), and closed when layer 2 ends a session that was open (or, lent state attached, connecting): a reset, a new hello, a lost link. Reported with or without the lent state. Optional |
| `data` | the data path's state, `mgds_data_t` (72 B on rv32), at a fixed address: layer 2's only between `mgds_data_attach()` and `mgds_data_detach()` (a static on a board with RAM to spare; else a fixed spot in a lease the board holds while the VM session runs). NULL: no data path |
| `data_message(ctx, msg, len)` | reassembled (a buffer given to `mgds_data_attach()`, at least the device's `max_message`, e.g. 4,096): a client message is reassembled there sealed, opened in place after its tag is checked, and handed over once: `len` <= max_message - 20 sealed, valid only during the call (wiped after), its credit already sent |
| `data_piece(ctx, off, piece, n, len)`, `data_end(ctx, ok)` | in pieces (no buffer): plaintext bytes [off, off + n) of a message of len bytes, in order, at most 64 bytes each, then `data_end(true)` (whole, tag matched, a ServiceResponseEnvelope) or `data_end(false)` (drop it: a bad tag, not an envelope, an abort, a refused fragment, a state change, a reset, a detach). Sealed pieces are decrypted before the tag is checked: act only at `data_end(true)` |
| `data_sent(ctx, ok)` | the message given to `mgds_send()` went out whole (true) or was dropped (false: a state other than open, a reset, a new hello, a lost link, a seal failure); its buffer is the port's again. Not for `mgds_send_abort()` or a detach. Optional |

Calls (the same context as `mgds_rx` / `mgds_poll`):

| Call | What it does |
|---|---|
| `mgds_data_attach(t, buf, cap)` | lends `port.data` (wiped) and, for whole messages, `buf` (`cap` >= max_message; NULL: pieces). The client's hello limits come with it: exact if it was attached at the hello, else as mgds_t keeps them (rounded down to powers of two: never more than the client allows). Data flows from the next state it sees (every state resets credit): attach before the session opens (before `mgds_open_request(true)`, or at the latest in the `data_state()` callback for open); attached while the session is already open, it carries nothing until the next state. Attached again, it detaches first; an abort or credit that detach still owes goes from the next poll |
| `mgds_data_detach(t)` | true: detached (wiped, the board's again). false: something was part-way or owed and the stack had no buffer for it now (the port's message part-way: dropped, abort owed; a client message in flight: dropped, its credit owed; credit owed): call again after the next poll. Not from `data_message` / `data_piece` / `data_end` |
| `mgds_ready(t)` | static inline, any build (no code unless used): the client reported open and the hello and credentials are out; boards use it instead of the core's private state |
| `mgds_send_max(t)` | the largest message `mgds_send()` takes now: the client's max_message, less 20 sealed; 0 without a session or a live lent state |
| `mgds_can_send(t, len)` | the state is open, the credentials are out, the lent state saw the session open, no message of the port's is going out, 1 <= len <= `mgds_send_max()`. The client's credit doesn't count: a message larger than its window goes out as credit comes back |
| `mgds_send(t, msg, len)` | `MGDS_OK`: taken, `msg` stays valid and unchanged until `data_sent()`; sent from the next poll (or the end of the `mgds_rx()` it was called from), sealed under the next device -> client counter (reserved as its first fragment is made), fragmented to the MTU and `MGDS_FRAME_MAX`, within credit. `MGDS_FAIL`: not taken, nothing used |
| `mgds_send_abort(t)` | drops the port's message: part-way, abort goes out (its counter used on both sides); before any byte, its counter is given back |
| `mgds_open_request(t, open)` | open_request [1] / [0] from the next poll. [0] stops the re-asks until a hello, a saved update, an open or [1]; [1] drops a re-ask waiting. Ignored before the hello, [1] while open |
| `mgds_ask_quiet(t, on)` | no task: on, layer 2 asks nothing by itself (no re-ask, no ask behind a save) until `mgds_task_start()`, `mgds_open_request(t, true)` or `mgds_ask_quiet(t, false)`; drops a waiting re-ask and an owed [1]; survives a save, a session's end and a hello |
| `mgds_task_start(t, now)` | a task starts (a wake from the client too): quiet off; with the hello done and the VM session down, the ended kind's series asks, at once if it hasn't asked yet, else at its next step; not after not_enrolled, auth_failed, too_large or the port's [0] |
| `mgds_ask_hold(t)`, `mgds_ask_pending(t)` | for a session only while a task is active: drops the re-ask waiting for its time without sending anything (the series keeps its step); a port with no active task calls it from its `data_state()` callback after a closed or no_network it didn't ask for (layer 2 schedules the re-ask before the callback), and when a task ends with the session down; the next task asks with `mgds_open_request(true)`. `mgds_ask_pending()`: a re-ask waits. Never called: today's behavior |
| `mgds_vm_state(t, &epoch)` | the client's last state (closed before any) and epoch; without the lent state open or closed and epoch 0 |

Without the lent state (never attached, detached, or `port.data` NULL) every
state is still reported and the re-ask series run (their state is in
`mgds_t`), client data is dropped and credited back exactly as with
`MGDS_DATA=0`, `mgds_can_send()` is false, and a session open then counts as
short for the series (its open time is in the lent state, so the series goes
on: never asked faster). What stays in `mgds_t` (in padding: it doesn't grow
on rv32 in any option set, nor on 64-bit hosts): the re-ask flags (1 byte),
both series' steps and the attached bit (1 byte), the client's hello limits
as powers of two (2 bytes); the waiting re-ask's time shares `data_ctr`
(only used while a client data message is in flight, which needs the
session open, and a re-ask only waits while it isn't).

The rules it keeps:

- Sealed data on the shared counters (mgcommands.h, Sealed data): client -> device
  as before (credentials_update and data share one counter); device -> client
  the credentials take 0, then data and sealed credentials_saved answers take
  the next in the order their first fragments go. A data message reserves its
  counter as its first fragment is made; a sealed answer never goes between a
  data message's fragments (the Muse apps count a device
  message's counter when it completes or a state cuts it off): it waits for the
  data's last fragment, the abort owed for it, or the state that ends it. An
  answer owed goes before the next data message is reserved, and a data
  message waits for an answer part-way. A state that drops the port's message
  before any of its bytes went out gives its reserved counter back (the client
  never saw it); once its first fragment went, the counter stays used on both
  sides.
- Credit: the client's window from its hello, credit back from the client
  adds to it, and every state (and a hello) makes it the window again. A
  client message's bytes go back at its end, before the port has it (as the
  reference device: credit, then what handling sends); one larger than the
  device's window that the port keeps gives back what it holds once that
  fills the window (as the reference device and the ESP32), or it could
  never finish; one the port doesn't keep, its bytes as they land. A
  fragment that would take the client past the credit the device granted
  (its window, less what it hasn't given back) is dropped with its message:
  `invalid_value`, every byte given back, counted in `st.bad_data`
  (mgcommands.h, Credit). Credit the stack had no buffer
  for goes from the next poll.
- The re-ask series (mgcommands.h, Session state;
  `mg-session-v1.json` + `mg_session_ref.py`, the ESP32 and Zephyr
  implementations): after a closed or no_network the device didn't ask for,
  open_request [1] at once, then the series' steps on the port's clock (the
  `now` of `mgds_rx` / `mgds_poll`): after closed 1, 2, 4 ... s capped at
  60 s, after no_network capped at 15 s, two series that never restart each
  other; an ask of the same kind already waiting stands (the series doesn't
  advance), one of the other kind waiting yields to the earlier; connecting
  drops one waiting. A session that stayed open 60 s restarts both at its
  end (after a shorter one the same kind continues its series); a hello, and
  a saved credentials_update, restart both; a save while the session has
  ended asks at once, behind its credentials_saved, as the ended kind's first
  ask (that series' next step 1 s). None after the device's own [0],
  not_enrolled, auth_failed or too_large until a hello, a saved update or an
  open (and [1] for [0]). A closed or no_network after connecting with no
  session open (a failed open) is an unasked end of its kind: never dropped,
  so a failing open is asked at least every 60 s (15 s) while the link is up.
  A link loss or reset ends everything.
- Caps: `MGDS_CAPS` defaults to 0x03 (no `mg_session_cap_credentials_only`).
- Needs `MGDS_SEALED_DATA=1` and the `uint32_t` device counter
  (`MGDS_TX_CTR_U8=0`): `#error` otherwise.

Cost (rv32imc, -Os, -ffunction-sections): 11,788 B of code against 7,620
(+4.1 KB); `mgds_t` unchanged (264 B; 108 with `MGDS_KEEP_KEYS=0` and
`MGDS_TAG_LEND=1`), `mgds_data_t` 72 B where the board lends it,
`mgds_port_t` +24 B; no static RAM; the reassembly buffer is the board's.
Each device message costs its tag (two SHA-256 blocks plus one per 64 bytes)
and its keystream twice (once for the tag, once as each fragment is made: no
output buffer).

Adopting it from an older copy: take `include/`, `src/` and the tests
unchanged; nothing in the port changes. Code reading `mgds_t.state` still
works (`state` is now a 4-bit field beside `owe0`); its address can't be
taken.

Options (compile time; the defaults keep everything in `mgds_t`). Rows marked
**experimental**, and `MGDS_DATA`, `MGDS_STALL` and `MGDS_GIVE_UP_LATER` above,
have no user in this repository and are tested on the host only:

| Option | Default | What it does |
|---|---|---|
| `MGDS_KEEP_KEYS` | 1 | 0 is **experimental**. The session's seal keys kept from its sealed hello (64 B); 0 keeps the nonces (32 B) and derives the keys again (HKDF: three HMACs, about 0.25 M instructions on rv32) for each sealed message and each credentials fragment |
| `MGDS_TAG_LEND` | 0 | **Experimental.** 1: the tag state of a sealed data message in flight (`mgds_dtag_t`, 128 B) is lent by the board per message: it defines `mgds_tag_lend(ctx)` and `mgds_tag_return(ctx, d)` (e.g. from its setup lease); no lend: the message is refused `busy` at its end, its counter unused |
| `MGDS_END_ON_BAD_TAG` | 0 | **Experimental.** 1: a sealed message refused at its tag (data or `credentials_update`) also ends the sealed session (a device may end a sealed session on any refusal): one forged message per proof. `insecure` refusals cost no crypto and don't end it; counter refusals end it only as `MGDS_CTR_END` says |
| `MGDS_CTR_END` | 3 (0 without `MGDS_SEALED_DATA`) | This many counter refusals in a row in one sealed session (a first fragment, data or `credentials_update`, whose counter is neither the next one nor the hole) end it, as any session end: no new message. The end is complete, not a new state or epoch under the old seal: `end_session()` wipes the session's keys (or nonces), clears the hello, and sets both counters to 0, and a sealed hello is accepted again only with a new token proof's nonces (`take_nonces()`: one sealed session per proof; until then `proof_required`), so the client re-proves, says hello, and both sides count from 0 under new keys. The case: a relay that counted a message whose first fragment never left (a second epoch cut it) is one ahead for good, and the exact-next counter rule would refuse every later message. Only counter refusals count (a tag refusal gives its counter back and resets the run, so a forged message can't end a good session); an acceptable counter, a hello or a session end resets the run. 2 bits beside `data_bad`, `st.ctr_ends` beside `st.last_refusal`: `mgds_t`'s size unchanged. 0: off, 1..3 |
| `MGDS_SEALED_DATA` | 1 | 0: the counter rules without the tag (a bad tag is then not refused: a conformance gap) |
| `MGDS_STATS` | 1 | 0: no counters (16 B) |
| `MGDS_TX_CTR_U8` | 0 | **Experimental.** 1: the device -> client counter (credentials 0, each sealed `credentials_saved` the next) is a `uint8_t` in padding (no growth in any config, for RAM-tight boards); after the answer with counter 255 (254 refreshes into one sealed session) the session is spent (sealed client messages get `invalid_value`) and the client re-proves. 0: a `uint32_t` |
| `MGDS_ATT_MTU_MAX` | 0 | the largest ATT_MTU the board's stack can negotiate, if below `MG_MIN_ATT_MTU` (100): `MGDS_CAPS` then leaves out `mg_session_cap_sealed` (no session can open on such a board). 0: it can reach 100 |

`mgds_t` on rv32: 264 B by default; 232 with `MGDS_KEEP_KEYS=0`; 136 with
`MGDS_TAG_LEND=1`; 104 with both; 92 with `MGDS_STATS=0` too; 260 with
`MGSEAL_EXT_SHA=1`; 128 with `MGDS_SEALED_DATA=0`. With `MGDS_TX_CTR_U8=1`
every one of them is unchanged by the sealed `credentials_saved`; with the default `uint32_t` device
counter the default stays 264 and the others take 4 bytes more (8 with
`MGDS_STATS=0` alone, the 8-byte alignment).

Cost (rv32imc, -Os, -ffunction-sections): 6.5 KB of code (layer 1 3.0 KB more;
the sealed `credentials_saved` added about 0.9 KB and 0.2 KB of it),
`mgds_t` per connection (above), no static RAM, and about 1.1 KB of stack at
the deepest (a fragment: the 244-byte fragment buffer, `MGDS_FRAME_MAX`, plus
one HMAC). A sealed data message costs its tag: two SHA-256 blocks plus one
per 64 bytes (with `MGDS_KEEP_KEYS=0`, two key derivations more).

## Tests

    portable/mg_device_session/tests/run.sh

A strict `-Werror` build (the warning set of `mg_link_setup`), the
exported-symbol prefix check, the seal vectors byte for byte at `-O2` and
under ASan + UBSan (with and without the streaming tag), the reference
checker's rejections (a flipped tag, a reflected direction, a replayed
counter), every truncation and bit flip of each vector, and the streaming
path at every fragment size against the in-place one.

Layer 2 end to end (`test_session.py`): the gadget (`sim_session.c`, ASan +
UBSan) against a client built from the references (the seal reference and
the exchange reference, `protocols/test-vectors/mg_session_ref.py`): the
exchanges of `mg-session-v1.json` a credentials-only device answers, byte for
byte (every session sealed, a new proof before each further hello; the
credentials rewritten without the device key fields; state open opens
nothing): the credentials and refresh exchanges, the errors, the gating ones
(an unsealed hello refused `insecure` on any link: credentials leave and are
taken only sealed); an unsealed hello on both
links leaving the proof's nonces for the sealed hello after it;
`one_session_per_proof`; sealed handoffs at ATT MTU 23 to 517;
updates with counters, replays, skips, reflections, flipped tags, other keys
and unsealed writes (`insecure`), counters used by aborted and dropped
updates (not by one refused at its tag); the `sealed_data` vector frame by frame
(the credentials byte for byte, every client data and update write with the
gadget's answer: credit, credentials_saved, a replayed counter's
`invalid_value`, unsealed data's `insecure`, an aborted message's used
counter), a bad tag, the tag split at every write boundary, malformed sealed
data, a state change mid-message, a new proof restarting the counters, and
no plain session (data after a refused unsealed hello refused); the JSON rules; `access_expires_at` kept and handed back
(every form but an unsigned integer stored as none, a duplicate refused), and
ignored by a board without it; deferral and its limits (a hello not
answered within 1 s gets busy and doesn't spend the proof; a commit waits
30 s); no storage, storage taken back mid-message;
a failed commit; back-pressure, abort on a reset part-way, a lost link; an
update held while the credentials still go out; an unsafe or oversized
stored token refused before any byte. Five builds: the default,
`MGDS_SEALED_DATA=0`, `MGSEAL_EXT_SHA=1`, `MGDS_TX_CTR_U8=1` with
`MGSEAL_EXT_SHA=1`, and `MGDS_STATS=0` with a 20-byte fragment buffer; then `MGDS_KEEP_KEYS=0` with `MGDS_TAG_LEND=1` (the whole
suite, plus the lend's return on every way a message ends and a refused lend
as `busy` with its counter unused), and `MGDS_END_ON_BAD_TAG=1` (a bad data or
update tag ends the session, a counter or `insecure` refusal doesn't). The
sealed `credentials_saved` (`SealedSaved`, and every sealed exchange above
with its sealed answers): `sealed_credentials_saved` byte for byte where a
conforming device makes the bytes (saved, not saved) and the device's own
answer where the vector shows a broken or old one, each read by the
reference client's verdict; `sealed_data_large`'s answer (the same plaintext,
counter 1 here: this device sends no data); every give-up answered `[0]` (30
s, storage taken back from a held update or mid-message, no storage, a refusal
without a buffer), a held update kept through refused first writes, nothing on a lost link; a `[0]` waiting for the
credentials and for an answer still going out, a commit waiting behind both;
a refused write offered again; an abort at a reset part-way; the counter
past 256 (`uint32_t`), or with `MGDS_TX_CTR_U8=1` (its own build, with
`MGSEAL_EXT_SHA=1`) the session spent after 255 and a re-proof opening a new
one; both counters at the top (0xFFFFFFFF: the client's taken, then 0 refused, and a hole near it; the device's
sealed, then every sealed message refused, an update held behind that last answer included, nothing committed) and a
sealed hello below `MG_MIN_ATT_MTU`; no plain session to take an
update. Layer 1 also replays every sealed message of `sealed_data` and
`sealed_data_large` (seal, open, and the incremental tag in pieces of 1 to
4,096 bytes; a flipped byte, another counter or the other direction
refused), and every sealed answer of `sealed_credentials_saved` against
`mgseal_saved_plaintext()`.

With `MGDS_DATA=1` (five more builds: the default, `MGDS_KEEP_KEYS=0` with
`MGDS_TAG_LEND=1`, `MGSEAL_EXT_SHA=1`, `MGDS_STATS=0` with a 20-byte fragment
buffer, and `MGDS_END_ON_BAD_TAG=1` for its own class) the whole suite runs
again, plus: every write of every exchange of `mg-session-v1.json` (sealed
data both ways, credit in sealed bytes) through
the C gadget with a board's VM client on top (the reference device's VM end:
/link-control, link.register, link.invoke -> link.result), every notification
byte for byte (the credentials rewritten without the device key fields), with
client messages reassembled and again in pieces; the re-ask series on the
host clock (closed 0, 1, 2, 4 ... 60 s; no_network ... 15 s; separate series;
restart only after 60 s open; hello and a saved update restart both; a save
while ended asks at once as that kind's first, behind a sealed answer the
stack had no buffer for; the blocks and what lifts them; the port's [0] and
[1]; connecting drops a waiting ask; one of the same kind waiting stands; no
buffer for the ask; 40 failed opens in a row, each asked within its cap);
`sealed_data` (with a hello without `sealed_saved`) and `sealed_data_large` byte for byte with
the device's own data sent by the port and every client message handed over;
sealed both ways on the shared counters, an answer held until a data message's
last fragment, an answer waiting for a reserved first fragment, abort part-way and
before any byte (counter used or given back), state change, reset and lost
link mid-message, a client message larger than the window, pieces with a bad
tag, no plain session, ATT MTU 23 to 517, the caps and limits (all with the
lent state attached from the start); the re-ask suite again with it never
attached (a session counted short); the lent state absent (states reported,
data dropped and credited, nothing sent), `port.data` NULL, detached and
attached again between sessions (the counters go on, the window rounded down
when attached after the hello), attached inside the open callback, attached
late (nothing until the next state), detached with the port's message
part-way (abort, retried when the stack had no buffer), with a client message
in flight (credit back, `data_end(false)`) and with a reserved counter never
sent (given back). The counter after updates (from a board that did
not open at counter 4), checked by the Muse app's own rules
(counted at completion or a state's cut, no interleave): two updates then the
VM's link-control and a 2.5 KB link.register; an answer owed when register
starts and one that comes due part-way; a reserved counter dropped by a state
(given back); a message a state cuts part-way (its counter used, which a client
counting only completed messages misses); the saved stat counts the boot's
saves, not the session's. `run.sh` also checks that `mgds_t` keeps its size
with `MGDS_DATA=1`. With `MGDS_STALL=1` (with and without `MGDS_DATA`), the whole
suite again plus the stall rules: the clock idle with nothing owed (10 min),
then counting from when owing starts; an owed hello's answer, credentials
part-way and an owed sealed answer each end the session and then the link at
35 s (a VM session that was open reported closed first); a client frame
restarts the clock, any other client write (`mgds_client_frame`) too; an update
part-way starts no 35 s clock and ends with the link after 2 min with no
session frame, its storage back, and one discarded for want of storage the
same; a fragment restarts the 2 min, another client write doesn't; fragments
90 s apart still complete and save; a connected sleep past 35 s or 2 min with
`mgds_woke()` at its exit is no stall (without it, it is); `end_link` NULL
turns the rules off.
