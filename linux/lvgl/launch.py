#!/usr/bin/env python3
"""Open the native Linux app on this Mac's XQuartz display."""
import os
import argparse
from pathlib import Path
import secrets
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
STATE = ROOT / "linux/lvgl/.local"
BACKEND = os.environ.get("MUSE_LVGL_CONNECTOR", "muse-ma35d1-a1-sim")
APP = "muse-linux-lvgl-app"

def run(*args, capture=False, **kwargs):
    return subprocess.run(args, check=True, text=True, capture_output=capture, **kwargs)

def main(prompt=None):
    app = APP
    run("open", "-a", "XQuartz")
    existing = subprocess.run(["docker", "inspect", app, "--format", "{{.State.Running}}"], capture_output=True, text=True)
    if existing.returncode == 0 and existing.stdout.strip() == "true":
        print("Muse Linux LVGL is already open.")
        return
    image = subprocess.run(["docker", "image", "inspect", "muse-linux-lvgl:local"], capture_output=True)
    if image.returncode:
        run("docker", "build", "-f", "linux/lvgl/Dockerfile", "-t", "muse-linux-lvgl:local", ".", cwd=ROOT)
    run("docker", "start", BACKEND)
    STATE.mkdir(mode=0o700, exist_ok=True)
    media = STATE / "media"
    media.mkdir(mode=0o700, exist_ok=True)
    helper_pid = media / "host-ready.txt"
    helper_running = False
    if helper_pid.exists():
        try:
            pid = int(helper_pid.read_text())
            os.kill(pid, 0)
            helper_running = True
            if (ROOT / "linux/lvgl/host_media.py").stat().st_mtime > helper_pid.stat().st_mtime:
                os.kill(pid, signal.SIGTERM)
                for _ in range(30):
                    try:
                        os.kill(pid, 0)
                    except ProcessLookupError:
                        break
                    time.sleep(0.1)
                else:
                    raise RuntimeError("Previous hardware helper did not stop")
                helper_running = False
        except (OSError, ValueError):
            pass
    if not helper_running:
        log = open(media / "host-media.log", "a")
        arguments = ["python3", str(ROOT / "linux/lvgl/host_media.py"), "--directory", str(media), "--connector", BACKEND]
        transcriber = ROOT.parent / "mimo-music-lab/tools/listen-env/bin/python"
        if transcriber.exists(): arguments.extend(["--transcriber", str(transcriber)])
        subprocess.Popen(arguments, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    token = STATE / "bridge-token"
    if not token.exists():
        fd = os.open(token, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as file:
            file.write(secrets.token_hex(32) + "\n")
    records = []
    for _ in range(20):
        auth = run("/opt/X11/bin/xauth", "-i", "-f", str(Path.home() / ".Xauthority"), "nlist", ":0", capture=True)
        records = auth.stdout.splitlines()
        if records: break
        time.sleep(0.25)
    if not records:
        raise RuntimeError("XQuartz has no authorization for display :0 yet. Try opening the app again.")
    authority = STATE / "Xauthority"
    fd = os.open(authority, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600); os.close(fd)
    run("/opt/X11/bin/xauth", "-f", str(authority), "nmerge", "-",
        input="\n".join("ffff" + line[4:] for line in records) + "\n", capture=True)
    run("docker", "cp", str(ROOT / "linux/lvgl/bridge.py"), BACKEND + ":/tmp/muse-lvgl-bridge.py")
    run("docker", "cp", str(token), BACKEND + ":/tmp/muse-lvgl-token")
    start_bridge = """from pathlib import Path
import os, signal, socket, subprocess, time
stopping = []
for directory in Path('/proc').iterdir():
    if not directory.name.isdigit(): continue
    try:
        argv = (directory / 'cmdline').read_bytes().split(b'\\0')
        # ARM emulation and Python flags can precede the script argument.
        if b'/tmp/muse-lvgl-bridge.py' in argv[1:]:
            os.kill(int(directory.name), signal.SIGTERM)
            stopping.append(directory)
    except (OSError, ProcessLookupError): pass
for attempt in range(50):
    if not any(directory.exists() for directory in stopping): break
    time.sleep(0.1)
else: raise RuntimeError('Previous chat bridge did not stop')
log = open('/tmp/muse-lvgl-bridge.log', 'a')
process = subprocess.Popen(['python3', '/tmp/muse-lvgl-bridge.py', '--token-file', '/tmp/muse-lvgl-token'],
    stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
for attempt in range(50):
    if process.poll() is not None: raise RuntimeError('Chat bridge did not start; check /tmp/muse-lvgl-bridge.log')
    try:
        connection = socket.create_connection(('127.0.0.1', 8765), 0.2)
        connection.close()
        break
    except OSError:
        if process.poll() is not None: raise RuntimeError('Chat bridge did not start; check /tmp/muse-lvgl-bridge.log')
        time.sleep(0.1)
else: raise RuntimeError('Chat bridge startup timed out')
print('Muse chat bridge started.')
"""
    run("docker", "exec", BACKEND, "python3", "-c", start_bridge)
    network = run("docker", "inspect", BACKEND, "--format", "{{range $name, $config := .NetworkSettings.Networks}}{{$name}}{{end}}", capture=True).stdout.strip()
    run("docker", "run", "--rm", "-d", "--name", app, "--network", network,
        "--user", f"{os.getuid()}:{os.getgid()}",
        "-e", "DISPLAY=host.docker.internal:0", "-e", "XAUTHORITY=/run/muse/Xauthority",
        "-e", "SDL_FRAMEBUFFER_ACCELERATION=0", "-e", "SDL_RENDER_DRIVER=software",
        "-e", "MUSE_CHAT_HOST=" + BACKEND, "-e", "MUSE_CHAT_TOKEN_FILE=/run/muse/bridge-token",
        "-e", "MUSE_MEDIA_DIR=/run/muse/media", "-v", str(media) + ":/run/muse/media",
        "-v", str(STATE) + ":/run/muse:ro", "muse-linux-lvgl:local",
        *(["--prompt", prompt] if prompt else []))
    print("Muse Linux LVGL is open. Type a message and press Enter or Send.")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prompt", help="Send an opening message when the window starts")
    args = parser.parse_args()
    main(args.prompt)
