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
import { readFile } from 'node:fs/promises';
import { flattenStatus, statusValue } from './serial_status.mjs';
import { DEFAULT_LANGUAGE, TEXT, translate, errorText } from './serial_i18n.mjs';

test('every known status field including read-only nested fields is preserved', () => {
  const status = {board:'SpotPear',chat:false,device:{name:'Muse',fw:'test',battery:-1,
    wifi:{on:true,state:'connected',ssid:'日本語SSID',ip:'192.0.2.1',rssi:-54},
    hatch:{host:'example.test',vm:'',token:true,state:'Connected'},link:{paired:true,state:'ready'},
    volume:0,speaker:false,mic_gain:30,brightness:100,sleep:0,last:'ready'}};
  const rows=flattenStatus(status), values=Object.fromEntries(rows.map(row=>[row.path,row.value]));
  assert.equal(rows.length,22);
  for(const row of rows) assert.ok(TEXT[`field.${row.path}`],row.path);
  assert.equal(values.chat,false);assert.equal(values['device.volume'],0);
  assert.equal(values['device.speaker'],false);assert.equal(values['device.hatch.vm'],'');
  assert.equal(values['device.wifi.ssid'],'日本語SSID');
  assert.equal(values['device.hatch.token'],true);
});
test('future unknown fields, null and empty arrays/objects remain visible', () => {
  assert.deepEqual(flattenStatus({new:{count:0,flags:[false,null,''],empty:[],object:{}}}),[
    {path:'new.count',value:0},{path:'new.flags[0]',value:false},{path:'new.flags[1]',value:null},
    {path:'new.flags[2]',value:''},{path:'new.empty',value:[]},{path:'new.object',value:{}},
  ]);
});
test('values are not translated, coerced, or interpreted as markup', () => {
  for(const [value, expected] of [[false,'false'],[0,'0'],[null,'null'],[[],'[]'],[{},'{}'],['<img src=x>','<img src=x>']]) assert.equal(statusValue(value),expected);
});
test('default is always English; each message has both translations', () => {
  assert.equal(DEFAULT_LANGUAGE,'en');
  for(const [key,pair] of Object.entries(TEXT)) { assert.equal(pair.length,2,key);for(const v of pair)assert.ok(v.trim(),key); }
  assert.equal(translate('en','connect'),'Connect via USB');
  assert.equal(translate('ja','connect'),'USBで接続');
  assert.equal(translate('fr','connect'),'Connect via USB');
});
test('dynamic messages and errors can be rendered in either language', () => {
  assert.equal(translate('en','dirty',{count:2}),'2 unsaved setting(s)');
  assert.equal(translate('ja','dirty',{count:2}),'2項目の変更が未保存です');
  assert.match(errorText('en',{code:'TIMEOUT'}),/No response/);
  assert.match(errorText('ja',{code:'TIMEOUT'}),/応答/);
  assert.match(errorText('en',new SyntaxError()),/invalid Muse status/);
});
test('all static UI labels reference defined translations and English markup', async () => {
  const html=await readFile(new URL('serial_setup.html',import.meta.url),'utf8');
  assert.match(html,/<html lang="en">/);
  assert.match(html,/<option value="en" selected>/);
  for(const [,key] of html.matchAll(/data-i18n(?:-aria)?="([^"]+)"/g)) assert.ok(TEXT[key],key);
});
test('every integer setting the firmware stores loads without rounding', async () => {
  // A coarser step would snap a stored value (e.g. mic_gain 32) and turn it
  // into a phantom edit that the next save writes back.
  const html=await readFile(new URL('serial_setup.html',import.meta.url),'utf8');
  for(const id of ['brightness','sleep','volume','mic_gain']) assert.match(html,new RegExp(`<input id="${id}"[^>]*\\bstep="1"`),id);
});
