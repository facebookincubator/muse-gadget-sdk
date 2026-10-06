import asyncio
import hashlib
import json
import stat
import pytest
from provision import prepare, private_json, enroll


def test_prepare_is_private_and_preserves_key(tmp_path):
    prepare('https://calm.example/', tmp_path)
    bootstrap = tmp_path / 'Watch/CloudBootstrap.json'
    value = json.loads(bootstrap.read_text())
    assert value['address'] == 'https://calm.example'
    assert len(value['token']) >= 32
    assert stat.S_IMODE(bootstrap.stat().st_mode) == 0o600
    expected = hashlib.sha256(value['token'].encode()).hexdigest()
    assert json.loads((tmp_path / 'build/vercel-watch-env.json').read_text()) == {'WATCH_API_TOKEN_SHA256': expected}
    prepare('https://new.example', tmp_path)
    changed = json.loads(bootstrap.read_text())
    assert changed['token'] == value['token'] and changed['address'] == 'https://new.example'


@pytest.mark.parametrize('address', ['http://calm.example', 'https://calm.example/path',
    'https://key@calm.example', 'https://calm.example?q=1', 'https://calm.example/#fragment'])
def test_invalid_origin_writes_nothing(tmp_path, address):
    with pytest.raises(ValueError): prepare(address, tmp_path)
    assert list(tmp_path.iterdir()) == []


def test_private_rewrite_restricts_existing_file(tmp_path):
    path = tmp_path / 'secret.json'
    path.write_text('{}'); path.chmod(0o644)
    private_json(path, {'test': 'value'})
    assert stat.S_IMODE(path.stat().st_mode) == 0o600


def test_enrollment_does_not_overwrite_rotated_tokens(tmp_path, monkeypatch):
    monkeypatch.delenv('MUSEGADGET_SDK_TOKEN', raising=False)
    (tmp_path / 'identity.json').write_text(json.dumps({'mac': '02:00:00:ab:cd:ef'}))
    (tmp_path / 'pairing.json').write_text(json.dumps({'access_token': 'test-access', 'refresh_token': 'test-refresh'}))
    (tmp_path / 'sdk_token').write_text('mgst_' + 'A' * 43)
    class Memory:
        state = None
        async def command(self, *args):
            assert args[:2] == ('SET', 'muse:credentials') and args[-1] == 'NX'
            if self.state is not None: return None
            self.state = json.loads(args[2]); return 'OK'
    store = Memory()
    asyncio.run(enroll(tmp_path, store))
    assert store.state['node_id'] == 'homelink-abcdef'
    store.state['pairing']['refresh_token'] = 'rotated-test-token'
    with pytest.raises(RuntimeError): asyncio.run(enroll(tmp_path, store))
    assert store.state['pairing']['refresh_token'] == 'rotated-test-token'
