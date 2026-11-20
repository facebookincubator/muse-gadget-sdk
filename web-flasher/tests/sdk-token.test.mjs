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

import assert from "node:assert/strict";
import test from "node:test";

import { installPacedSerial } from "../paced-serial.js";
import { sendSdkToken } from "../sdk-token.js";

const token = "mgst_" + "A".repeat(43);

test("a stalled paced token write reports the timeout before releasing the port", { timeout: 5000 }, async () => {
  let finishWrite;
  let closed = false;
  class Port {
    constructor() {
      this.readable = new ReadableStream();
      this.serialWritable = new WritableStream({
        write: () => new Promise((resolve) => { finishWrite = resolve; }),
      });
    }
    get writable() { return this.serialWritable; }
    async open() {}
    async setSignals() {}
    async close() {
      assert.equal(this.readable.locked, false);
      assert.equal(this.serialWritable.locked, false);
      assert.equal(this.writable.locked, false);
      closed = true;
    }
  }
  installPacedSerial(Port).set({ chunk: 128, factor: 0 });
  const port = new Port();
  let reportTimeout;
  const reported = new Promise((resolve) => { reportTimeout = resolve; });
  const send = sendSdkToken(port, token, {
    timeoutMs: 1500,
    onWriteTimeout: reportTimeout,
  });
  const rejected = assert.rejects(send, (error) => error.i18n?.key === "token.writeTimeout");
  try {
    const error = await Promise.race([reported, send]);
    assert.equal(error.i18n.key, "token.writeTimeout");
    assert.equal(port.serialWritable.locked, true);
    assert.equal(closed, false);
  } finally {
    finishWrite?.();
  }
  await rejected;
  assert.equal(closed, true);
});

test("an acknowledged token write still succeeds and closes the port", async () => {
  let reader;
  let closed = false;
  const port = {
    readable: new ReadableStream({ start: (controller) => { reader = controller; } }),
    writable: new WritableStream({
      write(bytes) {
        assert.equal(new TextDecoder().decode(bytes), "\n>sdk_token=" + token + "\n");
        reader.enqueue(new TextEncoder().encode("@sdk_token ok\n"));
      },
    }),
    async open() {},
    async setSignals() {},
    async close() { closed = true; },
  };
  await sendSdkToken(port, token);
  assert.equal(closed, true);
  assert.equal(port.readable.locked, false);
  assert.equal(port.writable.locked, false);
});

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
  await sendSdkToken(p, token, { retryMs: 10, openRetryMs: 1 });
  assert.equal(p.calls, 3);
  assert.deepEqual(p.writes, [`\n>sdk_token=${token}\n`]);
  assert.equal(p.closes, 1);
  assert.deepEqual(p.signals, [{ dataTerminalReady: false, requestToSend: false }]);
});

test("does not retry permission or I/O failures or close another owner", async () => {
  for (const name of ["SecurityError", "NetworkError"]) {
    const p = port(1, name);
    await assert.rejects(sendSdkToken(p, token), (error) => error.i18n?.key === "token.openFailed");
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
  await assert.rejects(sendSdkToken(p, token), (error) => error.i18n?.key === "token.openFailed");
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
  await assert.rejects(sendSdkToken(p, token, { openTimeoutMs: 30, openRetryMs: 5 }), (error) => error.i18n?.key === "token.openFailed");
  assert.ok(p.calls > 1);
  assert.ok(Date.now() - started >= 30);
  assert.ok(Date.now() - started < 1000);
  assert.equal(p.closes, 0);
  assert.equal(p.writes.length, 0);
});

test("rejects an invalid token before opening the port", async () => {
  const p = port();
  await assert.rejects(sendSdkToken(p, "invalid"), (error) => error.i18n?.key === "token.invalid");
  assert.equal(p.calls, 0);
});
