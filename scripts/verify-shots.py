#!/usr/bin/env python3
"""verify-shots.py — every manifest entry produced a real, distinct image.

Three failures this exists to catch, none of which is visible at the file level:

1. The GL context dies, every capture reads back a uniform buffer, and a dozen
   black rectangles get committed as documentation.
2. The scene kind never actually switches, so every 3D shot is the default
   address plane under a different filename. (Real bug shape: SceneView's
   hud.req_kind is inert unless req_kind_change is also set.)
3. The layer set never applies, so shots meant to isolate different layers come
   out identical. (Real bug shape: SceneLayers defaults every flag TRUE, so
   "switch these on" is a no-op unless you clear first.)

All three produce correctly-sized, non-blank, plausible-looking PNGs. Only
comparing the images to each other reveals 2 and 3."""
import hashlib
import json
import struct
import sys
import zlib

MANIFEST = sys.argv[1] if len(sys.argv) > 1 else "desktop/shots.json"
SHOTS = sys.argv[2] if len(sys.argv) > 2 else "build/shots"


def png_info(path):
    """Return (width, height, distinct_colour_count) using only the stdlib."""
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    w, h = struct.unpack(">II", data[16:24])
    bitd, colr = data[24], data[25]
    if bitd != 8 or colr != 6:
        raise ValueError(f"expected 8-bit RGBA, got bitdepth={bitd} colour={colr}")

    idat, pos = b"", 8
    while pos < len(data):
        (ln,) = struct.unpack(">I", data[pos:pos + 4])
        typ = data[pos + 4:pos + 8]
        if typ == b"IDAT":
            idat += data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
    raw = zlib.decompress(idat)

    stride = w * 4
    out, prev = bytearray(), bytearray(stride)
    p = 0
    for _ in range(h):
        f = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            if f == 1:
                line[i] = (line[i] + a) & 0xFF
            elif f == 2:
                line[i] = (line[i] + b) & 0xFF
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif f == 4:
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out += line
        prev = line

    colours = set()
    for y in range(0, h, 7):  # sampled; a full scan is needlessly slow
        for x in range(0, w, 7):
            o = y * stride + x * 4
            colours.add(bytes(out[o:o + 3]))
    return w, h, len(colours)


with open(MANIFEST, encoding="utf-8") as fh:
    shots = json.load(fh)

failures = []
digests = {}
for s in shots:
    path = f"{SHOTS}/{s['name']}.png"
    try:
        w, h, ncol = png_info(path)
    except (OSError, ValueError) as exc:
        failures.append(f"{s['name']}: {exc}")
        continue
    want_w, want_h = s.get("size", [1600, 1000])
    if (w, h) != (want_w, want_h):
        failures.append(f"{s['name']}: {w}x{h}, manifest says {want_w}x{want_h}")
    if ncol < 8:
        failures.append(
            f"{s['name']}: only {ncol} distinct sampled colours — "
            "this image is blank or near-blank, not a screenshot")
    with open(path, "rb") as fh:
        digests[s["name"]] = hashlib.sha256(fh.read()).hexdigest()

# EVERY shot must differ from every other. Two identical images mean the thing
# the shot was supposed to vary — the view, the scene kind, the layer set — never
# actually changed, which no amount of size- or blankness-checking would reveal.
# Documentation that shows the same picture under two captions is worse than
# documentation with one picture.
names = [s["name"] for s in shots if s["name"] in digests]
for i, a in enumerate(names):
    for b in names[i + 1:]:
        if digests[a] == digests[b]:
            failures.append(
                f"{a} and {b} are byte-identical — whatever distinguishes them "
                "(view, scene kind, or layer set) did not take effect")

if failures:
    print("FAIL:")
    for f in failures:
        print("  " + f)
    sys.exit(1)
print(f"PASS: {len(shots)} shots, correctly sized, non-blank, and all distinct")
