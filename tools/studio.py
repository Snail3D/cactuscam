#!/usr/bin/env python3
"""CactusCam studio — the Mac-side half of the camera agent.

The XIAO streams raw MJPEG frame bursts here (it has no video encoder);
this service encodes them to MP4 with ffmpeg and posts the result to the
same Telegram chat barkcam uses. Photos are archived as a bonus.

Endpoints:
  POST /clip   chunked body of concatenated JPEG frames; X-Fps header
  POST /photo  plain JPEG body, archived only

Media lands in ../media/{clips,photos}. The bot token is read from the
firmware's credentials.h so there is one source of truth.
"""
import os
import re
import subprocess
import sys
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MEDIA = os.path.join(ROOT, "media")
CLIPS = os.path.join(MEDIA, "clips")
PHOTOS = os.path.join(MEDIA, "photos")
os.makedirs(CLIPS, exist_ok=True)
os.makedirs(PHOTOS, exist_ok=True)

CRED_PATH = os.path.join(ROOT, "fw", "include", "credentials.h")


def load_credentials():
    with open(CRED_PATH) as f:
        src = f.read()
    token = re.search(r'TELEGRAM_BOT_TOKEN\s+"([^"]+)"', src).group(1)
    chat = re.search(r'TELEGRAM_CHAT_ID\s+"([^"]+)"', src).group(1)
    return token, chat


def read_chunked(rfile):
    """Read a chunked transfer-encoded body to completion."""
    data = bytearray()
    while True:
        line = rfile.readline(64).strip()
        if not line:
            break
        size = int(line.split(b";")[0], 16)
        if size == 0:
            rfile.readline(2)  # trailing CRLF
            break
        data += rfile.read(size)
        rfile.readline(2)
    return bytes(data)


def upload_video(token, chat_id, path):
    with open(path, "rb") as f:
        video = f.read()
    boundary = b"----cactuscam" + str(int(time.time())).encode()
    body = (
        b"--" + boundary + b"\r\n"
        b'Content-Disposition: form-data; name="chat_id"\r\n\r\n' + chat_id.encode() + b"\r\n"
        b"--" + boundary + b"\r\n"
        b'Content-Disposition: form-data; name="caption"\r\n\r\ncactus clip\r\n'
        b"--" + boundary + b"\r\n"
        b'Content-Disposition: form-data; name="video"; filename="clip.mp4"\r\n'
        b"Content-Type: video/mp4\r\n\r\n" + video + b"\r\n--" + boundary + b"--\r\n"
    )
    req = urllib.request.Request(
        f"https://api.telegram.org/bot{token}/sendVideo",
        data=body,
        headers={"Content-Type": f"multipart/form-data; boundary={boundary.decode()}"},
    )
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.status == 200


def handle_clip(token, chat_id, body, fps):
    stamp = time.strftime("%Y%m%d-%H%M%S")
    raw = os.path.join(CLIPS, f"clip-{stamp}.mjpeg")
    mp4 = os.path.join(CLIPS, f"clip-{stamp}.mp4")
    with open(raw, "wb") as f:
        f.write(body)
    print(f"[studio] clip received: {len(body)//1024} KB @ {fps} fps -> encoding", flush=True)
    try:
        subprocess.run(
            ["ffmpeg", "-y", "-loglevel", "error", "-f", "mjpeg",
             "-framerate", str(fps), "-i", raw,
             "-c:v", "libx264", "-preset", "veryfast", "-crf", "23",
             "-pix_fmt", "yuv420p", mp4],
            check=True, timeout=120,
        )
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as e:
        print(f"[studio] encode failed: {e}", flush=True)
        return False
    if not upload_video(token, chat_id, mp4):
        print("[studio] telegram upload failed", flush=True)
        return False
    os.remove(raw)  # keep the MP4, drop the intermediate
    print(f"[studio] clip sent: {mp4} ({os.path.getsize(mp4)//1024} KB)", flush=True)
    return True


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        print("[studio]", fmt % args, flush=True)

    def do_POST(self):
        if self.path == "/clip":
            if "chunked" in (self.headers.get("Transfer-Encoding") or "").lower():
                body = read_chunked(self.rfile)
            else:
                body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
            fps = int(self.headers.get("X-Fps", "10"))
            ok = handle_clip(TOKEN, CHAT_ID, body, fps)
        elif self.path == "/photo":
            body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
            p = os.path.join(PHOTOS, time.strftime("photo-%Y%m%d-%H%M%S.jpg"))
            with open(p, "wb") as f:
                f.write(body)
            print(f"[studio] photo archived: {p} ({len(body)//1024} KB)", flush=True)
        else:
            self.send_response(404); self.end_headers(); return
        self.send_response(200 if ok else 500)
        self.end_headers()


if __name__ == "__main__":
    TOKEN, CHAT_ID = load_credentials()
    print(f"[studio] up on :8377 — chat {CHAT_ID}", flush=True)
    ThreadingHTTPServer(("0.0.0.0", 8377), Handler).serve_forever()
