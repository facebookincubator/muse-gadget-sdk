# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
# http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Execute the real discovery builder/guide dispatch under sanitizers and OOM."""
import json
import importlib.util
import sys
import types
from unittest import mock
from pathlib import Path
import re
import tempfile
import unittest
# Discovery imports this module as test_apps_capabilities, while focused
# runs import tests.test_apps_capabilities. Resolve the sibling by filename
# so neither mode depends on a tests package or a third-party package of
# the same name.
_helper_spec = importlib.util.spec_from_file_location(
    'muse_device_description_test_helpers',
    Path(__file__).with_name('test_link_device_description.py'))
device_description = importlib.util.module_from_spec(_helper_spec)
_helper_spec.loader.exec_module(device_description)
function_source = device_description.function_source
NOISE = device_description.NOISE

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'components/muse_apps/muse_apps_capabilities.c').read_text()
API_TEXT = (ROOT / 'components/muse_apps/sdk/api/api.capabilities.json').read_text()
RUNTIME = (ROOT / 'components/muse_apps/muse_apps.c').read_text()


def prelude():
    # Only platform includes and linker labels are replaced; execute all builder logic.
    body = re.sub(r'^#include "[^\n]*\n', '', SOURCE, flags=re.M)
    body = re.sub(r'^extern const uint8_t api_capabilities_json_.*\n', '', body, flags=re.M)
    return r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
''' + '#include "' + str(ROOT / 'components/capabilities_registry/include/capabilities_registry.h') + '"\n' + r'''
#define nullptr NULL
#define MUSE_APPS_API_MAJOR 1
#define MUSE_APPS_API_MINOR 1
#define MUSE_APP_BUTTON_BACK 8
static const char api_text[] = ''' + json.dumps(API_TEXT) + r''';
#define api_capabilities_json_start ((const uint8_t *)api_text)
#define api_capabilities_json_end ((const uint8_t *)api_text + sizeof(api_text))
static const struct { const char *name; bool touch; } board = {"Test board", true}, *muse_board = &board;
static bool round_screen;
static bool api_only;
static int device_count = 1;
static size_t budget = 1310720;
static void muse_app_view_size(int *w, int *h, bool *round) { *w = 240; *h = 240; *round = round_screen; }
static unsigned muse_app_view_buttons_present(void) { return (1u << 1) | (1u << 2); }
static int muse_app_view_exit_button(void) { return 1; }
static size_t muse_apps_mem_budget(void) { assert(!api_only); return budget; }
static const char *muse_apps_device_dir(void) { return "workspace/gadgets/devices/test"; }
static bool muse_settings_apps_on(void) { return true; }
static int forbidden_read(const capability_t *d, float *v) { (void)d; (void)v; abort(); }
static const capability_field_t fields[] = {{"value", "V", 1, 5}, {"raw", NULL, 0, 0}};
static const capability_action_t actions[] = {{"set", "Set a quoted \"value\"", fields, 1, true}};
static const char *events[] = {"changed"};
static const capability_t device = {
    .name="test_sensor", .desc="A sensor", .fields=fields, .n_fields=2, .rate=10,
    .actions=actions, .n_actions=1, .events=events, .n_events=1, .read=forbidden_read,
};
static int capabilities_registry_count(void) { assert(!api_only); return device_count; }
static const capability_t *capabilities_registry_at(int i) { (void)i; return &device; }
''' .replace('static int capabilities_registry_count', 'int capabilities_registry_count').replace('static const capability_t *capabilities_registry_at', 'const capability_t *capabilities_registry_at') + function_source(RUNTIME, 'int muse_apps_safe_inset(') + '\n' + function_source(RUNTIME, 'cJSON *muse_apps_error(') + '\n' + body


class CapabilitiesTest(unittest.TestCase):
    def run_c(self, harness):
        with tempfile.TemporaryDirectory() as tmp:
            result = device_description.DeviceDescriptionTest().compile_and_run(Path(tmp), harness)
        return [json.loads(line) for line in result.splitlines()]

    def test_shared_schema_and_live_facts_without_hardware_reads(self):
        result = self.run_c(prelude() + r'''
int main(void) {
    for (int i=0; i<2; i++) {
        round_screen = i; device_count = i ? 0 : 1; budget = i ? 0 : 1310720;
        cJSON *d = cJSON_CreateObject(); assert(muse_apps_add_capabilities(d));
        char *s = cJSON_PrintUnformatted(d); puts(s); free(s); cJSON_Delete(d);
    }
}
''')
        first = result[0]['capabilities']
        self.assertEqual(set(first), {'board', 'devices', 'wasm'})
        self.assertEqual(first['board']['buttons'], ['primary', 'secondary'])
        self.assertEqual(first['board']['exit_hold'], 'primary')
        self.assertEqual(first['board']['safe_inset'], 0)
        self.assertEqual(first['wasm']['budget_kb'], 1280)
        self.assertEqual(first['wasm']['api_command'], 'app.api')
        self.assertEqual(first['wasm']['api_file'], 'api.capabilities.json')
        self.assertEqual(first['wasm']['api_parts'], 3)
        self.assertTrue(first['wasm']['enabled'])
        self.assertEqual(first['devices'][0]['actions'][0]['params'][0]['max'], 5)
        self.assertNotIn('max', first['devices'][0]['fields'][1])
        self.assertEqual(result[1]['capabilities']['devices'], [])
        self.assertEqual(result[1]['capabilities']['board']['safe_inset'], 35)
        self.assertEqual(result[1]['capabilities']['wasm']['budget_kb'], 0)

    def test_chunked_api_and_invalid_parts(self):
        result = self.run_c(prelude() + r'''
int main(void) {
    api_only = true;
    const char *requests[] = {"{\"part\":0}", "{\"part\":1}", "{\"part\":2}",
        "{\"part\":-1}", "{\"part\":3}", "{\"part\":1.5}", "{\"part\":null}",
        "{\"part\":\"1\"}", "{\"part\":1e40}", "{}"};
    for (size_t i=0; i<sizeof(requests)/sizeof(requests[0]); i++) {
        cJSON *p=cJSON_Parse(requests[i]); cJSON *r=muse_apps_api_command(p);
        char *s=cJSON_PrintUnformatted(r); puts(s); free(s); cJSON_Delete(p); cJSON_Delete(r);
    }
}
''')
        chunks = result[:3]
        self.assertEqual([r['payload']['part'] for r in chunks], [0, 1, 2])
        self.assertTrue(all(len(r['payload']['text'].encode()) <= 6000 for r in chunks))
        self.assertEqual(''.join(r['payload']['text'] for r in chunks), API_TEXT.rstrip('\n\0'))
        for r in result[3:]:
            self.assertFalse(r['ok'])
            self.assertEqual(r['error']['code'], 'invalid_param')

    def test_guide_and_discovery_allocation_failures(self):
        # Fail every cJSON allocation once. A successful response must be complete;
        # failed descriptions must not attach partially filled JSON to the guide.
        result = self.run_c(prelude() + r'''
typedef uint64_t noise_ctrl_session_generation_t;
const char muse_device_guide_md[] = "# Test\n\nA \"quoted\" guide.\n";
static cJSON *queued;
static int allocations, fail_at, live;
static void *test_malloc(size_t n) {
    if (++allocations == fail_at) return NULL;
    void *p=malloc(n); if(p) live++; return p;
}
static void test_free(void *p) { if(p) { live--; free(p); } }
static void queue_result(noise_ctrl_session_generation_t g, const char *id, cJSON *r) {
    assert(g==7 && !strcmp(id,"req-1") && !queued); queued=r;
}
#define CONFIG_MUSE_WASM_APPS 1
''' + function_source(NOISE, 'static void send_device_guide(') + r'''
int main(void) {
    cJSON_Hooks hooks={test_malloc,test_free}; cJSON_InitHooks(&hooks);
    for (int mode=0; mode<2; mode++) {
        int count=0;
        for (int n=0; n<=count; n++) {
            allocations=0; fail_at=n;
            cJSON *params=NULL;
            if(mode==0) send_device_guide(7,"req-1");
            else {
                fail_at=0; params=cJSON_Parse("{\"part\":0}"); allocations=0; fail_at=n;
                queued=muse_apps_api_command(params);
            }
            if(!n) count=allocations;
            fail_at=0;
            char *s=cJSON_PrintUnformatted(queued); assert(s); puts(s); cJSON_free(s);
            cJSON_Delete(queued); queued=NULL; cJSON_Delete(params); assert(live==0);
        }
    }
}
''')
        successes = [r for r in result if r['ok']]
        guides = [r for r in successes if 'guide' in r['payload']]
        self.assertTrue(guides)
        self.assertEqual(guides[0]['payload']['capabilities']['wasm']['api_command'], 'app.api')
        for r in successes:
            if 'guide' not in r['payload']:
                self.assertEqual(set(r['payload']), {'part', 'parts', 'text'})
        for r in guides:
            self.assertEqual(r, guides[0])
        errors = [r for r in result if not r['ok']]
        self.assertTrue(errors)
        self.assertTrue(all(r['error']['code'] == 'out_of_memory' for r in errors))


    def test_directory_initialization_is_once_and_publishes_complete_text(self):
        harness = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_HOMEHUB_BLE_NAME_PREFIX "Test"
#define CONFIG_HOMEHUB_BLE_NAME_SUFFIX "-Dev"
#define ESP_MAC_WIFI_STA 0
static char s_device_dir[80];
static pthread_once_t s_device_dir_once = PTHREAD_ONCE_INIT;
static atomic_int reads;
static void esp_read_mac(uint8_t *mac, int interface) {
    (void)interface; atomic_fetch_add(&reads, 1);
    for(int i=0; i<1000; i++) sched_yield();
    mac[3]=1; mac[4]=2; mac[5]=3;
}
''' + function_source(RUNTIME, 'static void init_device_dir(') + '\n' + function_source(RUNTIME, 'const char *muse_apps_device_dir(') + r'''
static void *reader(void *unused) {
    (void)unused;
    assert(!strcmp(muse_apps_device_dir(), "workspace/gadgets/devices/Test-Dev-010203"));
    return NULL;
}
int main(void) {
    pthread_t threads[32];
    for(int i=0; i<32; i++) assert(!pthread_create(&threads[i], NULL, reader, NULL));
    for(int i=0; i<32; i++) assert(!pthread_join(threads[i], NULL));
    assert(atomic_load(&reads)==1);
    puts("{}");
}
'''
        self.run_c(harness)

    def load_usb_tool(self):
        spec = importlib.util.spec_from_file_location('apps_usb_api_test', ROOT / 'tools/muse/app.py')
        tool = importlib.util.module_from_spec(spec)
        chat = types.SimpleNamespace(Board=None, BoardError=RuntimeError, pick_port=lambda: None)
        with mock.patch.dict(sys.modules, {'chat': chat}):
            spec.loader.exec_module(tool)
        return tool

    def test_usb_tool_fetches_only_api_parts(self):
        tool = self.load_usb_tool()
        text = API_TEXT.rstrip('\n\0')
        pieces = [text[i:i+6000] for i in range(0, len(text), 6000)]
        calls = []
        def command(board, line):
            calls.append(line)
            k = int(line.split('=')[1])
            return {'ok': True, 'payload': {'part': k, 'parts': len(pieces), 'text': pieces[k]}}
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(tool, 'command', command):
            out = Path(tmp) / 'api.capabilities.json'
            result = tool.fetch_api(None, str(out))
            self.assertEqual(json.loads(out.read_text()), json.loads(API_TEXT))
        self.assertEqual(calls, ['app.api=0', 'app.api=1', 'app.api=2'])
        self.assertEqual(set(result['payload']), {'api', 'parts', 'bytes', 'functions', 'written'})
        self.assertEqual(result['payload']['functions'], 80)

    def test_usb_tool_rejects_malformed_api_without_writing(self):
        tool = self.load_usb_tool()
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / 'api.capabilities.json'
            for count in (0, -1, '3', True, 257):
                with mock.patch.object(tool, 'command', return_value={
                        'ok': True, 'payload': {'part': 0, 'parts': count, 'text': '{}'}}):
                    with self.assertRaises(RuntimeError):
                        tool.fetch_api(None, str(out))
                self.assertFalse(out.exists())
            responses = [
                {'ok': True, 'payload': {'part': 0, 'parts': 2, 'text': '{'}},
                {'ok': True, 'payload': {'part': 0, 'parts': 2, 'text': '}'}}]
            with mock.patch.object(tool, 'command', side_effect=responses):
                with self.assertRaises(RuntimeError):
                    tool.fetch_api(None, str(out))
            self.assertFalse(out.exists())


if __name__ == '__main__':
    unittest.main()
