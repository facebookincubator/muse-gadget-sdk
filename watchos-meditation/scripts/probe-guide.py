"""Check the exact app prompt through the existing cloud connection, without Health data."""
import json
from pathlib import Path
import re
import textwrap
import urllib.request
import uuid

root = Path(__file__).resolve().parents[1]
(root / 'build').mkdir(exist_ok=True)
source = (root / 'Shared/CalmPlan.swift').read_text()
prompt = textwrap.dedent(source.split('static let prompt = """', 1)[1].split('"""', 1)[0]).strip()
credentials = json.loads((root / 'Watch/CloudBootstrap.json').read_text())
body = {'op': 'chat', 'message': prompt, 'request_id': str(uuid.uuid4()), 'session_id': str(uuid.uuid4())}
request = urllib.request.Request(credentials['address'] + '/v1/muse', data=json.dumps(body).encode(),
    headers={'Content-Type': 'application/json', 'Authorization': 'Bearer ' + credentials['token']})
replies, order = {}, []
done = False
with urllib.request.urlopen(request, timeout=115) as response:
    for line in response:
        event = json.loads(line)
        if event.get('type') == 'error': raise RuntimeError(event.get('error'))
        if event.get('type') == 'reply':
            mid = event['message_id']
            if mid not in order: order.append(mid)
            replies[mid] = event['text']
        if event.get('type') == 'turn_finished': done = True
if not done: raise RuntimeError('Incomplete guide stream')
text = '\n'.join(replies[mid] for mid in order)
(root / 'build/live-guide.txt').write_text(text)
print(json.dumps({'stream_completed': done, 'reply_characters': len(text), 'saved_for_swift_validation': True}))
