# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Real pixel backend plus real config_store transactions on fake private NVS.

The filesystem/locks/scheduler harness is shared with test_pixel_backend; NVS
writes/markers/rollback execute production config_store.c. Digests are fake,
not cryptographic evidence. No real keys, private assets, or devices are read.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
from test_avatar_context import function
from test_pixel_art import pack_bytes
from test_pixel_backend import FAKES, HARNESS
from pixel_test_sanitizers import ASAN_OPTIONS

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / 'components/muse'

JOURNAL_MAIN = r'''
static void step(void) {worker_state_t state={1,0};worker_step(&state);}
static void journal_failed_write(void) {
    s_fail_set="access_token";s_fail_set_once=true;
    assert(!config_set_token_pair("candidate-access","candidate-refresh",0));
    s_fail_set=NULL;
    assert(!config_reprovision_open());assert(!strcmp(get("refresh_token"),"synthetic-credential"));
    disk_is(asset_a);step();live_is(asset_a);
}
int main(int argc,char **argv) {
    assert(argc==3);const char *which=argv[1];
    read_asset(argv[2],&asset_a,&asset_size);asset_b=malloc(asset_size);assert(asset_b);memcpy(asset_b,asset_a,asset_size);
    memcpy(key_a,asset_a+24,32);memcpy(key_b,key_a,32);key_b[0]^=1;
    reset();put("access_token","synthetic-access");put("refresh_token","synthetic-credential");
    put("node_id","synthetic-node");put("setup_complete","1");put("pixel_style","1");seed();
    if(!strcmp(which,"rp_defer")) {
        assert(config_reprovision_begin());assert(!s_have_owner && !muse_pixel_avatar_acquire());
        assert(config_set_str("refresh_token","provisional-refresh"));assert(config_set_str("node_id","provisional-node"));
        int before=raw_owner_reads;step();assert(raw_owner_reads==before && !s_model);
        /* Any marker presence (even 0/malformed) means raw state is unsafe. */
        put("rp_open","0");step();assert(raw_owner_reads==before && !s_model);
        put("rp_open","1");assert(config_reprovision_rollback());disk_is(asset_a);step();live_is(asset_a);
    } else if(!strcmp(which,"lookup_errors")) {
        const char *keys[]={"rp_open","tok_update"};
        for(unsigned i=0;i<2;i++) {
            muse_pixel_avatar_invalidate();s_fail_lookup=keys[i];int before=raw_owner_reads;
            step();assert(raw_owner_reads==before && !s_model);disk_is(asset_a);
            s_fail_lookup=NULL;step();live_is(asset_a);
        }
    } else if(!strcmp(which,"legacy_defer")) {
        assert(config_set_str("tok_update","malformed-but-present"));assert(!s_have_owner);
        int before=raw_owner_reads;step();assert(raw_owner_reads==before && !s_model);
        assert(config_erase_key("tok_update"));step();live_is(asset_a);disk_is(asset_a);
    } else if(!strcmp(which,"overlap")) {
        const char *keys[]={"rp_open","tok_update"};
        for(unsigned i=0;i<2;i++) {
            arm(H_OWNER);struct job w={{1,0},false,false};pthread_t t;
            assert(!pthread_create(&t,NULL,worker_job,&w));wait_hit(H_OWNER);unsigned before=s_epoch;
            assert(config_set_str(keys[i],"1"));unsigned opened=s_epoch;assert(opened!=before);
            assert(config_erase_key(keys[i]));assert(s_epoch!=opened && !s_have_owner);
            unblock();assert(!pthread_join(t,NULL));assert(!s_have_owner && !s_model);disk_is(asset_a);
            step();live_is(asset_a);
        }
    } else if(!strcmp(which,"after_probe")) {
        /* Journal begins during the final owner-key read, remains open through
         * the after-check, and no provisional owner can be published. */
        arm(H_OWNER);struct job w={{1,0},false,false};pthread_t t;
        assert(!pthread_create(&t,NULL,worker_job,&w));wait_hit(H_OWNER);
        assert(config_reprovision_begin());assert(config_set_str("refresh_token","provisional"));
        unblock();assert(!pthread_join(t,NULL));assert(!s_have_owner && !s_model);
        assert(config_reprovision_rollback());step();live_is(asset_a);disk_is(asset_a);
    } else if(!strcmp(which,"failed_update")) {
        journal_failed_write();
        /* Failed rollback retains rp_open. Its old committed readers still
         * return the old pair, while pixel probes defer until recovery. */
        assert(config_reprovision_begin());assert(config_set_str("refresh_token","provisional"));
        s_fail_set="refresh_token";assert(!config_reprovision_rollback());assert(config_reprovision_open());
        char at[128],rt[128];assert(config_get_token_pair(at,sizeof(at),rt,sizeof(rt)));
        assert(!strcmp(rt,"synthetic-credential"));int before=raw_owner_reads;step();assert(raw_owner_reads==before && !s_model);disk_is(asset_a);
        s_fail_set=NULL;assert(config_reprovision_rollback());step();live_is(asset_a);
    } else if(!strcmp(which,"failed_commit")) {
        assert(config_reprovision_begin());assert(config_set_str("refresh_token","provisional"));
        s_fail_erase="rp_open";unsigned before=s_epoch;assert(!config_reprovision_commit());assert(before==s_epoch);
        step();assert(!s_model);disk_is(asset_a);s_fail_erase=NULL;assert(config_reprovision_rollback());step();live_is(asset_a);
    } else if(!strcmp(which,"authority_rollback")) {
        muse_pixel_avatar_identity(key_b);assert(!muse_pixel_avatar_acquire());
        assert(config_reprovision_begin());assert(config_set_str("refresh_token","provisional"));
        assert(config_reprovision_rollback());step();
        assert(s_have_identity && !memcmp(s_identity,key_b,32) && !s_model);disk_is(asset_a);
        muse_pixel_avatar_identity_unavailable();
        s_fail_set="access_token";s_fail_set_once=true;
        assert(!config_set_token_pair("candidate-access","candidate-refresh",0));step();
        assert(s_identity_required && !s_have_identity && !s_model);disk_is(asset_a);
        muse_pixel_avatar_identity(key_a);step();live_is(asset_a);
    } else if(!strcmp(which,"rotation")) {
        assert(config_set_token_pair("committed-new-access","committed-new-refresh",0));
        assert(!config_reprovision_open());step();assert(s_have_owner && !s_model);disk_is(asset_a);
        /* Expected documented v1 limit: successful credential rotation changes
         * the fingerprint, unlike failed update/rollback above. */
    } else assert(!"unknown journal scenario");
    cleanup();free(asset_a);free(asset_b);puts("PASS real config-store marker fences, deferred owner probes, failed rollback preservation");
}
'''


class PixelOwnerJournals(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='pixel-owner-journals-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.stage = Path(cls.tmp.name)
        # Keep production config-store declarations, not the sequential backend
        # fixture's enum/API. All actual writes/erases link to config_store.c.
        fakes = FAKES.replace(
            'typedef enum {CONFIG_KEY_LOOKUP_ERROR=-1,CONFIG_KEY_NOT_FOUND=0,CONFIG_KEY_FOUND=1} config_key_lookup_t;',
            '#include "config_store.h"')
        (cls.stage / 'fakes.h').write_text(fakes)
        (cls.stage / 'config_store.h').write_text(
            '#include "' + str(ROOT / 'main/config_store.h') + '"\n')
        for name in ('muse_state.h', 'muse_settings.h', 'muse_voice.h', 'esp_heap_caps.h',
                     'esp_timer.h', 'mbedtls/md.h', 'mbedtls/base64.h', 'freertos/FreeRTOS.h',
                     'freertos/task.h', 'freertos/semphr.h', 'freertos/idf_additions.h'):
            p = cls.stage / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text('#include "fakes.h"\n')
        for name in ('muse_avatar.h', 'muse_avatar_render.h', 'muse_pixel_avatar.h', 'muse_pixel_art.h'):
            (cls.stage / name).write_bytes((MUSE / name).read_bytes())
        (cls.stage / 'sdkconfig.h').write_text('#define CONFIG_MUSE_PIXEL_AVATAR 1\n#define CONFIG_MUSE_AVATAR_ANIMATIONS 1\n')
        backend = (ROOT / 'main/pixel_avatar.c').read_text()
        backend = backend.replace('"/files/.muse-avatar-v1/pixel.dat"', '"' + str(cls.stage / 'pixel.dat') + '"')
        backend = backend.replace('"/files/.muse-avatar-v1/pixel.tmp"', '"' + str(cls.stage / 'pixel.tmp') + '"')
        backend = backend.replace('mkdir("/files/.muse-avatar-v1",0700);', '')
        (cls.stage / 'backend.c').write_text(backend)
        common = HARNESS[:HARNESS.index('int main(int argc,char **argv)')]
        for sig in ('bool config_is_provisioned(', 'bool config_setup_complete(',
                    'config_key_lookup_t config_key_lookup(', 'bool config_get_str(', 'bool config_set_str('):
            common = common.replace(function(common, sig), '')
        # Existing private fake NVS machinery; production API/transaction code
        # is linked unchanged rather than reimplemented in the fixture.
        nvs_source = (ROOT / 'tests/link_reprovision_journal_harness.c').read_text()
        nvs = nvs_source[nvs_source.index('#define MAX_ENTRIES'):nvs_source.index('// A set-up token-only gadget')]
        nvs = 'static const char *s_fail_lookup;\nstatic int raw_owner_reads;\nstatic bool s_fail_set_once;\n' + nvs
        nvs = nvs.replace('if (s_fail_set && strcmp(s_fail_set, key) == 0) return ESP_FAIL;',
            'if (s_fail_set && strcmp(s_fail_set, key) == 0) {\n'
            '    if(s_fail_set_once) {s_fail_set=NULL;s_fail_set_once=false;}\n'
            '    return ESP_FAIL;\n}')
        nvs = nvs.replace('    entry_t *e = find(key);\n    if (!e) return ESP_ERR_NVS_NOT_FOUND;',
            '    if(s_fail_lookup && !strcmp(s_fail_lookup,key))return ESP_FAIL;\n'
            '    if(!strcmp(key,"refresh_token") || !strcmp(key,"node_id"))raw_owner_reads++;\n'
            '    entry_t *e = find(key);\n    if (!e) return ESP_ERR_NVS_NOT_FOUND;')
        code = '#include "config_store.h"\n#include "nvs.h"\n#include <setjmp.h>\n' + common + nvs + JOURNAL_MAIN
        (cls.stage / 'test.c').write_text(code)
        (cls.stage / 'a.msp').write_bytes(pack_bytes())
        cls.exe = cls.stage / 'journal-test'
        built = subprocess.run([
            *shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_POSIX_C_SOURCE=200809L',
            '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-variable',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-pthread',
            '-include', str(cls.stage / 'sdkconfig.h'), '-I', str(cls.stage),
            '-I', str(ROOT / 'tests/link_fakes'), '-I', str(ROOT / 'main'), '-I', str(MUSE),
            str(cls.stage / 'test.c'), str(ROOT / 'main/config_store.c'),
            str(MUSE / 'muse_pixel_art.c'), '-o', str(cls.exe),
        ], capture_output=True, text=True)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    def run_case(self, case):
        for name in ('pixel.dat', 'pixel.tmp'):
            (self.stage / name).unlink(missing_ok=True)
        ran = subprocess.run([str(self.exe), case, str(self.stage / 'a.msp')],
            capture_output=True, text=True, timeout=30,
            env={**os.environ, 'ASAN_OPTIONS': ASAN_OPTIONS,
                 'UBSAN_OPTIONS': 'halt_on_error=1'})
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn('PASS real config-store', ran.stdout)

    def test_provisional_and_noncanonical_reprovision_marker_defers_raw_owner_reads(self):
        self.run_case('rp_defer')

    def test_unreadable_journal_markers_fail_closed_without_hashing_raw_keys(self):
        self.run_case('lookup_errors')

    def test_present_legacy_token_journal_is_not_hashed_or_applied_by_pixel_probe(self):
        self.run_case('legacy_defer')

    def test_journal_start_and_finish_entirely_inside_probe_fence_old_snapshot(self):
        self.run_case('overlap')

    def test_journal_start_during_probe_blocks_after_read_publication(self):
        self.run_case('after_probe')

    def test_failed_token_update_and_partly_failed_rollback_preserve_disk_and_old_owner(self):
        self.run_case('failed_update')

    def test_failed_commit_does_not_pretend_journal_completed(self):
        self.run_case('failed_commit')

    def test_failed_transactions_cannot_erase_authoritative_mismatch_or_digest_failure(self):
        self.run_case('authority_rollback')

    def test_committed_rotation_keeps_disk_but_requires_documented_owner_reinstallation(self):
        self.run_case('rotation')


if __name__ == '__main__':
    unittest.main()
