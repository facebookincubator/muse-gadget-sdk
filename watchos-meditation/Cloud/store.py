"""Small, fail-closed Redis REST adapter; no credentials or prompts in logs."""
import asyncio
import json
import os
import urllib.request


class StoreError(RuntimeError):
    pass


class Store:
    def __init__(self):
        self.url = os.environ.get('KV_REST_API_URL') or os.environ.get('UPSTASH_REDIS_REST_URL', '')
        self.token = os.environ.get('KV_REST_API_TOKEN') or os.environ.get('UPSTASH_REDIS_REST_TOKEN', '')
        if not self.url.startswith('https://') or not self.token:
            raise StoreError('Cloud storage is not configured.')

    async def command(self, *parts):
        def send():
            request = urllib.request.Request(self.url, data=json.dumps(parts).encode(),
                headers={'Authorization': 'Bearer ' + self.token, 'Content-Type': 'application/json'})
            try:
                with urllib.request.urlopen(request, timeout=8) as response:
                    raw = response.read(1024 * 1024 + 1)
                if len(raw) > 1024 * 1024:
                    raise StoreError('Storage response exceeded its limit.')
                result = json.loads(raw)
                if 'error' in result:
                    raise StoreError('Storage rejected the operation.')
                return result['result']
            except Exception:
                raise StoreError('Cloud storage is unavailable.') from None
        return await asyncio.to_thread(send)

    async def load(self):
        value = await self.command('GET', 'muse:credentials')
        if not value:
            raise StoreError('Muse enrollment has not been transferred to the cloud.')
        return json.loads(value)

    async def save(self, value, owner):
        # Only the holder of the device lease may rotate its tokens.
        result = await self.command('EVAL',
            "if redis.call('GET',KEYS[1]) == ARGV[1] then "
            "redis.call('SET',KEYS[2],ARGV[2]); return 1 else return 0 end",
            2, 'muse:lease', 'muse:credentials', owner, json.dumps(value))
        if result != 1:
            raise StoreError('The Muse connection lease expired.')

    async def acquire(self, owner):
        return await self.command('SET', 'muse:lease', owner, 'NX', 'EX', 150) == 'OK'

    async def release(self, owner):
        await self.command('EVAL',
            "if redis.call('GET',KEYS[1]) == ARGV[1] then return redis.call('DEL',KEYS[1]) else return 0 end",
            1, 'muse:lease', owner)

    async def reserve(self, request_id):
        # Retain only an opaque ID, never the request text or measurements.
        return await self.command('SET', 'muse:request:' + request_id, 'accepted', 'NX', 'EX', 86400) == 'OK'
