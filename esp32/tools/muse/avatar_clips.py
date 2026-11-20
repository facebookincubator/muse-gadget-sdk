#!/usr/bin/env python3
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

"""Export the paired avatar's clips, convert them, and install a preview pack.

This development tool uses USB + the existing authenticated gadget session;
no token is copied to the computer. It does not regenerate or change the phone
avatar. A computer is currently required for conversion and installation.
Use --help. Phone clips do not supply talking/listening/petting/error/off rigs.
"""
import argparse
import base64
import json
import hashlib
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import time
import zlib

KINDS = ('idle', 'waking', 'working', 'making_something', 'waiting_for_subagents', 'milestone_level_up')
MAX_PACK = 1024 * 1024
HEADER = struct.Struct('<8sIHHHHII32sI')
CLIP = struct.Struct('<HHIII')
FRAME = struct.Struct('<II')


def private_write(path, data):
    path = Path(path)
    fd, temporary=tempfile.mkstemp(prefix='.'+path.name+'-',dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(temporary,path)
    finally:
        if os.path.exists(temporary): os.unlink(temporary)


def receive(board, prefix, seconds=20):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        line = board.read_line(end)
        if not line:
            continue
        if '@avatar_export_error ' in line:
            raise RuntimeError(line)
        if '@avatar_install_error ' in line:
            raise RuntimeError(line[line.index('@avatar_install_error '):])
        at = line.find(prefix)
        if at >= 0:
            return line[at + len(prefix):]
    raise TimeoutError('Timed out waiting for ' + prefix)



def request(board, command, prefix):
    for attempt in range(4):
        board.write_line(command)
        try:
            return receive(board, prefix)
        except (RuntimeError, TimeoutError) as error:
            if attempt == 3 or (isinstance(error, RuntimeError) and
                    not any(reason in str(error) for reason in ('busy', 'request_failed', 'unavailable', 'online_0', 'metadata_status_0'))):
                raise RuntimeError(command + ": " + str(error)) from error
            print(json.dumps({"retry":command,"reason":str(error),"attempt":attempt+1}),flush=True)
            time.sleep(min(4, 0.25 * 2**attempt))


def export_clips(port, target, kinds):
    import chat
    target = Path(target)
    target.mkdir(parents=True, exist_ok=True, mode=0o700)
    with chat.Board(port) as b:
        b.write(b'w')  # An explicit export is foreground maintenance.
        deadline=time.monotonic()+90
        while True:
            status=b.status() or {}
            link=status.get('device',{}).get('link',{})
            if not link.get('paired'):
                raise RuntimeError('A paired gadget must answer status before export')
            if link.get('state')=='Online': break
            if time.monotonic()>deadline: raise TimeoutError('Waiting for the paired Muse connection')
            b.drain(0.5)
        manifest = json.loads(request(b, 'avatar.assets', '@avatar_assets '))
        manifest['files']={}
        partial=target/'export-checkpoint.json'
        if partial.exists():
            prior=json.loads(partial.read_text())
            if prior.get('source')==manifest['source']:manifest['files']=prior.get('files',{})
        for kind in kinds:
            saved=manifest['files'].get(kind)
            existing=target/(kind+'.mp4')
            if saved and existing.exists():
                old=existing.read_bytes()
                if len(old)==saved['bytes'] and hashlib.sha256(old).hexdigest()==saved['sha256']:
                    print(json.dumps({'reused':kind,'bytes':len(old)}),flush=True)
                    continue
            index = KINDS.index(kind)
            if not manifest['coverage'] & (1 << index):
                continue
            data = bytearray()
            while len(data) < 2 * 1024 * 1024:
                offset = len(data)
                for attempt in range(4):
                    meta = json.loads(request(b, 'avatar.asset=%d,%d' % (index, offset), '@avatar_asset '))
                    if meta['kind'] != index or meta['offset'] != offset:
                        raise ValueError('Out-of-order export response')
                    chunk = bytearray()
                    invalid=False
                    end = time.monotonic() + 15
                    while time.monotonic() < end:
                        line = b.read_line(end)
                        if not line: continue
                        if line == '@avatar_end': break
                        if line.startswith('@avatar_bytes '):
                            try: chunk.extend(base64.b64decode(line.split(' ', 1)[1], validate=True))
                            except ValueError: invalid=True
                    else: invalid=True
                    if not invalid and len(chunk)==meta['len'] and zlib.crc32(chunk)==meta['crc']: break
                    if attempt==3: raise ValueError('Export checksum mismatch at offset %d'%offset)
                    print(json.dumps({'retry_offset':offset,'expected_bytes':meta['len'],'received_bytes':len(chunk)}),flush=True)
                    time.sleep(0.1)
                data.extend(chunk)
                if len(data)//65536 != offset//65536:
                    print(json.dumps({'downloading':kind,'bytes':len(data)}),flush=True)
                if meta['status'] == 416 or len(chunk) < 4096:
                    break
            else:
                raise ValueError('Phone asset exceeds 2 MiB bound')
            if not data:
                raise ValueError('Empty phone asset')
            private_write(target / (kind + '.mp4'), data)
            manifest['files'][kind]={'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}
            private_write(partial,(json.dumps(manifest,indent=2)+'\n').encode())
            print(json.dumps({'exported': kind, 'bytes': len(data)}), flush=True)
        # A daily change during export invalidates the whole candidate.
        latest = json.loads(request(b, 'avatar.assets', '@avatar_assets '))
        if latest['source'] != manifest['source']:
            raise ValueError('Avatar changed during export; retry')
    private_write(target / 'manifest.json', (json.dumps(manifest, indent=2) + '\n').encode())


def access_units(data):
    nals = list(re.finditer(b'\x00\x00(?:\x00)?\x01', data))
    aud = [m.start() for m in nals if m.end() < len(data) and data[m.end()] & 31 == 9]
    if not aud or aud[0] != 0:
        raise ValueError('Expected AUD-prefixed Annex B')
    units = [data[a:b] for a, b in zip(aud, aud[1:] + [len(data)])]
    # Normalize three-byte prefixes so the firmware has one unambiguous format.
    units = [b'\x00' + u if u.startswith(b'\x00\x00\x01') else u for u in units]
    types = {data[m.end()] & 31 for m in nals if m.end() < (aud[1] if len(aud) > 1 else len(data))}
    if not {5, 7, 8}.issubset(types):
        raise ValueError('First access unit must carry SPS, PPS, and IDR')
    if not 1 <= len(units) <= 240 or any(not 6 <= len(u) <= 32768 for u in units):
        raise ValueError('Frame count or access-unit size exceeds firmware bounds')
    return units


def convert_clip(path, ffmpeg='ffmpeg', edge=240, fps=16, crf=20):
    if edge not in (160, 192, 224, 240) or not 1 <= fps <= 24 or not 16 <= crf <= 32:
        raise ValueError('Unsupported dimensions or frame rate')
    with tempfile.TemporaryDirectory(prefix='muse-avatar-') as tmp:
        output = str(Path(tmp) / 'clip.h264')
        subprocess.run([ffmpeg, '-nostdin', '-v', 'error', '-y', '-i', str(path),
            '-map', '0:v:0', '-an', '-sn', '-dn', '-t', '10', '-frames:v', '240',
            '-vf', 'fps=%d,scale=%d:%d:force_original_aspect_ratio=decrease,pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black,setsar=1' % (fps, edge, edge, edge, edge),
            '-c:v', 'libx264', '-profile:v', 'baseline', '-level:v', '2.0',
            '-pix_fmt', 'yuv420p', '-preset', 'medium', '-crf', str(crf),
            '-maxrate', '400k', '-bufsize', '400k',
            '-x264-params', 'aud=1:repeat-headers=1:ref=1:bframes=0:cabac=0:keyint=240:min-keyint=240:scenecut=0:open-gop=0',
            '-f', 'h264', output], check=True, timeout=120, capture_output=True)
        encoded = Path(output).read_bytes()
    return access_units(encoded)


def make_pack(source, clips, edge=240, fps=16):
    if len(source) != 32 or not clips or 'idle' not in clips:
        raise ValueError('Pack needs an exact source binding and idle clip')
    if edge not in (160, 192, 224, 240) or not 1 <= fps <= 24:
        raise ValueError('Unsupported dimensions or frame rate')
    payload = bytearray()
    mask = 0
    for kind, units in sorted(clips.items(), key=lambda item: KINDS.index(item[0])):
        if not 1 <= len(units) <= 240 or any(not 6 <= len(u) <= 32768 for u in units):
            raise ValueError('Frame bounds exceeded')
        body = b''.join(FRAME.pack(len(u), zlib.crc32(u)) + u for u in units)
        index = KINDS.index(kind)
        loop = kind not in ('waking', 'milestone_level_up')
        payload.extend(CLIP.pack(index, int(loop), len(units), len(body), zlib.crc32(body)))
        payload.extend(body)
        mask |= 1 << index
    size = HEADER.size + len(payload)
    if size > MAX_PACK:
        raise ValueError('Pack exceeds 1 MiB; select fewer clips or reduce resolution/frame rate')
    header = HEADER.pack(b'MAVPK01\0', size, edge, edge, fps, len(clips), mask, zlib.crc32(payload), source, 0)
    return header[:-4] + struct.pack('<I', zlib.crc32(header[:-4])) + payload


def convert(directory, output, kinds, edge, fps, ffmpeg, crf=20):
    directory = Path(directory)
    manifest = json.loads((directory / 'manifest.json').read_text())
    clips={}
    for kind in kinds:
        record=manifest.get('files',{}).get(kind)
        if not record: continue
        path=directory/(kind+'.mp4')
        data=path.read_bytes()
        if len(data)!=record['bytes'] or hashlib.sha256(data).hexdigest()!=record['sha256']:
            raise ValueError('Exported clip does not match its manifest')
        clips[kind]=convert_clip(path,ffmpeg,edge,fps,crf)
    pack = make_pack(bytes.fromhex(manifest['source']), clips, edge, fps)
    private_write(output, pack)
    print(json.dumps({'pack_bytes': len(pack), 'edge': edge, 'fps': fps, 'clips': list(clips), 'parity_complete': False}))


def install(port, path):
    import chat
    data = Path(path).read_bytes()
    if len(data) < 64 or len(data) > MAX_PACK:
        raise ValueError('Pack size outside firmware bounds')
    with chat.Board(port) as b:
        if not b.status():
            raise RuntimeError('Gadget did not answer status before installation')
        b.write(b'w')
        manifest=json.loads(request(b,'avatar.assets','@avatar_assets '))
        if manifest.get('source')!=data[28:60].hex():
            raise ValueError('Avatar changed: export and convert its current clips before installation')
        def command(cmd, timeout=20):
            b.write_line(cmd)
            if receive(b, '@avatar_install ', timeout) != 'ok':
                raise RuntimeError('Firmware rejected installation')
        try:
            command('avatar.motion.begin=%d' % len(data))
            for offset in range(0, len(data), 600):
                command('avatar.motion.chunk=%d:%s' % (offset, base64.b64encode(data[offset:offset+600]).decode()))
            command('avatar.motion.commit', 60)
        except BaseException:
            try:
                command('avatar.motion.abort')
            except Exception:
                pass
            raise
        b.write_line('avatar.motion')
        print(receive(b, '@avatar_motion '))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    exp = sub.add_parser('export'); exp.add_argument('--port', required=True); exp.add_argument('--directory', required=True)
    conv = sub.add_parser('convert'); conv.add_argument('--directory', required=True); conv.add_argument('--output', required=True)
    conv.add_argument('--edge', type=int, default=192); conv.add_argument('--fps', type=int, default=20); conv.add_argument('--ffmpeg', default='ffmpeg')
    conv.add_argument('--crf', type=int, default=20, help='H.264 quality, 16..32; lower is sharper (default: 20)')
    for p in (exp, conv):
        p.add_argument('--clips', nargs='+', choices=KINDS, default=['idle', 'waking', 'working'])
    inst = sub.add_parser('install'); inst.add_argument('--port', required=True); inst.add_argument('pack')
    a = parser.parse_args()
    if a.action == 'export': export_clips(a.port, a.directory, a.clips)
    elif a.action == 'convert': convert(a.directory, a.output, a.clips, a.edge, a.fps, a.ffmpeg,a.crf)
    elif a.action == 'install': install(a.port, a.pack)


if __name__ == '__main__':
    main()
