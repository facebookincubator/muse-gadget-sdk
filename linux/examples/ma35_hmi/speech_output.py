# SPDX-License-Identifier: Apache-2.0
"""Local speech synthesis, streamed to the board's headphone PCM output."""
import os
import io
import json
import logging
from pathlib import Path
import queue
import re
import subprocess
import threading
import wave
import time

log=logging.getLogger(__name__)
ROOT=Path(__file__).resolve().parent
from settings import STATE
MODEL=Path(os.environ.get('MA35_TTS_MODEL', str(ROOT/'voices/en_US-lessac-medium.onnx'))).expanduser()

class SpeechOutput:
    def __init__(self,ssh):
        self.ssh=ssh;self.queue=queue.Queue();self.offsets={};self.generation=0
        self.proc=None;self.lock=threading.Lock();self.closed=False;self.suspended=False
        try:
            settings=json.loads((STATE/'headphones.json').read_text())
            self.volume=max(0,min(100,int(settings['volume'])))
            self.muted=bool(settings.get('muted',False))
        except (OSError,ValueError,KeyError):self.volume=75;self.muted=False
        self.thread=threading.Thread(target=self.run,daemon=True,name='headphone-tts')
        self.thread.start()

    def apply_volume(self,volume,muted=False):
        self.volume=max(0,min(100,int(volume)));self.muted=muted
        STATE.mkdir(parents=True, exist_ok=True, mode=0o700)
        (STATE/'headphones.json').write_text(json.dumps({'volume':self.volume,'muted':self.muted}))
        proc=subprocess.run(self.ssh+[f'amixer -q sset Headphone {self.volume}% {"mute" if self.muted else "unmute"}'],capture_output=True,timeout=10)
        if proc.returncode:raise RuntimeError(proc.stderr.decode(errors='replace')[:120])
        log.info('Headphone volume: %d%%, muted=%s',self.volume,self.muted)

    def pause(self):
        self.suspended=True
        self.cancel()

    def resume(self):
        self.suspended=False

    def cancel(self):
        with self.lock:
            self.generation+=1
            if self.proc is not None and self.proc.poll() is None:self.proc.terminate()

    def update(self,mid,text,finished=False):
        offset=self.offsets.get(mid,0)
        remaining=text[offset:]
        if finished:
            end=len(remaining)
        else:
            ends=list(re.finditer(r'[.!?](?:\s|$)',remaining))
            end=ends[-1].end() if ends else 0
        if end:
            phrase=remaining[:end].strip()
            self.offsets[mid]=offset+end
            if phrase:self.queue.put((self.generation,phrase))
        if len(self.offsets)>100:self.offsets.pop(next(iter(self.offsets)))

    def run(self):
        try:
            from piper import PiperVoice
            voice=PiperVoice.load(MODEL)
            log.info('Headphone speech synthesis ready')
            while not self.closed:
                try:generation,text=self.queue.get(timeout=1)
                except queue.Empty:continue
                while self.suspended and not self.closed:
                    threading.Event().wait(0.1)
                if generation!=self.generation or self.closed:continue
                text=re.sub(r'```.*?```',' Code shown on screen. ',text,flags=re.S)
                text=re.sub(r'[*`#]','',text)
                buf=io.BytesIO()
                with wave.open(buf,'wb') as wav:voice.synthesize_wav(text[:1800],wav)
                with self.lock:
                    if generation!=self.generation:continue
                    self.proc=subprocess.Popen(self.ssh+[f'amixer -q sset Headphone {self.volume}% {"mute" if self.muted else "unmute"}; amixer -q sset Speaker mute; amixer -q sset PCM 84%; amixer -q sset "Left Output Mixer LDAC" on; amixer -q sset "Right Output Mixer RDAC" on; exec 3<&0; aplay -q -D plughw:0,0 - <&3 & ttspid=$!; echo "$ttspid" > /tmp/muse-tts.pid; trap "kill -TERM "$ttspid" 2>/dev/null" HUP TERM; wait "$ttspid"; tts_status=$?; rm -f /tmp/muse-tts.pid; exit "$tts_status"'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
                    proc=self.proc
                started=time.monotonic()
                _,err=proc.communicate(buf.getvalue(),timeout=90)
                elapsed=time.monotonic()-started
                if proc.returncode and generation==self.generation:log.warning('Headphone playback failed: %s',err.decode(errors='replace')[:200])
                elif generation==self.generation:log.info('Headphone speech played: %d characters, %.2f seconds',len(text),elapsed)
        except Exception:
            log.exception('Headphone speech service failed')

    def close(self):
        self.closed=True;self.cancel();self.thread.join(timeout=3)
