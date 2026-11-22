#!/usr/bin/env python3
"""Regenerate src/app_icon_rgba.h from the 64x64 frame of gui/res/hdmon.ico."""
from pathlib import Path
from PIL import Image, IcoImagePlugin

root = Path(__file__).resolve().parents[1]
ico = IcoImagePlugin.IcoImageFile(root / "gui/res/hdmon.ico")
src = ico.ico.getimage((64, 64)).convert("RGBA")
px, (w, h) = src.load(), src.size
xs, ys = [], []
for y in range(h):
    for x in range(w):
        if px[x, y][3] > 0:
            xs.append(x)
            ys.append(y)
pad = 1
l, t, r, b = max(0, min(xs) - pad), max(0, min(ys) - pad), min(w, max(xs) + 1 + pad), min(h, max(ys) + 1 + pad)
crop = src.crop((l, t, r, b))
cw, ch = crop.size
data = crop.tobytes()
out = root / "src/app_icon_rgba.h"
lines = [
    "// Cropped 64x64 frame of gui/res/hdmon.ico (plus a 1 px alpha halo).",
    "// Used by the tray icon; regenerate: python3 tools/gen_app_icon_rgba.py",
    "#pragma once",
    "#include <cstdint>",
    "",
    "namespace hdmon::tray {",
    "",
    f"inline constexpr int APP_ICON_W = {cw};",
    f"inline constexpr int APP_ICON_H = {ch};",
    "inline constexpr unsigned char APP_ICON_RGBA[] = {",
]
row = []
for b in data:
    row.append(f"0x{b:02x}")
    if len(row) == 16:
        lines.append("    " + ",".join(row) + ",")
        row = []
if row:
    lines.append("    " + ",".join(row) + ",")
lines += ["};", "", "}  // namespace hdmon::tray", ""]
out.write_text("\n".join(lines))
print(f"wrote {out} {cw}x{ch}")
