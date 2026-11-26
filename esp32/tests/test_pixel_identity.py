# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Pinned canonical image/key pairs remain consistent across a newer identity."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest
from test_avatar_context import function
from pixel_test_sanitizers import ASAN_OPTIONS

ROOT = Path(__file__).resolve().parents[1]


class PixelIdentity(unittest.TestCase):
    def test_pinned_image_key_is_not_the_new_global_identity(self):
        source = (ROOT / 'main/avatar_sync.c').read_text()
        image_type = re.search(r'typedef struct \{ muse_avatar_image_t view;[^\n]+pixel_key_valid; \} image_t;', source).group()
        helper = function(source, 'bool muse_avatar_copy_image_key(')
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "muse_avatar.h"
'''
        main = r'''
int main(void) {
 image_t a={0},b={0};uint8_t key[32];
 memset(a.pixel_key,0x11,32);memset(b.pixel_key,0x22,32);
 a.pixel_key_valid=b.pixel_key_valid=true;
 assert(muse_avatar_copy_image_key(&a.view,key));assert(key[0]==0x11);
 /* A remains pinned while B is published: exporting A never labels it B. */
 const muse_avatar_image_t *current=&b.view;(void)current;
 assert(muse_avatar_copy_image_key(&a.view,key));assert(key[0]==0x11);
 assert(muse_avatar_copy_image_key(&b.view,key));assert(key[0]==0x22);
 a.pixel_key_valid=false;assert(!muse_avatar_copy_image_key(&a.view,key));
 assert(!muse_avatar_copy_image_key(NULL,key));assert(!muse_avatar_copy_image_key(&b.view,NULL));
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp)
            (stage / 'sdkconfig.h').write_text('#define CONFIG_MUSE_PIXEL_AVATAR 1\n')
            (stage / 'test.c').write_text(harness + image_type + '\n' + helper + main)
            exe = stage / 'identity-test'
            subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-O1', '-g',
                            '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-I', str(stage), '-I', str(ROOT / 'components/muse'),
                            str(stage / 'test.c'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True,
                           env={**os.environ, 'ASAN_OPTIONS': ASAN_OPTIONS})

    def test_validated_metadata_invalidates_before_downloading_still(self):
        source = (ROOT / 'main/avatar_sync.c').read_text()
        update = function(source, 'static bool sync_once(')
        self.assertLess(update.index('pixel_authoritative_identity(&identity,content_epoch)'), update.index('fetch_webp('))
        self.assertIn('same_key', update)
        self.assertIn('set_image_pixel_key(image,&identity)', update)


if __name__ == '__main__':
    unittest.main()
