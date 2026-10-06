# SPDX-License-Identifier: Apache-2.0
import asyncio
import struct
from voice_control import VoiceController,wav_stream_header,RATE

class Display:
    def __init__(self):self.state=None
    def set_voice(self,state):self.state=state

class FakeSession:
    registered_at=1

def test_wav_stream_header_matches_vendor_mono_pcm_format():
    h=wav_stream_header()
    v=struct.unpack('<4sI4s4sIHHIIHH4sI',h)
    assert len(h)==44
    assert v==(b'RIFF',0xffffffff,b'WAVE',b'fmt ',16,1,1,RATE,RATE*2,2,16,b'data',0xffffffff)

def test_first_press_starts_second_press_stops_and_does_not_restart():
    async def scenario():
        d=Display();v=VoiceController([],d,lambda:FakeSession())
        started=asyncio.Event();finished=asyncio.Event();calls=[]
        async def record():started.set();await finished.wait()
        async def stop():calls.append('stop')
        v.record=record;v.stop_capture=stop
        await v.toggle();await started.wait()
        assert d.state=='recording'
        await v.toggle()
        assert calls==['stop'] and d.state=='sending'
        await v.toggle()
        assert calls==['stop']
        finished.set();await v.record_task
    asyncio.run(scenario())

def test_press_without_muse_connection_does_not_open_mic():
    async def scenario():
        d=Display();v=VoiceController([],d,lambda:None)
        await v.toggle()
        assert d.state=='error' and v.record_task is None
    asyncio.run(scenario())


def test_completed_wav_header_has_exact_file_and_data_sizes():
    import io,wave
    data=b'\x01\x00'*16000
    wav=wav_stream_header(len(data))+data
    assert struct.unpack_from('<I',wav,4)[0]==len(wav)-8
    with wave.open(io.BytesIO(wav),'rb') as f:
        assert f.getnframes()==16000
        assert f.readframes(16000)==data
