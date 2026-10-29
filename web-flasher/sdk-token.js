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

// Sends the maker's SDK token to a flashed gadget over Web Serial. The web
// flasher's builds have no token of their own, and a gadget needs one to pair.
// The gadget stores it in NVS and restarts with it (esp32/main/sdk_token_console.h).

// The same check as esp32/cmake/validate_config.cmake: "mgst_", then base64url
// of 32 bytes, whose last character carries only four bits.
export const isSdkToken = (token) => /^mgst_[A-Za-z0-9_-]{42}[AEIMQUYcgkosw048]$/.test(token);

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// Resolves when the gadget has stored the token; rejects with the reason
// otherwise. Opening the port can restart the gadget, so the command is sent
// again until it answers, for up to `timeoutMs`.
export async function sendSdkToken(port, token, { timeoutMs = 25000, retryMs = 3000 } = {}) {
  if (!isSdkToken(token)) throw new Error("That isn't an SDK token. Copy it again from gadgets.muse.ai.");
  try {
    await port.open({ baudRate: 115200 });
  } catch (e) {
    throw new Error(`Couldn't open the port (${e.message}). Close the installer's dialog first.`);
  }
  // Leave EN and BOOT alone on boards whose bridge drives them.
  await port.setSignals({ dataTerminalReady: false, requestToSend: false }).catch(() => {});

  const reader = port.readable.getReader();
  let answer = null;
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
        const m = line.match(/@sdk_token (ok|error \S+)/);
        if (m) answer = m[1];
      }
    }
  })().catch(() => {});

  try {
    const deadline = Date.now() + timeoutMs;
    await sleep(1000);   // a gadget restarted by the port opening boots first
    while (!answer && Date.now() < deadline) {
      const writer = port.writable.getWriter();
      try {
        await writer.write(new TextEncoder().encode(`\n>sdk_token=${token}\n`));
      } finally {
        writer.releaseLock();
      }
      const wait = Date.now() + retryMs;
      while (!answer && Date.now() < wait) await sleep(100);
    }
  } finally {
    await reader.cancel().catch(() => {});
    await reading;
    reader.releaseLock();
    await port.close().catch(() => {});
  }

  if (answer === "ok") return;
  if (answer === "error invalid") throw new Error("The gadget says that isn't an SDK token.");
  if (answer) throw new Error(`The gadget couldn't store the token (${answer}).`);
  throw new Error(
    "The gadget didn't answer. Check it runs firmware from this page and you picked its console port. " +
      "If it still doesn't answer, unplug it, plug it back in and try again.",
  );
}
