"""Exercise actual Vercel generation; save private local audition files, no credentials in output."""
import concurrent.futures
import hashlib
import json
from pathlib import Path
import time
import urllib.error
import urllib.request
import uuid
import wave

root = Path(__file__).resolve().parents[1]
credentials = json.loads((root / 'Watch/CloudBootstrap.json').read_text())
folder = root / 'build/live-audio'
folder.mkdir(parents=True, exist_ok=True)

def generate(label, **command):
    body = {'request_id': str(uuid.uuid4()), **command}
    request = urllib.request.Request(credentials['address'].rstrip('/') + '/v1/calm/audio',
        data=json.dumps(body).encode(), headers={'Content-Type': 'application/json', 'Authorization': 'Bearer ' + credentials['token']})
    started = time.monotonic()
    try:
        with urllib.request.urlopen(request, timeout=115) as response:
            headers = dict(response.headers)
            data = response.read(20*1024*1024+1)
            expected = response.headers.get('X-Audio-Sha256')
            seed = response.headers.get('X-Audio-Seed')
            duration = response.headers.get('X-Audio-Duration')
            generator = response.headers.get('X-Audio-Generator')
            director = response.headers.get('X-Audio-Director')
    except urllib.error.HTTPError as error:
        print(json.dumps({'label':label,'status':error.code,'error':error.read(2000).decode(errors='replace')}),flush=True)
        raise RuntimeError('Production audio request failed') from None
    digest = hashlib.sha256(data).hexdigest()
    assert expected == digest and len(data) <= 20*1024*1024
    target = folder / (label + '.wav'); target.write_bytes(data)
    with wave.open(str(target)) as audio:
        result = {'label':label,'seconds':round(time.monotonic()-started,2), 'bytes':len(data),
            'duration':audio.getnframes()/audio.getframerate(), 'rate':audio.getframerate(),
            'channels':audio.getnchannels(), 'sample_bytes':audio.getsampwidth(), 'sha256':digest, 'seed':seed,
            'generator':generator, 'director':director}
        assert abs(result['duration'] - float(duration)) < .01
    print(json.dumps(result),flush=True)
    return result

if __name__ == '__main__':
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        sound = pool.submit(generate, 'brush-1', kind='sound', scene='brush')
        voice = pool.submit(generate, 'warm-voice', kind='voice', voice='af_heart', text='Let these gentle sounds settle around you. There is nowhere else you need to be.')
        results = [sound.result(), voice.result()]
    results.append(generate('brush-2',kind='sound',scene='brush'))
    assert results[0]['seed'] != results[2]['seed'] and results[0]['sha256'] != results[2]['sha256']
    results.append(generate('airy-voice',kind='voice',voice='af_nicole',text='Rest here for a moment. Let the quiet textures move gently through the space around you.'))
    (folder/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print('Fresh seeds, distinct audio, complete lossless downloads, and both neural voices verified.',flush=True)
