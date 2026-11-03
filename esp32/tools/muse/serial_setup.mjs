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
import { MuseSerial, SETTINGS } from './serial_settings.mjs';
import { DEFAULT_LANGUAGE, TEXT, translate, errorText } from './serial_i18n.mjs';
import { flattenStatus, statusValue } from './serial_status.mjs';
const $ = id => document.getElementById(id);
let session = null, confirmed = null, busy = false, language = DEFAULT_LANGUAGE, readAt = null;
let notice = { key: 'initial', kind: '', params: {} };
const history = [], keys = Object.keys(SETTINGS);
const t = (key, params) => translate(language, key, params);
const clock = time => new Date(time).toLocaleString(language === 'ja' ? 'ja-JP' : 'en-US');
const values = () => Object.fromEntries(keys.map(key => [key, key === 'speaker' ? $(key).checked : $(key).value === '' ? NaN : Number($(key).value)]));
function params(raw = {}) {
  return { ...raw, ...(raw.setting ? { setting: t(`setting.${raw.setting}`) } : {}), ...(raw.error ? { error: errorText(language, raw.error) } : {}) };
}
function message(key, kind = '', args = {}) { notice = { key, kind, params: args }; renderMessage(); }
function renderMessage() { $('message').textContent = t(notice.key, params(notice.params)); $('message').className = notice.kind; }
function event(key, args = {}) { history.unshift({ key, args, at: Date.now() }); if (history.length > 40) history.pop(); renderHistory(); }
function renderHistory() {
  $('events').replaceChildren(...history.map(item => {
    const row = document.createElement('li'); row.textContent = `${clock(item.at)} ${t(item.key, params(item.args))}`; return row;
  }));
}
function renderStatus() {
  const rows = confirmed ? flattenStatus(confirmed) : [];
  $('status-empty').hidden = Boolean(confirmed);
  $('status-table-container').hidden = $('status-json-details').hidden = !confirmed;
  $('status-count').textContent = confirmed ? t('statusCount', { count: rows.length, time: clock(readAt) }) : '—';
  $('status-rows').replaceChildren(...rows.map(({ path, value }) => {
    const tr = document.createElement('tr');
    const labelKey = `field.${path}`;
    const content = [path, TEXT[labelKey] ? t(labelKey) : '—', value === '' ? t('emptyString') : statusValue(value)];
    for (const text of content) { const td = document.createElement('td'); td.textContent = text; tr.append(td); }
    return tr;
  }));
  $('status-json').textContent = confirmed ? JSON.stringify(confirmed, null, 2) : '';
}
function changed() {
  if (!confirmed) return [];
  const draft = values(); return keys.filter(key => draft[key] !== confirmed.device[key]);
}
function update() {
  const online = Boolean(session?.online), edits = changed();
  $('connect').disabled = busy || !window.isSecureContext || !navigator.serial;
  $('connect').textContent = t(online ? 'disconnect' : 'connect');
  $('connection').textContent = t(busy ? 'busy' : online ? 'online' : 'offline');
  $('connection').classList.toggle('online', online);
  $('fields').disabled = busy || !online || !confirmed;
  $('refresh').disabled = $('wake').disabled = busy || !online;
  $('save').disabled = busy || !online || !confirmed || !edits.length || !$('settings').checkValidity();
  $('change-count').textContent = t(!confirmed ? 'waiting' : edits.length ? 'dirty' : 'unchanged', { count: edits.length });
  for (const key of ['brightness', 'volume', 'mic_gain']) $(`${key}-value`).textContent = confirmed ? `${$(key).value}${key === 'mic_gain' ? ' dB' : '%'}` : '—';
  if (!confirmed) { $('board').textContent = t('boardPlaceholder'); $('device-info').textContent = t('devicePlaceholder'); }
  else {
    const dev = confirmed.device;
    $('board').textContent = confirmed.board;
    $('device-info').textContent = `${dev.name || 'Muse Gadget'} · FW ${dev.fw || '—'} · Wi-Fi ${dev.wifi?.state || '—'}`;
  }
  $('saved').textContent = readAt ? t('savedAt', { time: clock(readAt) }) : t('savedPlaceholder');
}
function show(status, { preserveDraft = false } = {}) {
  confirmed = status; readAt = Date.now();
  if (!preserveDraft) for (const key of keys) {
    if (key === 'speaker') $(key).checked = status.device[key]; else $(key).value = status.device[key];
  }
  renderStatus(); update();
}
function clear() { confirmed = null; readAt = null; $('settings').reset(); renderStatus(); update(); }
function setLanguage(value) {
  language = value === 'ja' ? 'ja' : DEFAULT_LANGUAGE;
  document.documentElement.lang = language; document.title = t('title'); $('language').value = language;
  for (const el of document.querySelectorAll('[data-i18n]')) el.textContent = t(el.dataset.i18n);
  for (const el of document.querySelectorAll('[data-i18n-aria]')) el.setAttribute('aria-label', t(el.dataset.i18nAria));
  renderStatus(); renderMessage(); renderHistory(); update();
}
async function operation(action) {
  busy = true; update();
  try { await action(); }
  catch (error) { message('errorMessage', 'error', { error }); event('eventFailed'); }
  finally { busy = false; update(); }
}
$('connect').onclick = () => operation(async () => {
  if (session) { const old = session; session = null; await old.close(); clear(); message('disconnected'); event('eventDisconnected'); return; }
  if (!window.isSecureContext || !navigator.serial) { message('unsupported', 'error'); return; }
  let port;
  try { port = await navigator.serial.requestPort(); }
  catch (error) { if (error.name === 'NotFoundError') { message('cancelled'); return; } throw error; }
  const current = new MuseSerial(port, { onDisconnect: error => {
    if (session !== current) return;
    session = null; clear(); message('errorMessage', 'error', { error }); event('eventDisconnected'); void current.close();
  } });
  session = current; message('connecting');
  try { show(await current.connect()); message('loaded', 'success'); event('eventConnected'); }
  catch (error) { if (session === current) session = null; clear(); throw error; }
});
$('refresh').onclick = () => operation(async () => {
  if (changed().length && !window.confirm(t('discard'))) return;
  show(await session.query()); message('loaded', 'success'); event('refreshed');
});
$('wake').onclick = () => operation(async () => {
  show(await session.wake(), { preserveDraft: true }); message('wakeSent', 'success'); event('wakeSent');
});
$('settings').onsubmit = e => {
  e.preventDefault(); if (busy || !confirmed || !$('settings').reportValidity()) return;
  void operation(async () => {
    const requested = values(), edits = changed(); if (!edits.length) return;
    let saving = '';
    try {
      for (const key of edits) { saving = key; show(await session.set(key, requested[key])); event('eventSaved', { setting: key }); }
      message('saved', 'success');
    } catch (error) {
      confirmed = null; readAt = null; renderStatus();
      try { if (session?.online) show(await session.query()); } catch {}
      message('saveFailed', 'error', { setting: saving, error }); event('eventFailed');
    }
  });
};
for (const key of keys) $(key).addEventListener('input', update);
for (const button of document.querySelectorAll('[data-sleep]')) button.onclick = () => { $('sleep').value = button.dataset.sleep; update(); };
$('language').onchange = () => setLanguage($('language').value);
window.addEventListener('pagehide', () => { void session?.close(); });
clear(); setLanguage(DEFAULT_LANGUAGE);
if (!window.isSecureContext || !navigator.serial) message('unsupported', 'error');
