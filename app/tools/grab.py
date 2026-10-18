"""Save the app's current screen as PNG: grab.py [http://host:port] [out.png]
Works against the host simulator and the real unit (GET /frame = raw RGB565 1024x600)."""
import sys
import urllib.request
from PIL import Image

url = (sys.argv[1] if len(sys.argv) > 1 else "http://localhost:8080").rstrip("/") + "/frame"
out = sys.argv[2] if len(sys.argv) > 2 else "screen.png"
raw = urllib.request.urlopen(url, timeout=5).read()
px = bytearray()
for i in range(0, len(raw), 2):
    v = raw[i] | raw[i + 1] << 8
    px += bytes(((v >> 8) & 0xF8, (v >> 3) & 0xFC, (v << 3) & 0xF8))
Image.frombytes("RGB", (1024, 600), bytes(px)).save(out)
print("saved", out)
