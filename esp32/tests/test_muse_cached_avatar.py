"""Exercise the real expression clock and cache, with the panel held up or clipped."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / "tests"
MUSE = ROOT / "components/muse"
CC = os.environ.get("CC", "cc")


class CachedAvatar(unittest.TestCase):
    def test_presentation_clock(self):
        with tempfile.TemporaryDirectory() as d:
            exe = Path(d)/"clock"
            subprocess.run([CC, "-std=c11", "-Wall", "-Werror", "-I", MUSE, MUSE/"muse_expression.c",
                            TESTS/"muse_expression_harness.c", "-o", exe], check=True)
            subprocess.run([exe], check=True)

    @unittest.skipUnless(shutil.which("magick"), "art reference requires ImageMagick")
    def test_cache_and_registered_patches(self):
        spec = importlib.util.spec_from_file_location("gen", ROOT/"tools/gen_muse_3d_assets.py")
        gen = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(gen)
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp/"esp_timer.h").write_text("""
#include <stdint.h>
static int64_t esp_timer_get_time(void) { static int64_t t; return t+=1000; }
""")
            (tmp/"esp_log.h").write_text('#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGE(...) ((void)0)\n')
            (tmp/"esp_heap_caps.h").write_text("""
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
extern int allocations, fail_at, live_allocations;
static void *heap_caps_malloc(size_t n, unsigned caps) {
    if (++allocations == fail_at) return NULL;
    live_allocations++; return malloc(n);
}
static void heap_caps_free(void *p) { if (p) live_allocations--; free(p); }
""")
            blob = tmp/"muse3d.bin"
            shutil.copyfile(ROOT/"avatar/3d/muse3d.bin", blob)
            (tmp/"art.S").write_text(f'.data\n.global _binary_muse3d_bin_start\n_binary_muse3d_bin_start:\n'
                                     f'.incbin "{blob}"\n.global _binary_muse3d_bin_end\n_binary_muse3d_bin_end:\n')
            exe = tmp/"cache"
            compile_cache = [CC, "-std=gnu11","-fsanitize=address,undefined","-I",tmp,"-I",MUSE,
                            ROOT/"avatar/muse_3d.c", TESTS/"muse_3d_harness.c",tmp/"art.S","-o",exe]
            subprocess.run(compile_cache, check=True)
            for fail_at in range(1,6):
                subprocess.run([exe,str(fail_at)],check=True)
            out = subprocess.check_output([exe])
            manifest = json.loads((gen.ART/"manifest.json").read_text())
            sources = {n:gen.load(n) for n in (*gen.POSES, "closed-eyes")}
            offset = 0
            for size in (320,173):
                for name in reversed(gen.POSES):
                    base, stages = gen.expressions(name,sources,manifest)
                    for image in [*stages.values(),base]:
                        native = gen.rgb565(image)
                        expected = native
                        if size != 320:
                            expected = b"".join(native[i:i+2] for y in range(size) for x in range(size)
                                                for i in [2*((y*320//size)*320+x*320//size)])
                        with self.subTest(name=name,size=size,offset=offset):
                            self.assertTrue(out[offset:offset+len(expected)] == expected)
                        offset += len(expected)
            self.assertEqual(offset,len(out))
            with blob.open("r+b") as f:
                f.write(bytes(2))  # illegal zero-length packet in boot; idle is still valid
            subprocess.run(compile_cache, check=True)
            subprocess.run([exe, "bad-boot"], check=True)
