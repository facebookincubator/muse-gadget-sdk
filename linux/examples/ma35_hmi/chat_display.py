# SPDX-License-Identifier: Apache-2.0
"""Render streamed Muse chat to the MA35H0 framebuffer over its Ethernet relay."""
import collections
import io
import logging
from pathlib import Path
import subprocess
import threading
import time
from PIL import Image, ImageDraw, ImageFont, ImageSequence
from speech_output import SpeechOutput
from thermostat_demo import ThermostatDemo

ROOT = Path(__file__).resolve().parent
log = logging.getLogger(__name__)
W, H = 1024, 600
FONT_PATH = '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'

class ChatDisplay:
    def __init__(self, ssh):
        self.ssh = ssh
        self.speaker = SpeechOutput(ssh)
        self.volume = self.speaker.volume
        self.muted = self.speaker.muted
        self.messages = collections.OrderedDict()
        self.seen = collections.deque(maxlen=2048)
        self.status = 'Connecting to Muse'
        self.voice_state = 'idle'
        self.mic_level = 0.0
        self.waiting_confirmation = False
        self.setpoint = 72
        self.mode = 'Auto'
        self.thermostat = ThermostatDemo()
        self.lock = threading.Lock()
        self.dirty = threading.Event()
        self.closed = threading.Event()
        self.previous = None
        self.frames = 0
        self.events = 0
        self.font = ImageFont.truetype(FONT_PATH, 23)
        self.small = ImageFont.truetype(FONT_PATH, 17)
        self.title = ImageFont.truetype(FONT_PATH, 35)
        # Optional user-supplied artwork; no third-party artwork is bundled.
        self.logo = Image.new('RGBA', (70, 70), '#18233f')
        ImageDraw.Draw(self.logo).text((17, 12), 'M', font=self.title, fill='white')
        if (ROOT / 'muse-logo.svg').exists():
            import cairosvg
            png = cairosvg.svg2png(url=str(ROOT / 'muse-logo.svg'), output_width=70, output_height=70)
            self.logo = Image.open(io.BytesIO(png)).convert('RGBA')
        bot = Image.new('RGBA', (78, 78))
        draw = ImageDraw.Draw(bot)
        draw.rounded_rectangle((9, 13, 69, 65), radius=12, fill='#635bff')
        draw.ellipse((22, 27, 30, 35), fill='white')
        draw.ellipse((48, 27, 56, 35), fill='white')
        draw.arc((23, 33, 55, 55), 0, 180, fill='white', width=3)
        self.avatar_frames = [bot]
        if (ROOT / 'jollybot.gif').exists():
            avatar = Image.open(ROOT / 'jollybot.gif')
            self.avatar_frames = [frame.convert('RGBA').resize((78,78), Image.Resampling.LANCZOS) for frame in ImageSequence.Iterator(avatar)]
        self.thread = threading.Thread(target=self._render_loop, daemon=True, name='board-display')
        self.thread.start()
        self.dirty.set()

    def set_status(self, status):
        with self.lock:
            self.status = status
        self.dirty.set()

    def event(self, event):
        name, mid, text = event['event'], event['message_id'], event['text']
        if name=='task.status':
            self.waiting_confirmation=event.get('status')=='pending_user_confirmation'
            self.set_status('Approval needed in Muse app' if self.waiting_confirmation else 'Muse is working' if event.get('status') in ('running','in_progress') else 'Live chat')
            return
        if name=='approvals.snapshot' and event.get('pending_approvals'):
            self.waiting_confirmation=True
            self.set_status('Approval needed in Muse app')
            return
        if name not in ('message.user', 'message.assistant', 'delta.message_start', 'delta.text_append', 'delta.message_done'):
            return
        if not mid:
            return
        key = (name, mid, event.get('seq'))
        with self.lock:
            if event.get('seq') is not None and key in self.seen:
                return
            self.seen.append(key)
            role = 'You' if name == 'message.user' else 'Muse'
            item = self.messages.setdefault(mid, {'role': role, 'text': '', 'streaming': False})
            if name == 'delta.text_append':
                item['text'] += text
                item['streaming'] = True
            elif name == 'delta.message_start':
                item['streaming'] = True
            else:
                if text:
                    item['text'] = text
                item['streaming'] = False
            while len(self.messages) > 60:
                self.messages.popitem(last=False)
            self.status = 'Approval needed in Muse app' if getattr(self,'waiting_confirmation',False) else 'Muse is replying' if item['streaming'] else 'Waiting for Muse' if role=='You' else 'Live chat'
            self.events += 1
        if self.events <= 5 or name in ('message.user', 'delta.message_done', 'message.assistant'):
            log.info('chat display event %s: %d text characters', name, len(text))
        speaker=getattr(self,'speaker',None)
        if speaker:
            if name=='message.user': speaker.cancel()
            else: speaker.update(mid,item['text'],not item['streaming'])
        self.dirty.set()

    def _wrap(self, text, width):
        lines = []
        for paragraph in text.replace('\r', '').split('\n'):
            line = ''
            for word in paragraph.split(' '):
                trial = (line + ' ' + word).strip()
                if self.font.getlength(trial) <= width:
                    line = trial
                    continue
                if line:
                    lines.append(line)
                line = word
                while self.font.getlength(line) > width:
                    n = max(1, int(len(line)*width/self.font.getlength(line)))
                    lines.append(line[:n])
                    line = line[n:]
            lines.append(line)
        return lines or ['']

    def tap(self, x, y):
        # Coordinates refer to the upright layout; touch input compensates for rotation.
        if 20 <= x <= 658 and 520 <= y <= 589:
            return 'voice'
        if 688 <= x <= 1023 and 505 <= y <= 599:
            with self.lock:
                if x<=782:self.volume=max(0,self.volume-5);self.muted=False
                elif x>=912:self.volume=min(100,self.volume+5);self.muted=False
                else:self.muted=not self.muted
            self.dirty.set()
            return 'volume'
        with self.lock:
            if 724 <= x <= 816 and 374 <= y <= 438:
                self.setpoint = max(50, self.setpoint-1)
            elif 876 <= x <= 976 and 374 <= y <= 438:
                self.setpoint = min(90, self.setpoint+1)
            elif 711 <= x <= 985 and 450 <= y <= 495:
                modes = ['Off','Heat','Cool','Auto']
                self.mode = modes[(modes.index(self.mode)+1)%4]
            else:
                return None
        self.dirty.set()
        return 'temperature'

    def set_mic_level(self, level):
        with self.lock:self.mic_level = level
        self.dirty.set()

    def set_voice(self, state):
        with self.lock:
            self.voice_state = state
        self.dirty.set()

    def render(self):
        with self.lock:
            messages = [dict(m) for m in self.messages.values()]
            waiting = self.waiting_confirmation
            volume,muted = self.volume,self.muted
            mic_level = self.mic_level
            thermostat = self.thermostat.snapshot(self.setpoint,self.mode)
            status, voice, target, mode = self.status, self.voice_state, self.setpoint, self.mode
        img = Image.new('RGB', (W, H), '#f4f6fc')
        d = ImageDraw.Draw(img)
        d.rectangle((0, 0, W, 96), fill='white')
        img.paste(self.logo, (24,12), self.logo)
        d.text((109,17),'Muse',font=self.title,fill='#18233f')
        d.text((110,61),'Talk, chat, and make yourself at home',font=self.small,fill='#6a7590')
        avatar=self.avatar_frames[int(time.monotonic()*5)%len(self.avatar_frames)]
        img.paste(avatar,(656,9),avatar)
        d.ellipse((765,32,777,44),fill='#34b88c' if status != 'Connecting to Muse' else '#d9a443')
        d.text((787,25),'Approval needed' if waiting else status,font=self.small,fill='#465878')
        entries=[]
        for m in messages[-12:]:
            text=m['text'] or ('Thinking…' if m['streaming'] else '')
            if not text: continue
            lines=self._wrap(text,515)
            if len(lines)>8: lines=['…']+lines[-7:]
            entries.append((m,lines,42+30*len(lines)))
        if not entries:
            d.text((32,165),'Hello. Let’s talk.',font=self.title,fill='#24324f')
            d.text((34,226),'Tap the microphone to talk to Muse.',font=self.font,fill='#61708b')
            d.text((34,266),'Or chat in the Muse app on your phone.',font=self.font,fill='#61708b')
        else:
            while len(entries)>1 and sum(e[2]+12 for e in entries)>391: entries.pop(0)
            y=max(111,504-sum(e[2]+12 for e in entries))
            for m,lines,height in entries:
                user=m['role']=='You';x=84 if user else 24;right=658 if user else 598
                d.rounded_rectangle((x,y,right,y+height),radius=18,fill='#dee7ff' if user else 'white')
                d.text((x+18,y+10),m['role']+(' · replying' if m['streaming'] else ''),font=self.small,fill='#3761c4' if user else '#547096')
                for n,line in enumerate(lines): d.text((x+18,y+36+n*30),line,font=self.font,fill='#172747')
                y+=height+12
        # Room temperature and HVAC state are explicitly simulated.
        d.rounded_rectangle((688,112,1004,583),radius=24,fill='white')
        d.text((720,130),'THERMOSTAT · DEMO',font=self.small,fill='#72819a')
        d.text((720,156),f"Room {thermostat['room_temperature_f']:.1f}° · {thermostat['hvac_state']}",font=self.small,fill='#657895')
        dial_color={'Heating':'#78452a','Cooling':'#263c69'}.get(thermostat['hvac_state'],'#253047')
        d.ellipse((731,177,961,407),fill=dial_color,outline='#cdd4df',width=7)
        d.arc((741,187,951,397),195,345,fill='#87aafa',width=5)
        d.text((798,233),str(target)+'°',font=ImageFont.truetype(FONT_PATH,65),fill='white')
        d.text((798,315),'Target °F',font=self.small,fill='#b8c8e2')
        for box,text in [((720,381,815,439),'−'),((877,381,976,439),'+')]:
            d.rounded_rectangle(box,radius=16,fill='#e9effb')
            d.text((box[0]+33,box[1]+1),text,font=self.title,fill='#325bba')
        d.rounded_rectangle((714,454,980,496),radius=13,fill='#f1f4f9')
        d.text((785,465),mode+'  ›',font=self.small,fill='#496384')
        d.text((737,510),'HEADPHONE VOLUME',font=self.small,fill='#72819a')
        for box,label in [((720,539,782,581),'−'),((912,539,976,581),'+')]:
            d.rounded_rectangle(box,radius=12,fill='#e9effb')
            d.text((box[0]+19,box[1]-1),label,font=self.title,fill='#325bba')
        d.rounded_rectangle((790,539,904,581),radius=12,fill='#fff0cb' if muted else '#e9effb')
        d.text((805,550),'Muted' if muted else str(volume)+'%',font=self.font,fill='#325bba')
        if waiting:
            d.rounded_rectangle((24,468,658,513),radius=12,fill='#fff0cb')
            d.text((40,480),'Open Muse on your phone to review the pending request.',font=self.small,fill='#855712')
        recording=voice=='recording'
        if recording:
            d.rounded_rectangle((40,511,642,519),radius=4,fill='#c8d2e6')
            if mic_level>0:d.rounded_rectangle((40,511,40+max(3,int(602*min(1,mic_level*3))),519),radius=4,fill='#31b68b')
        d.rounded_rectangle((24,524,658,583),radius=20,fill='#dc5764' if recording else '#2656bd')
        label={'idle':'Tap to talk to Muse','recording':'Recording… speak, then tap to send','sending':'Sending voice…','error':'Voice error — tap to retry'}.get(voice,'Tap to talk to Muse')
        d.ellipse((47,539,69,560),fill='white')
        d.line((58,561,58,568),fill='white',width=3)
        d.text((91,538),label,font=self.font,fill='white')
        return img.transpose(Image.Transpose.ROTATE_180)

    def _write(self, image):
        raw=image.convert('RGBA').tobytes('raw','BGRA')
        changed = [y for y in range(H) if self.previous is None or raw[y*4096:(y+1)*4096] != self.previous[y*4096:(y+1)*4096]]
        if not changed:
            return
        start,end=min(changed),max(changed)+1
        proc=subprocess.run(self.ssh+[f'dd of=/dev/fb0 bs=4096 seek={start}'],
                            input=raw[start*4096:end*4096],capture_output=True,timeout=8)
        if proc.returncode:
            raise RuntimeError(proc.stderr.decode(errors='replace')[:200])
        self.previous=raw
        self.frames+=1
        if self.frames==1:
            log.info('Muse logo and chat view written to board framebuffer')
        image.save('/tmp/ma35-muse-chat-preview.png')

    def _render_loop(self):
        while not self.closed.is_set():
            self.dirty.wait(0.20)
            self.dirty.clear()
            try:
                self._write(self.render())
            except Exception as exc:
                log.warning('board display write failed: %s',exc)
                self.dirty.set()
                self.closed.wait(1)
            self.closed.wait(0.08)

    def close(self):
        self.closed.set()
        self.speaker.close()
        self.dirty.set()
        self.thread.join(timeout=9)
