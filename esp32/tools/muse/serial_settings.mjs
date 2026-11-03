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
// Muse's existing USB console protocol. No SDK tokens or firmware writes.
export class MuseSerialError extends Error {
  constructor(code, message) { super(message); this.code = code; }
}

export const SETTINGS = Object.freeze({
  sleep: { min: 0, max: 3600 }, brightness: { min: 10, max: 100 },
  volume: { min: 0, max: 100 }, mic_gain: { min: 0, max: 36 }, speaker: { boolean: true },
});

export function validateSetting(key, value) {
  const rule = Object.hasOwn(SETTINGS, key) ? SETTINGS[key] : null;
  if (!rule) throw new MuseSerialError('KEY', 'This setting is not supported.');
  if (rule.boolean ? typeof value !== 'boolean' :
    !Number.isInteger(value) || value < rule.min || value > rule.max) {
    throw new MuseSerialError('VALUE', 'The value is outside the allowed range.');
  }
  return rule.boolean ? Number(value) : value;
}

export function parseStatus(line) {
  const at = line.indexOf('@status ');
  if (at < 0) return null;
  const status = JSON.parse(line.slice(at + 8));
  if (typeof status.board !== 'string' || !status.device) throw new MuseSerialError('STATUS', 'The device returned an invalid Muse status.');
  for (const key of Object.keys(SETTINGS)) validateSetting(key, status.device[key]);
  return status;
}

export class MuseSerial {
  constructor(port, { timeout = 4000, onDisconnect = () => {} } = {}) {
    this.port = port; this.timeout = timeout; this.onDisconnect = onDisconnect;
    this.online = false; this.waiters = new Set(); this.queue = Promise.resolve();
    this.buffer = ''; this.decoder = new TextDecoder();
  }
  async connect({ readyTimeout = 18000 } = {}) {
    try {
      await this.port.open({ baudRate: 115200, flowControl: 'none' });
      // Same inactive control lines as the repository's SDK-token tool.
      await this.port.setSignals?.({ dataTerminalReady: false, requestToSend: false }).catch(() => {});
      this.reader = this.port.readable.getReader();
      this.writer = this.port.writable.getWriter();
      this.online = true;
      this.reading = this.readLoop();
      const deadline = Date.now() + readyTimeout;
      do {
        try { return await this.query(Math.min(2500, Math.max(1, deadline - Date.now()))); }
        catch (error) { if (error.code !== 'TIMEOUT' || !this.online) throw error; }
      } while (Date.now() < deadline);
      throw new MuseSerialError('CONNECT_TIMEOUT', 'No device response. Close other serial monitors and reconnect USB.');
    } catch (error) { await this.close(); throw error; }
  }
  async readLoop() {
    try {
      while (this.online) {
        const { value, done } = await this.reader.read();
        if (done) break;
        this.feed(value);
      }
      if (this.online) this.fail(new MuseSerialError('DISCONNECTED', 'USB connection lost. Reconnect the device.'));
    } catch (error) { if (this.online) this.fail(error); }
    finally { this.reader.releaseLock(); }
  }
  feed(bytes) {
    this.buffer += this.decoder.decode(bytes, { stream: true });
    let newline;
    while ((newline = this.buffer.indexOf('\n')) >= 0) {
      const line = this.buffer.slice(0, newline).replace(/\x1b\[[0-?]*[ -/]*[@-~]/g, '').replace(/\r$/, '');
      this.buffer = this.buffer.slice(newline + 1);
      for (const waiter of [...this.waiters]) {
        try { const answer = waiter.match(line); if (answer !== null) waiter.resolve(answer); }
        catch (error) { waiter.reject(error); }
      }
    }
    // Boot/log output is not a terminal: retain only a bounded partial line.
    if (this.buffer.length > 65536) this.buffer = '';
  }
  fail(error) {
    this.online = false;
    for (const waiter of [...this.waiters]) waiter.reject(error);
    this.onDisconnect(error);
  }
  enqueue(action) {
    const task = this.queue.then(() => {
      if (!this.online) throw new MuseSerialError('NOT_CONNECTED', 'Connect a USB device first.');
      return action();
    });
    this.queue = task.catch(() => {});
    return task;
  }
  async request(command, match, timeout = this.timeout) {
    let waiter;
    const response = new Promise((resolve, reject) => {
      const finish = (fn, result) => { clearTimeout(waiter.timer); this.waiters.delete(waiter); fn(result); };
      waiter = { match, resolve: v => finish(resolve, v), reject: e => finish(reject, e) };
      waiter.timer = setTimeout(() => {
        const error = new MuseSerialError('TIMEOUT', 'No response was confirmed. Reload the device values.');
        waiter.reject(error);
      }, timeout);
      this.waiters.add(waiter);
    });
    // Install the handler before writing: an immediate ACK must not be lost.
    try { void this.writer.write(new TextEncoder().encode(`\n>${command}\n`)).catch(error => waiter.reject(error)); }
    catch (error) { waiter.reject(error); }
    return response;
  }
  query(timeout) { return this.enqueue(() => this.queryNow(timeout)); }
  queryNow(timeout) { return this.request('status', parseStatus, timeout); }
  set(key, value) {
    const wire = validateSetting(key, value);
    return this.enqueue(async () => {
      const command = `${key}=${wire}`;
      // The firmware acknowledges a setting only in its INFO log line
      // ("cmd key=value -> ok"); the status readback below confirms the write.
      await this.request(command, line => {
        const at = line.indexOf(`cmd ${command} -> `);
        if (at < 0) return null;
        const result = line.slice(at + `cmd ${command} -> `.length).trim();
        if (result !== 'ok') throw new MuseSerialError('DEVICE_REJECTED', 'The device rejected this setting.');
        return true;
      });
      const status = await this.queryNow();
      if (status.device[key] !== value) throw new MuseSerialError('READBACK', 'The readback did not match. Reload the device values.');
      return status;
    });
  }
  wake() {
    return this.enqueue(async () => {
      // Bound the write like request(): a stalled port must not leave the page busy.
      let timer;
      const stalled = new Promise((_, reject) => {
        timer = setTimeout(() => reject(new MuseSerialError('TIMEOUT', 'No response was confirmed. Reload the device values.')), this.timeout);
      });
      try { await Promise.race([this.writer.write(new TextEncoder().encode('w')), stalled]); }
      finally { clearTimeout(timer); }
      return this.queryNow();
    });
  }
  async close() {
    this.online = false;
    for (const waiter of [...this.waiters]) waiter.reject(new MuseSerialError('CLOSED', 'USB connection closed.'));
    await this.reader?.cancel().catch(() => {});
    await this.reading;
    this.writer?.releaseLock();
    await this.port.close().catch(() => {});
  }
}
