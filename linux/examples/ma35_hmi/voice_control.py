# SPDX-License-Identifier: Apache-2.0
"""Tap-to-start/tap-to-stop board microphone, streamed as a Muse voice note."""
import asyncio
import base64
import json
import logging
import math
from pathlib import Path
import struct
import uuid
import os
import live_link
from musegadget.noise import Header

log=logging.getLogger(__name__)
ROOT=Path(__file__).resolve().parent
RATE=16000

def wav_stream_header(pcm_bytes=None):
    return struct.pack('<4sI4s4sIHHIIHH4sI',b'RIFF',36+pcm_bytes if pcm_bytes is not None else 0xffffffff,b'WAVE',b'fmt ',16,1,1,RATE,RATE*2,2,16,b'data',pcm_bytes if pcm_bytes is not None else 0xffffffff)

class VoiceController:
    def __init__(self, ssh, display, session):
        self.ssh=ssh;self.display=display;self.session=session
        self.record_task=None;self.touch_task=None;self.capture=None;self.stopping=False
        self.last_tap=0

    async def start(self):
        self.touch_task=asyncio.create_task(self.touch_loop())

    async def toggle(self):
        if self.record_task is not None and not self.record_task.done():
            if self.stopping:
                return
            self.stopping=True
            self.display.set_voice('sending')
            await self.stop_capture()
            return
        if self.session() is None or self.session().registered_at is None:
            self.display.set_voice('error')
            return
        self.stopping=False
        if getattr(self.display, 'speaker', None): self.display.speaker.pause()
        self.display.set_voice('recording')
        self.record_task=asyncio.create_task(self.record())

    async def stop_capture(self):
        p=await asyncio.create_subprocess_exec(*self.ssh,
            'if test -f /tmp/muse-mic.pid; then kill -INT "$(cat /tmp/muse-mic.pid)"; fi',
            stdout=asyncio.subprocess.DEVNULL,stderr=asyncio.subprocess.DEVNULL)
        try: await asyncio.wait_for(p.wait(),5)
        except asyncio.TimeoutError: p.kill();await p.wait()

    async def record(self):
        session=self.session();stream_id=None;request=None
        seconds=0
        try:
            if getattr(self.display,'speaker',None):
                stop=await asyncio.create_subprocess_exec(*self.ssh,'if test -f /tmp/muse-tts.pid; then kill -TERM "$(cat /tmp/muse-tts.pid)" 2>/dev/null; fi',stdout=asyncio.subprocess.DEVNULL,stderr=asyncio.subprocess.DEVNULL)
                await asyncio.wait_for(stop.wait(),5)
            cmd='amixer -q sset PGA 100%; amixer -q sset ADC 100%; amixer -q sset \"PGA Boost\" 100%; arecord -q -D hw:0,0 -f S16_LE -r 16000 -c 2 -t raw -d 30 & micpid=$!; echo "$micpid" > /tmp/muse-mic.pid; trap \'kill -INT "$micpid" 2>/dev/null\' HUP TERM; wait "$micpid"; rm -f /tmp/muse-mic.pid'
            self.capture=await asyncio.create_subprocess_exec(*self.ssh,cmd,stdout=asyncio.subprocess.PIPE,stderr=asyncio.subprocess.PIPE)
            stereo=bytearray();pcm_bytes=0
            while True:
                chunk=await self.capture.stdout.read(4096)
                if not chunk:break
                if self.stopping and pcm_bytes == 0:
                    await self.stop_capture()
                pcm_bytes+=len(chunk)
                stereo.extend(chunk)
                if hasattr(self.display,'set_mic_level'):
                    samples=struct.unpack('<'+str(len(chunk)//2)+'h',chunk[:len(chunk)//2*2])
                    self.display.set_mic_level(max(map(abs,samples),default=0)/32768)
            stderr=await self.capture.stderr.read()
            code=await self.capture.wait()
            usable=len(stereo)//4*4
            pairs=list(struct.iter_unpack('<hh',stereo[:usable]))
            energy=[sum(pair[c]*pair[c] for pair in pairs) for c in (0,1)]
            channel=0 if energy[0]>=energy[1] else 1
            audio=b''.join(struct.pack('<h',pair[channel]) for pair in pairs)
            pcm_bytes=len(audio)
            log.info('Microphone channels RMS: left=%.1f right=%.1f; selected=%s',math.sqrt(energy[0]/max(1,len(pairs))),math.sqrt(energy[1]/max(1,len(pairs))), 'onboard/left' if channel==0 else 'headset/right')
            seconds=pcm_bytes/(RATE*2)
            if not pcm_bytes or (code and not self.stopping):
                raise RuntimeError('microphone capture failed: '+stderr.decode(errors='replace')[:160])
            self.display.set_voice('sending')
            headers=[Header('Content-Type','application/json'),Header('x-request-id',str(uuid.uuid4())),Header('x-app-id',live_link.APP_ID)]
            encrypted=session._transport.start_stream_request('POST',live_link.CHAT_PATH,headers=headers)
            stream_id=encrypted.stream_id
            request=live_link._Request(asyncio.get_running_loop().create_future())
            session._requests[stream_id]=request
            await session._send_frames(encrypted.frames)
            prefix=json.dumps({'message':'Listen to this voice recording and respond to what I said. If you cannot understand it, ask me to record again.','output_modality':'text','device_id':session._device.node_id})[:-1]
            prefix+=',"items":[{"type":"file","mime_type":"audio/wav","filename":"voice_note.wav","data_base64":"'
            await session._send_frames(session._transport.encrypt_body_chunk(stream_id,prefix.encode()))
            wav=wav_stream_header(pcm_bytes)+audio
            if os.environ.get('MA35_VOICE_DIAGNOSTIC_PATH'):
                fd=os.open(os.environ['MA35_VOICE_DIAGNOSTIC_PATH'],os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o600)
                with os.fdopen(fd,'wb') as f:f.write(wav)
            encoded=base64.b64encode(wav)
            for offset in range(0,len(encoded),16384):
                await session._send_frames(session._transport.encrypt_body_chunk(stream_id,encoded[offset:offset+16384]))
            await session._send_frames(session._transport.encrypt_body_chunk(stream_id,b'"}]}',end_body=True))
            status,response=await asyncio.wait_for(request.done,60)
            if not 200<=status<300:
                raise RuntimeError('Muse rejected voice note: HTTP '+str(status))
            ack=json.loads(response) if response else {}
            mid=ack.get('message_id') or 'voice-'+str(uuid.uuid4())
            self.display.event({'event':'message.user','message_id':mid,'text':f'Voice message · {seconds:.1f} seconds','seq':None})
            log.info('Voice note accepted by Muse: %.2f seconds, HTTP %d',seconds,status)
            self.display.set_voice('idle')
        except asyncio.CancelledError:
            await self.stop_capture()
            raise
        except Exception as exc:
            log.warning('voice capture/send failed: %s',exc)
            self.display.set_voice('error')
            if self.capture is not None and self.capture.returncode is None:
                await self.stop_capture()
        finally:
            if stream_id is not None:
                session._requests.pop(stream_id,None)
                if request is not None and not request.done.done():
                    try:await session._send_frames(session._transport.encrypt_reset(stream_id))
                    except Exception:pass
            self.capture=None
            self.stopping=False
            if getattr(self.display,'speaker',None): self.display.speaker.resume()

    async def touch_loop(self):
        while True:
            proc=None
            try:
                upload=await asyncio.create_subprocess_exec(*self.ssh,'cat > /tmp/muse-touch-reader; chmod 755 /tmp/muse-touch-reader',stdin=asyncio.subprocess.PIPE,stdout=asyncio.subprocess.DEVNULL,stderr=asyncio.subprocess.PIPE)
                _,err=await upload.communicate((ROOT/'touch_reader').read_bytes())
                if upload.returncode:raise RuntimeError(err.decode(errors='replace')[:150])
                proc=await asyncio.create_subprocess_exec(*self.ssh,'/tmp/muse-touch-reader',stdout=asyncio.subprocess.PIPE,stderr=asyncio.subprocess.PIPE)
                limits=None
                while True:
                    line=await proc.stdout.readline()
                    if not line:break
                    fields=line.decode().split()
                    if fields[0]=='RANGE':
                        limits=list(map(int,fields[1:]))
                        log.info('Board touchscreen ready: ranges %s',limits)
                    elif fields[0]=='UP' and limits:
                        xmin,xmax,ymin,ymax=limits
                        # ADC X already runs opposite to framebuffer X; rotation cancels it.
                        x=round((int(fields[1])-xmin)*1023/max(1,xmax-xmin))
                        y=599-round((int(fields[2])-ymin)*599/max(1,ymax-ymin))
                        now=asyncio.get_running_loop().time()
                        if now-self.last_tap<0.30:continue
                        self.last_tap=now
                        action=self.display.tap(x,y)
                        log.info('Board tap: %d,%d action=%s',x,y,action)
                        if action=='voice':await self.toggle()
                        elif action=='volume':
                            await asyncio.to_thread(self.display.speaker.apply_volume,self.display.volume,self.display.muted)
                raise RuntimeError('touch reader disconnected')
            except asyncio.CancelledError:
                if proc is not None and proc.returncode is None:proc.terminate()
                raise
            except Exception as exc:
                log.warning('touch input unavailable: %s',exc)
                await asyncio.sleep(5)

    async def close(self):
        if self.touch_task is not None:self.touch_task.cancel()
        if self.record_task is not None and not self.record_task.done():self.record_task.cancel()
        tasks=[t for t in (self.touch_task,self.record_task) if t is not None]
        if tasks:await asyncio.gather(*tasks,return_exceptions=True)
