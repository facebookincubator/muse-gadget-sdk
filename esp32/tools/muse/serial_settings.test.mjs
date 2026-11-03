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
import test from 'node:test';
import assert from 'node:assert/strict';
import { MuseSerial, parseStatus } from './serial_settings.mjs';

const initial = () => ({ board: 'SpotPear ESP32-AI-1.28 v1.0', device: {
  name: 'MuseGadget-test', fw: 'test', sleep: 0, brightness: 80, volume: 70, mic_gain: 30, speaker: true,
} });
class Port {
  constructor() {
    this.state = initial(); this.commands = []; this.reads = 0;
    this.readable = new ReadableStream({ start: c => { this.controller = c; } });
    this.writable = new WritableStream({ write: bytes => {
      const command = new TextDecoder().decode(bytes).trim(); this.commands.push(command);
      if (this.onWrite) return this.onWrite(command);
      if (command === '>status') this.status();
      else if (command.startsWith('>')) {
        const [key, raw] = command.slice(1).split('=');
        this.state.device[key] = key === 'speaker' ? raw === '1' : Number(raw);
        this.emit(`\x1b[32mI (12) muse_ble: cmd ${key}=${raw} -> ok\x1b[0m\r\n`);
      }
    } });
  }
  async open(options) { this.options = options; }
  async setSignals(signals) { this.signals = signals; }
  async close() { this.closed = true; assert.equal(this.readable.locked, false); assert.equal(this.writable.locked, false); }
  emit(text) {
    // Split every byte, including UTF-8 and the JSON/status prefix.
    for (const byte of new TextEncoder().encode(text)) this.controller.enqueue(Uint8Array.of(byte));
  }
  status() { this.reads++; this.emit(`ordinary log\n@status ${JSON.stringify(this.state)}\r\n`); }
}
async function connect(port = new Port(), options = {}) {
  const session = new MuseSerial(port, { timeout: 50, ...options });
  await session.connect({ readyTimeout: 100 }); return { port, session };
}

test('fragmented UTF-8 status is read without sending setting writes', async t => {
  const port = new Port(); port.state.board = '日本語のMuse';
  const { session } = await connect(port); t.after(() => session.close());
  assert.equal((await session.query()).board, '日本語のMuse');
  assert.deepEqual(port.commands, ['>status', '>status']);
  assert.equal(port.options.baudRate, 115200);
});
test('write waits for ACK and verifies actual value, including zero and false', async t => {
  const { port, session } = await connect(); t.after(() => session.close());
  assert.equal((await session.set('sleep', 0)).device.sleep, 0);
  assert.equal((await session.set('speaker', false)).device.speaker, false);
  assert.deepEqual(port.commands, ['>status', '>sleep=0', '>status', '>speaker=0', '>status']);
});
test('parallel changes are serialized as write/readback pairs', async t => {
  const { port, session } = await connect(); t.after(() => session.close());
  await Promise.all([session.set('brightness', 55), session.set('volume', 42)]);
  assert.deepEqual(port.commands.slice(1), ['>brightness=55', '>status', '>volume=42', '>status']);
});
test('out-of-range values and unknown commands never reach serial', async t => {
  const { port, session } = await connect(); t.after(() => session.close());
  for (const [key, value] of [['sleep', -1], ['sleep', 3601], ['sleep', NaN], ['brightness', 0], ['mic_gain', 37], ['speaker', 1], ['wifi.forget', 1], ['__proto__', 0], ['constructor', 0]]) {
    assert.throws(() => session.set(key, value));
  }
  assert.deepEqual(port.commands, ['>status']);
});
test('device rejection is not reported as saved', async t => {
  const { port, session } = await connect(); t.after(() => session.close());
  port.onWrite = () => port.emit('muse_ble: cmd volume=20 -> error: bad value\n');
  await assert.rejects(session.set('volume', 20), { code: 'DEVICE_REJECTED' });
});
test('ACK with mismatched readback is an error', async t => {
  const { port, session } = await connect(); t.after(() => session.close());
  port.onWrite = command => command === '>status' ? port.status() : port.emit('muse_ble: cmd volume=20 -> ok\n');
  await assert.rejects(session.set('volume', 20), { code: 'READBACK' });
});
test('irrelevant ACKs do not satisfy a command; timeout leaves no waiter', async t => {
  const { port, session } = await connect(); t.after(() => session.close());
  port.onWrite = () => port.emit('muse_ble: cmd sleep=0 -> ok\n');
  await assert.rejects(session.set('volume', 20), { code: 'TIMEOUT' });
  assert.equal(session.waiters.size, 0);
});
test('timeout also works when the underlying write stalls', async t => {
  const { port, session } = await connect();
  let finish;
  port.onWrite = () => new Promise(resolve => { finish = resolve; });
  await assert.rejects(session.query(15), { code: 'TIMEOUT' });
  finish(); await session.close(); assert.ok(port.closed);
});
test('unplug rejects active requests and releases locks', async () => {
  let disconnected = false;
  const { port, session } = await connect(new Port(), { onDisconnect: () => { disconnected = true; } });
  port.onWrite = () => port.controller.close();
  await assert.rejects(session.query(), { code: 'DISCONNECTED' });
  await session.close(); assert.ok(disconnected); assert.ok(port.closed);
});
test('wake uses only the wake character and read-only status', async t => {
  const { port, session } = await connect(); t.after(() => session.close());
  await session.wake(); assert.deepEqual(port.commands.slice(1), ['w', '>status']);
});
test('wake times out when the underlying write stalls', async () => {
  const { port, session } = await connect();
  let finish;
  port.onWrite = () => new Promise(resolve => { finish = resolve; });
  await assert.rejects(session.wake(), { code: 'TIMEOUT' });
  finish(); await session.close(); assert.ok(port.closed);
});
test('missing fields, malformed JSON, and unsupported device status fail', () => {
  assert.equal(parseStatus('some log'), null);
  assert.throws(() => parseStatus('@status {bad}'));
  assert.throws(() => parseStatus('@status {"board":"other","device":{}}'));
});
test('connect timeout closes the port and never changes settings', async () => {
  const port = new Port(); port.onWrite = () => {};
  const session = new MuseSerial(port);
  await assert.rejects(session.connect({ readyTimeout: 20 }), { code: 'CONNECT_TIMEOUT' });
  assert.ok(port.closed); assert.deepEqual(port.commands, ['>status']);
});
