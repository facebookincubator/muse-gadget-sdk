"""Local provisioning; secrets stay in ignored files, never in command output."""
import argparse
import asyncio
import hashlib
import json
import os
from pathlib import Path
import secrets
from urllib.parse import urlsplit
from store import Store
from musegadget.config import sdk_token

ROOT = Path(__file__).resolve().parent


def private_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(path, os.O_CREAT | os.O_TRUNC | os.O_WRONLY, 0o600)
    with os.fdopen(descriptor, 'w') as file:
        os.fchmod(file.fileno(), 0o600)
        json.dump(value, file)


def load_local():
    path = ROOT / '.env.redis.local'
    if path.exists():
        for line in path.read_text().splitlines():
            if '=' in line and not line.startswith('#'):
                key, value = line.split('=', 1)
                if key not in ('UPSTASH_REDIS_REST_URL', 'UPSTASH_REDIS_REST_TOKEN'):
                    raise ValueError('Unexpected variable in .env.redis.local')
                os.environ[key] = json.loads(value)


def prepare(address, root=ROOT.parent):
    url = urlsplit(address)
    if url.scheme != 'https' or not url.hostname or url.username or url.password or url.query or url.fragment or url.path not in ('', '/'):
        raise ValueError('Use an HTTPS origin without a path, credentials, query or fragment.')
    token_path = root / 'build/cloud-access.json'
    if not token_path.exists():
        private_json(token_path, {'token': secrets.token_urlsafe(32)})
    token = json.loads(token_path.read_text())['token']
    if not 32 <= len(token) <= 256 or any(char.isspace() for char in token):
        raise ValueError('The saved watch token is invalid.')
    private_json(root / 'build/vercel-watch-env.json', {'WATCH_API_TOKEN_SHA256': hashlib.sha256(token.encode()).hexdigest()})
    private_json(root / 'Watch/CloudBootstrap.json', {'address': address.rstrip('/'), 'token': token})


async def enroll(directory, store):
    identity = json.loads((directory / 'identity.json').read_text())
    pairing = json.loads((directory / 'pairing.json').read_text())
    token = sdk_token(directory)
    if not pairing.get('refresh_token') or not pairing.get('access_token') or not token:
        raise ValueError('Pair a dedicated Muse gadget before transferring enrollment.')
    mac = identity.get('mac', '').replace(':', '')
    if len(mac) != 12 or any(char not in '0123456789abcdefABCDEF' for char in mac):
        raise ValueError('Invalid Muse device identity.')
    state = {'node_id': 'homelink-' + mac[-6:].lower(), 'pairing': pairing, 'sdk_token': token}
    if await store.command('SET', 'muse:credentials', json.dumps(state), 'NX') != 'OK':
        raise RuntimeError('Cloud enrollment already exists; it was not overwritten.')


async def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['prepare', 'enroll'])
    parser.add_argument('--address')
    parser.add_argument('--state-dir', type=Path)
    args = parser.parse_args()
    if args.action == 'prepare':
        if not args.address: parser.error('prepare requires --address')
        prepare(args.address)
        print('Prepared private Watch/CloudBootstrap.json and build/vercel-watch-env.json. Existing watch key preserved.')
    else:
        if not args.state_dir: parser.error('enroll requires --state-dir')
        load_local()
        store = Store()
        if await store.command('PING') != 'PONG': raise RuntimeError('Redis connectivity failed.')
        await enroll(args.state_dir, store)
        print('Enrollment transferred once; keep the original gadget service stopped.')


if __name__ == '__main__':
    asyncio.run(main())
