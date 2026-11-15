# Draws PinPlay's icon (icon.png, 48x48), HOME Menu banner (meta/banner.png) and banner sound (meta/banner.wav).
# The mark is our own: a map pin with a play triangle in it.
import math, struct, wave
from PIL import Image, ImageDraw, ImageFont
BLUE, LIGHT, DARK = (42, 159, 216), (90, 200, 255), (24, 28, 40)
def badge(s):
    S = s * 4; im = Image.new('RGBA', (S, S), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, S - 1, S - 1), radius=S // 5, fill=DARK, outline=LIGHT, width=max(4, S // 24))
    cx, cy, r = S / 2, S * 0.40, S * 0.24
    d.polygon([(cx - r * 0.75, cy + r * 0.6), (cx + r * 0.75, cy + r * 0.6), (cx, cy + r * 1.85)], fill=BLUE)
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=BLUE)
    d.polygon([(cx - r * 0.32, cy - r * 0.45), (cx - r * 0.32, cy + r * 0.45), (cx + r * 0.5, cy)], fill=(255, 255, 255))
    return im.resize((s, s), Image.LANCZOS)
badge(48).save('icon.png')
b = Image.new('RGBA', (256, 128), (0, 0, 0, 0)); b.alpha_composite(badge(96), (12, 16))
d = ImageDraw.Draw(b)
try: f = ImageFont.truetype('arialbd.ttf', 34); f2 = ImageFont.truetype('arial.ttf', 15)
except OSError: f = f2 = ImageFont.load_default()
d.text((118, 30), 'Pin', font=f, fill=(255, 255, 255)); d.text((118, 66), 'Play', font=f, fill=LIGHT)
b.save('meta/banner.png')
rate = 32728; n = int(rate * 1.0); fr = bytearray()
for i in range(n):                                           # three quick rising notes
    t = i / rate; v = 0
    for k, (st, fq) in enumerate([(0, 523), (0.12, 659), (0.24, 784)]):
        if t >= st: v += 0.22 * math.exp(-6 * (t - st)) * math.sin(2 * math.pi * fq * (t - st))
    fr += struct.pack('<hh', int(v * 32767), int(v * 32767))
with wave.open('meta/banner.wav', 'wb') as w: w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate); w.writeframes(bytes(fr))
