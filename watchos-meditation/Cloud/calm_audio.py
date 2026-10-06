"""Fresh Muse-directed sound synthesis and neural speech. Never loops an asset."""
import hashlib
import io
import os
from pathlib import Path
import threading
import urllib.request

SCENES = ('water', 'rain', 'leaves', 'brush', 'fabric', 'wood')
MODEL_BASE = 'https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.1/'
MODEL_FILES = {
    'kokoro-v1.0.fp16.onnx': (163527961, 'f3a290d384fbb27966d462905c71a46cef9e5fd00516b40df32a0b4afe77ac96'),
    'voices-v1.0.bin': (28214398, 'bca610b8308e8d99f32e6fe4197e7ec01679264efed0cac9140fe9c29f1fbf7d'),
}
_voice_lock = threading.Lock()
_voice_model = None
VOICE_LANGUAGES = {"af_heart": "en-us", "af_nicole": "en-us", "am_michael": "en-us"}


def model_file(name):
    folder = Path(os.environ.get('CALM_MODEL_DIR', '/tmp/muse-calm-models'))
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / name
    size, digest = MODEL_FILES[name]
    if path.exists() and path.stat().st_size == size:
        return path
    temporary = path.with_suffix('.download')
    sha = hashlib.sha256(); written = 0
    try:
        with urllib.request.urlopen(MODEL_BASE + name, timeout=25) as response, temporary.open('wb') as output:
            while chunk := response.read(1024 * 1024):
                written += len(chunk)
                if written > size: raise ValueError('Unexpected model size')
                sha.update(chunk); output.write(chunk)
        if written != size or sha.hexdigest() != digest: raise ValueError('Model checksum mismatch')
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)
    return path


def encode_audio(samples, rate, sound=False):
    import numpy as np
    import soundfile as sf
    samples = np.asarray(samples, dtype=np.float32)
    if samples.ndim == 1: samples = samples[:, None]
    if not np.isfinite(samples).all() or samples.shape[1] not in (1, 2):
        raise ValueError('Invalid generated samples')
    if not 16000 <= rate <= 48000: raise ValueError('Unsupported audio rate')
    if len(samples) > rate * 90 or len(samples) < rate * (1 if sound else 0.15): raise ValueError('Invalid audio duration')
    if sound:
        # Remove model-created opening/closing fades, rather than replaying them.
        if len(samples) < rate * 60: raise ValueError('Generated sound was too short')
        samples = samples[2 * rate:-2 * rate].copy()
        # Gentle high-frequency rolloff; preserve the original sample rate/stereo.
        t = np.arange(129) - 64
        cutoff = 6500 / rate
        kernel = 2 * cutoff * np.sinc(2 * cutoff * t) * np.hamming(129)
        kernel /= kernel.sum()
        samples = np.column_stack([np.convolve(samples[:, c], kernel, mode='same') for c in range(samples.shape[1])])
        rms = float(np.sqrt(np.mean(samples ** 2)))
        if rms < 1e-5: raise ValueError('Generated sound was silent')
        # Tame isolated splashes/taps before setting an even, quiet listening level.
        samples = np.tanh(samples / max(rms * 5, 0.01))
        target = 10 ** (-24 / 20)
    else:
        target = 10 ** (-19 / 20)
    rms = float(np.sqrt(np.mean(samples ** 2)))
    if rms < 1e-5: raise ValueError('Generated audio was silent')
    gain = min(target / rms, 10 ** (-5 / 20) / float(np.max(np.abs(samples))))
    samples *= gain
    # Each unique clip starts/ends softly. Playback crossfades different clips.
    fade = min(int(rate * (0.8 if sound else 0.015)), len(samples) // 4)
    ramp = np.linspace(0, 1, fade)[:, None]
    samples[:fade] *= ramp; samples[-fade:] *= ramp[::-1]
    buffer = io.BytesIO()
    sf.write(buffer, samples, rate, format='WAV', subtype='PCM_16')
    return buffer.getvalue(), len(samples) / rate


def sound(scene, stop, recipe=None):
    from calm_synthesis import samples
    data, rate, seed = samples(scene, stop, recipe)
    payload, duration = encode_audio(data, rate, sound=True)
    return payload, {'X-Audio-Seed': str(seed), 'X-Audio-Duration': str(duration),
                     'X-Audio-Scene': scene, 'X-Audio-Generator': 'procedural',
                     'X-Audio-Director': 'muse' if recipe is not None else 'default'}


def voice(text, style, stop):
    global _voice_model
    with _voice_lock:
        if stop.is_set(): raise TimeoutError('Speech cancelled')
        if _voice_model is None:
            import onnxruntime as ort
            from kokoro_onnx import Kokoro
            ort.set_default_logger_severity(3)
            options = ort.SessionOptions()
            options.intra_op_num_threads = 2; options.inter_op_num_threads = 1
            session = ort.InferenceSession(str(model_file('kokoro-v1.0.fp16.onnx')), sess_options=options, providers=['CPUExecutionProvider'])
            _voice_model = Kokoro.from_session(session, str(model_file('voices-v1.0.bin')))
        if stop.is_set(): raise TimeoutError('Speech cancelled')
        language = VOICE_LANGUAGES[style]
        samples, rate = _voice_model.create(text, voice=style, speed=1.0 if style == 'am_michael' else 0.95, lang=language)
        payload, duration = encode_audio(samples, rate)
        return payload, {'X-Audio-Duration': str(duration), 'X-Audio-Voice': style}


def render(command, stop):
    if command['kind'] == 'sound': return sound(command['scene'], stop, command.get('recipe'))
    return voice(command['text'], command['voice'], stop)
