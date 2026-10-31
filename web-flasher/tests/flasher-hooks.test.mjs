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
import { readFile } from "node:fs/promises";
import test from "node:test";

// Import the browser module without changing the site's module configuration.
const source = await readFile(new URL("../flasher-hooks.js", import.meta.url), "utf8");
const { installFlasherHooks } = await import(
  `data:text/javascript;base64,${Buffer.from(source).toString("base64")}`
);
const MB = 1024 * 1024;

function fixture({ minFlashBytes = 4 * MB, size = MB, failures = 0 } = {}) {
  const calls = { reads: 0, erases: [], writes: [], installed: [] };
  const port = {};
  Object.defineProperty(globalThis, "navigator", {
    configurable: true,
    value: { serial: { requestPort: async () => port } },
  });
  globalThis.window = {};
  installFlasherHooks({
    getSelection: () => ({ usb: null, minFlashBytes }),
    onInstalled: (installedPort) => calls.installed.push(installedPort),
  });
  const loader = {
    DETECTED_FLASH_SIZES: { 0x14: "1MB" },
    async readFlashId() {
      calls.reads++;
      if (calls.reads <= failures) throw new Error("chip ID read failed");
      return 0x140000;
    },
    flashSizeBytes: () => size,
    async eraseFlash(...args) { calls.erases.push(args); },
    async writeFlash(options) { calls.writes.push(options); },
  };
  window.esploader = loader;
  return { calls, loader, port };
}

for (const retry of ["eraseFlash", "writeFlash"]) {
  test(`failed erase capacity read is retried before ${retry}`, async () => {
    const { calls, loader } = fixture({ failures: 1 });
    await assert.rejects(loader.eraseFlash(), /chip ID read failed/);
    if (retry === "writeFlash") {
      await assert.rejects(loader.writeFlash({}), /needs 4 MB.*has 1 MB/);
    } else {
      await loader.eraseFlash();
    }
    assert.equal(calls.reads, 2);
    assert.deepEqual(calls.erases, []);
    assert.deepEqual(calls.writes, []);
    assert.deepEqual(calls.installed, []);
  });
}

test("repeated read failures never allow an erase or write", async () => {
  const { calls, loader } = fixture({ failures: Infinity });
  await assert.rejects(loader.eraseFlash(), /chip ID read failed/);
  await assert.rejects(loader.writeFlash({}), /chip ID read failed/);
  assert.equal(calls.reads, 2);
  assert.deepEqual(calls.erases, []);
  assert.deepEqual(calls.writes, []);
});

test("a failed write capacity read can recover on a sufficiently large board", async () => {
  const { calls, loader } = fixture({ failures: 1, size: 4 * MB });
  await assert.rejects(loader.writeFlash({}), /chip ID read failed/);
  const options = { fileArray: [] };
  await loader.writeFlash(options);
  assert.equal(calls.reads, 2);
  assert.deepEqual(calls.writes, [options]);
});

test("successful capacity checks are cached and operations retain their arguments", async () => {
  const { calls, loader, port } = fixture({ size: 4 * MB });
  await navigator.serial.requestPort();
  await loader.eraseFlash("erase argument");
  const options = { fileArray: [] };
  await loader.writeFlash(options);
  assert.equal(calls.reads, 1);
  assert.deepEqual(calls.erases, [["erase argument"]]);
  assert.deepEqual(calls.writes, [options]);
  assert.deepEqual(calls.installed, [port]);
});

test("a known undersized board stays blocked after erase is skipped", async () => {
  const { calls, loader } = fixture();
  await loader.eraseFlash();
  await assert.rejects(loader.writeFlash({}), /needs 4 MB.*has 1 MB/);
  assert.equal(calls.reads, 1);
  assert.deepEqual(calls.erases, []);
  assert.deepEqual(calls.writes, []);
});

test("unknown capacity retains the existing allow behavior", async () => {
  const { calls, loader } = fixture({ size: 0 });
  await loader.eraseFlash();
  await loader.writeFlash({});
  assert.equal(calls.reads, 1);
  assert.equal(calls.erases.length, 1);
  assert.equal(calls.writes.length, 1);
});

test("a build without a minimum capacity needs no chip ID read", async () => {
  const { calls, loader } = fixture({ minFlashBytes: 0 });
  await loader.eraseFlash();
  await loader.writeFlash({});
  assert.equal(calls.reads, 0);
  assert.equal(calls.erases.length, 1);
  assert.equal(calls.writes.length, 1);
});
