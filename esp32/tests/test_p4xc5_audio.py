"""Check the board's real PCM writer at the codec boundary without hardware."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    start = source.rfind('static ', 0, source.index(name + '('))
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class P4XC5AudioTest(unittest.TestCase):
    def test_existing_boards_keep_default_codec_path(self):
        source = (ROOT / 'components/muse/muse_audio.c').read_text(encoding='utf-8')
        code = r'''
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
typedef void *esp_codec_dev_handle_t;
typedef int esp_err_t;
typedef struct { int sample_rate, channel, bits_per_sample; } esp_codec_dev_sample_info_t;
#define MUSE_AUDIO_RATE 16000
#define CHANNELS 2
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_CODEC_DEV_OK 0
#define ESP_RETURN_ON_ERROR(expr, tag, msg) do { int e = (expr); if (e) return e; } while (0)
#define ESP_RETURN_ON_FALSE(expr, err, tag, msg) do { if (!(expr)) return (err); } while (0)
static struct {
    int (*audio_read)(void *, void *, int);
    int (*audio_write)(void *, void *, int);
    int (*audio_open)(void *, void *);
} board;
static void *s_spk, *s_mic;
static bool s_open;
static const __typeof__(board) *muse_board = &board;
static int opens, reads, writes, fail_open;
static int esp_codec_dev_open(const void *dev, esp_codec_dev_sample_info_t *fs) {
    assert(dev == s_spk || dev == s_mic);
    assert(fs->sample_rate == 16000 && fs->channel == 2 && fs->bits_per_sample == 16);
    ++opens; return fail_open;
}
static int esp_codec_dev_read(void *dev, void *pcm, int bytes) { (void)dev; (void)pcm; ++reads; return bytes; }
static int esp_codec_dev_write(void *dev, void *pcm, int bytes) { (void)dev; (void)pcm; ++writes; return bytes; }
static int custom_open(void *spk, void *mic) { assert(spk == s_spk && mic == s_mic); return 7; }
static int custom_io(void *dev, void *pcm, int bytes) { (void)dev; (void)pcm; return bytes + 1; }
'''
        code += '\n'.join(function(source, name) for name in ('codec_read', 'codec_write', 'open_codecs'))
        code += r'''
int main(void) {
    s_spk = (void *)1; s_mic = (void *)2;
    assert(open_codecs() == 0 && s_open && opens == 2);
    assert(codec_read(s_mic, NULL, 4) == 4 && reads == 1);
    assert(codec_write(s_spk, NULL, 4) == 4 && writes == 1);
    s_open = false; fail_open = 1;
    assert(open_codecs() == 1 && !s_open && opens == 3);
    board.audio_open = custom_open; board.audio_read = custom_io; board.audio_write = custom_io;
    assert(open_codecs() == 7 && !s_open && opens == 3);
    assert(codec_read(s_mic, NULL, 4) == 5 && reads == 1);
    assert(codec_write(s_spk, NULL, 4) == 5 && writes == 1);
    return 0;
}
'''
        self.compile_and_run(code)

    def compile_and_run(self, code):
        with tempfile.TemporaryDirectory() as tmp:
            c = Path(tmp) / 'audio.c'
            exe = Path(tmp) / 'audio.exe'
            c.write_text(code, encoding='utf-8')
            subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
                           ['-std=c11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_codec_format_and_pcm_frames(self):
        source = (ROOT / 'components/muse/boards/board_ksdiy_p4xc5.c').read_text(encoding='utf-8')
        code = r'''
#include <stdint.h>
#include <assert.h>
#include <stddef.h>
#include <string.h>
typedef void *esp_codec_dev_handle_t;
typedef int esp_err_t;
typedef struct { int sample_rate, bits_per_sample, channel, channel_mask; } esp_codec_dev_sample_info_t;
#define MUSE_AUDIO_RATE 16000
#define ESP_CODEC_DEV_MAKE_CHANNEL_MASK(n) (1 << (n))
#define ESP_CODEC_DEV_OK 0
#define ESP_ERR_INVALID_ARG 3
static int calls, count, fail_at;
static int32_t captured[1024];
static int audio_codec_set_fs(esp_codec_dev_sample_info_t *tx, esp_codec_dev_sample_info_t *rx) {
    assert(tx->sample_rate == 16000 && rx->sample_rate == 16000);
    assert(tx->channel == 2 && tx->bits_per_sample == 32);
    assert(rx->channel == 4 && rx->bits_per_sample == 16 && rx->channel_mask == 5);
    return 17;
}
static int esp_codec_dev_write(void *dev, void *pcm, int bytes) {
    assert(dev == (void *)1 && bytes % 8 == 0 && bytes <= 1024);
    ++calls;
    if (calls == fail_at) return 29;
    memcpy(captured + count, pcm, bytes);
    count += bytes / 4;
    return 0;
}
'''
        code += '\n' + function(source, 'audio_open') + '\n' + function(source, 'audio_write')
        code += r'''
int main(void) {
    assert(audio_open(NULL, NULL) == 17);
    int16_t pcm[600];
    for (int i = 0; i < 600; ++i) pcm[i] = (int16_t)(i * 113 - 32768);
    pcm[0] = INT16_MIN; pcm[1] = INT16_MAX; pcm[2] = -1; pcm[3] = 0;
    assert(audio_write((void *)1, pcm, sizeof(pcm)) == 0);
    assert(calls == 3 && count == 600);
    for (int i = 0; i < 600; ++i) assert(captured[i] == (int32_t)pcm[i] * 65536);
    assert(audio_write((void *)1, NULL, 4) == 3);
    assert(audio_write((void *)1, pcm, 0) == 3);
    assert(audio_write((void *)1, pcm, 2) == 3);
    assert(audio_write((void *)1, pcm, -4) == 3);
    assert(calls == 3);
    calls = count = 0; fail_at = 2;
    assert(audio_write((void *)1, pcm, sizeof(pcm)) == 29);
    assert(calls == 2 && count == 256);
    return 0;
}
'''
        self.compile_and_run(code)
