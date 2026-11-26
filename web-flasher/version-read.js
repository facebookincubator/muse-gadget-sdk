// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Reads a gadget's firmware version over Web Serial at 115200: it asks with
// the console's ">version" (answered "@version X") and also takes the boot
// banner's "Version: X", whichever comes first. Writes go through
// port.writable, so a board with paced writes (the Watcher's CH342,
// paced-serial.js) gets them paced.

import { parseVersionLine } from "./flasher-core.js";
import { LocalizedError } from "./i18n.js";

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// Resolves to { version, source } or null if the gadget didn't say within
// timeoutMs (or `signal` aborted). open: open the port first (and close it
// after), with EN and BOOT left alone as sdk-token.js does; otherwise the
// port is already open and stays open. onLine(line) sees every console line.
// A write still pending at the deadline is a stalled port: onWriteTimeout(error)
// hears it at once, and the call throws that error once the write settles.
// Opening retries an InvalidStateError for up to openTimeoutMs while the port
// is still closing (both streams gone), as sdk-token.js does.
export async function readGadgetVersion(
  port,
  {
    open = true, timeoutMs = 20000, askEveryMs = 2000, firstAskMs = 300, openTimeoutMs = 3000, openRetryMs = 100,
    signal, onLine, onWriteTimeout,
  } = {},
) {
  if (open) {
    // ESP Web Tools starts an asynchronous close when its dialog closes. Web
    // Serial can clear both streams before that close finishes, so open may
    // still report InvalidStateError. Never close a port owned by another reader.
    const openingDeadline = Date.now() + openTimeoutMs;
    for (;;) {
      try {
        await port.open({ baudRate: 115200 });
        break;
      } catch (e) {
        const remaining = openingDeadline - Date.now();
        if (e.name === "InvalidStateError" && !port.readable && !port.writable && remaining > 0 && !signal?.aborted) {
          await sleep(Math.min(openRetryMs, remaining));
          continue;
        }
        throw new Error(`Couldn't open the port (${e.message}).`);
      }
    }
    await port.setSignals({ dataTerminalReady: false, requestToSend: false }).catch(() => {});
  }
  let found = null;
  let reader;
  try {
    reader = port.readable.getReader();
  } catch (e) {
    if (open) await port.close().catch(() => {});
    throw new Error(`Couldn't read the port (${e.message}).`);
  }
  const onAbort = () => reader.cancel().catch(() => {});
  signal?.addEventListener("abort", onAbort);

  const reading = (async () => {
    const decoder = new TextDecoder();
    let text = "";
    for (;;) {
      const { value, done } = await reader.read();
      if (done) return;
      text += decoder.decode(value, { stream: true });
      let nl;
      while ((nl = text.indexOf("\n")) >= 0) {
        const line = text.slice(0, nl).replace(/\r$/, "");
        text = text.slice(nl + 1);
        onLine?.(line);
        found ??= parseVersionLine(line);
      }
      if (text.length > 4096) text = text.slice(-4096);   // no newline: a stuck stream
    }
  })().catch(() => {});

  try {
    const deadline = Date.now() + timeoutMs;
    let ask = Date.now() + firstAskMs;
    while (!found && !signal?.aborted && Date.now() < deadline) {
      if (Date.now() >= ask) {
        const writer = port.writable.getWriter();
        let timer;
        let stop;
        let write;
        try {
          write = writer.write(new TextEncoder().encode("\n>version\n"));
          const finished = await Promise.race([
            write.then(() => true),
            new Promise((resolve) => {
              timer = setTimeout(() => resolve(false), Math.max(0, deadline - Date.now()));
              stop = () => resolve(false);
              signal?.addEventListener("abort", stop);
            }),
          ]);
          if (!finished && !signal?.aborted) {
            const error = new LocalizedError("version.writeTimeout");
            onWriteTimeout?.(error);
            throw error;
          }
        } finally {
          clearTimeout(timer);
          signal?.removeEventListener("abort", stop);
          // A paced write can still hold the underlying serial writer. Keep
          // the send pending until it settles so the page keeps the port reserved.
          await write?.catch(() => {});
          writer.releaseLock();
        }
        ask = Date.now() + askEveryMs;
      }
      await sleep(50);
    }
  } catch (e) {
    if (e instanceof LocalizedError) throw e;   // a stalled port: the caller says so
    // A write that failed (the gadget went away): what was read stands.
  } finally {
    signal?.removeEventListener("abort", onAbort);
    await reader.cancel().catch(() => {});
    await reading;
    reader.releaseLock();
    if (open) await port.close().catch(() => {});
  }
  return found;
}
