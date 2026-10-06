import asyncio
import hashlib
import io
import uuid
import numpy as np
import pytest
import soundfile as sf
from fastapi.testclient import TestClient
from api import index
import calm_audio
TOKEN = 'c' * 43
AUTH = {'Authorization': 'Bearer ' + TOKEN}
class Redis:
    state = {}
    async def command(self, *args):
        if args[0] == 'SET':
            if args[1] in self.state: return None
            self.state[args[1]] = args[2]
            return 'OK'
        if args[0] == 'EVAL':
            key, owner = args[-2:]
            if self.state.get(key) == owner: del self.state[key]
            return 1
        raise AssertionError(args)
@pytest.fixture(autouse=True)
def setup(monkeypatch):
    Redis.state = {}
    monkeypatch.setenv('WATCH_API_TOKEN_SHA256', hashlib.sha256(TOKEN.encode()).hexdigest())
    monkeypatch.setattr(index, 'Store', Redis)
def command(**extra):
    return {'request_id': str(uuid.uuid4()), 'kind': 'sound', 'scene': 'brush', **extra}
@pytest.mark.parametrize('body', [None, {}, command(kind='shell'), command(scene='mix'), command(scene=[]),
    command(request_id='invalid'), command(kind='voice', text='x'*241, voice='af_heart'),
    command(kind='voice', text='Relax for a moment.', voice='unknown')])
def test_audio_validation(body):
    assert TestClient(index.app).post('/v1/calm/audio', json=body, headers=AUTH).status_code == 400
    assert Redis.state == {}
def test_audio_authorization_before_generation(monkeypatch):
    def forbidden(*args): raise AssertionError('should not generate')
    monkeypatch.setattr(calm_audio, 'render', forbidden)
    assert TestClient(index.app).post('/v1/calm/audio', json=command()).status_code == 401
    assert TestClient(index.app).post('/v1/calm/audio', content='x'*4097, headers=AUTH).status_code == 413
    assert Redis.state == {}
def test_stream_large_payload_dedupe_and_release(monkeypatch):
    payload = b'RIFF' + bytes(6 * 1024 * 1024)
    monkeypatch.setattr(calm_audio, 'render', lambda *_: (payload, {'X-Audio-Duration': '60'}))
    client = TestClient(index.app); body = command()
    response = client.post('/v1/calm/audio', json=body, headers=AUTH)
    assert response.status_code == 200 and response.content == payload
    assert response.headers['x-audio-sha256'] == hashlib.sha256(payload).hexdigest()
    assert response.headers['cache-control'] == 'no-store'
    assert 'calm:lease:sound' not in Redis.state
    assert client.post('/v1/calm/audio', json=body, headers=AUTH).status_code == 409
    assert 'calm:lease:sound' not in Redis.state
def test_voice_and_sound_use_separate_leases(monkeypatch):
    Redis.state['calm:lease:sound'] = 'another'
    monkeypatch.setattr(calm_audio, 'render', lambda *_: (b'RIFFvoice', {'X-Audio-Duration': '5'}))
    client = TestClient(index.app)
    assert client.post('/v1/calm/audio', json=command(), headers=AUTH).status_code == 409
    assert client.post('/v1/calm/audio', json=command(kind='voice', text='Let the world wait a moment.', voice='af_heart'), headers=AUTH).status_code == 200
    assert Redis.state['calm:lease:sound'] == 'another'
def test_generation_failure_is_sanitized_and_stops_worker(monkeypatch):
    stops = []
    def broken(cmd, stop):
        stops.append(stop)
        raise RuntimeError('private service detail')
    monkeypatch.setattr(calm_audio, 'render', broken)
    response = TestClient(index.app).post('/v1/calm/audio', json=command(), headers=AUTH)
    assert response.status_code == 503 and 'private service detail' not in response.text
    assert stops[0].is_set() and 'calm:lease:sound' not in Redis.state
def test_cancel_releases_and_signals_thread(monkeypatch):
    stops = []
    async def cancelled(render, cmd, stop):
        stops.append(stop)
        raise asyncio.CancelledError()
    monkeypatch.setattr(asyncio, 'to_thread', cancelled)
    from starlette.requests import Request
    import json
    async def run():
        async def receive(): return {'type': 'http.request', 'body': json.dumps(command()).encode()}
        request = Request({'type': 'http', 'headers': [(b'authorization', AUTH['Authorization'].encode())]}, receive)
        with pytest.raises(asyncio.CancelledError): await index.calm(request)
    asyncio.run(run())
    assert stops[0].is_set() and 'calm:lease:sound' not in Redis.state
def test_stereo_audio_preserves_detail_and_limits_peaks():
    rate = 44100
    t = np.arange(rate * 64) / rate
    source = np.column_stack([0.2*np.sin(2*np.pi*210*t), 0.1*np.sin(2*np.pi*320*t)])
    source[rate*10, 0] = 4
    payload, duration = calm_audio.encode_audio(source, rate, sound=True)
    data, actual_rate = sf.read(io.BytesIO(payload))
    assert actual_rate == rate and duration == 60
    assert data.shape == (rate*60, 2)
    assert np.max(np.abs(data)) <= 10**(-5/20) + 1/32768
    assert np.max(np.abs(data[0])) == 0 and np.max(np.abs(data[-1])) == 0
    assert not np.allclose(data[:, 0], data[:, 1])
    assert sf.info(io.BytesIO(payload)).subtype == 'PCM_16'
@pytest.mark.parametrize('data,rate', [(np.zeros(24000),24000), (np.full(24000,np.nan),24000), (np.ones(100),24000)])
def test_bad_generated_audio_is_rejected(data, rate):
    with pytest.raises(ValueError): calm_audio.encode_audio(data, rate)

@pytest.mark.parametrize('recipe', [{'brightness': 0.2}, dict(brightness=2,activity=.3,pace=.3,space=.6),
    dict(brightness=True,activity=.3,pace=.3,space=.6), dict(brightness=.3,activity=.3,pace=float('inf'),space=.6), []])
def test_bad_sound_recipes_are_rejected(recipe):
    with pytest.raises(ValueError): index.calm_command(command(recipe=recipe))


def test_muse_recipe_reaches_real_cpu_renderer():
    recipe = dict(brightness=.3,activity=.35,pace=.25,space=.65)
    response = TestClient(index.app).post('/v1/calm/audio',json=command(recipe=recipe),headers=AUTH)
    assert response.status_code == 200
    assert response.headers['x-audio-generator'] == 'procedural'
    assert response.headers['x-audio-director'] == 'muse'
    data, rate = sf.read(io.BytesIO(response.content))
    assert data.shape == (44100*60,2) and rate == 44100
    assert np.isfinite(data).all() and np.max(np.abs(data)) <= 10**(-5/20)+1/32768


@pytest.mark.parametrize('scene', ['brush','fabric','rain','leaves','water','wood'])
def test_all_textures_are_finite_stereo_and_not_silent(scene):
    from calm_synthesis import samples
    import threading
    data,rate,seed = samples(scene,threading.Event(),seed=12345)
    assert data.shape == (64*rate,2) and np.isfinite(data).all()
    assert np.sqrt(np.mean(data**2)) > 1e-4
    assert not np.allclose(data[:,0],data[:,1])


def test_renderer_changes_with_seed_and_honors_cancellation():
    from calm_synthesis import samples
    import threading
    first,_,_ = samples('brush',threading.Event(),seed=10)
    second,_,_ = samples('brush',threading.Event(),seed=11)
    assert not np.array_equal(first,second)
    stop=threading.Event();stop.set()
    with pytest.raises(TimeoutError): samples('rain',stop)

@pytest.mark.parametrize('style,text', [('af_nicole','Thanks.'),('am_michael','Yes.'),('af_heart','Hi.')])
def test_call_voices_and_short_lines_are_valid(style,text):
    parsed = index.calm_command(command(kind='voice',voice=style,text=text))
    assert parsed['voice'] == style and parsed['text'] == text


def test_call_voices_use_the_english_tokenizer(monkeypatch):
    import threading
    calls=[]
    class Model:
        def create(self,text,**options):
            calls.append((text,options))
            t=np.arange(24000)/24000
            return .1*np.sin(2*np.pi*200*t),24000
    monkeypatch.setattr(calm_audio,'_voice_model',Model())
    for style in ('af_heart','am_michael'):
        calm_audio.voice('Hello there.',style,threading.Event())
    assert all(options['lang']=='en-us' and 'is_phonemes' not in options for _,options in calls)
