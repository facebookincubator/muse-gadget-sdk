#!/usr/bin/env python3
"""Mac hardware bridge for the native Linux app; commands stay in a private folder."""
import argparse
from array import array
import json
import math
import os
from pathlib import Path
import subprocess
import time
import wave

def call(args, *, timeout=25):
    result = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(result.stderr.strip()[-1500:] or "Media operation failed")
    return result

def atomic(path, text):
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(text)
    os.replace(tmp, path)

def operation(kind, text, directory, args):
    if kind == "camera":
        call(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "avfoundation", "-pixel_format", "uyvy422",
              "-framerate", "30", "-video_size", "640x480", "-i", args.camera + ":none", "-frames:v", "1",
              "-pix_fmt", "bgr24", "-update", "1", "-y", str(directory / "camera.tmp.bmp")])
        for quality in (6, 12, 20, 28):
            call(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(directory / "camera.tmp.bmp"),
                  "-q:v", str(quality), "-frames:v", "1", "-update", "1", str(directory / "camera.tmp.jpg")], timeout=5)
            if (directory / "camera.tmp.jpg").stat().st_size <= 180000: break
        else: raise ValueError("Camera photo is too large for Muse's attachment limit")
        os.replace(directory / "camera.tmp.jpg", directory / "camera.jpg")
        os.replace(directory / "camera.tmp.bmp", directory / "camera.bmp")
        return "Camera photo captured: 640 x 480. Uploading to Muse for analysis."
    if kind == "microphone":
        call(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "avfoundation", "-i", "none:" + args.microphone,
              "-t", "5", "-ac", "1", "-ar", "16000", "-c:a", "pcm_s16le", "-y", str(directory / "microphone.wav")])
        with wave.open(str(directory / "microphone.wav")) as audio:
            samples = array("h", audio.readframes(audio.getnframes()))
            seconds = len(samples) / audio.getframerate()
        rms = math.sqrt(sum(x*x for x in samples) / max(1, len(samples))) / 32768
        peak = max((abs(x) for x in samples), default=0) / 32768
        call(["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", str(directory / "microphone.wav"),
              "-filter_complex", "showwavespic=s=640x200:colors=0xe0bf74", "-frames:v", "1", "-pix_fmt", "bgr24",
              "-update", "1", "-y", str(directory / "waveform.bmp")])
        return f"Real microphone recording: {seconds:.2f}s, 16 kHz mono.\nRMS: {20*math.log10(max(rms, 1e-9)):.1f} dBFS; peak: {peak*100:.1f}%.\nUse Play recording to hear it."
    if kind == "play":
        call(["afplay", str(directory / "microphone.wav")])
        return "Microphone recording played."
    if kind == "speak":
        if not text.strip():
            raise ValueError("Get a Muse reply first, then use Speak reply.")
        speech = directory / "speech.txt"
        speech.write_text(text[:8000])
        call(["say", "-f", str(speech)], timeout=120)
        return "Muse's reply spoken using the Mac's speech voice."
    if kind == "transcribe":
        if not args.transcriber:
            raise ValueError("Speech recognition has not been configured.")
        result = call([args.transcriber, str(Path(__file__).with_name("transcribe.py")),
                       str(directory / "microphone.wav"), str(directory / "models")], timeout=180)
        transcript = result.stdout.strip()
        atomic(directory / "transcript.txt", transcript)
        if not transcript:
            return "No speech detected. Record again while speaking, then transcribe."
        return "Transcribed speech (ready in the message box):\n\n" + transcript
    raise ValueError("Unknown media action")

def main(args):
    directory = Path(args.directory)
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    atomic(directory / "host-ready.txt", str(os.getpid()))
    print("Mac camera/microphone bridge ready.", flush=True)
    try:
        while True:
            request = directory / "request.txt"
            if request.exists():
                working = directory / "working.txt"
                os.replace(request, working)
                content = working.read_text()
                request_id, _, content = content.partition("\n")
                kind, _, text = content.partition("\n")
                try:
                    message = operation(kind, text, directory, args)
                    atomic(directory / "response.txt", request_id + "\nOK\n" + kind + "\n" + message)
                except Exception as exc:
                    atomic(directory / "response.txt", request_id + "\nERROR\n" + kind + "\n" + str(exc))
                working.unlink(missing_ok=True)
            time.sleep(0.1)
    finally:
        (directory / "host-ready.txt").unlink(missing_ok=True)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", required=True)
    parser.add_argument("--camera", default="0")
    parser.add_argument("--microphone", default="1")
    parser.add_argument("--transcriber")
    parser.add_argument("--connector", default="muse-ma35d1-a1-sim")
    main(parser.parse_args())
