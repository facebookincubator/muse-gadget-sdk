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

// Reads a Muse board's settings over its USB console and changes five of
// them, for the "Device settings" card. It speaks the console's existing
// commands (esp32/components/muse/muse_input.c, muse_ble.c): ">status"
// answers "@status {json}", and ">key=value" logs "cmd key=value -> ok".
// Only boards with the full Muse UI have that console.
//
// It sends nothing but ">status" and the five settings below, each checked
// here first: never Wi-Fi, pairing or token commands (the SDK token goes
// through sdk-token.js).

import { LocalizedError, hasString, t } from "./i18n.js";

// `code` says what went wrong; the message is "error.<code>" in i18n.js.
export class DeviceSettingsError extends LocalizedError {
  constructor(code, params = {}) {
    super(`error.${code}`, params);
    this.code = code;
  }
}

// The firmware's own ranges (muse_ble.c run_command(); MUSE_MIC_GAIN_MAX).
export const SETTINGS = Object.freeze({
  brightness: Object.freeze({ min: 10, max: 100 }),
  sleep: Object.freeze({ min: 0, max: 3600 }),
  volume: Object.freeze({ min: 0, max: 100 }),
  speaker: Object.freeze({ boolean: true }),
  mic_gain: Object.freeze({ min: 0, max: 36 }),
});

// Returns the value as the console takes it, or throws.
export function validateSetting(key, value) {
  const rule = Object.hasOwn(SETTINGS, key) ? SETTINGS[key] : null;
  if (!rule) throw new DeviceSettingsError("KEY");
  const ok = rule.boolean
    ? typeof value === "boolean"
    : Number.isInteger(value) && value >= rule.min && value <= rule.max;
  if (!ok) throw new DeviceSettingsError("VALUE");
  return rule.boolean ? Number(value) : value;
}

// One console line -> the status object, or null for any other line.
export function parseStatus(line) {
  const at = line.indexOf("@status ");
  if (at < 0) return null;
  let status;
  try {
    status = JSON.parse(line.slice(at + 8));
  } catch {
    status = null;
  }
  if (typeof status?.board !== "string" || !status.device || typeof status.device !== "object") {
    throw new DeviceSettingsError("STATUS");
  }
  for (const key of Object.keys(SETTINGS)) {
    try {
      validateSetting(key, status.device[key]);
    } catch {
      throw new DeviceSettingsError("STATUS");
    }
  }
  return status;
}

// Status JSON -> [{ path, value }] for every leaf, false, 0, null and empty
// containers included, so nothing the gadget reports is hidden. Anything
// nested deeper than the firmware's three levels shows as "…".
const MAX_DEPTH = 6;
export function flattenStatus(value, path = "", depth = 0) {
  if (value !== null && typeof value === "object" && Object.keys(value).length) {
    if (depth >= MAX_DEPTH) return [{ path, value: "…" }];
    return Object.entries(value).flatMap(([key, child]) =>
      flattenStatus(child, Array.isArray(value) ? `${path}[${key}]` : path ? `${path}.${key}` : key, depth + 1),
    );
  }
  return [{ path, value }];
}

// Labels for the status fields the firmware sends today are
// "status.<path>" in i18n.js. The "hatch" key is the firmware's
// compatibility name for the Muse connection.

// Rows for the status table, as plain strings (the page sets them as text).
export function statusRows(status) {
  return flattenStatus(status).map(({ path, value }) => [
    hasString(`status.${path}`) ? t(`status.${path}`) : path,
    typeof value === "string" ? value : JSON.stringify(value),
  ]);
}

const ESCAPES = /\x1b\[[0-?]*[ -/]*[@-~]/g;   // the log's colours
const MAX_LINE = 65536;

// One open console. Requests run one at a time; every write waits for its
// answer, or fails after `timeout` ms.
export class DeviceSettingsSession {
  constructor(port, { timeout = 4000, onDisconnect = () => {}, onCloseStuck = () => {} } = {}) {
    this.port = port;
    this.timeout = timeout;
    this.onDisconnect = onDisconnect;
    this.onCloseStuck = onCloseStuck;
    this.online = false;
    this.waiters = new Set();
    this.queue = Promise.resolve();
    this.buffer = "";
    this.decoder = new TextDecoder();
    this.closing = false;
    this.opening = null;   // port.open() while it runs
    this.writing = Promise.resolve();   // the last write
  }

  // Opens the port and reads the status. Opening can restart the gadget, so
  // ">status" goes again until it answers, for up to `readyTimeout` ms.
  async connect({ readyTimeout = 18000 } = {}) {
    try {
      this.opening = this.port.open({ baudRate: 115200 });
      try {
        await this.opening;
      } catch (e) {
        this.opening = null;
        throw new DeviceSettingsError("OPEN", { error: e.message });
      }
      // Closed while opening (the board changed, an install started): give
      // the port back rather than keep a session nobody holds.
      if (this.closing) throw new DeviceSettingsError("CLOSED");
      // Leave EN and BOOT alone on boards whose bridge drives them, as sdk-token.js does.
      await this.port.setSignals?.({ dataTerminalReady: false, requestToSend: false }).catch(() => {});
      // Closed meanwhile: close() has gone past the streams, so don't take
      // them. From here to readLoop() nothing waits, so close() sees them.
      if (this.closing) throw new DeviceSettingsError("CLOSED");
      this.reader = this.port.readable.getReader();
      this.writer = this.port.writable.getWriter();
      this.online = true;
      this.reading = this.readLoop();
      const deadline = Date.now() + readyTimeout;
      do {
        try {
          return await this.query(Math.min(2500, Math.max(1, deadline - Date.now())));
        } catch (error) {
          if (error.code !== "TIMEOUT" || !this.online) throw error;
        }
      } while (Date.now() < deadline);
      throw new DeviceSettingsError("CONNECT_TIMEOUT");
    } catch (error) {
      await this.close().catch(() => {});
      throw error;
    }
  }

  async readLoop() {
    try {
      while (this.online) {
        const { value, done } = await this.reader.read();
        if (done) break;
        this.feed(value);
      }
      if (this.online) this.fail(new DeviceSettingsError("DISCONNECTED"));
    } catch (error) {
      if (this.online) this.fail(error);
    } finally {
      this.reader.releaseLock();
    }
  }

  feed(bytes) {
    this.buffer += this.decoder.decode(bytes, { stream: true });
    let nl;
    while ((nl = this.buffer.indexOf("\n")) >= 0) {
      const line = this.buffer.slice(0, nl).replace(ESCAPES, "").replace(/\r$/, "");
      this.buffer = this.buffer.slice(nl + 1);
      for (const waiter of [...this.waiters]) {
        try {
          const answer = waiter.match(line);
          if (answer !== null) waiter.resolve(answer);
        } catch (error) {
          waiter.reject(error);
        }
      }
    }
    // Keep only a bounded partial line of boot or log output.
    if (this.buffer.length > MAX_LINE) this.buffer = "";
  }

  fail(error) {
    this.online = false;
    for (const waiter of [...this.waiters]) waiter.reject(error);
    this.onDisconnect(error);
  }

  enqueue(action) {
    const task = this.queue.then(() => {
      if (!this.online) throw new DeviceSettingsError("NOT_CONNECTED");
      return action();
    });
    this.queue = task.catch(() => {});
    return task;
  }

  // Writes ">command" and resolves with the first line `match` accepts. The
  // timeout covers the write too: a stalled port mustn't leave the page busy.
  request(command, match, timeout = this.timeout) {
    let waiter;
    const response = new Promise((resolve, reject) => {
      const finish = (fn, result) => {
        clearTimeout(waiter.timer);
        this.waiters.delete(waiter);
        fn(result);
      };
      waiter = { match, resolve: (v) => finish(resolve, v), reject: (e) => finish(reject, e) };
      waiter.timer = setTimeout(
        () => waiter.reject(new DeviceSettingsError("TIMEOUT")),
        timeout,
      );
      this.waiters.add(waiter);
    });
    // Listen before writing, so an immediate answer isn't missed.
    const write = this.writer.write(new TextEncoder().encode(`\n>${command}\n`));
    write.catch((e) => waiter.reject(e));
    this.writing = write.catch(() => {});
    return response;
  }

  query(timeout) {
    return this.enqueue(() => this.request("status", parseStatus, timeout));
  }

  // Changes one setting, then reads the status back to check it took.
  set(key, value) {
    const wire = validateSetting(key, value);
    return this.enqueue(async () => {
      const command = `${key}=${wire}`;
      const prefix = `cmd ${command} -> `;
      // The console answers a setting only in its INFO log line; a quieter log
      // level times out here, never reports a false success.
      await this.request(command, (line) => {
        const at = line.indexOf(prefix);
        if (at < 0) return null;
        if (line.slice(at + prefix.length).trim() !== "ok") {
          throw new DeviceSettingsError("REJECTED");
        }
        return true;
      });
      const status = await this.request("status", parseStatus);
      if (status.device[key] !== value) {
        throw new DeviceSettingsError("READBACK");
      }
      return status;
    });
  }

  // Gives the port back. A write still under way can keep it open: then
  // onCloseStuck(error) says so and the port closes once that write ends
  // (unplugging ends it). Throws if it stays open, so the page can say so.
  async close() {
    if (this.closed) return this.closed;
    this.closing = true;
    this.online = false;
    for (const waiter of [...this.waiters]) {
      waiter.reject(new DeviceSettingsError("CLOSED"));
    }
    this.closed = (async () => {
      // A port still opening is closed once it has opened.
      const opened = await this.opening?.then(() => true, () => false);
      if (!opened) return;
      await this.reader?.cancel().catch(() => {});
      await this.reading;
      // Let the last write finish: paced-serial.js holds the port's own
      // writer through each write, and the port can't close while it's held.
      let timer;
      await Promise.race([this.writing, new Promise((resolve) => { timer = setTimeout(resolve, this.timeout); })]);
      clearTimeout(timer);
      this.writer?.releaseLock();
      try {
        await this.port.close();
        return;
      } catch (e) {
        this.onCloseStuck(new DeviceSettingsError("CLOSE", { error: e.message }));
      }
      // paced-serial.js still holds the port's own writer: try again once
      // the write has ended, rather than leave the port open.
      await this.writing;
      try {
        await this.port.close();
      } catch (e) {
        throw new DeviceSettingsError("CLOSE", { error: e.message });
      }
    })();
    return this.closed;
  }
}
