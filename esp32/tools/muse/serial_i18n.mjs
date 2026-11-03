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
export const DEFAULT_LANGUAGE = 'en';
export const TEXT = {
  title: ['Muse USB settings', 'Muse USB設定'],
  language: ['Language', '表示言語'],
  eyebrow: ['Make Muse your own, over USB.', 'USBで、手元のMuseを整える。'],
  heading: ['Display, sound & device status', '画面・音・デバイスの状態'],
  intro: ['Connect a USB cable to read the current values. Changes are written to your device only when you select Save changes.', 'USBケーブルで現在値を読み込みます。「変更を保存」を押すと実機に保存されます。'],
  usbRegion: ['USB connection', 'USB接続'],
  boardPlaceholder: ['Connect your Muse', 'Museを接続してください'],
  devicePlaceholder: ['Open in Chrome and connect a USB data cable.', 'Chromeで開き、USBデータケーブルを接続してください。'],
  connect: ['Connect via USB', 'USBで接続'], disconnect: ['Disconnect', '切断する'],
  offline: ['Not connected', '未接続'], online: ['USB connected', 'USB接続中'], busy: ['Working', '通信中'],
  initial: ['Connect to display the values read from your device.', '接続すると実機の現在値を表示します。'],
  display: ['Display', 'ディスプレイ'], displayHint: ['Keep it easy to read.', 'いつでも、読みやすく。'],
  'setting.brightness': ['Brightness', '明るさ'], darker: ['Dimmer', '暗く'], brighter: ['Brighter', '明るく'],
  'setting.sleep': ['Auto-sleep delay', '自動消灯までの時間'], seconds: ['seconds', '秒'],
  never: ['Never', '消灯しない'], twoMinutes: ['2 minutes', '2分'], tenMinutes: ['10 minutes', '10分'],
  sleepHint: ['0 keeps the display on. Otherwise, enter 1–3600 seconds.', '0で常時表示。1〜3600秒で指定できます。'],
  sound: ['Sound', 'サウンド'], soundHint: ['Set comfortable levels.', '使いやすい音量に。'],
  'setting.volume': ['Speaker volume', 'スピーカー音量'], 'setting.speaker': ['Enable speaker', 'スピーカーを有効にする'],
  'setting.mic_gain': ['Microphone gain', 'マイクゲイン'],
  micHint: ['Adjust microphone sensitivity. This does not add text-to-speech.', '声の拾いやすさを調整します。音声の読み上げ機能を追加する設定ではありません。'],
  waiting: ['Waiting for device settings', '設定を読み込み待ち'],
  unchanged: ['Matches the device settings', '実機の設定と一致しています'],
  dirty: ['{count} unsaved setting(s)', '{count}項目の変更が未保存です'],
  savedPlaceholder: ['Current values are read again on every connection.', '再接続時にも実機から現在値を読み込みます。'],
  savedAt: ['Read from device: {time}', '実機から確認：{time}'],
  save: ['Save changes', '変更を保存'], refresh: ['Reload settings & status', '設定・Statusを読み直す'], wake: ['Wake display', '画面を起こす'],
  statusHeading: ['Complete device status', 'デバイスの全Status'],
  statusHint: ['Read-only values returned by >status. Keys and device-reported values are shown as received.', '>statusが返す読み取り専用の値です。キーと実機が返した値はそのまま表示します。'],
  statusEmpty: ['Connect to read all status fields.', '接続するとStatusの全項目を表示します。'],
  statusKey: ['Status key', 'Statusのキー'], statusLabel: ['Description', '説明'], statusValue: ['Reported value', '取得値'],
  statusCount: ['{count} fields · {time}', '{count}項目・{time}'],
  emptyString: ['(empty string)', '（空文字）'], rawJson: ['Original status JSON', '取得したStatus JSON'],
  help: ['Connection help & usage', '接続できない場合・使い方'],
  helpConnection: ['Use Chrome on localhost or HTTPS. Connecting may restart the device. If there is no response, close other serial monitors and reconnect the USB cable.', 'ChromeでlocalhostまたはHTTPSから開いてください。接続操作で実機が再起動する場合があります。応答がない場合は、他のシリアルモニターを閉じてUSBを接続し直してください。'],
  helpPrivacy: ['This page reads status and edits the listed settings. It does not reflash firmware or erase Wi-Fi or pairing. Status stays in this page; no data is sent to external services or saved in browser storage.', 'Statusの確認と表示中の設定変更を行います。ファームウェアの書き込みやWi-Fi・ペアリングの消去は行いません。取得値を外部送信せず、ブラウザのストレージにも保存しません。'],
  history: ['Activity', '操作履歴'], footer: ['Settings stay on your device.', '設定は手元のデバイスへ。'],
  connecting: ['Connecting. Waiting for the device to start and report its settings…', '接続しています。実機の起動と設定の応答を待っています…'],
  loaded: ['Current settings and status loaded.', '現在の設定とStatusを読み込みました。'],
  disconnected: ['USB disconnected.', 'USB接続を終了しました。'],
  cancelled: ['Device selection cancelled.', 'デバイス選択をキャンセルしました。'],
  unsupported: ['USB access is unavailable. Open this page in Chrome on localhost or HTTPS.', 'USB接続を利用できません。ChromeでlocalhostまたはHTTPSから開いてください。'],
  refreshed: ['Settings and status reloaded.', '設定とStatusを再読み込みしました。'],
  wakeSent: ['Wake command sent.', '画面の復帰コマンドを送信しました。'],
  saved: ['Changes saved to the device.', '変更を実機に保存しました。'],
  saveFailed: ['Could not verify {setting}. {error}', '{setting}の保存確認に失敗しました。{error}'],
  errorMessage: ['{error}', '{error}'],
  discard: ['Discard unsaved edits and reload the device values?', '未保存の変更を取り消して、実機の値を読み直しますか？'],
  eventConnected: ['USB connected; settings and status read.', 'USB接続・設定とStatusの読み込み完了。'],
  eventDisconnected: ['USB connection ended.', 'USB接続を終了しました。'],
  eventFailed: ['Operation could not be completed.', '操作を完了できませんでした。'],
  eventSaved: ['{setting} saved and readback verified.', '{setting}を保存し、読み戻しを確認しました。'],
  'error.KEY': ['This setting is not supported.', 'この設定には対応していません。'],
  'error.VALUE': ['The value is outside the allowed range.', '設定値が範囲外です。'],
  'error.STATUS': ['The device returned an invalid Muse status.', 'MuseのStatus情報を確認できません。'],
  'error.CONNECT_TIMEOUT': ['No device response. Close other serial monitors and reconnect USB.', '実機から応答がありません。他のシリアルモニターを閉じ、USBを接続し直してください。'],
  'error.DISCONNECTED': ['USB connection lost. Reconnect the device.', 'USB接続が切れました。接続し直してください。'],
  'error.NOT_CONNECTED': ['Connect a USB device first.', 'USBで接続してください。'],
  'error.TIMEOUT': ['No response was confirmed. Reload the device values.', '応答を確認できませんでした。実機の値を読み直してください。'],
  'error.DEVICE_REJECTED': ['The device rejected this setting.', '実機が設定を受け付けませんでした。'],
  'error.READBACK': ['The readback did not match. Reload the device values.', '設定の読み戻し値が一致しません。実機の値を読み直してください。'],
  'error.CLOSED': ['USB connection closed.', 'USB接続を終了しました。'],
  'error.UNKNOWN': ['USB communication failed. Check the connection and try again.', 'USB通信に失敗しました。接続を確認してやり直してください。'],
  'field.board': ['Board', 'ボード'], 'field.chat': ['Serial chat support', 'シリアルチャット対応'],
  'field.device.name': ['Device name', 'デバイス名'], 'field.device.fw': ['Firmware version', 'ファームウェアのバージョン'],
  'field.device.battery': ['Battery (%)', 'バッテリー残量（%）'],
  'field.device.wifi.on': ['Wi-Fi enabled', 'Wi-Fi有効'], 'field.device.wifi.state': ['Wi-Fi state', 'Wi-Fi接続状態'],
  'field.device.wifi.ssid': ['Wi-Fi SSID', 'Wi-FiのSSID'], 'field.device.wifi.ip': ['IP address', 'IPアドレス'],
  'field.device.wifi.rssi': ['Wi-Fi RSSI (dBm)', 'Wi-Fi電波強度（dBm）'],
  'field.device.hatch.host': ['Muse host', 'Museホスト'], 'field.device.hatch.vm': ['Muse VM', 'Muse VM'],
  'field.device.hatch.token': ['Muse credential configured', 'Muse認証情報の設定有無'],
  'field.device.hatch.state': ['Muse session state', 'Museセッションの状態'],
  'field.device.link.paired': ['Link paired', 'Linkペアリング済み'], 'field.device.link.state': ['Link state', 'Link接続状態'],
  'field.device.volume': ['Speaker volume (%)', 'スピーカー音量（%）'], 'field.device.speaker': ['Speaker enabled', 'スピーカー有効'],
  'field.device.mic_gain': ['Microphone gain (dB)', 'マイクゲイン（dB）'],
  'field.device.brightness': ['Brightness (%)', '明るさ（%）'], 'field.device.sleep': ['Auto-sleep delay (seconds)', '自動消灯までの時間（秒）'],
  'field.device.last': ['Last command result', '直前のコマンド結果'],
};
export function translate(language, key, params = {}) {
  const text = TEXT[key]?.[language === 'ja' ? 1 : 0] ?? key;
  return text.replace(/\{(\w+)\}/g, (_, name) => String(params[name] ?? `{${name}}`));
}
export function errorText(language, error) {
  const code = error instanceof SyntaxError ? 'STATUS' : error?.code;
  return translate(language, `error.${TEXT[`error.${code}`] ? code : 'UNKNOWN'}`);
}
