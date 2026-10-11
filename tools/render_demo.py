"""Render `hdmon status --from-json tests/fixtures` (synthetic data) to demo-status.png.

Needs Pillow:  pip install pillow
"""
import os
import re
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONTS = ["/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/System/Library/Fonts/Menlo.ttc",
         "/Library/Fonts/Menlo.ttc", r"C:\Windows\Fonts\consola.ttf"]
BOLD = ["/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"]
SYMBOL_FALLBACK = ["/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"]
BG, FG = (24, 26, 33), (220, 223, 228)
PALETTE = {"31": (240, 82, 79), "32": (98, 200, 110), "33": (240, 190, 60), "34": (90, 150, 240),
           "36": (86, 200, 220), "2": (130, 135, 145)}


def font(paths, size):
    for p in paths:
        if os.path.exists(p):
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def spans(line):
    """Split an ANSI-colored line into (text, color, bold) chunks."""
    color, bold, out, pos = FG, False, [], 0
    for m in re.finditer(r"\x1b\[([0-9;]*)m", line):
        if m.start() > pos:
            out.append((line[pos:m.start()], color, bold))
        for code in (m.group(1) or "0").split(";"):
            if code == "0":
                color, bold = FG, False
            elif code == "1":
                bold = True
            elif code in PALETTE:
                color = PALETTE[code]
        pos = m.end()
    if pos < len(line):
        out.append((line[pos:], color, bold))
    return out


def main():
    cmd = [sys.executable, "-m", "hard_drive_monitor", "status", "--from-json", "tests/fixtures", "--color", "always"]
    env = dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONPATH=ROOT)
    text = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", cwd=ROOT, env=env).stdout
    lines = ["\x1b[2m$ hdmon status --from-json tests/fixtures      # synthetic demo data\x1b[0m", ""] + text.rstrip().splitlines()
    size = 15
    reg, bold, sym = font(FONTS, size), font(BOLD + FONTS, size), font(SYMBOL_FALLBACK, size)
    cw = reg.getlength("M")
    lh = int(size * 1.45)
    width = max(len(re.sub(r"\x1b\[[0-9;]*m", "", l)) for l in lines)
    pad = 22
    title_h = 30
    img = Image.new("RGB", (int(width * cw) + 2 * pad, len(lines) * lh + 2 * pad + title_h), BG)
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, img.width, title_h], fill=(40, 43, 52))
    for i, c in enumerate([(237, 106, 94), (245, 191, 79), (98, 197, 84)]):
        d.ellipse([14 + i * 20, 10, 26 + i * 20, 22], fill=c)
    d.text((img.width // 2 - 80, 7), "Hard Drive Monitor", font=reg, fill=(170, 175, 185))
    y = pad + title_h
    for line in lines:
        x = pad
        for chunk, color, b in spans(line):
            f = bold if b else reg
            for ch in chunk:
                use = f if f.getmask(ch).getbbox() or ch == " " else sym
                d.text((x, y), ch, font=use, fill=color)
                x += cw
        y += lh
    out = os.path.join(ROOT, "demo-status.png")
    img.save(out)
    print("wrote", out, img.size)


if __name__ == "__main__":
    main()
