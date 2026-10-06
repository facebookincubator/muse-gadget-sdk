import hashlib
import hmac
import json
import os
import uuid
import asyncio
import threading
import contextlib
import logging
import anyio
from contextlib import aclosing
from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse, StreamingResponse
from relay import stream
from store import Store, StoreError
import calm_audio
from calm_synthesis import recipe_from

app = FastAPI(docs_url=None, redoc_url=None, openapi_url=None)
HEADERS = {'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff'}


def failure(code, message):
    return JSONResponse({'error': message}, status_code=code, headers=HEADERS)


def authorized(header):
    expected = os.environ.get('WATCH_API_TOKEN_SHA256', '')
    if len(expected) != 64 or not header.startswith('Bearer '):
        return False
    token = header[7:]
    return 32 <= len(token) <= 256 and hmac.compare_digest(hashlib.sha256(token.encode()).hexdigest(), expected)


def command_from(value):
    if not isinstance(value, dict) or value.get('op') not in ('check', 'chat'):
        raise ValueError('Unsupported command.')
    request_id = value.get('request_id')
    if not isinstance(request_id, str) or str(uuid.UUID(request_id)) != request_id:
        raise ValueError('Invalid request identifier.')
    if value['op'] == 'check':
        return {'op': 'check', 'request_id': request_id}
    message, session = value.get('message'), value.get('session_id')
    if not isinstance(message, str) or not message.strip() or len(message.encode()) > 32000:
        raise ValueError('Enter a message under 32 KB.')
    if not isinstance(session, str) or str(uuid.UUID(session)) != session:
        raise ValueError('Invalid conversation identifier.')
    if 'walking_news' in value and not isinstance(value['walking_news'], bool):
        raise ValueError('Invalid walking news flag.')
    result = {'op': 'chat', 'request_id': request_id, 'message': message, 'session_id': session}
    if value.get('walking_news') is True: result['walking_news'] = True
    return result


@app.get('/')
@app.get('/health')
async def health():
    # Liveness only: never claim Muse is connected or disclose account details.
    return JSONResponse({'service': 'muse-watch', 'status': 'running', 'protocol': 1}, headers=HEADERS)


def calm_command(value):
    if not isinstance(value, dict): raise ValueError('Invalid request')
    request = value.get('request_id')
    if not isinstance(request, str) or str(uuid.UUID(request)) != request: raise ValueError('Invalid id')
    if value.get('kind') == 'sound' and value.get('scene') in calm_audio.SCENES:
        return {'kind': 'sound', 'scene': value['scene'], 'request_id': request, 'recipe': recipe_from(value.get('recipe'))}
    text = value.get('text')
    if value.get('kind') == 'voice' and isinstance(text, str) and 1 <= len(text.strip()) <= 240 and len(text.encode()) <= 1000 and isinstance(value.get('voice'), str) and value['voice'] in calm_audio.VOICE_LANGUAGES:
        return {'kind': 'voice', 'text': text, 'voice': value['voice'], 'request_id': request}
    raise ValueError('Invalid audio request')


@app.post('/v1/calm/audio')
async def calm(request: Request):
    if not authorized(request.headers.get('authorization', '')):
        return failure(401, 'Watch connection key is missing or invalid.')
    raw = bytearray()
    async for chunk in request.stream():
        raw.extend(chunk)
        if len(raw) > 4096: return failure(413, 'Audio request too large.')
    try: command = calm_command(json.loads(raw))
    except (ValueError, TypeError, AttributeError): return failure(400, 'Invalid audio request.')
    owner = str(uuid.uuid4()); key = 'calm:lease:' + command['kind']
    stop = threading.Event(); acquired = False
    try:
        store = Store()
        acquired = await store.command('SET', key, owner, 'NX', 'EX', 115) == 'OK'
        if not acquired: return failure(409, 'Audio is already being created. Try again shortly.')
        if await store.command('SET', 'calm:request:' + command['request_id'], 'accepted', 'NX', 'EX', 3600) != 'OK':
            return failure(409, 'This audio request was already accepted.')
        async with asyncio.timeout(100):
            payload, metadata = await asyncio.to_thread(calm_audio.render, command, stop)
        if len(payload) > 20 * 1024 * 1024 or not payload.startswith(b'RIFF'):
            raise ValueError('Invalid generated audio')
    except StoreError:
        return failure(503, 'Audio storage is temporarily unavailable. Saved audio can still play.')
    except Exception as error:
        logging.getLogger(__name__).warning('Calm %s generation failed: %s', command['kind'], type(error).__name__)
        return failure(503, 'Fresh audio is temporarily unavailable. Saved audio can still play.')
    finally:
        stop.set()
        if acquired:
            with anyio.CancelScope(shield=True):
                with contextlib.suppress(Exception):
                    await store.command('EVAL', "if redis.call('GET',KEYS[1]) == ARGV[1] then return redis.call('DEL',KEYS[1]) else return 0 end", 1, key, owner)
    async def chunks():
        for offset in range(0, len(payload), 64 * 1024):
            yield payload[offset:offset+64*1024]
    return StreamingResponse(chunks(), media_type='audio/wav', headers={**HEADERS, **metadata,
        'X-Audio-Sha256': hashlib.sha256(payload).hexdigest(), 'X-Audio-Bytes': str(len(payload))})


@app.post('/v1/muse')
async def muse(request: Request):
    if not authorized(request.headers.get('authorization', '')):
        return failure(401, 'Watch connection key is missing or invalid.')
    raw = bytearray()
    async for chunk in request.stream():
        raw.extend(chunk)
        if len(raw) > 65536:
            return failure(413, 'Request too large.')
    try:
        command = command_from(json.loads(raw))
    except (ValueError, TypeError, AttributeError):
        return failure(400, 'Invalid watch request.')
    owner = str(uuid.uuid4())
    acquired = False
    try:
        store = Store()
        acquired = await store.acquire(owner)
        if not acquired:
            return failure(409, 'Muse is finishing another request. Wait a moment before reconnecting.')
        if command['op'] == 'chat' and not await store.reserve(command['request_id']):
            await store.release(owner)
            return failure(409, 'This message was already accepted. It will not be sent again.')
        await store.load()
    except (StoreError, ValueError):
        if acquired:
            try: await store.release(owner)
            except StoreError: pass
        return failure(503, 'The Muse backend is not ready. Check cloud enrollment and storage.')

    async def encoded():
        async with aclosing(stream(store, owner, command)) as source:
            async for event in source:
                line = json.dumps(event, ensure_ascii=False, separators=(',', ':')).encode() + b'\n'
                if len(line) > 256 * 1024:
                    yield b'{"type":"error","error":"Muse reply exceeded the watch limit."}\n'
                    yield b'{"type":"turn_finished"}\n'
                    return
                yield line
    return StreamingResponse(encoded(), media_type='application/x-ndjson', headers=HEADERS)
