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

// The web flasher's text, in English (the default) and Japanese.
//
// index.html marks each piece of text with data-i18n="key" (its text) or
// data-i18n-label="key" (its aria-label); the scripts call t(key, params).
// "{name}" is a parameter. In board notes, **text** is bold and `text` is
// code. Everything is set as text, never as HTML.

export const STRINGS = Object.freeze({
  en: Object.freeze({
    "page.title": "Muse Gadget Web Flasher",
    "page.lede": "Flash the open source Muse Gadget firmware onto your ESP32 board, straight from Chrome or Edge on a desktop computer.",
    "page.noscript": "The web flasher needs JavaScript. Turn it on and use Chrome or Edge on a desktop computer.",

    "lang.label": "Language",

    "token.heading": "SDK token",
    "token.change": "Change",
    "token.forget": "Forget",
    "token.intro.before": "A gadget needs your SDK token to pair. Get it from ",
    "token.intro.after": " (Account > SDK tokens). It's sent to the gadget over USB after installing, the gadget keeps it through updates, and this browser remembers it for next time.",
    "token.label": "SDK token",
    "token.invalid": "That isn't an SDK token. Copy it again from gadgets.muse.ai.",

    "install.heading": "Install",
    "install.board": "Board",
    "install.allPorts": "Show all serial ports, not just this board's USB device",
    "install.noToken": "You haven't entered your SDK token yet. You can install without it, but the gadget can't pair until it has one.",
    "install.button": "Connect & install",
    "install.unsupported": "Your browser doesn't support Web Serial. Use Chrome or Edge on a desktop computer.",
    "install.notAllowed": "Web Serial needs a secure (https) connection.",
    "install.note": "Keep this tab in front while installing. Updating keeps your pairing, Wi-Fi settings and token. When it's done, close the installer: the page sends your token, and you set the gadget up in the Muse app.",
    "install.dialogLanguage": "",
    "install.holdsPort": "Device settings below is connected to a gadget. Disconnect it there to install.",
    "install.needToken": "Installed. Now enter your SDK token in step 1, then send it under “Already installed?”.",

    "token.sending": "Sending your SDK token to the gadget…",
    "token.sent": "Done. The gadget restarts with your token; set it up in the Muse app.",
    "token.openFailed": "Couldn't open the port ({error}). Close the installer's dialog first.",
    "token.rejected": "The gadget says that isn't an SDK token.",
    "token.storeFailed": "The gadget couldn't store the token ({answer}).",
    "token.noAnswer": "The gadget didn't answer. Check it runs firmware from this page and you picked its console port. If it still doesn't answer, unplug it, plug it back in and try again.",

    "already.summary": "Already installed? Send your token to a gadget",
    "already.note": "For a gadget running firmware from this page. Pick its port when asked.",
    "already.send": "Send token to gadget",
    "already.holds": "Disconnect Device settings below first.",
    "already.needToken": "Enter your SDK token in step 1 first.",

    "settings.summary": "Device settings",
    "settings.intro": "Read a gadget's screen and sound settings over USB and change them. For boards with the Muse screen UI, running firmware from this page. Pick its port when asked. Nothing else on the gadget changes.",
    "settings.connect": "Connect to gadget",
    "settings.disconnect": "Disconnect",
    "settings.reload": "Reload",

    "setting.brightness": "Brightness",
    "setting.sleep": "Screen sleep",
    "setting.sleepUnit": "seconds; 0 keeps it on",
    "setting.volume": "Speaker volume",
    "setting.speaker": "Speaker",
    "setting.speakerOn": "On",
    "setting.mic_gain": "Microphone gain",

    "settings.save": "Save changes",
    "settings.unsaved": "{count} unsaved",
    "settings.device": "{board} · {name} · firmware {fw} · Wi-Fi {wifi}",
    "settings.unknown": "unknown",
    "settings.all": "Everything the gadget reports",
    "settings.reading": "Reading the gadget's settings…",
    "settings.current": "These are the gadget's current settings.",
    "settings.saving": "Saving…",
    "settings.saved": "Saved. The gadget confirmed every change.",
    "settings.disconnected": "Disconnected.",
    "settings.unplugged": "The gadget was unplugged.",
    "settings.talkFailed": "Couldn't talk to the gadget. Check the cable and try again.",
    "settings.saveFailed": "{setting}: {error}",
    "settings.notSaved": "{setting}: couldn't save it.",

    "error.KEY": "This page can't change that setting.",
    "error.VALUE": "That value is out of range.",
    "error.STATUS": "The gadget's status didn't make sense. Reload to try again.",
    "error.OPEN": "Couldn't open the port ({error}). Close other serial monitors and try again.",
    "error.CONNECT_TIMEOUT": "The gadget didn't answer. Settings work on boards with the Muse screen UI running firmware from this page; check you picked its console port, or unplug it, plug it back in and try again.",
    "error.DISCONNECTED": "The gadget was unplugged. Connect it again.",
    "error.NOT_CONNECTED": "Connect the gadget first.",
    "error.TIMEOUT": "The gadget didn't answer. Reload to see its settings.",
    "error.REJECTED": "The gadget didn't accept that value.",
    "error.READBACK": "The gadget reports a different value. Reload to see its settings.",
    "error.CLOSED": "Disconnected.",
    "error.CLOSE": "Couldn't close the port ({error}). Unplug the gadget and plug it back in.",

    "status.board": "Board",
    "status.chat": "Chat over USB",
    "status.device.name": "Name",
    "status.device.fw": "Firmware",
    "status.device.battery": "Battery (%)",
    "status.device.wifi.on": "Wi-Fi on",
    "status.device.wifi.state": "Wi-Fi",
    "status.device.wifi.ssid": "Wi-Fi network",
    "status.device.wifi.ip": "IP address",
    "status.device.wifi.rssi": "Wi-Fi signal (dBm)",
    "status.device.hatch.host": "Muse host",
    "status.device.hatch.vm": "Muse VM",
    "status.device.hatch.token": "Has a Muse credential",
    "status.device.hatch.state": "Muse connection",
    "status.device.link.paired": "Paired",
    "status.device.link.state": "Link",
    "status.device.volume": "Speaker volume (%)",
    "status.device.speaker": "Speaker on",
    "status.device.mic_gain": "Microphone gain (dB)",
    "status.device.brightness": "Brightness (%)",
    "status.device.sleep": "Screen sleep (seconds)",
    "status.device.last": "Last command",

    "flash.tooSmall": "This build needs {needed} MB of flash, but this board has {size} MB. Check you picked the right board; nothing was changed on it.",

    "footer.boot.before": "Board not showing up? Some boards need you to hold ",
    "footer.boot.after": " while plugging in USB.",
    "footer.built.before": " Built from ",
    "footer.built.after": ".",

    "note.muse-sensecap-watcher": "Pick the **second** USB Dual_Serial port, the one with the higher number; the first is the camera chip. Flashing is slow on this board, about 4 minutes; keep the tab in front. Muse overwrites the Watcher's factory data, which is unique to each device, so **back it up first** and don't erase the device: `esp32/tools/muse/paced_esptool.py --chip esp32s3 -p PORT read-flash 0x9000 0x32000 nvsfactory.bin`",
    "note.reterminal-sticky": "Flashing takes about 3 minutes; keep the tab in front. Muse replaces Seeed's factory firmware. To go back to it later, back up the flash first: `python -m esptool --chip esp32s3 -p PORT read-flash 0 ALL sticky-factory.bin`",
  }),
  ja: Object.freeze({
    "page.title": "Muse Gadget Webフラッシャー",
    "page.lede": "オープンソースのMuse Gadgetファームウェアを、PCのChromeまたはEdgeから直接ESP32ボードに書き込みます。",
    "page.noscript": "このWebフラッシャーにはJavaScriptが必要です。JavaScriptを有効にし、PCのChromeまたはEdgeをご利用ください。",

    "lang.label": "表示言語",

    "token.heading": "SDKトークン",
    "token.change": "変更",
    "token.forget": "削除",
    "token.intro.before": "実機のペアリングにはSDKトークンが必要です。",
    "token.intro.after": "（Account > SDK tokens）から取得してください。トークンはインストール後にUSB経由で実機へ送信され、ファームウェアを更新しても実機に保持されます。また、次回のためにこのブラウザにも保存されます。",
    "token.label": "SDKトークン",
    "token.invalid": "有効なSDKトークンではありません。gadgets.muse.ai からもう一度コピーしてください。",

    "install.heading": "インストール",
    "install.board": "ボード",
    "install.allPorts": "このボードのUSBデバイスだけでなく、すべてのシリアルポートを表示する",
    "install.noToken": "SDKトークンがまだ入力されていません。トークンなしでもインストールできますが、トークンを設定するまで実機はペアリングできません。",
    "install.button": "接続してインストール",
    "install.unsupported": "お使いのブラウザはWeb Serialに対応していません。PCのChromeまたはEdgeをご利用ください。",
    "install.notAllowed": "Web Serialには安全な接続（https）が必要です。",
    "install.note": "インストール中はこのタブを前面に表示したままにしてください。更新してもペアリング情報、Wi-Fi設定、トークンは保持されます。完了したらインストーラーを閉じてください。このページからトークンが送信され、その後Museアプリで実機をセットアップできます。",
    "install.dialogLanguage": "インストーラーのウィンドウは英語で表示されます。",
    "install.holdsPort": "下の「デバイス設定」が実機に接続されています。インストールするには、そちらで切断してください。",
    "install.needToken": "インストールが完了しました。ステップ1でSDKトークンを入力し、「インストール済みの場合」から送信してください。",

    "token.sending": "SDKトークンを実機に送信しています…",
    "token.sent": "完了しました。実機はトークンを反映して再起動します。Museアプリでセットアップしてください。",
    "token.openFailed": "ポートを開けませんでした（{error}）。先にインストーラーのダイアログを閉じてください。",
    "token.rejected": "実機により、有効なSDKトークンではないと判定されました。",
    "token.storeFailed": "実機にトークンを保存できませんでした（{answer}）。",
    "token.noAnswer": "実機から応答がありませんでした。このページのファームウェアが動作していること、またコンソールポートを選択したことを確認してください。それでも応答しない場合は、USBを抜き差ししてやり直してください。",

    "already.summary": "インストール済みの場合：実機にトークンを送信",
    "already.note": "このページのファームウェアが動作している実機向けです。求められたら対象のポートを選択してください。",
    "already.send": "実機にトークンを送信",
    "already.holds": "先に下の「デバイス設定」で切断してください。",
    "already.needToken": "先にステップ1でSDKトークンを入力してください。",

    "settings.summary": "デバイス設定",
    "settings.intro": "USB経由で実機の画面・音声設定を読み取り、変更します。このページのファームウェアが動作する、Muse画面UI搭載ボード向けです。求められたら対象のポートを選択してください。実機のその他の設定やデータは変更されません。",
    "settings.connect": "USBで接続",
    "settings.disconnect": "切断する",
    "settings.reload": "設定・Statusを読み直す",

    "setting.brightness": "明るさ",
    "setting.sleep": "自動消灯までの時間",
    "setting.sleepUnit": "秒（0で常時表示）",
    "setting.volume": "スピーカー音量",
    "setting.speaker": "スピーカー",
    "setting.speakerOn": "有効",
    "setting.mic_gain": "マイクゲイン",

    "settings.save": "変更を保存",
    "settings.unsaved": "{count}項目の変更が未保存です",
    "settings.device": "{board} · {name} · ファームウェア {fw} · Wi-Fi {wifi}",
    "settings.unknown": "不明",
    "settings.all": "デバイスの全Status",
    "settings.reading": "接続しています。実機の起動と設定の応答を待っています…",
    "settings.current": "現在の設定とStatusを読み込みました。",
    "settings.saving": "保存しています…",
    "settings.saved": "変更を実機に保存し、読み戻しを確認しました。",
    "settings.disconnected": "USB接続を終了しました。",
    "settings.unplugged": "USB接続が切れました。",
    "settings.talkFailed": "USB通信に失敗しました。接続を確認してやり直してください。",
    "settings.saveFailed": "{setting}の保存確認に失敗しました。{error}",
    "settings.notSaved": "{setting}の保存確認に失敗しました。",

    "error.KEY": "この設定には対応していません。",
    "error.VALUE": "設定値が範囲外です。",
    "error.STATUS": "MuseのStatus情報を確認できません。もう一度読み直してください。",
    "error.OPEN": "ポートを開けませんでした（{error}）。他のシリアルモニターを閉じてやり直してください。",
    "error.CONNECT_TIMEOUT": "実機から応答がありません。設定機能は、このページのファームウェアが動作するMuse画面UI搭載ボードで利用できます。コンソールポートを選択したか確認するか、USBを抜き差ししてやり直してください。",
    "error.DISCONNECTED": "USB接続が切れました。接続し直してください。",
    "error.NOT_CONNECTED": "USBで接続してください。",
    "error.TIMEOUT": "応答を確認できませんでした。実機の値を読み直してください。",
    "error.REJECTED": "実機が設定を受け付けませんでした。",
    "error.READBACK": "設定の読み戻し値が一致しません。実機の値を読み直してください。",
    "error.CLOSED": "USB接続を終了しました。",
    "error.CLOSE": "ポートを閉じられませんでした（{error}）。実機のUSBを抜き差ししてください。",

    "status.board": "ボード",
    "status.chat": "シリアルチャット対応",
    "status.device.name": "デバイス名",
    "status.device.fw": "ファームウェアのバージョン",
    "status.device.battery": "バッテリー残量（%）",
    "status.device.wifi.on": "Wi-Fi有効",
    "status.device.wifi.state": "Wi-Fi接続状態",
    "status.device.wifi.ssid": "Wi-FiのSSID",
    "status.device.wifi.ip": "IPアドレス",
    "status.device.wifi.rssi": "Wi-Fi電波強度（dBm）",
    "status.device.hatch.host": "Museホスト",
    "status.device.hatch.vm": "Muse VM",
    "status.device.hatch.token": "Muse認証情報の設定有無",
    "status.device.hatch.state": "Museセッションの状態",
    "status.device.link.paired": "Linkペアリング済み",
    "status.device.link.state": "Link接続状態",
    "status.device.volume": "スピーカー音量（%）",
    "status.device.speaker": "スピーカー有効",
    "status.device.mic_gain": "マイクゲイン（dB）",
    "status.device.brightness": "明るさ（%）",
    "status.device.sleep": "自動消灯までの時間（秒）",
    "status.device.last": "直前のコマンド結果",

    "flash.tooSmall": "このビルドには {needed} MB のフラッシュメモリが必要ですが、このボードには {size} MB しかありません。正しいボードを選択したか確認してください。ボードの内容は変更されていません。",

    "footer.boot.before": "ボードが表示されない場合：ボードによっては、",
    "footer.boot.after": " を押しながらUSBを接続する必要があります。",
    "footer.built.before": " ビルド元：",
    "footer.built.after": "",

    "note.muse-sensecap-watcher": "番号が大きい方の**2番目**の USB Dual_Serial ポートを選択してください（1番目はカメラチップ用です）。このボードへの書き込みには約4分かかります。タブを前面に表示したままにしてください。Museは各Watcherに固有の工場出荷時データを上書きするため、**必ず事前にバックアップを取り**、デバイスの消去（イレース）は行わないでください：`esp32/tools/muse/paced_esptool.py --chip esp32s3 -p PORT read-flash 0x9000 0x32000 nvsfactory.bin`",
    "note.reterminal-sticky": "書き込みには約3分かかります。タブを前面に表示したままにしてください。MuseはSeeedの工場出荷時ファームウェアを上書きします。あとから元に戻せるよう、事前にフラッシュメモリをバックアップしてください：`python -m esptool --chip esp32s3 -p PORT read-flash 0 ALL sticky-factory.bin`",
  }),
});

export const LANGUAGES = Object.freeze(Object.keys(STRINGS));
export const STORAGE_KEY = "muse-flasher-language";

let current = "en";
export const language = () => current;
export const hasString = (key) => Object.hasOwn(STRINGS.en, key);

// Something to show now and draw again in another language later.
// Parameters can be messages too; anything else shows as it is.
export const message = (key, params = {}) => ({ key, params });

const isMessage = (value) => value !== null && typeof value === "object" && typeof value.key === "string";

// A message, an Error or a plain string, as text in the current language.
export function render(value) {
  if (isMessage(value)) return t(value.key, value.params);
  if (value instanceof Error) return value.message;
  return value == null ? "" : String(value);
}

export function t(key, params = {}) {
  const table = STRINGS[current];
  const text = Object.hasOwn(table, key) ? table[key] : hasString(key) ? STRINGS.en[key] : key;
  return text.replace(/\{(\w+)\}/g, (all, name) => (Object.hasOwn(params, name) ? render(params[name]) : all));
}

// An Error whose message follows the page's language.
export class LocalizedError extends Error {
  constructor(key, params = {}) {
    super();
    this.i18n = message(key, params);
  }

  get message() {
    return t(this.i18n.key, this.i18n.params);
  }
}

// The saved choice, else the browser's first language until one is made.
export function initLanguage({ storage, languages = [] } = {}) {
  let saved = null;
  try {
    saved = storage?.getItem(STORAGE_KEY) ?? null;
  } catch {}
  current = LANGUAGES.includes(saved) ? saved : /^ja\b/i.test(languages[0] ?? "") ? "ja" : "en";
  return current;
}

export function setLanguage(lang, { storage } = {}) {
  if (!LANGUAGES.includes(lang)) return current;
  current = lang;
  try {
    storage?.setItem(STORAGE_KEY, lang);
  } catch {}
  return current;
}

// Sets the page's language and every marked element's text.
export function translatePage(doc) {
  doc.documentElement.lang = current;
  doc.title = t("page.title");
  for (const el of doc.querySelectorAll("[data-i18n]")) el.textContent = t(el.dataset.i18n);
  for (const el of doc.querySelectorAll("[data-i18n-label]")) el.setAttribute("aria-label", t(el.dataset.i18nLabel));
  for (const el of doc.querySelectorAll("[data-lang]")) el.setAttribute("aria-pressed", String(el.dataset.lang === current));
}

// Text with **bold** and `code` (board notes), built as elements.
export function renderRich(el, text) {
  const doc = el.ownerDocument;
  el.replaceChildren(
    ...text.split(/(\*\*[^*]+\*\*|`[^`]+`)/).filter(Boolean).map((part) => {
      const tag = /^\*\*[^*]+\*\*$/.test(part) ? "strong" : /^`[^`]+`$/.test(part) ? "code" : null;
      if (!tag) return doc.createTextNode(part);
      const node = doc.createElement(tag);
      node.textContent = tag === "strong" ? part.slice(2, -2) : part.slice(1, -1);
      return node;
    }),
  );
}
