# Reply option selection

On PSRAM boards with the LVGL touchscreen UI, Muse's text option questions open
a scrollable picker showing the question and its answers. Tap an answer to send
it, or tap outside the panel to return to the conversation. A text-only **Options** control
appears only when an unanswered question is dismissed; it disappears as soon as
an answer is submitted. A selected answer is retained internally to prevent
repeat submission. Voice recording and calls
temporarily hide the picker.

The question comes from the widget title when supplied, otherwise from Muse's
reply text. “Choose an option” is used only when no question text is available.

On round displays, the panel is scaled so its entire rectangular diagonal fits
within the display diameter minus 20 pixels. This keeps its corners and
scrollbar inside the circular boundary, with a 10-pixel bezel margin. Shadows
are disabled and children clip to the panel's rounded corners.

The first version retains one question, with up to eight choices of at most
191 UTF-8 bytes each. Choice labels and identifiers are never shortened for
submission. Image choices, multiple simultaneous selections, URL actions,
free-text entry, and non-touch/e-ink pickers are outside this version's scope.
Use the Muse app for those cases.

## Protocol and failure behavior

The gadget receives a `kind: "option"` widget in an assistant message's
`widgets` array. Its `data.options` is a string array. Data and state can also
arrive as JSON-encoded strings. The model retains the host message identifier,
widget identifier, labels, selected index, and state version after a turn ends.
`widget.state_updated` events update a matching retained question.

Selecting sends the original option label to `POST /chat/stream`. The request's
`metadata.message_origin` contains `kind: "options_widget"`, `widget_id`,
`message_id`, zero-based `option_index`, and a fresh UUID `interaction_id`.
The reply uses the existing voice/caption pipeline.

After the chat acknowledgement, the gadget attempts `POST /widgets/state` with
`widget_id`, `base_version`, a UUID `mutation_id`, and
`state: {"selectedText": "<original label>"}`. Saving state never resends the
chat answer. A save failure keeps the local answer selected without reopening the picker. USB
diagnostics report whether saving succeeded; check the Muse app for its state. Loss of an ambiguous chat
acknowledgement disables retry rather than risk sending a second answer.

**Current server limitation:** real gadget requests to `/widgets/state` returned
HTTP 403 on November 13, 2026. The chat answer was accepted and Muse correctly
recognized it, but cross-client selected-state persistence was not achieved.
Supplying the SDK's existing default client identity in the WebSocket upgrade
also failed to resolve this; that experimental change was removed. Do not
assume a choice made on the gadget locks the corresponding buttons on the phone.

## Validation, November 13, 2026

| Check | Result |
| --- | --- |
| Option model and JSON adapter under ASan/UBSan | 7 tests passed, including malformed/oversized widgets, Unicode, duplicate taps, stale generations, disconnects, and phone state versions |
| Chat session harness with the real option adapter | 27 tests passed, including idle widget delivery and phone selection events |
| Existing Link chat tests | 13 tests passed |
| Voice/input regression harness | 54 tests passed with a macOS compiler wrapper for Linux-specific flags |
| Simulator under ASan/UBSan | 38 checks passed on the standalone upstream branch, including real LVGL pointer dispatch, scrolling, selection, duplicate rejection, waking touches, circular corner containment at 412/466 px, and unchanged rectangular panel sizing |
| Simulator avatar-selection regression | 3 tests passed; fixture includes the new units and uses Ninja |
| 1.75C application build, ESP-IDF 6.0.3 | Passed; signed binary `0x221000` bytes in a `0x400000`-byte app slot |
| Connected physical 1.75C | Received Red/Green/Blue widget; selected Green through the USB selection API; rejected a second selection; played/displayed the following reply; a verification chat returned `Green` |
| Server selected-state persistence | HTTP 403; local selection remains locked, with no automatic chat retry |

Touch dispatch was exercised in the simulator. The physical-device selection
test used a local build including the separately proposed swipe-volume feature
and the same model request through USB, not a human finger or an automated
physical touchscreen press. Simulator captures are not device screenshots.

The full host suite was exercised (1,104 tests in the first run), but that run
was not clean: sandbox socket restrictions, missing FreeInk configuration,
macOS compiler/linker flags, and the simulator fixture caused failures. Failed
modules were rerun with prerequisites supplied; the relevant reruns above
passed. Five tests skipped in the full run.

The full firmware build also encountered a local bootloader link error involving
an orphan `.tbss` section. The application target built successfully and was
flashed at `0x20000`, preserving the existing bootloader, partition table,
pairing, and settings. A default C5 firmware build was not verified. Some
physical-board startups reported a tunnel-task allocation warning; the final
chat/selection run connected successfully. Those warnings remain outside this
feature's validation claim.

Development caught and fixed two memory regressions: retained snapshots now use
PSRAM instead of consuming audio DMA memory, and USB status copies only scalars
instead of placing an entire question on the small serial-task stack.

## USB diagnostics

On boards supporting serial chat:

```text
>options
>options.select=GENERATION,INDEX
```

The index is zero-based. Copy the generation from the `@options` status response;
a stale generation or second submission is rejected. Status numbers are empty
(0), ready (1), queued (2), sending (3), selected (4), uncertain (5), and stale
(6). `saved` indicates server state confirmation, independently of chat
acceptance. Serial chat emits `@chat` events of type `options` for received
widgets; long JSON strings can span consecutive console events.
