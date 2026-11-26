# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""The voice note upload: muse_chat_text.c's WAV header and base64, and the
POST body muse_chat_session.cpp streams from them, against CPython's struct,
wave, base64 and json."""

import base64
import io
import json
import os
import random
import shlex
import struct
import subprocess
import tempfile
import unittest
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / 'components/muse'
SANITIZE = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
ENV = {**os.environ,
       'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1',
       'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
GUARD = b'\xa5' * 16   # muse_voice_note_harness.c


def riff_header(rate):
    """The canonical 44-byte PCM WAVE header (Microsoft RIFF spec, 1991), mono
    16-bit. Both sizes are 0xFFFFFFFF: the note streams before its length is
    known, so the firmware sends the "unknown length" convention on purpose."""
    return struct.pack('<4sI4s4sIHHIIHH4sI', b'RIFF', 0xFFFFFFFF, b'WAVE', b'fmt ',
                       16, 1, 1, rate, rate * 2, 2, 16, b'data', 0xFFFFFFFF)


# The POST /chat/stream envelope around the base64 WAV, written out rather than
# read from muse_chat_priv.h: changing what the device sends must change this.
HEAD = (b'{"node_id":"homelink-0a1b2c","message":"","output_modality":"text","device":"mcu-wearable",'
        b'"items":[{"type":"file",'
        b'"mime_type":"audio/wav","filename":"voice_note.wav","data_base64":"')
TAIL = b'"}]}'


def pcm_bytes(n, seed):
    return random.Random(seed).randbytes(n)


def expected_parts(wav_len, part_pcm, part_chars):
    """Chunk lengths of a note request: the head, a full chunk per whole part,
    then the remainder's base64 and the tail."""
    full, rest = divmod(wav_len, part_pcm)
    return [len(HEAD)] + [part_chars] * full + [(rest + 2) // 3 * 4 + len(TAIL)]


def compile_c(out, sources):
    cc = shlex.split(os.environ.get('CC', 'cc'))
    result = subprocess.run(
        [*cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', *SANITIZE,
         '-include', str(ROOT / 'tests/host_compat.h'), '-I', str(MUSE),
         *map(str, sources), '-o', str(out)],
        capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)


class NoteRequestAssertions:
    """Checks a captured request against a body built from CPython's base64."""

    def assert_request(self, run, pcm, part_pcm, part_chars):
        wav = riff_header(16000) + pcm
        self.assertEqual(run['body'], HEAD + base64.b64encode(wav) + TAIL)
        self.assertEqual(run['parts'], expected_parts(len(wav), part_pcm, part_chars))
        self.assertEqual(run['ends'], [False] * (len(run['parts']) - 1) + [True])
        if run['body_sent'] is not None:
            self.assertEqual(run['body_sent'], len(run['body']))
        at = len(HEAD)
        for size in run['parts'][1:-1]:
            chunk = run['body'][at:at + size]
            self.assertEqual(len(chunk), part_chars)
            self.assertNotIn(b'=', chunk)
            at += size


def parse_run(stdout):
    meta, body = stdout.split(b'\n', 1)
    fields = dict(item.split('=', 1) for item in meta.decode().split())
    parts = [p.split(':') for p in fields['parts'].split(',')] if fields['parts'] else []
    return {
        'ok': fields['ok'] == '1',
        'failed': fields['failed'].replace('_', ' '),
        'pcm': int(fields['pcm']),
        'body_sent': int(fields['body_sent']) if 'body_sent' in fields else None,
        'phase': fields.get('phase'),
        'chat_posted': fields.get('chat_posted') == '1',
        'chat_us': int(fields.get('chat_us', -1)),
        'parts': [int(n) for n, _ in parts],
        'ends': [e == '1' for _, e in parts],
        'body': body,
    }


class VoiceNoteHelpers(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name) / 'muse_voice_note'
        compile_c(cls.binary, [ROOT / 'tests/muse_voice_note_harness.c',
                               MUSE / 'muse_chat_text.c', MUSE / 'muse_text.c'])

    def run_harness(self, *args, data=b''):
        ran = subprocess.run([str(self.binary), *args], input=data, capture_output=True, env=ENV)
        self.assertEqual(ran.returncode, 0, ran.stderr.decode(errors='replace'))
        self.assertEqual(ran.stderr, b'')
        return ran.stdout

    def encode(self, inputs):
        """Each input through muse_hatch_base64: (returned length, output, guard bytes)."""
        out = self.run_harness('base64', data=b''.join(struct.pack('<I', len(d)) + d for d in inputs))
        results = []
        for data in inputs:
            want = (len(data) + 2) // 3 * 4
            (ret,) = struct.unpack_from('<I', out)
            results.append((ret, out[4:4 + want], out[4 + want:4 + want + len(GUARD)]))
            out = out[4 + want + len(GUARD):]
        self.assertEqual(out, b'')
        return results

    def test_wav_header_matches_riff_layout(self):
        header = self.run_harness('header', '16000')
        self.assertEqual(header, riff_header(16000))
        self.assertEqual(header[4:8], b'\xff\xff\xff\xff')
        self.assertEqual(header[40:44], b'\xff\xff\xff\xff')

    def test_wav_header_opens_with_wave(self):
        pcm = pcm_bytes(2 * 1000, 1)
        with wave.open(io.BytesIO(self.run_harness('header', '16000') + pcm)) as w:
            self.assertEqual((w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getcomptype()),
                             (1, 2, 16000, 'NONE'))
            self.assertEqual(w.readframes(10 ** 6), pcm)

    def test_wav_header_carries_rate(self):
        for rate in (8000, 16000, 22050, 48000):
            header = self.run_harness('header', str(rate))
            self.assertEqual(header, riff_header(rate), rate)
            self.assertEqual(struct.unpack_from('<II', header, 24), (rate, rate * 2), rate)

    def test_base64_rfc4648_vectors(self):
        # RFC 4648 section 10, plus the 64 six-bit values in order, which must
        # spell the alphabet (section 4, table 1), '+' and '/' included.
        vectors = [(b'', b''), (b'f', b'Zg=='), (b'fo', b'Zm8='), (b'foo', b'Zm9v'),
                   (b'foob', b'Zm9vYg=='), (b'fooba', b'Zm9vYmE='), (b'foobar', b'Zm9vYmFy')]
        sextets = int(''.join(format(i, '06b') for i in range(64)), 2).to_bytes(48, 'big')
        vectors.append((sextets, b'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'))
        for (data, expected), (_, out, _) in zip(vectors, self.encode([d for d, _ in vectors])):
            self.assertEqual(out, expected, data)

    def test_base64_matches_cpython(self):
        lengths = [*range(65), 1535, 1536, 1537, 6143, 6144, 6145, 158124, 158125, 158126]
        inputs = [pcm_bytes(n, n) for n in lengths]
        inputs += [bytes(range(256)) * 3, b'\x00' * 64, b'\xff' * 64, b'\xfb\xef\xbe' * 21]
        for data, (_, out, _) in zip(inputs, self.encode(inputs)):
            self.assertEqual(out, base64.b64encode(data), len(data))

    def test_base64_fills_exactly_four_per_three(self):
        inputs = [pcm_bytes(n, n) for n in [*range(13), 1535, 1536, 1537, 6143, 6144, 6145]]
        for data, (ret, _, guard) in zip(inputs, self.encode(inputs)):
            self.assertEqual(ret, (len(data) + 2) // 3 * 4, len(data))
            self.assertEqual(guard, GUARD, len(data))

    def test_base64_streams_in_multiples_of_three(self):
        # The backends encode a note a part at a time: joined parts equal the
        # whole only while every part but the last is a multiple of 3 bytes.
        whole = pcm_bytes(8000, 2)
        expected = base64.b64encode(whole)
        for cuts, joins in (((1536, 3072), True), ((6144,), True), ((1537, 3073), False), ((6145,), False)):
            pieces = [whole[a:b] for a, b in zip((0, *cuts), (*cuts, len(whole)))]
            joined = b''.join(out for _, out, _ in self.encode(pieces))
            self.assertEqual(joined == expected, joins, cuts)
            if not joins:
                self.assertIn(b'=', joined[:-4])

    def test_request_body_round_trips(self):
        pcm = pcm_bytes(2 * 4001, 3)
        body = self.run_harness('body', data=pcm)
        self.assertEqual(body, HEAD + base64.b64encode(riff_header(16000) + pcm) + TAIL)
        request = json.loads(body)
        wav = base64.b64decode(request['items'][0].pop('data_base64'), validate=True)
        self.assertEqual(request, {'node_id': 'homelink-0a1b2c', 'message': '', 'output_modality': 'text',
                                   'device': 'mcu-wearable', 'items': [
            {'type': 'file', 'mime_type': 'audio/wav', 'filename': 'voice_note.wav'}]})
        self.assertEqual(wav, riff_header(16000) + pcm)
        with wave.open(io.BytesIO(wav)) as w:
            self.assertEqual((w.getnchannels(), w.getsampwidth(), w.getframerate()), (1, 2, 16000))
            self.assertEqual(w.readframes(10 ** 6), pcm)


HATCH_DRIVER = r'''
extern "C" const char *muse_chat_node_id(void) { return "homelink-0a1b2c"; }

static StreamBufferHandle_t s_in;
static turn_t s_turn;

/* The mic backlog: `avail` bytes of `mic` have arrived; each receive is also
 * held to the next entry of `sched`, when there is one, to vary its size. */
static uint8_t *mic;
static size_t mic_len, mic_avail, mic_pos, sched[16], sched_n, sched_i;
static size_t xStreamBufferReceive(StreamBufferHandle_t, void *dst, size_t want, int)
{
    size_t n = mic_avail - mic_pos < want ? mic_avail - mic_pos : want;
    if (sched_n && n > sched[sched_i % sched_n]) n = sched[sched_i % sched_n];
    sched_i++;
    memcpy(dst, mic + mic_pos, n);
    mic_pos += n;
    return n;
}

static char sent[1 << 20];
static size_t sent_len, part_len[256], nparts;
static bool part_end[256];
static bool send_body(int64_t id, const uint8_t *data, size_t len, bool end_body)
{
    CHECK(id == 7 && nparts < 256 && sent_len + len <= sizeof(sent));
    CHECK(!nparts || !part_end[nparts - 1]);
    memcpy(sent + sent_len, data, len);
    sent_len += len;
    part_len[nparts] = len;
    part_end[nparts++] = end_body;
    return true;
}
static int64_t open_stream(kind_t kind, const char *verb, const char *path, const char *content_type,
                           const char *accept, const char *body, bool end_body)
{
    CHECK(kind == K_CHAT && !strcmp(verb, "POST") && !strcmp(path, "/chat/stream"));
    CHECK(!strcmp(content_type, "application/json") && !accept && !body && !end_body);
    return 7;
}
static void mark(mark_t) {}
static int64_t now_us(void) { return 4242; }
static const char *failed = "-";
static void turn_fail(const char *why) { failed = why; s_turn.phase = P_IDLE; }
extern "C" void muse_state_page(const char *, int *cols, int *lines) { *cols = 16; *lines = 2; }
'''

HATCH_MAIN = r'''
/* argv: bytes the mic adds between polls, 1 to release after the last of
 * stdin, and receive sizes ("-" for none). Prints the outcome, then the body. */
int main(int argc, char **argv)
{
    CHECK(argc == 4);
    size_t step = strtoul(argv[1], nullptr, 10), cap = 1 << 21;
    bool release = argv[2][0] == '1';
    for (char *p = argv[3]; *p && *p != '-' && sched_n < 16; p += strcspn(p, ",") + (p[strcspn(p, ",")] == ',')) {
        sched[sched_n++] = strtoul(p, nullptr, 10);
    }
    mic = static_cast<uint8_t *>(malloc(cap));
    for (size_t got; (got = fread(mic + mic_len, 1, cap - mic_len, stdin)) > 0;) mic_len += got;
    /* Sized as muse_hatch_start() allocates them, so ASan catches an overrun. */
    s_turn.chunk = static_cast<uint8_t *>(malloc(DICT_CHUNK_BYTES + sizeof(MUSE_HATCH_NOTE_TAIL)));
    s_turn.note = static_cast<uint8_t *>(malloc(NOTE_PART_BYTES));
    s_turn.phase = P_LISTEN;
    bool ok = open_note();
    while (ok && s_turn.phase == P_LISTEN && mic_avail < mic_len) {
        mic_avail = mic_len - mic_avail < step ? mic_len : mic_avail + step;
        ok = record_note();
    }
    if (ok && s_turn.phase == P_LISTEN && release) {
        s_turn.end_requested = true;
        ok = record_note();
    }
    printf("ok=%d failed=", ok);
    for (const char *c = failed; *c; c++) putchar(*c == ' ' ? '_' : *c);
    static const char *const phases[] = {"idle", "listen", "wait_final", "wait_reply"};
    CHECK(s_turn.phase <= P_WAIT_REPLY);
    printf(" phase=%s chat_posted=%d chat_us=%lld", phases[s_turn.phase], s_turn.chat_posted,
           (long long)s_turn.chat_us);
    printf(" pcm=%zu body_sent=%zu parts=", s_turn.pcm_bytes, s_turn.body_sent);
    for (size_t i = 0; i < nparts; i++) printf("%s%zu:%d", i ? "," : "", part_len[i], part_end[i]);
    putchar('\n');
    fwrite(sent, 1, sent_len, stdout);
    free(mic);
    free(s_turn.chunk);
    free(s_turn.note);
    return 0;
}
'''


class HatchVoiceNote(NoteRequestAssertions, unittest.TestCase):
    """muse_chat_session.cpp's open_note/record_note/send_note_part, cut out of
    the source with the transport and mic backlog stubbed."""

    PART_PCM, PART_CHARS = 6144, 8192   # NOTE_PART_BYTES and the chunk it base64s to
    MIC = 16000 * 2

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        source = (MUSE / 'muse_chat_session.cpp').read_text()

        def line(start):
            at = source.index(start)
            return source[at:source.index('\n', at) + 1]

        constants = source[source.index('#define MIC_RATE'):source.index('/* ---- Voice task')]
        types = source[source.index('enum phase_t'):source.index('/* 10 KB')]
        note = source[source.index('static bool send_note_part('):source.index('static void send_chat(')]
        code = r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "host_compat.h"
#include "minimp3.h"
#include "muse_chat_priv.h"
#define ESP_LOGI(...) ((void)0)
/* Not assert(): the fakes must still check under -DNDEBUG. */
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(3); } } while (0)
typedef struct fake_stream *StreamBufferHandle_t;
''' + constants + line('enum kind_t') + line('enum mark_t') + types + HATCH_DRIVER + note + HATCH_MAIN
        (out / 'note.cpp').write_text(code)
        flags = ['-Wall', '-Wextra', '-Werror', *SANITIZE,
                 '-I', str(ROOT / 'tests'), '-I', str(MUSE),
                 '-I', str(ROOT / 'components/minimp3/include')]
        cc = shlex.split(os.environ.get('CC', 'cc'))
        cxx = shlex.split(os.environ.get('CXX', 'c++'))
        commands = [
            [*cc, '-std=gnu11', *flags, '-include', str(ROOT / 'tests/host_compat.h'),
             '-c', str(MUSE / 'muse_chat_text.c'), '-o', str(out / 'text.o')],
            [*cc, '-std=gnu11', *flags, '-include', str(ROOT / 'tests/host_compat.h'),
             '-c', str(MUSE / 'muse_text.c'), '-o', str(out / 'muse_text.o')],
            [*cxx, '-std=gnu++17', *flags, str(out / 'note.cpp'),
             str(out / 'text.o'), str(out / 'muse_text.o'), '-o', str(out / 'note')],
        ]
        for command in commands:
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
        cls.binary = out / 'note'

    def record(self, pcm, step, release=True, sched=()):
        ran = subprocess.run([str(self.binary), str(step), '1' if release else '0',
                              ','.join(map(str, sched)) or '-'],
                             input=pcm, capture_output=True, env=ENV)
        self.assertEqual(ran.returncode, 0, ran.stderr.decode(errors='replace'))
        self.assertEqual(ran.stderr, b'')
        return parse_run(ran.stdout)

    def assert_sent(self, run, pcm):
        self.assertTrue(run['ok'])
        self.assertEqual(run['failed'], '-')
        # Posted and waiting for the reply, timed from now_us() (4242 here).
        self.assertEqual((run['phase'], run['chat_posted'], run['chat_us']), ('wait_reply', True, 4242))
        self.assertEqual(run['pcm'], len(pcm))
        self.assert_request(run, pcm, self.PART_PCM, self.PART_CHARS)

    def test_short_note_fails_before_the_body_is_finalized(self):
        # Under 0.3 s calls turn_fail() before the last chunk: no tail and no
        # end flag. Parts that streamed during the press stay sent; resetting
        # the half-sent stream is turn_fail()'s job, stubbed here.
        for n, parts in ((3200, [len(HEAD)]), (9598, [len(HEAD), self.PART_CHARS])):
            run = self.record(pcm_bytes(n, n), 320)
            self.assertEqual(run['failed'], "DIDN'T CATCH THAT", n)
            self.assertEqual(run['parts'], parts, n)
            self.assertNotIn(True, run['ends'], n)
            self.assertEqual(run['body_sent'], len(run['body']), n)
            self.assertEqual((run['phase'], run['chat_posted']), ('idle', False), n)
        pcm = pcm_bytes(9600, 9600)   # exactly 0.3 s is kept
        self.assert_sent(self.record(pcm, 320), pcm)

    def test_note_ending_on_a_part_boundary(self):
        # The header shares the first part, so 6100 bytes of PCM fill it. Two
        # parts (6100 bytes alone is under 0.3 s) end the PCM on a boundary,
        # and the release sends only the tail.
        pcm = pcm_bytes(2 * self.PART_PCM - 44, 4)
        run = self.record(pcm, self.PART_PCM - 44)
        self.assert_sent(run, pcm)
        self.assertEqual(run['parts'], [len(HEAD), self.PART_CHARS, self.PART_CHARS, len(TAIL)])

    def test_uneven_receives_across_several_parts(self):
        pcm = pcm_bytes(self.PART_PCM - 44 + 3 * self.PART_PCM + 1000, 5)
        # Mic data trickling in, then all of it waiting with receives that
        # stop the first part 2 bytes short of full.
        for step, sched in ((320, (2, 6, 1000, 34, 4094, 6144)), (len(pcm), (self.PART_PCM - 46, 2, 6, 4094))):
            with self.subTest(step=step, sched=sched):
                self.assert_sent(self.record(pcm, step, sched=sched), pcm)

    def test_twenty_second_cap_ends_the_note(self):
        # 30000-byte polls put more than the cap in the backlog at once.
        pcm = pcm_bytes(20 * self.MIC + 3200, 6)
        run = self.record(pcm, 30000, release=False)
        self.assert_sent(run, pcm[:20 * self.MIC])



# A voice note's photo (CONFIG_MUSE_PHOTO_NOTES): an image item after the audio.
PHOTO_HEAD = b'"},{"type":"image","mime_type":"image/jpeg","filename":"photo.jpg","data_base64":"'
CHAT_PART = 16 * 1024

PHOTO_DRIVER = r"""
static std::atomic<uint32_t> s_gen{5};

/* The photo is malloc'd, as the camera's is: a free of it is counted. */
static char *photo;
static int photo_frees;
static void counted_free(void *p) { photo_frees += p && p == photo; std::free(p); }
#define free counted_free

/* send_body() fails at photo piece fail_at (from 1), and moves the
 * generation (a press during the upload) once cancel_at pieces have gone. */
static long fail_at = -1, cancel_at = -1;
static size_t photo_pieces, resets;
static bool in_photo;
static void turn_finish(void) { resets++; s_turn.phase = P_IDLE; free(s_turn.photo); s_turn.photo = nullptr; }
"""

PHOTO_SEND = r"""
static bool send_body(int64_t id, const uint8_t *data, size_t len, bool end_body)
{
    CHECK(id == 7 && nparts < 256 && sent_len + len <= sizeof(sent));
    CHECK(!nparts || !part_end[nparts - 1]);
    if (len == sizeof(MUSE_CHAT_PHOTO_HEAD) - 1 && !memcmp(data, MUSE_CHAT_PHOTO_HEAD, len)) {
        in_photo = true;
    } else if (in_photo && !end_body) {
        CHECK(len <= CHAT_PART && (const char *)data >= photo);   /* straight from the photo */
        if ((long)photo_pieces + 1 == fail_at) return false;
        if ((long)++photo_pieces == cancel_at) s_gen++;
    }
    memcpy(sent + sent_len, data, len);
    sent_len += len;
    part_len[nparts] = len;
    part_end[nparts++] = end_body;
    return true;
}
"""

PHOTO_MAIN = r"""
/* argv: bytes the mic adds between polls, the photo's base64 length (0 for
 * none), the photo piece where send_body() fails and the one after which the
 * generation moves (-1 for neither), and how the photo is offered:
 * "note" (to the note recording), "gen" (another press's), "text" (a typed
 * turn), "ended" (after the note's end went) or "twice". */
int main(int argc, char **argv)
{
    CHECK(argc == 6);
    size_t step = strtoul(argv[1], nullptr, 10), photo_len = strtoul(argv[2], nullptr, 10), cap = 1 << 21;
    fail_at = strtol(argv[3], nullptr, 10);
    cancel_at = strtol(argv[4], nullptr, 10);
    const char *how = argv[5];
    mic = static_cast<uint8_t *>(malloc(cap));
    for (size_t got; (got = fread(mic + mic_len, 1, cap - mic_len, stdin)) > 0;) mic_len += got;
    s_turn.chunk = static_cast<uint8_t *>(malloc(DICT_CHUNK_BYTES + sizeof(MUSE_HATCH_NOTE_TAIL)));
    s_turn.note = static_cast<uint8_t *>(malloc(NOTE_PART_BYTES));
    s_turn.phase = P_LISTEN;
    s_turn.gen = 5;
    s_turn.text = !strcmp(how, "text");
    bool ok = open_note();
    while (ok && s_turn.phase == P_LISTEN && mic_avail < mic_len) {
        mic_avail = mic_len - mic_avail < step ? mic_len : mic_avail + step;
        ok = record_note();
    }
    if (photo_len) {
        photo = static_cast<char *>(malloc(photo_len));
        memcpy(photo, "/9j/", 4);
        for (size_t i = 4; i < photo_len; i++) photo[i] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[(i * 7) % 64];
        if (!strcmp(how, "ended")) s_turn.phase = P_WAIT_REPLY;
        photo_attach(photo, photo_len, !strcmp(how, "gen") ? 4 : 5);
        if (!strcmp(how, "twice")) {
            char *second = static_cast<char *>(malloc(8));
            photo_attach(second, 8, 5);   /* one photo per note: the second is dropped */
        }
        if (!strcmp(how, "ended")) s_turn.phase = P_LISTEN;
    }
    if (ok && s_turn.phase == P_LISTEN) {
        s_turn.end_requested = true;
        ok = record_note();
    }
    printf("ok=%d failed=", ok);
    for (const char *c = failed; *c; c++) putchar(*c == ' ' ? '_' : *c);
    static const char *const phases[] = {"idle", "listen", "wait_final", "wait_reply"};
    CHECK(s_turn.phase <= P_WAIT_REPLY);
    printf(" phase=%s chat_posted=%d chat_us=%lld", phases[s_turn.phase], s_turn.chat_posted,
           (long long)s_turn.chat_us);
    printf(" pcm=%zu body_sent=%zu resets=%zu photo_frees=%d kept=%d parts=", s_turn.pcm_bytes, s_turn.body_sent,
           resets, photo_frees, s_turn.photo != nullptr);
    for (size_t i = 0; i < nparts; i++) printf("%s%zu:%d", i ? "," : "", part_len[i], part_end[i]);
    putchar('\n');
    fwrite(sent, 1, sent_len, stdout);
#undef free
    free(mic);
    free(s_turn.chunk);
    free(s_turn.note);
    return 0;
}
"""


def photo_base64(n):
    """The harness's stand-in photo: /9j/ and a fixed run of the alphabet."""
    alphabet = b'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'
    return b'/9j/' + bytes(alphabet[(i * 7) % 64] for i in range(4, n))


class HatchPhotoNote(unittest.TestCase):
    """The same voice note code with CONFIG_MUSE_PHOTO_NOTES: a photo attached
    while the note records goes up after its audio, in the same request."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        source = (MUSE / 'muse_chat_session.cpp').read_text()

        def line(start):
            at = source.index(start)
            return source[at:source.index('\n', at) + 1]

        constants = source[source.index('#define MIC_RATE'):source.index('/* ---- Voice task')]
        types = source[source.index('enum phase_t'):source.index('/* 10 KB')]
        note = source[source.index('static bool send_note_part('):source.index('static void send_chat(')]
        driver = HATCH_DRIVER[:HATCH_DRIVER.index('static char sent[')] + HATCH_DRIVER[
            HATCH_DRIVER.index('static int64_t open_stream('):]
        sent = 'static char sent[1 << 20];\nstatic size_t sent_len, part_len[256], nparts;\nstatic bool part_end[256];\n'
        code = r"""
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "host_compat.h"
#include "minimp3.h"
#include "muse_chat_priv.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(3); } } while (0)
typedef struct fake_stream *StreamBufferHandle_t;
""" + constants + line('enum kind_t') + line('enum mark_t') + types + driver + sent + PHOTO_DRIVER + PHOTO_SEND + note + PHOTO_MAIN
        (out / 'photo.cpp').write_text(code)
        flags = ['-Wall', '-Wextra', '-Werror', *SANITIZE, '-DCONFIG_MUSE_PHOTO_NOTES=1',
                 '-I', str(ROOT / 'tests'), '-I', str(MUSE),
                 '-I', str(ROOT / 'components/minimp3/include')]
        cc = shlex.split(os.environ.get('CC', 'cc'))
        cxx = shlex.split(os.environ.get('CXX', 'c++'))
        commands = [
            [*cc, '-std=gnu11', *flags, '-include', str(ROOT / 'tests/host_compat.h'),
             '-c', str(MUSE / 'muse_chat_text.c'), '-o', str(out / 'text.o')],
            [*cc, '-std=gnu11', *flags, '-include', str(ROOT / 'tests/host_compat.h'),
             '-c', str(MUSE / 'muse_text.c'), '-o', str(out / 'muse_text.o')],
            [*cxx, '-std=gnu++17', *flags, str(out / 'photo.cpp'),
             str(out / 'text.o'), str(out / 'muse_text.o'), '-o', str(out / 'photo')],
        ]
        for command in commands:
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
        cls.binary = out / 'photo'

    def record(self, pcm, photo_len, how='note', fail_at=-1, cancel_at=-1, step=4000):
        ran = subprocess.run([str(self.binary), str(step), str(photo_len), str(fail_at), str(cancel_at), how],
                             input=pcm, capture_output=True, env=ENV)
        self.assertEqual(ran.returncode, 0, ran.stderr.decode(errors='replace'))
        self.assertEqual(ran.stderr, b'')
        meta, body = ran.stdout.split(b'\n', 1)
        run = parse_run(ran.stdout)
        fields = dict(item.split('=', 1) for item in meta.decode().split())
        run.update(resets=int(fields['resets']), photo_frees=int(fields['photo_frees']), kept=fields['kept'] == '1')
        return run

    def assert_photo_note(self, run, pcm, photo):
        wav = riff_header(16000) + pcm
        self.assertTrue(run['ok'])
        self.assertEqual((run['phase'], run['chat_posted'], run['resets']), ('wait_reply', True, 0))
        self.assertEqual(run['body'], HEAD + base64.b64encode(wav) + PHOTO_HEAD + photo + TAIL)
        self.assertEqual(run['body_sent'], len(run['body']))
        self.assertEqual(run['ends'], [False] * (len(run['parts']) - 1) + [True])
        request = json.loads(run['body'])
        audio, image = request['items']
        self.assertEqual(base64.b64decode(audio.pop('data_base64'), validate=True), wav)
        self.assertEqual(audio, {'type': 'file', 'mime_type': 'audio/wav', 'filename': 'voice_note.wav'})
        self.assertEqual(image, {'type': 'image', 'mime_type': 'image/jpeg', 'filename': 'photo.jpg',
                                 'data_base64': photo.decode()})
        # The photo goes straight from its buffer in CHAT_PART pieces, then the tail.
        at = run['parts'].index(len(PHOTO_HEAD))
        pieces = run['parts'][at + 1:-1]
        self.assertEqual(pieces, [CHAT_PART] * (len(photo) // CHAT_PART) + ([len(photo) % CHAT_PART] if len(photo) % CHAT_PART else []))
        self.assertEqual(run['parts'][-1], len(TAIL))
        self.assertEqual((run['photo_frees'], run['kept']), (1, False))

    def test_note_without_a_photo_is_unchanged(self):
        pcm = pcm_bytes(16000, 7)
        run = self.record(pcm, 0)
        self.assertEqual(run['body'], HEAD + base64.b64encode(riff_header(16000) + pcm) + TAIL)
        self.assertEqual(run['ends'][-1], True)

    def test_photo_follows_the_audio_in_one_request(self):
        for pcm_len, photo_len in ((16000, 1200), (2 * 6144 - 44, 4 * CHAT_PART), (40000, 3 * CHAT_PART + 4)):
            with self.subTest(pcm=pcm_len, photo=photo_len):
                pcm = pcm_bytes(pcm_len, pcm_len)
                self.assert_photo_note(self.record(pcm, photo_len), pcm, photo_base64(photo_len))

    def test_cancel_during_the_upload_resets_the_request(self):
        # A press or cancel after the first photo piece: nothing more goes, the
        # request never ends (the reset discards it), and the photo is freed.
        pcm = pcm_bytes(16000, 8)
        for cancel_at in (1, 2):
            with self.subTest(cancel_at=cancel_at):
                run = self.record(pcm, 3 * CHAT_PART, cancel_at=cancel_at)
                self.assertTrue(run['ok'])
                self.assertEqual((run['phase'], run['chat_posted'], run['resets']), ('idle', False, 1))
                self.assertNotIn(True, run['ends'])
                at = run['parts'].index(len(PHOTO_HEAD))
                self.assertEqual(run['parts'][at + 1:], [CHAT_PART] * cancel_at)
                self.assertEqual((run['photo_frees'], run['kept']), (1, False))

    def test_failed_send_frees_the_photo(self):
        pcm = pcm_bytes(16000, 9)
        run = self.record(pcm, 3 * CHAT_PART, fail_at=2)
        self.assertFalse(run['ok'])
        self.assertNotIn(True, run['ends'])
        self.assertEqual((run['photo_frees'], run['kept']), (1, False))

    def test_only_the_note_recording_takes_a_photo(self):
        pcm = pcm_bytes(16000, 10)
        for how in ('gen', 'text', 'ended'):
            with self.subTest(how=how):
                run = self.record(pcm, 1200, how=how)
                self.assertEqual(run['photo_frees'], 1)   # dropped at once
                self.assertNotIn(PHOTO_HEAD, run['body'])
        run = self.record(pcm, 1200, how='twice')
        self.assert_photo_note(run, pcm, photo_base64(1200))

    def test_short_note_drops_its_photo(self):
        run = self.record(pcm_bytes(3200, 11), 1200)
        self.assertEqual(run['failed'], "DIDN'T CATCH THAT")
        self.assertNotIn(PHOTO_HEAD, run['body'])



ATTACH_MAIN = r"""
typedef int QueueHandle_t;
static QueueHandle_t s_cmds;
static std::atomic<uint32_t> s_gen{9};
static cmd_t queued;
static int sends, frees;
static bool queue_full;
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
static int xQueueSend(QueueHandle_t q, const cmd_t *cmd, int wait)
{
    CHECK(q == 1 && wait == 1000);
    sends++;
    if (queue_full) return 0;
    queued = *cmd;
    return pdTRUE;
}
static void counted_free(void *p) { frees++; free(p); }
#define free counted_free
""" + "{attach}" + r"""
#undef free
/* stdin: the photo. argv: "full" for a full queue, "stopped" before the
 * session starts. Prints ok, the queued command and the frees. */
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    s_cmds = strcmp(argv[1], "stopped") ? 1 : 0;
    queue_full = !strcmp(argv[1], "full");
    CHECK(muse_chat_note_id() == 9);
    size_t cap = 1 << 20, len = 0;
    char *photo = static_cast<char *>(malloc(cap));
    for (size_t got; (got = fread(photo + len, 1, cap - len, stdin)) > 0;) len += got;
    bool ok = muse_chat_attach_jpeg(photo, len, 7);
    printf("ok=%d sends=%d frees=%d", ok, sends, frees);
    if (ok) {
        CHECK(queued.type == CMD_PHOTO && queued.gen == 7 && queued.text == photo && queued.len == len);
        free(queued.text);
    }
    putchar('\n');
    return 0;
}
"""


class HatchPhotoAttach(unittest.TestCase):
    """muse_chat_attach_jpeg(): checks the photo, then hands it to the session
    task tagged with its note; whatever it refuses, it frees."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        source = (MUSE / 'muse_chat_session.cpp').read_text()
        cmds = source[source.index('enum cmd_type_t'):source.index('struct ev_t')]
        attach = source[source.index('extern "C" uint32_t muse_chat_note_id('):source.index('#endif', source.index(
            'extern "C" bool muse_chat_attach_jpeg('))]
        code = r"""
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "muse_chat.h"
#include "muse_chat_priv.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(3); } } while (0)
""" + cmds + ATTACH_MAIN.replace('{attach}', attach)
        (out / 'attach.cpp').write_text(code)
        flags = ['-Wall', '-Wextra', '-Werror', *SANITIZE, '-DCONFIG_MUSE_PHOTO_NOTES=1',
                 '-I', str(ROOT / 'tests'), '-I', str(MUSE), '-include', str(ROOT / 'tests/host_compat.h')]
        cc = shlex.split(os.environ.get('CC', 'cc'))
        cxx = shlex.split(os.environ.get('CXX', 'c++'))
        for command in ([*cc, '-std=gnu11', *flags, '-c', str(MUSE / 'muse_chat_image.c'), '-o', str(out / 'image.o')],
                        [*cxx, '-std=gnu++17', *flags, str(out / 'attach.cpp'), str(out / 'image.o'),
                         '-o', str(out / 'attach')]):
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
        cls.binary = out / 'attach'

    def attach(self, photo, how='-'):
        ran = subprocess.run([str(self.binary), how], input=photo, capture_output=True, text=True, env=ENV)
        self.assertEqual(ran.returncode, 0, ran.stderr)
        return dict(item.split('=', 1) for item in ran.stdout.split())

    def test_queues_a_base64_jpeg_for_its_note(self):
        photo = base64.b64encode(b'\xff\xd8\xff\xe0' + bytes(range(256))).decode()
        self.assertEqual(self.attach(photo), {'ok': '1', 'sends': '1', 'frees': '0'})

    def test_refuses_and_frees_anything_else(self):
        jpeg = base64.b64encode(b'\xff\xd8\xff\xe0 a photo').decode()
        cases = {
            'not a JPEG': base64.b64encode(b'\x89PNG\r\n\x1a\n').decode(),
            'unpadded': jpeg.rstrip('=') + 'A',
            'padding inside': '/9j/' + 'AA==' + 'AAAA',
            'not base64': '/9j/' + 'AA*A',
            'too short': '/9j',
        }
        for name, photo in cases.items():
            with self.subTest(name):
                self.assertEqual(self.attach(photo), {'ok': '0', 'sends': '0', 'frees': '1'})
        self.assertEqual(self.attach(jpeg, 'stopped'), {'ok': '0', 'sends': '0', 'frees': '1'})
        self.assertEqual(self.attach(jpeg, 'full'), {'ok': '0', 'sends': '1', 'frees': '1'})

    def test_refuses_a_photo_over_the_limit(self):
        limit = 256 * 1024
        over = '/9j/' + 'A' * (limit - 4 + 4)
        self.assertEqual(self.attach(over), {'ok': '0', 'sends': '0', 'frees': '1'})
        self.assertEqual(self.attach('/9j/' + 'A' * (limit - 4)), {'ok': '1', 'sends': '1', 'frees': '0'})


if __name__ == '__main__':
    unittest.main()
