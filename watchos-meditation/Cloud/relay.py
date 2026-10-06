"""One bounded Muse connection per HTTPS request, using the unmodified SDK."""
import asyncio
import contextlib
import time
import anyio
import news
from musegadget import __version__, muse_api
from musegadget.link_client import DeviceDescription, LinkSession, Outcome


class RelayError(RuntimeError):
    pass


async def credentials(store, owner, force=False):
    state = await store.load()
    pairing = state['pairing']
    if state.get('revoked'):
        raise RelayError('Muse enrollment was revoked. Pair again before using the cloud connection.')
    if force or time.time() - pairing.get('access_token_saved_at', 0) >= 3 * 3600:
        tokens, status = await asyncio.to_thread(muse_api.refresh_device_token,
            pairing['refresh_token'], state['node_id'],
            muse_api.api_root(pairing.get('api_url_v2', '')), state.get('sdk_token'))
        if not tokens:
            if status == 401:
                await store.save({**state, 'revoked': True}, owner)
            raise RelayError('Muse login could not be refreshed. Reconnect or check enrollment.')
        state['pairing'] = {**pairing, 'access_token': tokens['access_token'],
            'refresh_token': tokens['refresh_token'], 'access_token_saved_at': int(time.time())}
        # Persist rotated refresh tokens BEFORE any chat is sent.
        await store.save(state, owner)
    return state


async def lookup(store, owner):
    state = await credentials(store, owner)
    for attempt in range(2):
        pairing = state['pairing']
        vms, status = await asyncio.to_thread(muse_api.fetch_vms_with_status,
            pairing['access_token'], muse_api.api_root(pairing.get('api_url_v2', '')))
        if status == 401 and attempt == 0:
            state = await credentials(store, owner, force=True)
            continue
        if status != 200 or not vms:
            raise RelayError('Muse is unavailable. No message was sent.')
        return state, next((vm for vm in vms if vm['is_default']), vms[0])
    raise RelayError('Muse login was rejected. No message was sent.')


def reject_tool(name, params, timeout_ms=None):
    return {'ok': False, 'error': 'This connection supports chat only; it cannot run host commands.'}


async def events(store, owner, command, session_factory=LinkSession):
    state, vm = await lookup(store, owner)
    session = session_factory(noise_host=state['pairing'].get('noise_host') or 'hatch.metaaivm.com',
        vm_id=vm['vm_id'] or vm['vm_name'], vm_auth_token=vm['vm_auth_token'],
        device=DeviceDescription(state['node_id'], 'Muse Watch', __version__, {}), run_command=reject_tool)
    stop = asyncio.Event()
    task = asyncio.create_task(session.run(stop))
    try:
        async with asyncio.timeout(30):
            while session.registered_at is None:
                if task.done():
                    outcome = task.result()
                    if outcome == Outcome.UNPAIRED:
                        await store.save({**state, 'revoked': True}, owner)
                    raise RelayError('Muse connection could not be established. No message was sent.')
                await asyncio.sleep(0.05)
        yield {'type': 'connection', 'paired': True, 'online': True}
        if command['op'] == 'chat':
            message = command['message']
            if command.get('walking_news'):
                message, headline = await news.update(store, command['session_id'], message)
                if headline: yield {'type': 'news_source', **headline}
            async for event in session.chat_events(message, command['session_id'], timeout=75):
                if event.get('type') in {'ack', 'reply', 'status', 'done'}:
                    yield event
    finally:
        # ASGI disconnect cancellation must still close the SDK session.
        with anyio.CancelScope(shield=True):
            stop.set()
            try:
                await asyncio.wait_for(asyncio.shield(task), 2)
            except (asyncio.CancelledError, Exception):
                task.cancel()
            await asyncio.gather(task, return_exceptions=True)


async def stream(store, owner, command, producer=events):
    try:
        async with asyncio.timeout(100):
            async with contextlib.aclosing(producer(store, owner, command)) as source:
                async for event in source:
                    yield event
    except asyncio.CancelledError:
        raise
    except RelayError as exc:
        yield {'type': 'error', 'error': str(exc)}
    except Exception:
        yield {'type': 'error', 'error': 'Muse connection failed or timed out. Your message was not automatically resent.'}
    finally:
        with anyio.CancelScope(shield=True):
            with contextlib.suppress(Exception):
                await store.release(owner)
    yield {'type': 'turn_finished'}
