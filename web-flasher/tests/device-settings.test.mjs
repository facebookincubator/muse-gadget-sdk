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

// The browser modules, as they are (Node 22 loads them as ES modules).
const load = (name) => import(new URL(`../${name}`, import.meta.url));
const { DeviceSettingsSession, SETTINGS, flattenStatus, parseStatus, statusRows, validateSetting } =
  await load("device-settings.js");
const { installPacedSerial } = await load("paced-serial.js");
const html = await readFile(new URL("../index.html", import.meta.url), "utf8");

const initial = () => ({
  board: "M5Stack StickS3",
  chat: true,
  device: { name: "MuseGadget-test", fw: "test", sleep: 0, brightness: 80, volume: 70, mic_gain: 30, speaker: true },
});

// A gadget's console: answers ">status" and ">key=value" the way the firmware does.
class Port {
  constructor() {
    this.state = initial();
    this.commands = [];
    this.chunks = [];
    this.pending = "";
    this._readable = new ReadableStream({ start: (c) => { this.controller = c; } });
    this._writable = new WritableStream({
      write: (bytes) => {
        this.chunks.push(bytes.length);
        this.pending += new TextDecoder().decode(bytes);
        let nl;
        let result;
        while ((nl = this.pending.indexOf("\n")) >= 0) {
          const command = this.pending.slice(0, nl).trim();
          this.pending = this.pending.slice(nl + 1);
          if (command) result = this.command(command);
        }
        return result;
      },
    });
  }
  get readable() { return this._readable; }
  get writable() { return this._writable; }
  command(command) {
    this.commands.push(command);
    if (this.onWrite) return this.onWrite(command);
    if (command === ">status") return this.status();
    const [key, raw] = command.slice(1).split("=");
    this.state.device[key] = key === "speaker" ? raw === "1" : Number(raw);
    this.emit(`\x1b[0;32mI (12) muse_ble: cmd ${key}=${raw} -> ok\x1b[0m\r\n`);
  }
  async open(options) { this.options = options; }
  async setSignals(signals) { this.signals = signals; }
  async close() {
    // Web Serial refuses to close a port whose streams are locked.
    if (this._readable.locked || this._writable.locked) throw new TypeError("locked stream");
    this.closed = true;
  }
  emit(text) {
    // One byte at a time, splitting UTF-8 and the "@status" prefix.
    for (const byte of new TextEncoder().encode(text)) this.controller.enqueue(Uint8Array.of(byte));
  }
  status() { this.emit(`I (5) other: log line\n@status ${JSON.stringify(this.state)}\r\n`); }
}

async function connect(port = new Port(), options = {}) {
  const session = new DeviceSettingsSession(port, { timeout: 50, ...options });
  await session.connect({ readyTimeout: 100 });
  return { port, session };
}

test("connecting reads the status and changes nothing", async (t) => {
  const port = new Port();
  port.state.board = "日本語のMuse";
  const { session } = await connect(port);
  t.after(() => session.close());
  assert.equal((await session.query()).board, "日本語のMuse");
  assert.deepEqual(port.commands, [">status", ">status"]);
  assert.equal(port.options.baudRate, 115200);
  assert.deepEqual(port.signals, { dataTerminalReady: false, requestToSend: false });
});

test("a change waits for the gadget's ok, then reads it back, zero and false included", async (t) => {
  const { port, session } = await connect();
  t.after(() => session.close());
  assert.equal((await session.set("sleep", 0)).device.sleep, 0);
  assert.equal((await session.set("speaker", false)).device.speaker, false);
  assert.deepEqual(port.commands, [">status", ">sleep=0", ">status", ">speaker=0", ">status"]);
});

test("changes made together go one at a time", async (t) => {
  const { port, session } = await connect();
  t.after(() => session.close());
  await Promise.all([session.set("brightness", 55), session.set("volume", 42)]);
  assert.deepEqual(port.commands.slice(1), [">brightness=55", ">status", ">volume=42", ">status"]);
});

test("only the five settings, in range, ever reach the gadget", async (t) => {
  const { port, session } = await connect();
  t.after(() => session.close());
  const bad = [
    ["sleep", -1], ["sleep", 3601], ["sleep", NaN], ["sleep", 1.5], ["sleep", "10"], ["brightness", 9],
    ["volume", 101], ["mic_gain", 37], ["speaker", 1], ["wifi.forget", 1], ["wifi.pass", "x"],
    ["hatch.token", "x"], ["sdk_token", "x"], ["__proto__", 0], ["constructor", 0], ["toString", 0],
  ];
  for (const [key, value] of bad) assert.throws(() => session.set(key, value), `${key}=${value}`);
  assert.deepEqual(port.commands, [">status"]);
  assert.deepEqual(Object.keys(SETTINGS).sort(), ["brightness", "mic_gain", "sleep", "speaker", "volume"]);
  assert.ok(Object.isFrozen(SETTINGS) && Object.values(SETTINGS).every(Object.isFrozen));
  assert.equal(validateSetting("speaker", true), 1);
});

test("a refused value isn't reported as saved", async (t) => {
  const { port, session } = await connect();
  t.after(() => session.close());
  port.onWrite = () => port.emit("I (9) muse_ble: cmd volume=20 -> error: unknown command or bad value\n");
  await assert.rejects(session.set("volume", 20), { code: "REJECTED" });
});

test("an ok whose readback differs is an error", async (t) => {
  const { port, session } = await connect();
  t.after(() => session.close());
  port.onWrite = (c) => (c === ">status" ? port.status() : port.emit("muse_ble: cmd volume=20 -> ok\n"));
  await assert.rejects(session.set("volume", 20), { code: "READBACK" });
});

test("another setting's ok doesn't count, and a timeout leaves no listener", async (t) => {
  const { port, session } = await connect();
  t.after(() => session.close());
  port.onWrite = () => port.emit("muse_ble: cmd sleep=0 -> ok\n");
  await assert.rejects(session.set("volume", 20), { code: "TIMEOUT" });
  assert.equal(session.waiters.size, 0);
});

test("a stalled write times out, and the port closes while it's still stalled", async () => {
  const { port, session } = await connect();
  let finish;
  port.onWrite = () => new Promise((resolve) => { finish = resolve; });
  await assert.rejects(session.query(15), { code: "TIMEOUT" });
  await session.close();   // Port.close() refuses locked streams
  assert.ok(port.closed);
  finish();
});

test("closing while the port is still opening gives it back and writes nothing", async () => {
  const port = new Port();
  let opened;
  port.open = () => new Promise((resolve) => { opened = resolve; });
  const session = new DeviceSettingsSession(port, { timeout: 50 });
  const connecting = session.connect({ readyTimeout: 100 });
  const closing = session.close();
  opened();
  await assert.rejects(connecting, { code: "CLOSED" });
  await closing;
  assert.ok(port.closed);
  assert.deepEqual(port.commands, []);
  assert.equal(session.online, false);
});

test("closing while the port's signals are being set takes no streams and writes nothing", async () => {
  const port = new Port();
  let signalled;
  port.setSignals = () => new Promise((resolve) => { signalled = resolve; });
  const session = new DeviceSettingsSession(port, { timeout: 50 });
  const connecting = session.connect({ readyTimeout: 100 });
  await new Promise((resolve) => setTimeout(resolve, 5));
  assert.ok(signalled, "connect() is waiting on setSignals()");
  const closing = session.close();
  await new Promise((resolve) => setTimeout(resolve, 5));
  signalled();
  await assert.rejects(connecting, { code: "CLOSED" });
  await closing;
  assert.ok(port.closed);
  assert.ok(!port.readable.locked && !port.writable.locked);
  assert.deepEqual(port.commands, []);
  assert.equal(session.online, false);
});

test("a port that won't close is reported, not hidden", async () => {
  const { port, session } = await connect();
  port.close = async () => { throw new Error("busy"); };
  await assert.rejects(session.close(), { code: "CLOSE" });
});

test("unplugging fails the request in flight and releases the port", async () => {
  let disconnected = false;
  const { port, session } = await connect(new Port(), { onDisconnect: () => { disconnected = true; } });
  port.onWrite = () => port.controller.close();
  await assert.rejects(session.query(), { code: "DISCONNECTED" });
  await session.close();
  assert.ok(disconnected);
  assert.ok(port.closed);
});

test("a board without the console times out, closes the port and changes nothing", async () => {
  const port = new Port();
  port.onWrite = () => {};
  const session = new DeviceSettingsSession(port);
  await assert.rejects(session.connect({ readyTimeout: 20 }), { code: "CONNECT_TIMEOUT" });
  assert.ok(port.closed);
  assert.deepEqual(port.commands, [">status"]);
});

test("bad status lines are rejected", () => {
  assert.equal(parseStatus("some log"), null);
  assert.throws(() => parseStatus("@status {bad}"), { code: "STATUS" });
  assert.throws(() => parseStatus('@status {"board":"x","device":{}}'), { code: "STATUS" });
  assert.throws(() => parseStatus('@status {"board":"x","device":{"sleep":0,"brightness":5,"volume":0,"mic_gain":0,"speaker":true}}'), { code: "STATUS" });
});

test("writes go through the page's paced serial for bridges like the Watcher's", async (t) => {
  // paced-serial.js wraps the class's own writable getter and open(), as on
  // the browser's SerialPort.
  class PacedPort extends Port {
    get writable() { return this._writable; }
    async open(options) { return super.open(options); }
  }
  const pacing = installPacedSerial(PacedPort);
  pacing.set({ chunk: 4, factor: 0 });
  const port = new PacedPort();
  const { session } = await connect(port);
  t.after(() => session.close());
  await session.set("volume", 42);
  assert.deepEqual(port.commands, [">status", ">volume=42", ">status"]);
  assert.ok(port.chunks.length > 3 && port.chunks.every((n) => n <= 4), String(port.chunks));
});

test("closing mid-way through a paced write waits for it, then closes the port", async () => {
  class PacedPort extends Port {
    get writable() { return this._writable; }
    async open(options) { return super.open(options); }
  }
  // One byte at a time, about 2 ms each: a command takes ~25 ms on the wire,
  // during which paced-serial.js holds the port's own writer.
  installPacedSerial(PacedPort).set({ chunk: 1, factor: 20 });
  const port = new PacedPort();
  const { session } = await connect(port);
  const before = port.chunks.length;
  const saving = session.set("volume", 42).catch((e) => e);
  await new Promise((resolve) => setTimeout(resolve, 8));
  const sent = port.chunks.length - before;
  assert.ok(sent > 0 && sent < "\n>volume=42\n".length, `the write is under way (${sent} bytes sent)`);
  await session.close();   // Port.close() refuses locked streams
  assert.ok(port.closed);
  assert.equal((await saving).code, "CLOSED");
});

test("a paced write stalled past the close deadline keeps the port until it ends, then closes it", async () => {
  class PacedPort extends Port {
    get writable() { return this._writable; }
    async open(options) { return super.open(options); }
  }
  installPacedSerial(PacedPort).set({ chunk: 64, factor: 0 });
  const port = new PacedPort();
  let stuck = null;
  const { session } = await connect(port, { onCloseStuck: (e) => { stuck = e; } });
  // The bridge stops taking bytes: paced-serial.js holds the port's own
  // writer for longer than close() waits (50 ms here).
  let finish;
  port.onWrite = () => new Promise((resolve) => { finish = resolve; });
  const querying = session.query(1000).catch((e) => e);
  await new Promise((resolve) => setTimeout(resolve, 10));
  assert.ok(finish, "the write is under way");
  let closed = false;
  const closing = session.close().then(() => { closed = true; });
  await new Promise((resolve) => setTimeout(resolve, 120));
  assert.equal(stuck?.code, "CLOSE", "the page is told why the port is still open");
  assert.ok(!port.closed && !closed, "the session still owns the open port");
  finish();   // the write ends (or the gadget is unplugged)
  await closing;
  assert.ok(port.closed);
  assert.equal((await querying).code, "CLOSED");
});

test("every status field is shown, with Muse names for the firmware's keys", () => {
  const status = {
    board: "StickS3", chat: false,
    device: {
      name: "Muse", fw: "test", battery: -1,
      wifi: { on: true, state: "connected", ssid: "日本語SSID", ip: "192.0.2.1", rssi: -54 },
      hatch: { host: "example.test", vm: "", token: true, state: "Connected" },
      link: { paired: true, state: "ready" },
      volume: 0, speaker: false, mic_gain: 30, brightness: 100, sleep: 0, last: "ready", future: { list: [] },
    },
  };
  const rows = statusRows(status);
  assert.equal(rows.length, flattenStatus(status).length);
  const shown = Object.fromEntries(rows);
  assert.equal(shown["Has a Muse credential"], "true");
  assert.equal(shown["Muse VM"], "");
  assert.equal(shown["Speaker on"], "false");
  assert.equal(shown["Wi-Fi network"], "日本語SSID");
  assert.equal(shown["device.future.list"], "[]");
  for (const [label] of rows.slice(0, -1)) assert.doesNotMatch(label, /hatch/i);
  // A gadget's own keys never pick up inherited names, and deep nesting stops.
  assert.deepEqual(statusRows({ constructor: 1, toString: 2 }), [["constructor", "1"], ["toString", "2"]]);
  let deep = {};
  for (let i = 0; i < 5000; i++) deep = { d: deep };
  assert.equal(statusRows(deep).length, 1);
});

test("the card loads every stored value without rounding, and draws device text as text", () => {
  // A coarser step would snap a stored value (mic_gain 32 on a 3 dB step) into
  // an edit the next save writes back.
  for (const key of Object.keys(SETTINGS).filter((k) => k !== "speaker")) {
    const input = html.match(new RegExp(`<input id="set-${key}"[^>]*>`))?.[0];
    assert.ok(input, key);
    assert.match(input, /\bstep="1"/, key);
    assert.match(input, new RegExp(`\\bmin="${SETTINGS[key].min}"`), key);
    assert.match(input, new RegExp(`\\bmax="${SETTINGS[key].max}"`), key);
  }
  assert.match(html, /<input id="set-speaker" type="checkbox">/);
  const script = html.slice(html.indexOf("// Device settings:"), html.indexOf("if (build?.commit)"));
  assert.ok(script.length > 1000);
  assert.doesNotMatch(script, /innerHTML|insertAdjacentHTML|outerHTML|eval\(|console\./);
});

test("the installer and the token step stay off while the card holds or is closing a port", () => {
  // Their port pickers need the click's user activation, so the card can't
  // close its port on their click; they're disabled until it has.
  assert.match(html, /<button slot="activate" class="button" id="install-activate"[^>]*>/);
  const render = html.slice(html.indexOf("const renderSettings"), html.indexOf("const showSettings"));
  assert.match(render, /const holding = Boolean\(settings \|\| settingsClosing\);/);
  assert.match(render, /\$\("install-activate"\)\.disabled = holding;/);
  assert.match(render, /\$\("send-token"\)\.disabled = !pacing \|\| holding;/);
  // From the moment the card reserves a port, not only once it has connected.
  assert.match(html, /settings = session;\n\s*renderSettings\(\);/);
  // And the card can't take a port while the token goes out.
  assert.match(render, /\$\("settings-connect"\)\.disabled = [^;]*tokenSending;/);
  const send = html.slice(html.indexOf("const send = async"), html.indexOf("// After an install"));
  assert.match(send, /tokenSending = true;\s*renderSettings\(\);[\s\S]*finally \{\s*tokenSending = false;\s*renderSettings\(\);/);
  const close = html.slice(html.indexOf("const closeSettings"), html.indexOf('$("settings-connect").addEventListener'));
  assert.match(close, /settingsClosing = closing;[\s\S]*await closing;[\s\S]*settingsClosing = null;[\s\S]*renderSettings\(\);/);
});
