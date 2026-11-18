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

"""Qt audio lives on the GUI thread. Link threads only fill bounded queues."""
import queue
import time

import numpy as np


class Resampler:
    """Band-limited interpolation between two rates: a Blackman-windowed sinc cut at 0.45 of the slower rate.
    Linear interpolation instead folds what lies above 8 kHz into speech going down (hiss), leaves images above
    it coming up (grit) and dulls the top of the band. It holds back 16 samples of the slower rate for the kernel's right side."""
    ZEROS = 16          # the sinc's zero crossings each side, at the slower rate

    def __init__(self, source_rate, target_rate):
        self.step = source_rate / target_rate                 # input samples an output sample
        self.cut = 0.45 * min(1.0, 1 / self.step)             # cycles an input sample
        self.half = int(np.ceil(self.ZEROS * max(1.0, self.step)))
        self.data = np.zeros(self.half)                       # input the kernel still reaches back to
        self.position = float(self.half)                      # the next output, in self.data's samples

    def feed(self, samples):
        samples = np.asarray(samples, dtype=np.float64)
        if self.step == 1 or not samples.size:
            return samples
        self.data = np.concatenate((self.data, samples))
        positions = np.arange(self.position, len(self.data) - self.half, self.step)
        if not len(positions):
            return np.empty(0)
        taps = np.floor(positions).astype(int)[:, None] + np.arange(1 - self.half, self.half + 1)
        t = positions[:, None] - taps
        window = 0.42 + 0.5 * np.cos(np.pi * t / self.half) + 0.08 * np.cos(2 * np.pi * t / self.half)
        out = (self.data[taps] * 2 * self.cut * np.sinc(2 * self.cut * t) * window).sum(axis=1)
        self.position = positions[-1] + self.step
        drop = int(self.position) - self.half
        if drop > 0:
            self.data, self.position = self.data[drop:], self.position - drop
        return out


class PCM:
    def __init__(self, rate, channels, kind, capture):
        self.channels, self.kind, self.capture = channels, kind, capture
        self.dtype = {'Int16': '<i2', 'Int32': '<i4', 'Float': '<f4', 'UInt8': 'u1'}[kind]
        self.frame_size = np.dtype(self.dtype).itemsize * channels
        self.raw = bytearray()
        self.resampler = Resampler(rate if capture else 16000, 16000 if capture else rate)
        self.hp = [0.0, 0.0]            # the microphone's high-pass state: last input, last output

    HP_A = 1 / (1 + 2 * np.pi * 80 / 16000)

    def high_pass(self, x):
        """80 Hz, one pole, as the board filters its own microphone: the computer's hum and rumble stay out."""
        out = np.empty_like(x)
        x1, y1 = self.hp
        for i, v in enumerate(x):
            y1 = self.HP_A * (y1 + v - x1)
            x1, out[i] = v, y1
        self.hp = [x1, y1]
        return out

    def convert(self, data):
        if self.capture:
            self.raw.extend(data)
            size = len(self.raw) // self.frame_size * self.frame_size
            samples = np.frombuffer(bytes(self.raw[:size]), self.dtype).astype(np.float64)
            del self.raw[:size]
            if not size:
                return b''
            if self.kind == 'UInt8':
                samples = (samples - 128) / 128
            elif self.kind != 'Float':
                samples /= 32768 if self.kind == 'Int16' else 2147483648
            mono = self.high_pass(self.resampler.feed(samples.reshape(-1, self.channels).mean(axis=1)))
            return np.clip(np.rint(mono * 32768), -32768, 32767).astype('<i2').tobytes()
        samples = np.frombuffer(data, '<i2').astype(np.float64) / 32768
        samples = self.resampler.feed(samples)
        samples = np.repeat(samples[:, None], self.channels, axis=1)
        if self.kind == 'UInt8':
            samples = np.clip(np.rint(samples * 128 + 128), 0, 255)
        elif self.kind == 'Int16':
            samples = np.clip(np.rint(samples * 32768), -32768, 32767)
        elif self.kind == 'Int32':
            samples = np.clip(np.rint(samples * 2147483648), -2147483648, 2147483647)
        return samples.astype(self.dtype).tobytes()


class AudioSession:
    def __init__(self, parent, report, encode):
        from PySide6.QtCore import QTimer
        from PySide6.QtMultimedia import QAudioFormat, QAudioSink, QAudioSource, QMediaDevices
        self.Format, self.Sink, self.Source, self.Devices = QAudioFormat, QAudioSink, QAudioSource, QMediaDevices
        self.parent, self.report, self.encode = parent, report, encode
        self.encoder = [0, 0]           # the microphone's ADPCM state, from chunk to chunk
        self.listen_link = self.talk_link = None
        self.closed = False
        self.listen_source = 0
        self.listen_solo = False        # the board's speaker silent while the viewer hears it
        self.talk_lend = 0              # the board's microphone with ours 1, ours alone 2
        self.mic_by_speaker = False     # the speaker's first click turned our microphone on
        self.sink = self.source = self.output = self.input = None
        self.pending = bytearray()
        self.priming = 0                # when playback began to wait for its cushion
        self.capture_pending = bytearray()
        self.timer = QTimer(parent, interval=10, timeout=self.pump)
        self.timer.start()

    def format(self, device):
        if device.isNull():
            raise ValueError('No audio device is available')
        # The device's own format: the rate conversion is then Resampler's, not the audio stack's.
        fmt = device.preferredFormat()
        if fmt.sampleFormat().name not in ('Int16', 'Int32', 'UInt8', 'Float'):
            fmt = self.Format()
            fmt.setSampleRate(16000)
            fmt.setChannelCount(1)
            fmt.setSampleFormat(self.Format.SampleFormat.Int16)
        if fmt.sampleFormat().name not in ('Int16', 'Int32', 'UInt8', 'Float'):
            raise ValueError('Unsupported audio device format')
        return fmt

    def stop_listen(self):
        if self.listen_link:
            if self.listen_solo:
                self.listen_link.send(bytes([9, 0]))
            self.listen_link.send(bytes([7, 0]))
        if self.sink:
            self.sink.stop()
            self.sink.deleteLater()
        self.listen_link = self.sink = self.output = None
        self.listen_source = 0
        self.listen_solo = False
        self.pending.clear()

    def listen(self, link, source):
        same = self.listen_link is link and self.listen_source == source
        self.stop_listen()
        if same:
            return
        try:
            device = self.Devices.defaultAudioOutput()
            fmt = self.format(device)
            self.play_pcm = PCM(fmt.sampleRate(), fmt.channelCount(), fmt.sampleFormat().name, False)
            self.sink = self.Sink(device, fmt, self.parent)
            second = fmt.sampleRate() * self.play_pcm.frame_size
            # A slow cable carries sound between screen messages: start, and restart after running dry,
            # with 300 ms in hand, and keep at most a second.
            self.prime, self.limit = second * 3 // 10, second
            self.sink.setBufferSize(max(1024, second // 8))
            self.output = self.sink.start()
            if self.output is None:
                raise ValueError('Cannot start audio output')
            while not link.audio.empty():
                try:
                    link.audio.get_nowait()
                except queue.Empty:
                    break
            self.listen_link, self.listen_source = link, source
            link.send(bytes([7, source]))
        except (ValueError, RuntimeError) as error:
            self.stop_listen()
            self.report(str(error))

    def stop_talk(self):
        if self.source:
            self.source.stop()
            self.source.deleteLater()
        if self.talk_link:
            self.talk_link.send(bytes([10, 0]))
        self.talk_link = self.source = self.input = None
        self.talk_lend = 0
        self.capture_pending.clear()

    def speaker_mode(self, link):
        """0 off, 1 on the board and here, 2 here alone."""
        if self.listen_link is not link or self.listen_source != 2:
            return 0
        return 2 if self.listen_solo else 1

    def mic_mode(self, link):
        """0 off, 1 our microphone with the board's, 2 ours alone."""
        return self.talk_lend if self.talk_link is link else 0

    def cycle_speaker(self, link):
        """Off, then here alone with our microphone for the board's, then the board and here, then off, with
        the microphone too if this button turned it on."""
        mode = self.speaker_mode(link)
        if mode == 0:
            self.listen(link, 2)
            if self.listen_link is link:
                link.send(bytes([9, 1]))
                self.listen_solo = True
                if not self.mic_mode(link):
                    if self.talk_link is not None:   # another board had our microphone: give its own back
                        self.stop_talk()
                    self.start_talk(link, 2)
                    self.mic_by_speaker = self.talk_link is link
        elif mode == 2:
            link.send(bytes([9, 0]))
            self.listen_solo = False
        else:
            self.stop_listen()
            if self.mic_by_speaker:                   # a microphone turned on by its own button stays
                self.stop_talk()
            self.mic_by_speaker = False
        return self.speaker_mode(link)

    def cycle_mic(self, link):
        self.mic_by_speaker = False
        mode = (self.mic_mode(link) + 1) % 3
        if mode == 0:
            self.stop_talk()
        elif mode == 1:
            self.stop_talk()
            self.start_talk(link, 1)
            mode = self.mic_mode(link)
        else:
            link.send(bytes([10, 2]))
            self.talk_lend = 2
        return mode

    def start_talk(self, link, lend):
        if self.closed or not link.live:
            return
        try:
            device = self.Devices.defaultAudioInput()
            fmt = self.format(device)
            self.capture_pcm = PCM(fmt.sampleRate(), fmt.channelCount(), fmt.sampleFormat().name, True)
            self.source = self.Source(device, fmt, self.parent)
            self.source.setBufferSize(max(1024, fmt.sampleRate() * self.capture_pcm.frame_size // 10))
            self.input = self.source.start()
            if self.input is None:
                raise ValueError('Cannot start microphone; check OS microphone permission')
            self.talk_link, self.talk_lend = link, lend
            self.encoder[:] = 0, 0
            link.send(bytes([10, lend]))
            self.input.readyRead.connect(self.capture)
        except (ValueError, RuntimeError) as error:
            self.stop_talk()
            self.report(str(error))

    def resume(self, link):
        """A board that took this viewer afresh (a new B) has forgotten its sound routes: send them again."""
        if self.listen_link is link:
            link.send(bytes([7, self.listen_source]))
            if self.listen_solo:
                link.send(bytes([9, 1]))
        if self.talk_link is link:
            link.send(bytes([10, self.talk_lend]))

    def capture(self):
        if not self.input or not self.talk_link:
            return
        self.capture_pending += self.capture_pcm.convert(bytes(self.input.readAll()))
        while len(self.capture_pending) >= 640:
            pcm = bytes(self.capture_pending[:640])
            del self.capture_pending[:640]
            # Do not queue seconds of stale microphone data behind a blocked cable.
            if self.talk_link.out.qsize() < 16:
                self.talk_link.send(b'S\x02' + pcm if self.talk_link.native else b'S\x03' + self.encode(pcm, self.encoder))

    def pump(self):
        if self.talk_link and not self.talk_link.live:
            self.stop_talk()
        if self.listen_link and not self.listen_link.live:
            self.stop_listen()
        if not self.listen_link:
            return
        while True:
            try:
                source, pcm = self.listen_link.audio.get_nowait()
            except queue.Empty:
                break
            if source == self.listen_source:
                self.pending += self.play_pcm.convert(pcm)
        if len(self.pending) > self.limit:
            excess = len(self.pending) - self.limit
            excess += (-excess) % self.play_pcm.frame_size
            del self.pending[:excess]
        if self.pending and self.output:
            if self.sink.bytesFree() >= self.sink.bufferSize() and len(self.pending) < self.prime:
                self.priming = self.priming or time.monotonic()
                if time.monotonic() - self.priming < .3:   # a short sound plays all the same
                    return
            self.priming = 0
            size = min(len(self.pending), self.sink.bytesFree())
            size -= size % self.play_pcm.frame_size
            if size:
                written = self.output.write(bytes(self.pending[:size]))
                if written < 0:
                    self.stop_listen()
                    self.report('Audio output stopped')
                else:
                    del self.pending[:written]

    def close(self):
        self.closed = True
        self.timer.stop()
        self.stop_talk()
        self.stop_listen()
