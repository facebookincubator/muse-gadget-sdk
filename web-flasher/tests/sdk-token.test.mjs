// Copyright (c) Meta Platforms, Inc. and affiliates.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

// Import the unchanged browser module without adding a Node package manifest.
const source = await readFile(new URL("../sdk-token.js", import.meta.url), "utf8");
const { sendSdkToken } = await import(
  "data:text/javascript;base64," + Buffer.from(source).toString("base64")
);
const token = "mgst_" + "A".repeat(43);

function port(failures = 0, errorName = "InvalidStateError") {
  let controller;
  const p = {
    calls: 0, writes: [], signals: [], closes: 0,
    readable: null, writable: null,
    async open() {
      this.calls++;
      if (this.calls <= failures) {
        throw new DOMException("The port is already open.", errorName);
      }
      this.readable = new ReadableStream({ start(c) { controller = c; } });
      this.writable = new WritableStream({
        write(value) {
          p.writes.push(new TextDecoder().decode(value));
          controller.enqueue(new TextEncoder().encode("@sdk_token ok\n"));
        },
      });
    },
    async setSignals(signals) { this.signals.push(signals); },
    async close() {
      this.closes++;
      this.readable = this.writable = null;
    },
  };
  return p;
}

test("waits for closing to finish even when both streams are already null", async () => {
  const p = port(2);
  await sendSdkToken(p, token, { retryMs: 10 });
  assert.equal(p.calls, 3);
  assert.deepEqual(p.writes, [`\n>sdk_token=${token}\n`]);
  assert.equal(p.closes, 1);
  assert.deepEqual(p.signals, [{ dataTerminalReady: false, requestToSend: false }]);
});

test("does not retry permission or I/O failures or close another owner", async () => {
  for (const name of ["SecurityError", "NetworkError"]) {
    const p = port(1, name);
    await assert.rejects(sendSdkToken(p, token), /Close the installer/);
    assert.equal(p.calls, 1);
    assert.equal(p.closes, 0);
    assert.equal(p.writes.length, 0);
  }
});

test("does not wait for an already open port owned by another send", async () => {
  const p = port();
  await p.open();
  p.open = async () => {
    p.calls++;
    throw new DOMException("The port is already open.", "InvalidStateError");
  };
  await assert.rejects(sendSdkToken(p, token), /already open/);
  assert.equal(p.calls, 2);
  assert.equal(p.closes, 0);
  assert.equal(p.writes.length, 0);
  assert.ok(p.readable);
  assert.ok(p.writable);
  await p.readable.cancel();
  await p.writable.close();
});

test("fails within a bounded deadline when the port stays busy", async () => {
  const p = port(Infinity);
  const started = Date.now();
  await assert.rejects(sendSdkToken(p, token), /already open/);
  assert.ok(Date.now() - started >= 3000);
  assert.ok(Date.now() - started < 4500);
  assert.equal(p.closes, 0);
  assert.equal(p.writes.length, 0);
});

test("rejects an invalid token before opening the port", async () => {
  const p = port();
  await assert.rejects(sendSdkToken(p, "invalid"), /isn't an SDK token/);
  assert.equal(p.calls, 0);
});
