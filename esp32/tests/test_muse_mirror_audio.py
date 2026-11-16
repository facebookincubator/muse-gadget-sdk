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

"""The mirror's sound: the rings between the audio path and the cable, and the wiring."""

import importlib.util
import os
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / 'components/muse'
MIRROR = ROOT / 'components/muse_mirror'

STUBS = {
    'sdkconfig.h': '#define CONFIG_MUSE_MIRROR 1\n',
    'esp_heap_caps.h': '#include <stddef.h>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_INTERNAL 2\n#define MALLOC_CAP_8BIT 4\nvoid *heap_caps_malloc(size_t size, unsigned caps);\nvoid heap_caps_free(void *p);\n',
    'freertos/FreeRTOS.h': 'typedef int portMUX_TYPE;\n#define portMUX_INITIALIZER_UNLOCKED 0\n'
                           '#define portENTER_CRITICAL(m) ((void)(m))\n#define portEXIT_CRITICAL(m) ((void)(m))\n',
}


class MirrorAudio(unittest.TestCase):
    @unittest.skipUnless(os.name == 'posix', 'C harness requires a POSIX sanitizer toolchain')
    def test_rings_and_routes(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            for name, text in STUBS.items():
                (folder / name).parent.mkdir(parents=True, exist_ok=True)
                (folder / name).write_text(text)
            exe = folder / 'audio-harness'
            command = shlex.split(os.environ.get('CC', 'cc')) + [
                '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                '-I' + str(folder), '-I' + str(MUSE), '-I' + str(MIRROR), str(MIRROR / 'muse_mirror_audio.c'), str(MUSE / 'muse_adpcm.c'),
                str(ROOT / 'tests/muse_mirror_audio_harness.c'), '-o', str(exe)]
            result = subprocess.run(command, capture_output=True, text=True,
                                    env={**os.environ, 'TMPDIR': tempfile.gettempdir()})
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([exe], capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr.decode())

    @unittest.skipUnless(os.name == 'posix' and importlib.util.find_spec('numpy'),
                         'needs a POSIX toolchain and numpy')
    def test_adpcm_from_the_board_decodes_in_the_viewer(self):
        import numpy as np
        spec = importlib.util.spec_from_file_location('muse_cable_viewer_audio', ROOT / 'tools/muse/mirror.py')
        mirror = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mirror)
        main = r'''
#include <stdio.h>
#include <stdlib.h>
#include "muse_mirror_audio.h"
void *heap_caps_malloc(size_t size, unsigned caps) { (void)caps; return malloc(size); }
void heap_caps_free(void *p) { free(p); }
int main(int argc, char **argv) {
    (void)argv;
    muse_mirror_adpcm_t state = { 0 };
    uint8_t pcm[MUSE_MIRROR_AUDIO_CHUNK], out[MUSE_MIRROR_ADPCM_CHUNK];
    if (argc > 1) {                 /* the viewer's microphone: chunks in, what the board records out */
        int16_t heard[MUSE_MIRROR_AUDIO_CHUNK / 2];
        if (!muse_mirror_audio_init()) return 1;
        muse_mirror_audio_mic(2);
        while (fread(out, 1, sizeof(out), stdin) == sizeof(out)) {
            muse_mirror_audio_mic_adpcm(out);
            muse_mirror_audio_capture(heard, MUSE_MIRROR_AUDIO_CHUNK / 2);
            fwrite(heard, sizeof(heard), 1, stdout);
        }
        return 0;
    }
    while (fread(pcm, 1, sizeof(pcm), stdin) == sizeof(pcm)) {
        muse_mirror_adpcm(&state, pcm, out);
        fwrite(out, 1, sizeof(out), stdout);
    }
    return 0;
}
'''
        t = np.arange(320 * 25) / 16000
        voice = (9000 * np.sin(2 * np.pi * 220 * t) + 5000 * np.sin(2 * np.pi * 1300 * t)
                 + 2000 * np.sin(2 * np.pi * 3100 * t)) * np.minimum(1, t * 20)
        loud = np.where((np.arange(len(t)) // 400) % 2, 32767, -32768)       # full-scale steps: no wrap-around
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            for name, text in STUBS.items():
                (folder / name).parent.mkdir(parents=True, exist_ok=True)
                (folder / name).write_text(text)
            (folder / 'main.c').write_text(main)
            exe = folder / 'adpcm'
            command = shlex.split(os.environ.get('CC', 'cc')) + [
                '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                '-I' + str(folder), '-I' + str(MUSE), '-I' + str(MIRROR), str(MIRROR / 'muse_mirror_audio.c'), str(MUSE / 'muse_adpcm.c'), str(folder / 'main.c'),
                '-o', str(exe)]
            result = subprocess.run(command, capture_output=True, text=True,
                                    env={**os.environ, 'TMPDIR': tempfile.gettempdir()})
            self.assertEqual(result.returncode, 0, result.stderr)
            for name, signal, limit in (('voice', voice, 0.06), ('steps', loud, 0.5)):
                pcm = np.clip(np.rint(signal), -32768, 32767).astype('<i2')
                packed = subprocess.run([exe], input=pcm.tobytes(), capture_output=True, timeout=10).stdout
                self.assertEqual(len(packed), 164 * 25, name)
                chunks = [packed[i:i + 164] for i in range(0, len(packed), 164)]
                heard = np.frombuffer(b''.join(mirror.adpcm_decode(c) for c in chunks), '<i2').astype(float)
                self.assertEqual(len(heard), len(pcm), name)
                error = np.sqrt(np.mean((heard[640:] - pcm[640:]) ** 2)) / np.sqrt(np.mean(pcm[640:].astype(float) ** 2))
                self.assertLess(error, limit, name)
                # Each chunk stands alone: the viewer hears the last one the same without the ones before.
                self.assertEqual(mirror.adpcm_decode(chunks[-1]), heard[-320:].astype('<i2').tobytes(), name)
                # The other way: the viewer packs its microphone exactly as the board would, and the board
                # records what the viewer's own decoder gives.
                state = [0, 0]
                mine = b''.join(mirror.adpcm_encode(pcm[i:i + 320].tobytes(), state) for i in range(0, len(pcm), 320))
                self.assertEqual(mine, packed, name)
                recorded = subprocess.run([exe, 'mic'], input=mine, capture_output=True, timeout=10).stdout
                # 60 ms gathered first: the board records two chunks of silence, then the chunks in order.
                self.assertEqual(recorded, bytes(1280) + heard[:-640].astype('<i2').tobytes(), name)
        for bad in (bytes([0, 0, 89, 0, 0]), bytes(4)):       # a step index past the table; no samples
            with self.assertRaises(ConnectionError):
                mirror.adpcm_decode(bad)

    def test_audio_path_calls_the_mirror_once_each_way(self):
        audio = (MUSE / 'muse_audio.c').read_text()
        read = audio[audio.index('esp_err_t muse_audio_read('):audio.index('esp_err_t muse_audio_write(')]
        write = audio[audio.index('esp_err_t muse_audio_write('):audio.index('float muse_audio_dbfs(')]
        self.assertEqual(audio.count('muse_mirror_audio_'), 2)
        self.assertEqual(read.count('muse_mirror_audio_capture(mono, n);'), 1)
        self.assertLess(read.index('s_hpf_y1 = y;'), read.index('muse_mirror_audio_capture'))
        self.assertEqual(write.count('muse_mirror_audio_playback(mono, frames)'), 1)
        self.assertLess(write.index('muse_mirror_audio_playback'), write.index('muse_board->mic_init'))
        self.assertIn('bool mute = mirror_only || !muse_settings_speaker_on();', write)

    def test_without_the_mirror_the_calls_are_empty(self):
        header = (MIRROR / 'muse_mirror_audio.h').read_text()
        off = header[header.index('#else'):header.index('#endif')]
        self.assertIn('static inline void muse_mirror_audio_capture', off)
        self.assertIn('return false;', off)
        cmake = (MIRROR / 'CMakeLists.txt').read_text()
        mirror = cmake[cmake.index('if(CONFIG_MUSE_MIRROR)'):]
        self.assertIn('"muse_mirror_audio.c"', mirror[:mirror.index('endif()')])

    def test_sound_needs_a_cable_fast_enough(self):
        adapter = (MIRROR / 'muse_mirror.c').read_text()
        self.assertIn('        .audio = muse_board->audio_init || muse_board->mic_init,\n'
                      '#if CONFIG_MUSE_CONSOLE_UART\n        .uart = true,\n#endif', adapter)
        session = (MIRROR / 'muse_mirror_session.c').read_text()
        self.assertIn('s_audio_rings = config->audio && muse_mirror_audio_init();', session)
        self.assertIn('s_audio = s_audio_rings && (s_link ? s_link->audio : !s_cfg.uart || uart_baud() >= AUDIO_BAUD);', session)
        self.assertIn('memcpy(s_audio_flag, s_audio ? "true " : "false", 5);', session)
        self.assertIn('    if (!s_audio) return true;\n', session)


VIEWER = r'''
import queue
import sys
import numpy as np
from PySide6.QtCore import QCoreApplication
sys.path.insert(0, sys.argv[1])
import mirror
import mirror_audio

app = QCoreApplication([])

# 48 kHz stereo float from the computer's microphone becomes the board's 16 kHz mono, and back.
tone = np.sin(np.arange(4800) * 2 * np.pi * 440 / 48000).astype('<f4')
capture = mirror_audio.PCM(48000, 2, 'Float', True)
wire = capture.convert(np.repeat(tone[:, None], 2, axis=1).tobytes()[:-3]) + capture.convert(b'\0' * 3)
mono = np.frombuffer(wire, '<i2')
assert 1580 <= len(mono) <= 1600 and abs(int(mono.max()) - 32767) < 1500, (len(mono), mono.max())   # 80 Hz high-pass
# Hum below the high-pass drops: one pole leaves about a third of 30 Hz (9 dB down).
hum = mirror_audio.PCM(48000, 1, 'Float', True)
low = np.frombuffer(hum.convert((0.9 * np.sin(np.arange(48000) * 2 * np.pi * 30 / 48000)).astype('<f4').tobytes()), '<i2')
assert np.abs(low[8000:]).max() < 0.4 * 32768, np.abs(low[8000:]).max()
play = mirror_audio.PCM(48000, 2, 'Int16', False)
out = np.frombuffer(play.convert(wire[:640]) + play.convert(wire[640:1280]), '<i2').reshape(-1, 2)
assert 1860 <= len(out) <= 1920 and (out[:, 0] == out[:, 1]).all()


class Format:
    class SampleFormat:
        Int16 = 'Int16'
    def setSampleRate(self, rate): pass
    def setChannelCount(self, count): pass
    def setSampleFormat(self, kind): pass
    def sampleRate(self): return 16000
    def channelCount(self): return 1
    def sampleFormat(self): return type('Kind', (), {'name': 'Int16'})


class Device:
    def __init__(self, missing=False): self.missing = missing
    def isNull(self): return self.missing
    def isFormatSupported(self, fmt): return True
    def preferredFormat(self): return Format()


class Stream:
    def __init__(self): self.written, self.data, self.readyRead = bytearray(), b'', self
    def connect(self, slot): self.slot = slot
    def write(self, data): self.written += data; return len(data)
    def readAll(self): data, self.data = self.data, b''; return data


class Endpoint:
    def __init__(self, device, fmt, parent): self.stream = Stream()
    def setBufferSize(self, size): pass
    def start(self): return self.stream
    def stop(self): pass
    def deleteLater(self): pass
    def bytesFree(self): return 1 << 20
    def bufferSize(self): return 1 << 20


class Devices:
    microphone = True
    @staticmethod
    def defaultAudioOutput(): return Device()
    @staticmethod
    def defaultAudioInput(): return Device(not Devices.microphone)


class Link:
    live = audio_capable = True
    native = False                  # a UART bridge: ADPCM
    def __init__(self): self.sent, self.audio, self.out = [], queue.Queue(maxsize=8), queue.Queue()
    def send(self, payload): self.sent.append(bytes(payload))


said = []
io = mirror_audio.AudioSession(None, said.append, mirror.adpcm_encode)
io.Format, io.Sink, io.Source, io.Devices = Format, Endpoint, Endpoint, Devices
link = Link()

# The speaker icon: off, here alone with our microphone too, the board and here, off with the microphone.
assert io.speaker_mode(link) == 0
assert io.cycle_speaker(link) == 2 and link.sent == [bytes([7, 2]), bytes([9, 1]), bytes([10, 2])]
assert io.mic_mode(link) == 2
link.audio.put((1, b'\1\0' * 320))        # the microphone's, which nobody asked for
for _ in range(7):                          # 140 ms: not yet the 300 ms a start waits for
    link.audio.put((2, b'\2\0' * 320))
io.pump()
assert not io.output.written
for _ in range(8):
    link.audio.put((2, b'\2\0' * 320))
io.pump()
assert bytes(io.output.written) == b'\2\0' * 320 * 15
assert io.cycle_speaker(link) == 1 and link.sent[-1] == bytes([9, 0]) and io.speaker_mode(link) == 1
assert io.cycle_speaker(link) == 0 and link.sent[-2:] == [bytes([7, 0]), bytes([10, 0])]
assert io.talk_link is None
# A microphone turned on by its own button stays on when the speaker goes off.
assert io.cycle_mic(link) == 1
for _ in range(3):
    io.cycle_speaker(link)
assert io.speaker_mode(link) == 0 and io.talk_link is link and io.mic_mode(link) == 1
assert io.cycle_mic(link) == 2 and io.cycle_mic(link) == 0 and io.talk_link is None

# The microphone icon: off, with the board's, instead of it, off; 20 ms to a message.
del link.sent[:]
assert io.cycle_mic(link) == 1 and link.sent == [bytes([10, 1])]
io.input.data = b'\3\0' * 500
io.capture()
assert [m[:2] for m in link.sent[1:]] == [b'S\x03'] and len(link.sent[1]) == 2 + 164 and len(io.capture_pending) == 360
link.native = True                  # the chip's own USB: PCM as it is
io.input.data = b'\4\0' * 280
io.capture()
assert link.sent[-1][:2] == b'S\x02' and len(link.sent[-1]) == 2 + 640 and len(io.capture_pending) == 280
link.native = False
assert io.cycle_mic(link) == 2 and link.sent[-1] == bytes([10, 2]) and io.mic_mode(link) == 2
assert io.cycle_mic(link) == 0 and link.sent[-1] == bytes([10, 0]) and io.talk_link is None

# The board went away: both stop on the next tick, and nothing more is sent to a dead cable's queue.
io.cycle_speaker(link); io.cycle_mic(link)
link.live = False
io.pump()
assert io.listen_link is None and io.talk_link is None

# No microphone on this computer: said, and the icon stays off.
link.live = True
Devices.microphone = False
del link.sent[:]
assert io.cycle_mic(link) == 0 and not link.sent and said == ['No audio device is available']
io.close()
'''


@unittest.skipUnless(importlib.util.find_spec('PySide6') and importlib.util.find_spec('numpy'),
                     'PySide6 and numpy are needed for the viewer sound test')
class ViewerSound(unittest.TestCase):
    def test_formats_and_icon_cycles(self):
        result = subprocess.run([sys.executable, '-c', VIEWER, str(ROOT / 'tools/muse')], capture_output=True,
                                text=True, timeout=60, env={**os.environ, 'QT_QPA_PLATFORM': 'offscreen'})
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_rate_conversion_keeps_speech_and_adds_nothing_above_it(self):
        sys.path.insert(0, str(ROOT / 'tools/muse'))
        import numpy as np
        import mirror_audio
        for source, target in ((16000, 48000), (48000, 16000), (16000, 44100), (44100, 16000)):
            t = np.arange(source) / source
            tone = lambda at: 0.5 * np.sin(2 * np.pi * 6000 * at) + 0.3 * np.sin(2 * np.pi * 1000 * at)
            r = mirror_audio.Resampler(source, target)
            out = np.concatenate([r.feed(tone(t[i:i + source // 50])) for i in range(0, source, source // 50)])
            ideal = tone(np.arange(len(out)) / target)[200:-200]
            error = out[200:-200] - ideal
            self.assertGreater(10 * np.log10(np.mean(ideal ** 2) / np.mean(error ** 2)), 50, (source, target))
        # Noise above 9 kHz at 48 kHz must not fold into the band at 16 kHz.
        noise = np.fft.rfft(np.random.default_rng(1).normal(0, 0.3, 48000))
        noise[np.fft.rfftfreq(48000, 1 / 48000) < 9000] = 0
        r = mirror_audio.Resampler(48000, 16000)
        folded = np.concatenate([r.feed(x) for x in np.split(np.fft.irfft(noise), 100)])
        self.assertLess(np.sqrt(np.mean(folded[100:] ** 2)), 0.003)

    def test_viewer_takes_sound_messages(self):
        viewer = (ROOT / 'tools/muse/mirror.py').read_text()
        self.assertIn('self.audio_capable = bool(info.get("audio"))', viewer)
        self.assertIn('BUTTONS = ("move", "talk", "listen", "rec", "shot", "close")', viewer)
        self.assertNotIn('import QtMultimedia', viewer.replace('PySide6.QtMultimedia import', 'import QtMultimedia'))


if __name__ == '__main__':
    unittest.main()
