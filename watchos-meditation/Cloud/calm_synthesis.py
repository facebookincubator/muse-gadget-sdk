"""CPU sound synthesis from bounded Muse recipes; no recordings or GPU service."""
import secrets
import numpy as np

DEFAULTS = {'brightness': 0.35, 'activity': 0.4, 'pace': 0.7, 'space': 0.6}


def recipe_from(value):
    if value is None: return None
    if not isinstance(value, dict) or set(value) != set(DEFAULTS): raise ValueError('Invalid sound recipe')
    result = {}
    for key in DEFAULTS:
        number = value[key]
        if isinstance(number, bool) or not isinstance(number, (int, float)) or not np.isfinite(number) or not 0 <= number <= 1:
            raise ValueError('Invalid sound parameter')
        result[key] = float(number)
    return result


def samples(scene, stop, recipe=None, seed=None):
    """Compose a unique stereo field of filtered noise and softly enveloped events."""
    if stop.is_set(): raise TimeoutError('Sound cancelled')
    seed = secrets.randbelow(2**31) if seed is None else seed
    rng = np.random.default_rng(seed)
    config = recipe_from(recipe) or DEFAULTS
    rate = 44100
    count = rate * 64
    output = np.zeros((count, 2), dtype=np.float32)
    brightness, activity, pace, space = [config[k] for k in DEFAULTS]

    def texture(length, cutoff, slope=0.6):
        # Fresh noise per layer/event, shaped in frequency; no periodic source loops.
        frequencies = np.fft.rfftfreq(length, 1/rate)
        weights = np.maximum(frequencies, 70.0)**(-slope)
        weights *= (1-np.exp(-(frequencies/100)**2)) * np.exp(-(frequencies/cutoff)**4)
        spectrum = np.fft.rfft(rng.standard_normal(length).astype(np.float32))
        result = np.fft.irfft(spectrum * weights, n=length).astype(np.float32)
        result /= max(float(np.sqrt(np.mean(result**2))), 1e-6)
        return result

    # Quiet diffuse bed, stronger for rain/water, almost absent behind individual taps.
    bed_gain = {'rain': .12, 'water': .085, 'leaves': .045, 'brush': .025, 'fabric': .022, 'wood': .012}[scene]
    for channel in range(2):
        if stop.is_set(): raise TimeoutError('Sound cancelled')
        bed = texture(count, 1000 + brightness*2300, .7)
        anchors = rng.uniform(.7, 1.0, 34)
        modulation = np.interp(np.arange(count), np.linspace(0,count-1,len(anchors)), anchors)
        output[:,channel] = bed * modulation * bed_gain

    def add_event(at, seconds, kind, amplitude):
        length = int(seconds*rate)
        offset = int(at*rate)
        if length < 2 or offset + length > count: return
        t = np.arange(length)/rate
        u = np.linspace(0,1,length)
        envelope = np.sin(np.pi*u)**2
        cutoff = (1000 + brightness*4000)*rng.uniform(.75,1.1)
        if kind == 'tap':
            # Damped wooden resonances with a rounded attack, not a sharp click.
            frequency = rng.uniform(220,540)
            waveform = sum(weight*np.sin(2*np.pi*frequency*ratio*t)*np.exp(-t/decay)
                           for weight,ratio,decay in [(1,1,.09),(.35,1.57,.05),(.18,2.21,.035)])
            waveform *= (1-np.exp(-t/.012))
            waveform /= max(float(np.max(np.abs(waveform))),1e-6)
        elif kind == 'drop':
            frequency = rng.uniform(450,1000)
            waveform = texture(length,cutoff,.4)*.6 + np.sin(2*np.pi*(frequency*t-100*t*t))*.12
            envelope *= np.exp(-u*3)
        else:
            waveform = texture(length, cutoff, .45 if kind == 'brush' else .7)
            # Irregular pressure gives each stroke or rustle its own motion.
            pressure = np.interp(u, np.linspace(0,1,8), rng.uniform(.5,1,8))
            waveform *= pressure
        pan = rng.uniform(-.7,.7)*space
        travel = rng.uniform(-.25,.25)*space*np.sin(np.pi*u)
        angle = (pan+travel+1)*np.pi/4
        event = waveform*envelope*amplitude
        output[offset:offset+length,0] += event*np.cos(angle)
        output[offset:offset+length,1] += event*np.sin(angle)

    position = rng.uniform(0,.6)
    while position < 62:
        if stop.is_set(): raise TimeoutError('Sound cancelled')
        if scene == 'wood':
            seconds, interval, kind, amplitude = rng.uniform(.18,.45), rng.uniform(.7,2.8)/(0.6+pace), 'tap', .09
        elif scene == 'rain':
            seconds, interval, kind, amplitude = rng.uniform(.035,.14), rng.uniform(.06,.25)/(0.6+activity), 'drop', .025
        elif scene == 'water':
            seconds, interval, kind, amplitude = rng.uniform(3,7), rng.uniform(2,4)/(0.6+pace), 'wash', .13
        elif scene == 'leaves':
            seconds, interval, kind, amplitude = rng.uniform(1.3,4), rng.uniform(.7,2.8)/(0.6+pace), 'rustle', .11
        elif scene == 'fabric':
            seconds, interval, kind, amplitude = rng.uniform(.7,2.3), rng.uniform(.8,2.8)/(0.6+pace), 'rustle', .11
        else:
            seconds, interval, kind, amplitude = rng.uniform(1.2,3.5), rng.uniform(1.4,3.6)/(0.6+pace), 'brush', .12
        add_event(position, seconds, kind, amplitude*rng.uniform(.65,1)*(0.7+activity*.6))
        position += interval
    return output, rate, seed
