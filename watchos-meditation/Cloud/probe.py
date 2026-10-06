"""Local smoke test; never prints credentials or account data."""
import argparse
import asyncio
import json
from pathlib import Path
import urllib.request
import uuid
from provision import load_local
from relay import stream
from store import Store


async def local():
    load_local()
    store = Store()
    owner = str(uuid.uuid4())
    if not await store.acquire(owner):
        raise RuntimeError('Muse is already handling a request.')
    events = [event async for event in stream(store, owner, {'op': 'check'})]
    print(json.dumps(events))
    if not any(event.get('online') is True for event in events):
        raise RuntimeError('Muse connection test failed.')


def remote(address, chat=False, news=False):
    token = json.loads((Path(__file__).parent.parent / 'build/cloud-access.json').read_text())['token']
    body = {'op': 'chat' if chat else 'check', 'request_id': str(uuid.uuid4())}
    if chat:
        body.update(message='Connection test. Reply with exactly: watch cloud connection works', session_id=str(uuid.uuid4()))
    if news:
        body.update(op='chat', message='Walking news demo. No real sensor measurements are supplied. Give one short current-events update from the supplied headline.', session_id=str(uuid.uuid4()), walking_news=True)
    request = urllib.request.Request(address.rstrip('/') + '/v1/muse', data=json.dumps(body).encode(), headers={
        'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json'})
    events = []
    with urllib.request.urlopen(request, timeout=115) as response:
        for line in response:
            event = json.loads(line)
            events.append(event)
            print(json.dumps(event), flush=True)
    if any(event.get('type') == 'error' for event in events):
        raise RuntimeError('Remote Muse test failed.')
    if not events or events[-1].get('type') != 'turn_finished':
        raise RuntimeError('Incomplete remote response.')
    if chat and not any(event.get('type') == 'reply' and event.get('text') for event in events):
        raise RuntimeError('No Muse reply received.')
    if news and not any(event.get('type') == 'news_source' for event in events):
        raise RuntimeError('No fresh headline received.')
    if news and not any(event.get('type') == 'reply' and event.get('text') for event in events):
        raise RuntimeError('No Muse news reply received.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--address')
    parser.add_argument('--chat', action='store_true')
    parser.add_argument('--news', action='store_true')
    args = parser.parse_args()
    remote(args.address, args.chat, args.news) if args.address else asyncio.run(local())
