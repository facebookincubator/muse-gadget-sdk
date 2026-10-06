# SPDX-License-Identifier: Apache-2.0
"""Muse laptop BLE gateway: commands execute on the MA35H0 over Ethernet."""
import asyncio
import logging
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent
os.environ.setdefault('MUSEGADGET_STATE_DIR', str(ROOT / 'state'))
os.environ.setdefault('MUSEGADGET_SOCKET', str(ROOT / 'musegadget.sock'))
from musegadget import config, identity
from musegadget.executor import Account, COMMAND_SPECS, ok, error
from musegadget.service import Service
from musegadget import service as service_module
import live_link
from chat_display import ChatDisplay
from voice_control import VoiceController
from display_controls import CONTROL_SPEC,apply_controls

DISPLAY = None
CHAT_STATE = {}

class DisplaySubscription(live_link._Subscription):
    def __init__(self, stream_id):
        super().__init__(stream_id)
        self.display_events = asyncio.Queue()
        self.listeners.add(self.display_events)
        self.display_task = asyncio.create_task(self.consume_display())

    async def consume_display(self):
        try:
            while not self.closed.is_set() or not self.display_events.empty():
                try:
                    event = await asyncio.wait_for(self.display_events.get(), 1)
                except asyncio.TimeoutError:
                    continue
                if DISPLAY is not None:
                    if event['event'] in ('task.status','agent.status','approvals.snapshot'):
                        CHAT_STATE[event['event']] = event
                    logging.getLogger(__name__).info('chat stream event %s, id=%s, text_length=%d', event['event'], bool(event['message_id']), len(event['text']))
                    DISPLAY.event(event)
        except asyncio.CancelledError:
            pass

    def on_frame(self, frame):
        if frame.kind == "response" and frame.value.status == 200 and DISPLAY is not None:
            DISPLAY.set_status("Live chat")
            logging.getLogger(__name__).info("Live chat subscription connected")
        super().on_frame(frame)

    def abort(self):
        super().abort()
        self.display_task.cancel()

live_link._Subscription = DisplaySubscription
service_module.LinkSession = live_link.LinkSession
service_module.Outcome = live_link.Outcome

from settings import SSH, SSH_TARGET


class BoardExecutor:
    account = Account.current()
    def run(self, command, params, timeout_ms=None):
        if command == 'display.controls':
            return apply_controls(DISPLAY,params)
        if command == 'device.health':
            text = 'uname -a; uptime; free; df -h /; ip addr show eth0; cat /proc/fb'
        elif command == 'system.run':
            text = params.get('command')
            if not isinstance(text, str) or not text.strip(): return error('command is required')
            cwd = params.get('cwd', '/root')
            if not isinstance(cwd, str): return error('cwd must be a string')
            text = f'cd {shlex.quote(cwd)} && /bin/sh -c {shlex.quote(text)}'
        else:
            return error('unsupported board command')
        try:
            seconds = max(1, min(float(params.get('timeout_ms') or timeout_ms or 120000)/1000, 600))
            started = time.monotonic()
            proc = subprocess.run(SSH+[f'PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin /bin/sh -c {shlex.quote(text)}'],
                                  stdin=subprocess.DEVNULL, capture_output=True, timeout=seconds+10)
            return ok({'stdout': proc.stdout[:98304].decode(errors='replace'),
                       'stderr': proc.stderr[:98304].decode(errors='replace'),
                       'exit_code': proc.returncode, 'duration_ms': int((time.monotonic()-started)*1000),
                       'target': SSH_TARGET})
        except (OSError, ValueError, subprocess.TimeoutExpired) as exc:
            return error(str(exc))

COMMAND_SPECS['display.controls'] = CONTROL_SPEC
COMMAND_SPECS.pop('file.read', None)
COMMAND_SPECS.pop('file.write', None)
COMMAND_SPECS['system.run']['description'] = 'Run a shell command on the MA35H0 board over SSH/Ethernet using /bin/sh. The laptop acts as Bluetooth and Internet gateway.'
COMMAND_SPECS['device.health']['description'] = 'Report MA35H0 board kernel, uptime, RAM, storage, Ethernet address and framebuffer devices.'

class BoardService(Service):
    voice = None
    async def _local_request(self, line):
        request = json.loads(line)
        if isinstance(request,dict) and request.get('action')=='controls':
            return await asyncio.to_thread(apply_controls,DISPLAY,request.get('params',{}))
        if isinstance(request,dict) and request.get('action')=='diagnostics':
            return {'ok':True,'chat_state':CHAT_STATE}
        if isinstance(request,dict) and request.get('action')=='output.test':
            DISPLAY.event({'event':'message.assistant','message_id':'output-test','text':'Output test: this is a local display and headphone check.','seq':None})
            return {'ok':True}
        if isinstance(request, dict) and request.get('action') == 'voice.toggle':
            await self.voice.toggle()
            return {'ok': True, 'voice_state': DISPLAY.voice_state}
        return await super()._local_request(line)

async def serve():
    global DISPLAY
    DISPLAY = ChatDisplay(SSH)
    service = BoardService(identity.load_or_create(), BoardExecutor(), config.sdk_token(), display_name='MA35H0 display board (Ethernet gateway)')
    loop = asyncio.get_running_loop()
    for signum in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(signum, service.stop)
    voice = VoiceController(SSH, DISPLAY, lambda: service._current)
    service.voice = voice
    await voice.start()
    server = await service.serve_local(config.socket_path())
    try:
        await service.run()
    finally:
        await voice.close()
        server.close()
        await server.wait_closed()
        DISPLAY.close()

if __name__ == '__main__':
    logging.basicConfig(level=logging.INFO, format='%(asctime)s %(levelname)s %(name)s: %(message)s')
    if '--check' in sys.argv:
        result=BoardExecutor().run('device.health', {})
        print(result)
        raise SystemExit(0 if result['ok'] and result['payload']['exit_code']==0 else 1)
    asyncio.run(serve())
