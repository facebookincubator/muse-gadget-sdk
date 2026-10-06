import asyncio
import hashlib
import json
import time
import uuid
from unittest.mock import patch
import pytest
from fastapi.testclient import TestClient
from api import index
import relay
from store import Store, StoreError

TOKEN = 't' * 43
AUTH = {'Authorization': 'Bearer ' + TOKEN}


class MemoryStore:
    owner = None
    requests = set()
    released = []
    state = {}
    async def acquire(self, owner):
        if self.owner: return False
        type(self).owner = owner
        return True
    async def release(self, owner):
        self.released.append(owner)
        if self.owner == owner: type(self).owner = None
    async def reserve(self, request):
        if request in self.requests: return False
        self.requests.add(request)
        return True
    async def load(self): return json.loads(json.dumps(self.state))
    async def save(self, state, owner): type(self).state = state


@pytest.fixture(autouse=True)
def setup(monkeypatch):
    monkeypatch.setenv('WATCH_API_TOKEN_SHA256', hashlib.sha256(TOKEN.encode()).hexdigest())
    MemoryStore.owner = None
    MemoryStore.requests = set()
    MemoryStore.released = []
    MemoryStore.state = {'node_id': 'homelink-123456', 'pairing': {
        'access_token': 'access', 'refresh_token': 'refresh', 'access_token_saved_at': int(time.time())}}
    monkeypatch.setattr(index, 'Store', MemoryStore)


def command(**extra):
    return {'op': 'chat', 'message': 'Hello 🌍', 'session_id': str(uuid.uuid4()),
        'request_id': str(uuid.uuid4()), **extra}


async def fake_stream(store, owner, request):
    try:
        yield {'type': 'ack'}
        yield {'type': 'reply', 'message_id': 'reply', 'text': 'Hello 🌍', 'complete': True}
        yield {'type': 'turn_finished'}
    finally:
        await store.release(owner)


def test_liveness_is_not_muse_status():
    response = TestClient(index.app).get('/health')
    assert response.status_code == 200
    assert response.json() == {'service': 'muse-watch', 'status': 'running', 'protocol': 1}
    assert response.headers['cache-control'] == 'no-store'


def test_auth_before_storage_or_body():
    client = TestClient(index.app)
    for headers in ({}, {'Authorization': 'Bearer ' + 'x' * 43}):
        assert client.post('/v1/muse', content='not json', headers=headers).status_code == 401
    assert not MemoryStore.requests


@pytest.mark.parametrize('body', [None, [], {}, command(op='pair'), command(op='system.run'),
    command(message=' '), command(message='x' * 32001), command(session_id='../secret'),
    command(request_id='bad'), command(op='watch_result')])
def test_reject_invalid_commands(body):
    assert TestClient(index.app).post('/v1/muse', json=body, headers=AUTH).status_code == 400
    assert not MemoryStore.requests


def test_request_body_limit():
    assert TestClient(index.app).post('/v1/muse', content='x' * 65537, headers=AUTH).status_code == 413


def test_news_opt_in_is_boolean_and_not_implicitly_added():
    assert index.command_from(command(walking_news=True))['walking_news'] is True
    assert 'walking_news' not in index.command_from(command())
    for value in ('yes', 1, [], {}):
        with pytest.raises(ValueError): index.command_from(command(walking_news=value))


def test_stream_and_duplicate_not_reposted(monkeypatch):
    monkeypatch.setattr(index, 'stream', fake_stream)
    client = TestClient(index.app)
    body = command()
    response = client.post('/v1/muse', json=body, headers=AUTH)
    assert response.status_code == 200
    events = [json.loads(line) for line in response.text.splitlines()]
    assert [e['type'] for e in events] == ['ack', 'reply', 'turn_finished']
    assert events[1]['text'] == 'Hello 🌍'
    assert MemoryStore.owner is None
    assert client.post('/v1/muse', json=body, headers=AUTH).status_code == 409


def test_concurrent_request_refused_without_stealing_lease():
    MemoryStore.owner = 'other'
    response = TestClient(index.app).post('/v1/muse', json=command(), headers=AUTH)
    assert response.status_code == 409
    assert MemoryStore.owner == 'other'
    assert not MemoryStore.requests


def test_storage_failure_closes_lease(monkeypatch):
    async def broken(self): raise StoreError('private detail')
    monkeypatch.setattr(MemoryStore, 'load', broken)
    response = TestClient(index.app).post('/v1/muse', json=command(), headers=AUTH)
    assert response.status_code == 503
    assert 'private detail' not in response.text
    assert MemoryStore.owner is None


def test_refresh_is_persisted_before_use():
    MemoryStore.state['pairing']['access_token_saved_at'] = 0
    with patch.object(relay.muse_api, 'refresh_device_token', return_value=({'access_token': 'new', 'refresh_token': 'next'}, 200)):
        result = asyncio.run(relay.credentials(MemoryStore(), 'owner'))
    assert result['pairing']['refresh_token'] == 'next'
    assert MemoryStore.state['pairing']['refresh_token'] == 'next'


def test_revoked_refresh_disables_future_calls():
    MemoryStore.state['pairing']['access_token_saved_at'] = 0
    with patch.object(relay.muse_api, 'refresh_device_token', return_value=(None, 401)) as refresh:
        for _ in range(2):
            with pytest.raises(relay.RelayError): asyncio.run(relay.credentials(MemoryStore(), 'owner'))
        assert refresh.call_count == 1
    assert MemoryStore.state['revoked'] is True


def test_refresh_storage_failure_never_uses_unpersisted_tokens(monkeypatch):
    MemoryStore.state['pairing']['access_token_saved_at'] = 0
    async def fail(*args): raise StoreError('failed')
    monkeypatch.setattr(MemoryStore, 'save', fail)
    with patch.object(relay.muse_api, 'refresh_device_token', return_value=({'access_token': 'new', 'refresh_token': 'next'}, 200)):
        with pytest.raises(StoreError): asyncio.run(relay.credentials(MemoryStore(), 'owner'))


def test_stream_error_is_sanitized_and_releases():
    async def bad(*args):
        yield {'type': 'ack'}
        raise ValueError('secret-token')
    async def run():
        return [event async for event in relay.stream(MemoryStore(), 'owner', command(), producer=bad)]
    result = asyncio.run(run())
    assert result[-1] == {'type': 'turn_finished'}
    assert result[-2]['type'] == 'error'
    assert 'secret-token' not in json.dumps(result)
    assert MemoryStore.released == ['owner']


def test_disconnect_closes_producer_and_releases():
    closed = []
    async def waiting(*args):
        try:
            yield {'type': 'ack'}
            await asyncio.sleep(300)
        finally: closed.append(True)
    async def run():
        source = relay.stream(MemoryStore(), 'owner', command(), producer=waiting)
        await anext(source)
        await source.aclose()
    asyncio.run(run())
    assert closed == [True]
    assert MemoryStore.released == ['owner']


def test_oversize_response_closes_stream(monkeypatch):
    async def large(store, owner, cmd):
        try: yield {'type': 'reply', 'text': 'x' * (256 * 1024)}
        finally: await store.release(owner)
    monkeypatch.setattr(index, 'stream', large)
    response = TestClient(index.app).post('/v1/muse', json=command(), headers=AUTH)
    assert [json.loads(line)['type'] for line in response.text.splitlines()] == ['error', 'turn_finished']
    assert MemoryStore.owner is None


def test_no_host_tools_registered():
    assert relay.reject_tool('system.run', {'command': 'anything'})['ok'] is False


def test_redis_lease_and_idempotency_contract(monkeypatch):
    monkeypatch.setenv('UPSTASH_REDIS_REST_URL', 'https://example.test')
    monkeypatch.setenv('UPSTASH_REDIS_REST_TOKEN', 'test')
    seen = []
    async def capture(self, *parts):
        seen.append(parts)
        return 'OK' if parts[0] == 'SET' else 1
    monkeypatch.setattr(Store, 'command', capture)
    async def run():
        store = Store()
        assert await store.acquire('owner')
        assert await store.reserve('request')
        await store.release('owner')
    asyncio.run(run())
    assert seen[0] == ('SET', 'muse:lease', 'owner', 'NX', 'EX', 150)
    assert seen[1][-4:] == ('accepted', 'NX', 'EX', 86400)
    assert "== ARGV[1]" in seen[2][1]
