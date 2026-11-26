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

// The web flasher's pure logic, with no DOM or Web Serial, so Node can test it
// (esp32/tests/test_web_flasher.py): reading a gadget's version out of its
// console, comparing it with the manifest's, what to say when an install
// stops, and the session log with every secret masked the way
// esp32/tools/redact.py masks it. What it says to the user is an i18n.js
// message (English and Japanese), drawn in the page's language.

import { message } from "./i18n.js";

// ---- SHA-256, synchronous (the log masks a line as it's added) -------------

const K = new Uint32Array([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]);

// Lowercase hex SHA-256 of `bytes` (a Uint8Array).
export function sha256Hex(bytes) {
  const bitLen = bytes.length * 8;
  const padded = new Uint8Array(((bytes.length + 9 + 63) >> 6) << 6);
  padded.set(bytes);
  padded[bytes.length] = 0x80;
  const view = new DataView(padded.buffer);
  view.setUint32(padded.length - 8, Math.floor(bitLen / 0x100000000));
  view.setUint32(padded.length - 4, bitLen >>> 0);
  const h = new Uint32Array([
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
  ]);
  const w = new Uint32Array(64);
  const rotr = (x, n) => (x >>> n) | (x << (32 - n));
  for (let off = 0; off < padded.length; off += 64) {
    for (let i = 0; i < 16; i++) w[i] = view.getUint32(off + i * 4);
    for (let i = 16; i < 64; i++) {
      const s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >>> 3);
      const s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >>> 10);
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) >>> 0;
    }
    let [a, b, c, d, e, f, g, hh] = h;
    for (let i = 0; i < 64; i++) {
      const t1 = (hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i]) >>> 0;
      const t2 = ((rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c))) >>> 0;
      hh = g;
      g = f;
      f = e;
      e = (d + t1) >>> 0;
      d = c;
      c = b;
      b = a;
      a = (t1 + t2) >>> 0;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  return Array.from(h, (x) => x.toString(16).padStart(8, "0")).join("");
}

// ---- Masking secrets: esp32/tools/redact.py's rule, line for line ----------

// JSON keys whose values are secrets (redact.py SECRET_KEYS).
const SECRET_KEYS = [
  "sdk_token", "access_token", "refresh_token", "auth_token", "device_token", "token",
  "hatch_token", "password", "passphrase", "psk", "wifi_password", "proof_key", "secret",
].sort();

const SDK_TOKEN = /mgst_[A-Za-z0-9_-]{4,}/g;
const BEARER = /\b(Bearer\s+(?:hatch_refresh:)?|hatch_refresh:)([A-Za-z0-9._~+/=-]{8,})/g;
// A value runs to its own closing quote, past escaped ones and the other
// quote; one never closed is masked to the end of its line.
const JSON_FIELD = new RegExp(
  `(["'](?:${SECRET_KEYS.join("|")})["']\\s*:\\s*)(?:"((?:[^"\\\\\\r\\n]|\\\\.)*)("?)|'((?:[^'\\\\\\r\\n]|\\\\.)*)('?))`,
  "gi",
);
const CONSOLE_SET = /((?:^|[>\s])(?:sdk_token|hatch\.token\+?|token)=)(\S+)/gi;
// A Wi-Fi password may hold spaces: the rest of the line after wifi.pass= or wifi=ssid,.
const CONSOLE_PASS = /((?:^|[>\s])(?:wifi\.pass=|wifi=[^,\r\n]*,))([^\r\n]+)/gi;

// What may be shown of a secret: "none", or "48 chars, sha256 1a2b3c4d" (its
// UTF-8 length and SHA-256 prefix, never a character of it).
export function fingerprint(secret) {
  if (!secret) return "none";
  const bytes = typeof secret === "string" ? new TextEncoder().encode(secret) : secret;
  return `${bytes.length} chars, sha256 ${sha256Hex(bytes).slice(0, 8)}`;
}

const mask = (secret) => `<redacted: ${fingerprint(secret)}>`;

// `text` with every secret in it masked (redact.py redact_text).
export function redactText(text) {
  if (!text) return text;
  return text
    .replace(JSON_FIELD, (all, head, dq, dqEnd, sq, sqEnd) => {
      const [quote, secret, end] = dq !== undefined ? ['"', dq, dqEnd] : ["'", sq, sqEnd];
      return secret ? head + quote + mask(secret) + end : all;
    })
    .replace(CONSOLE_PASS, (_, head, secret) => head + mask(secret))
    .replace(CONSOLE_SET, (_, head, secret) => head + mask(secret))
    .replace(BEARER, (_, head, secret) => head + mask(secret))
    .replace(SDK_TOKEN, (secret) => mask(secret));
}

// ---- Versions ---------------------------------------------------------------

// eslint-disable-next-line no-control-regex
const ANSI = /\x1b\[[0-9;]*[A-Za-z]/g;

// The version in one console line, or null. The console answers ">version"
// with "@version X" (esp32/main/sdk_token_console.c), and every boot logs
// "link.app:   Version:  X" (esp32/main/app.c log_banner).
export function parseVersionLine(line) {
  const text = line.replace(ANSI, "");
  let m = text.match(/@version (\S+)/);
  if (m) return { version: m[1], source: "answer" };
  m = text.match(/\blink\.app:\s+Version:\s+(\S+)/);
  if (m && m[1] !== "?") return { version: m[1], source: "boot" };
  return null;
}

// "1.0.0+g75d81c33.dirty" -> { number, commit, dirty }, or null.
const VERSION = /^v?(\d+\.\d+\.\d+)(?:\+g?([0-9a-f]{7,40}))?(\.dirty)?$/;
const parseVersion = (v) => {
  const m = VERSION.exec(v.trim());
  return m ? { number: m[1], commit: m[2] ?? null, dirty: Boolean(m[3]) } : null;
};

// Whether the version the gadget reports is the manifest's. Firmware versions
// are "X.Y.Z" at a release tag, else "X.Y.Z+g<commit>[.dirty]"
// (esp32/cmake/version.cmake); a manifest written before make_manifest.py read
// the image's own version says "X.Y.Z+<7 hex>", so a commit matches when one
// is a prefix of the other.
export function versionsMatch(expected, reported) {
  if (!expected || !reported) return false;
  if (expected.trim() === reported.trim()) return true;
  const a = parseVersion(expected);
  const b = parseVersion(reported);
  if (!a || !b || a.number !== b.number || a.dirty !== b.dirty) return false;
  if (!a.commit || !b.commit) return a.commit === b.commit;
  return a.commit.startsWith(b.commit) || b.commit.startsWith(a.commit);
}

// ---- What to say when an install stops before the end ----------------------

// progress: { erased, writeStarted } from the install's hooks; cancelled: the
// user closed the installer before installing; oldVersion: what the gadget
// reported before, or null. Returns a message (i18n.js "stop.*").
export function stopMessage({ erased = false, writeStarted = false, cancelled = false, oldVersion = null }) {
  // The new firmware is only partly written: the gadget may not start.
  if (writeStarted) return message("stop.partial");
  // Erased, nothing new written: it won't start until installed again.
  if (erased) return message("stop.erased");
  // Nothing was written: it still runs what it had.
  if (cancelled) return oldVersion ? message("stop.cancelled", { version: oldVersion }) : message("stop.cancelledUnknown");
  return oldVersion ? message("stop.failed", { version: oldVersion }) : message("stop.failedUnknown");
}

export const NO_TOUCH_LINE = message("transfer.noTouch");
export const WRIST_LINE = message("transfer.wrist");

// The lines (messages) to show during the transfer for a board from
// boards.json: `worn` boards (wrist-worn products) also get the wrist line.
export function transferLines(board) {
  return board?.worn ? [WRIST_LINE, NO_TOUCH_LINE] : [NO_TOUCH_LINE];
}

// ---- The session log ----------------------------------------------------------

const formatArg = (arg) => {
  if (typeof arg === "string") return arg;
  if (arg instanceof Error) return `${arg.name}: ${arg.message}`;
  if (arg === undefined) return "undefined";
  try {
    return JSON.stringify(arg) ?? String(arg);
  } catch {
    return String(arg);
  }
};

// The page's whole session log: ESP Web Tools' and esptool-js's console
// output, the installer's states, the version reads and the token step, each
// line masked as it's added. `maxLines` keeps the oldest and newest lines.
export function createSessionLog({ now = () => new Date(), maxLines = 50000 } = {}) {
  const lines = [];
  let dropped = 0;
  const log = {
    add(source, ...args) {
      const text = redactText(args.map(formatArg).join(" "));
      const stamp = now().toISOString();
      for (const part of text.split(/\r?\n/)) {
        if (lines.length >= maxLines) {
          lines.splice(maxLines / 2, 1);
          dropped++;
        }
        lines.push(`${stamp} [${source}] ${part}`);
      }
    },
    text() {
      const note = dropped ? [`(${dropped} lines from the middle of the session dropped)`] : [];
      return [...lines.slice(0, maxLines / 2), ...note, ...lines.slice(maxLines / 2)].join("\n") + "\n";
    },
    get length() {
      return lines.length;
    },
    // Copies console.log and friends into the log (ESP Web Tools and
    // esptool-js write theirs there), still printing them.
    teeConsole(target = console) {
      for (const level of ["log", "info", "warn", "error", "debug"]) {
        const original = target[level];
        target[level] = (...args) => {
          log.add(level, ...args);
          original.apply(target, args);
        };
      }
    },
  };
  return log;
}

// "muse-web-flasher-<board>-20261107-104201.txt"
export function logFileName(board, date) {
  const p = (n) => String(n).padStart(2, "0");
  const stamp = `${date.getFullYear()}${p(date.getMonth() + 1)}${p(date.getDate())}-${p(date.getHours())}${p(date.getMinutes())}${p(date.getSeconds())}`;
  return `muse-web-flasher-${(board || "session").replace(/[^A-Za-z0-9_-]/g, "_")}-${stamp}.txt`;
}
