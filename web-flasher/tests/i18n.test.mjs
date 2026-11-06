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
import { readFile, readdir } from "node:fs/promises";
import test from "node:test";

const dir = new URL("../", import.meta.url);
const load = (name) => import(new URL(name, dir));
const i18n = await load("i18n.js");
const { STRINGS, LANGUAGES, STORAGE_KEY, initLanguage, setLanguage, t, message, render, LocalizedError, translatePage, renderRich } = i18n;
const { DeviceSettingsError, SETTINGS, statusRows } = await load("device-settings.js");
const html = await readFile(new URL("index.html", dir), "utf8");
const boards = JSON.parse(await readFile(new URL("boards.json", dir), "utf8"));
const scripts = Object.fromEntries(
  await Promise.all(
    (await readdir(dir)).filter((f) => f.endsWith(".js")).map(async (f) => [f, await readFile(new URL(f, dir), "utf8")]),
  ),
);
const pageScript = html.slice(html.indexOf('<script type="module">\n'), html.lastIndexOf("</script>"));

// Runs fn in Japanese, then goes back to English.
const ja = (fn) => () => {
  setLanguage("ja");
  try {
    return fn();
  } finally {
    setLanguage("en");
  }
};

const placeholders = (text) => [...text.matchAll(/\{(\w+)\}/g)].map((m) => m[1]).sort();
const codeSpans = (text) => [...text.matchAll(/`[^`]+`/g)].map((m) => m[0]);
const JAPANESE = /[\u3040-\u30ff\u4e00-\u9fff]/;
// Never translated: names, domains, the website's menu path, console tokens.
const KEEP = [
  "gadgets.muse.ai", "Account > SDK tokens", "Muse", "ESP32", "Chrome", "Edge", "Web Serial", "USB", "Wi-Fi",
  "SDK", "https", "Seeed", "Watcher", "Dual_Serial", "PORT", "MB", "dBm", "VM", "IP",
];
// The contributor's own Japanese says "serial chat" here.
const REWORDED = { "status.chat": ["USB"] };
const decode = (text) =>
  text.replace(/&gt;/g, ">").replace(/&lt;/g, "<").replace(/&quot;/g, '"').replace(/&amp;/g, "&");
const squash = (text) => text.replace(/\s+/g, " ");

test("every key is in both languages, and nothing else is", () => {
  assert.deepEqual(LANGUAGES, ["en", "ja"]);
  assert.deepEqual(Object.keys(STRINGS.ja), Object.keys(STRINGS.en));
  for (const lang of LANGUAGES) {
    assert.ok(Object.isFrozen(STRINGS[lang]), lang);
    for (const [key, value] of Object.entries(STRINGS[lang])) assert.equal(typeof value, "string", `${lang} ${key}`);
  }
});

test("placeholders, code, bold and untranslatable words match in Japanese", () => {
  for (const [key, en] of Object.entries(STRINGS.en)) {
    const jaText = STRINGS.ja[key];
    assert.deepEqual(placeholders(jaText), placeholders(en), key);
    assert.deepEqual(codeSpans(jaText), codeSpans(en), key);
    assert.equal(jaText.split("**").length, en.split("**").length, key);
    for (const word of KEEP) {
      if (en.includes(word) && !REWORDED[key]?.includes(word)) assert.ok(jaText.includes(word), `${key}: ${word}`);
    }
  }
});

test("every Japanese string is translated", () => {
  // install.dialogLanguage is Japanese only (the installer is English); the
  // footer's closing "." has no Japanese counterpart.
  const emptyEn = ["install.dialogLanguage"];
  const emptyJa = ["footer.built.after"];
  const names = ["status.device.hatch.vm"];   // "Muse VM"
  for (const [key, en] of Object.entries(STRINGS.en)) {
    const jaText = STRINGS.ja[key];
    if (emptyJa.includes(key)) assert.equal(jaText, "", key);
    else if (names.includes(key)) assert.equal(jaText, en, key);
    else assert.match(jaText, JAPANESE, key);
    if (emptyEn.includes(key)) assert.equal(en, "", key);
    else assert.ok(en.trim(), key);
  }
});

test("every key the page and scripts use exists", () => {
  const used = new Set();
  for (const [, key] of html.matchAll(/data-i18n(?:-label)?="([^"]+)"/g)) used.add(key);
  for (const source of [pageScript, ...Object.values(scripts)]) {
    for (const [, key] of source.matchAll(/\b(?:t|message|LocalizedError)\(\s*"([^"]+)"/g)) used.add(key);
    for (const [, code] of source.matchAll(/new DeviceSettingsError\("(\w+)"/g)) used.add(`error.${code}`);
  }
  for (const key of Object.keys(SETTINGS)) used.add(`setting.${key}`);
  assert.ok(used.size > 70, String(used.size));
  for (const key of used) assert.ok(Object.hasOwn(STRINGS.en, key), key);
});

test("the page's English is the table's, and no visible text bypasses it", () => {
  const body = html.slice(html.indexOf("<body>"), html.indexOf('<script type="module">'));
  // Leaf elements carrying data-i18n: their text matches the table.
  const leaf = /<(\w+)([^>]*?)\sdata-i18n="([^"]+)"([^>]*)>([^<]*)<\/\1>/g;
  let count = 0;
  for (const [, , , key, , text] of body.matchAll(leaf)) {
    assert.equal(squash(decode(text)), squash(STRINGS.en[key]), key);
    count++;
  }
  assert.ok(count > 35, String(count));
  for (const [, label, key] of body.matchAll(/aria-label="([^"]+)" data-i18n-label="([^"]+)"/g)) {
    assert.equal(label, STRINGS.en[key], key);
  }
  // Whatever text is left is untranslatable: names, the link to the token
  // page, the commit link, the language names and the step numbers.
  const rest = body
    .replace(/<noscript>[\s\S]*?<\/noscript>/, "")
    .replace(leaf, "")
    .replace(/<[^>]+>/g, " ");
  const words = squash(decode(rest)).trim().split(" ").filter(Boolean);
  assert.deepEqual(words.sort(), ["1", "2", "BOOT", "EN", "gadgets.muse.ai", "main", "日本語"].sort());
});

test("the no-JavaScript message is the table's, in both languages", () => {
  const noscript = html.match(/<noscript>([\s\S]*?)<\/noscript>/)[1];
  for (const lang of LANGUAGES) {
    const text = noscript.match(new RegExp(`<p class="warning" lang="${lang}">([^<]*)</p>`))?.[1];
    assert.equal(decode(text ?? ""), STRINGS[lang]["page.noscript"], lang);
  }
});

test("board notes live in the table, for boards the page lists", () => {
  const ids = new Set(boards.map((b) => b.board));
  for (const key of Object.keys(STRINGS.en).filter((k) => k.startsWith("note."))) {
    assert.ok(ids.has(key.slice(5)), key);
  }
  // A note in boards.json would be ignored: notes go in i18n.js.
  for (const b of boards) assert.ok(!("note" in b), b.board);
});

test("text is only ever set as text", () => {
  for (const [name, source] of Object.entries({ "index.html": pageScript, ...scripts })) {
    assert.doesNotMatch(source, /innerHTML|insertAdjacentHTML|outerHTML|document\.write|eval\(/, name);
  }
});

// A small stand-in for the DOM: elements with text, attributes and children.
class FakeElement {
  constructor(doc, tag, attrs = {}) {
    this.ownerDocument = doc;
    this.localName = tag;
    this.attrs = { ...attrs };
    this.children = [];
    this.text = "";
  }
  get dataset() {
    const out = {};
    for (const [k, v] of Object.entries(this.attrs)) {
      if (k.startsWith("data-")) out[k.slice(5).replace(/-(\w)/g, (_, c) => c.toUpperCase())] = v;
    }
    return out;
  }
  setAttribute(k, v) {
    this.attrs[k] = String(v);
  }
  getAttribute(k) {
    return this.attrs[k] ?? null;
  }
  set textContent(v) {
    this.text = String(v);
    this.children = [];
  }
  get textContent() {
    return this.children.length ? this.children.map((c) => c.textContent).join("") : this.text;
  }
  replaceChildren(...nodes) {
    this.text = "";
    this.children = nodes;
  }
}
class FakeDocument {
  constructor(elements) {
    this.documentElement = { lang: "en" };
    this.title = "";
    this.elements = elements.map(([tag, attrs]) => new FakeElement(this, tag, attrs));
  }
  querySelectorAll(selector) {
    const attr = selector.match(/^\[([\w-]+)\]$/)[1];
    return this.elements.filter((el) => attr in el.attrs);
  }
  createElement(tag) {
    return new FakeElement(this, tag);
  }
  createTextNode(text) {
    return { nodeType: 3, textContent: text };
  }
}
// The page's own marked elements.
const pageDocument = () => {
  const elements = [];
  for (const [, tag, attrs] of html.matchAll(/<(\w+)((?:\s[\w-]+(?:="[^"]*")?)*)\s*>/g)) {
    const parsed = Object.fromEntries([...attrs.matchAll(/([\w-]+)(?:="([^"]*)")?/g)].map(([, k, v]) => [k, v ?? ""]));
    if ("data-i18n" in parsed || "data-i18n-label" in parsed || "data-lang" in parsed) elements.push([tag, parsed]);
  }
  return new FakeDocument(elements);
};

test("switching language redraws every marked element, the title and <html lang>", () => {
  const doc = pageDocument();
  assert.ok(doc.elements.length > 40);
  for (const lang of ["ja", "en", "ja"]) {
    setLanguage(lang);
    translatePage(doc);
    assert.equal(doc.documentElement.lang, lang);
    assert.equal(doc.title, STRINGS[lang]["page.title"]);
    for (const el of doc.elements) {
      if (el.attrs["data-i18n"]) assert.equal(el.textContent, STRINGS[lang][el.attrs["data-i18n"]], el.attrs["data-i18n"]);
      if (el.attrs["data-i18n-label"]) assert.equal(el.getAttribute("aria-label"), STRINGS[lang][el.attrs["data-i18n-label"]]);
      if (el.attrs["data-lang"]) assert.equal(el.getAttribute("aria-pressed"), String(el.attrs["data-lang"] === lang));
    }
  }
  setLanguage("en");
});

test("the choice persists; the browser's language counts only until one is made", () => {
  const store = new Map();
  const storage = { getItem: (k) => store.get(k) ?? null, setItem: (k, v) => store.set(k, String(v)) };
  assert.equal(initLanguage({ storage, languages: ["en-US", "ja"] }), "en");
  assert.equal(initLanguage({ storage, languages: ["ja-JP", "en"] }), "ja");
  assert.equal(store.size, 0);   // detecting stores nothing
  setLanguage("en", { storage });
  assert.equal(store.get(STORAGE_KEY), "en");
  assert.equal(initLanguage({ storage, languages: ["ja-JP"] }), "en");
  setLanguage("ja", { storage });
  assert.equal(initLanguage({ storage, languages: ["en-US"] }), "ja");
  // Unknown languages are refused and stored junk is ignored.
  assert.equal(setLanguage("fr", { storage }), "ja");
  assert.equal(store.get(STORAGE_KEY), "ja");
  store.set(STORAGE_KEY, "__proto__");
  assert.equal(initLanguage({ storage, languages: [] }), "en");
  // Storage that throws (blocked cookies) still lets the page switch.
  const broken = { getItem: () => { throw new Error("blocked"); }, setItem: () => { throw new Error("blocked"); } };
  assert.equal(initLanguage({ storage: broken, languages: ["ja"] }), "ja");
  assert.equal(setLanguage("en", { storage: broken }), "en");
  assert.equal(initLanguage(), "en");
});

test("messages and errors follow the language after they're made", () => {
  const status = message("settings.saveFailed", { setting: message("setting.brightness"), error: new DeviceSettingsError("TIMEOUT") });
  const e = new LocalizedError("token.storeFailed", { answer: "error nvs" });
  assert.equal(render(status), "Brightness: The gadget didn't answer. Reload to see its settings.");
  assert.equal(e.message, "The gadget couldn't store the token (error nvs).");
  ja(() => {
    assert.equal(render(status), "明るさの保存確認に失敗しました。応答を確認できませんでした。実機の値を読み直してください。");
    assert.equal(e.message, "実機にトークンを保存できませんでした（error nvs）。");
    assert.equal(new DeviceSettingsError("OPEN", { error: "Busy" }).message, "ポートを開けませんでした（Busy）。他のシリアルモニターを閉じてやり直してください。");
    assert.equal(t("flash.tooSmall", { needed: 16, size: 8 }).includes("16 MB"), true);
  })();
  // Text from elsewhere (the device, the browser) is shown as it is.
  assert.equal(render("connected"), "connected");
  assert.equal(render(new Error("NetworkError")), "NetworkError");
  assert.equal(t("settings.device", { board: "{fw}", name: "n", fw: "1", wifi: "x" }), "{fw} · n · firmware 1 · Wi-Fi x");
  assert.equal(t("no.such.key"), "no.such.key");
});

test("status labels are translated; unknown fields keep their names", ja(() => {
  const rows = statusRows({ board: "B", device: { brightness: 80, hatch: { token: true } }, constructor: 1 });
  assert.deepEqual(rows, [["ボード", "B"], ["明るさ（%）", "80"], ["Muse認証情報の設定有無", "true"], ["constructor", "1"]]);
}));

test("board notes become bold and code elements, and markup stays text", () => {
  const doc = new FakeDocument([]);
  const el = doc.createElement("p");
  renderRich(el, "Pick the **second** port: `read-flash 0 ALL x.bin` <img src=x onerror=alert(1)>");
  assert.deepEqual(
    el.children.map((c) => [c.localName ?? "#text", c.textContent]),
    [["#text", "Pick the "], ["strong", "second"], ["#text", " port: "], ["code", "read-flash 0 ALL x.bin"], ["#text", " <img src=x onerror=alert(1)>"]],
  );
  ja(() => {
    renderRich(el, t("note.muse-sensecap-watcher"));
    assert.deepEqual(el.children.filter((c) => c.localName).map((c) => c.localName), ["strong", "strong", "code"]);
  })();
});
