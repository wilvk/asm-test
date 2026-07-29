#!/usr/bin/env python3
# gen-app-icon.py — the single source of truth for the desktop app's icon.
#
# There is no SVG rasteriser on every lane (rsvg/inkscape/convert are all absent
# on a bare host), so the artwork is drawn procedurally with Pillow and this
# script IS the master. It is a DEV tool, not a build step: it writes committed
# outputs so the build itself needs no Pillow (the docker-desktop image has no
# PIL, and adding it for a cosmetic asset is not worth a heavier image). Re-run it
# by hand — `make desktop-icon-regen` — after changing the design, then commit the
# regenerated outputs.
#
# It emits two kinds of output from one 512px master raster:
#   1. desktop/src/ui/app_icon.h            raw RGBA mips, compiled into the app
#      and the viewer so glfwSetWindowIcon() gives an X11 window/titlebar/dock
#      icon with zero runtime PNG decoder (GLFW wants straight-alpha RGBA bytes).
#   2. desktop/assets/icons/hicolor/**.png  the freedesktop icon-theme tree the
#      .desktop file's `Icon=asmtest-desktop` resolves against — the path GNOME's
#      dash and native Wayland use (glfwSetWindowIcon is a no-op on Wayland).
#
# Design (reads at 16px): a dark rounded square holding three left-aligned
# instruction "rows" — the app's timeline/loom of executed steps — one amber
# (the current step), the others cyan, crossed by a light vertical playhead (the
# scrubber). The motif is the app: a trace of instructions with a playhead on it.

import os
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.stderr.write(
        "gen-app-icon.py needs Pillow (PIL).\n"
        "  python3 -m pip install --user Pillow\n"
        "Then re-run: make desktop-icon-regen\n"
    )
    sys.exit(1)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The compiled-in window icon ships these sizes; GLFW/_NET_WM_ICON picks the
# nearest for the titlebar (small) and the dock (larger). Kept lean — raw RGBA in
# a C array — so the header stays small; the dock upscales past 64 cleanly.
HEADER_SIZES = [16, 32, 48, 64]
# The installed freedesktop theme carries the full ramp the dock/settings want.
PNG_SIZES = [16, 24, 32, 48, 64, 128, 256]

MASTER = 512

# --- palette (the app's dark honesty-chrome, cool accent + one amber highlight) -
BG_TOP = (23, 33, 47)     # #17212f
BG_BOT = (12, 18, 27)     # #0c121b
BORDER = (51, 68, 93)     # #33445d
CYAN = (72, 182, 255)     # #48b6ff — a normal executed row
AMBER = (244, 178, 63)    # #f4b23f — the current step
PLAYHEAD = (234, 241, 248)  # #eaf1f8 — the scrubber line


def _rounded_mask(size, radius):
    m = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, size - 1, size - 1], radius, fill=255)
    return m


def draw_master():
    """Render the icon once at MASTER px; every output is a downsample of this."""
    img = Image.new("RGBA", (MASTER, MASTER), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    pad = 12
    B = MASTER - 2 * pad            # inner square side
    radius = int(0.23 * B)

    # Vertical gradient body, clipped to the rounded square.
    grad = Image.new("RGBA", (MASTER, MASTER), (0, 0, 0, 0))
    gd = ImageDraw.Draw(grad)
    for y in range(pad, MASTER - pad):
        t = (y - pad) / max(1, B - 1)
        col = tuple(int(BG_TOP[i] + (BG_BOT[i] - BG_TOP[i]) * t) for i in range(3))
        gd.line([(pad, y), (MASTER - pad, y)], fill=col + (255,))
    body_mask = Image.new("L", (MASTER, MASTER), 0)
    ImageDraw.Draw(body_mask).rounded_rectangle(
        [pad, pad, MASTER - pad, MASTER - pad], radius, fill=255)
    img.paste(grad, (0, 0), body_mask)

    # Inset border stroke — a subtle frame, drawn over the body.
    d.rounded_rectangle([pad, pad, MASTER - pad, MASTER - pad], radius,
                        outline=BORDER + (255,), width=4)

    # Three instruction rows (pills). Left-aligned, one amber = the current step.
    row_x0 = pad + int(0.20 * B)
    row_span = (pad + int(0.86 * B)) - row_x0
    h = int(0.13 * B)
    g = int(0.09 * B)
    total = 3 * h + 2 * g
    top = pad + (B - total) // 2
    rows = [
        (0.86, CYAN),   # a longer executed line
        (0.62, AMBER),  # the current step — shorter, highlighted
        (0.74, CYAN),
    ]
    ys = []
    for i, (wfrac, col) in enumerate(rows):
        y0 = top + i * (h + g)
        y1 = y0 + h
        x1 = row_x0 + int(wfrac * row_span)
        d.rounded_rectangle([row_x0, y0, x1, y1], h // 2, fill=col + (255,))
        ys.append((y0, y1))

    # The playhead: a light vertical bar crossing every row (< the shortest row so
    # it never dangles off an end), with a faint glow behind it.
    px = row_x0 + int(0.42 * row_span)
    pw = max(6, int(0.032 * B))
    py0 = ys[0][0] - int(0.03 * B)
    py1 = ys[2][1] + int(0.03 * B)
    glow = Image.new("RGBA", (MASTER, MASTER), (0, 0, 0, 0))
    ImageDraw.Draw(glow).rounded_rectangle(
        [px - pw, py0, px + pw, py1], pw, fill=PLAYHEAD + (70,))
    img = Image.alpha_composite(img, glow)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([px - pw // 2, py0, px + pw // 2, py1], pw // 2,
                        fill=PLAYHEAD + (235,))
    return img


def emit_header(master, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    blocks = []
    entries = []
    for s in HEADER_SIZES:
        rgba = master.resize((s, s), Image.LANCZOS).tobytes()  # straight-alpha RGBA
        name = "kAppIcon%d" % s
        # wrap at 18 bytes/line for a readable diff
        toks = ["0x%02x" % b for b in rgba]
        wrapped = ",\n    ".join(
            ", ".join(toks[i:i + 18]) for i in range(0, len(toks), 18))
        blocks.append(
            "static const unsigned char %s[%d] = {\n    %s};\n"
            % (name, len(rgba), wrapped))
        entries.append("    {%d, %s}," % (s, name))
    with open(path, "w") as f:
        f.write(
            "// app_icon.h — GENERATED by scripts/gen-app-icon.py. DO NOT EDIT.\n"
            "// Regenerate with `make desktop-icon-regen`, then commit this file.\n"
            "// clang-format off — leave the packed byte arrays as emitted.\n"
            "//\n"
            "// Raw straight-alpha RGBA mips (top-left origin, row-major) for the\n"
            "// full app + viewer window icon, fed to glfwSetWindowIcon() so an\n"
            "// X11 session shows a real titlebar/taskbar/dock icon with no runtime\n"
            "// PNG decoder. On Wayland glfwSetWindowIcon is a no-op — the installed\n"
            "// .desktop `Icon=` (scripts/gen-app-icon.py's PNG tree) covers that.\n"
            "#pragma once\n"
            "#include <cstddef>\n\n"
            "namespace asmdesk {\n\n"
            "struct AppIconImage {\n"
            "    int size;                  // width == height, in pixels\n"
            "    const unsigned char *rgba; // size*size*4 bytes, straight alpha\n"
            "};\n\n"
            + "\n".join(blocks) + "\n"
            "// Smallest-first; glfwSetWindowIcon picks the nearest per surface.\n"
            "static const AppIconImage kAppIconImages[] = {\n"
            + "\n".join(entries) + "\n};\n\n"
            "inline const AppIconImage *app_icon_images(size_t *count) {\n"
            "    if (count) *count = sizeof(kAppIconImages) / sizeof(kAppIconImages[0]);\n"
            "    return kAppIconImages;\n"
            "}\n\n"
            "} // namespace asmdesk\n")
    return path


def emit_pngs(master, base):
    written = []
    for s in PNG_SIZES:
        d = os.path.join(base, "icons", "hicolor", "%dx%d" % (s, s), "apps")
        os.makedirs(d, exist_ok=True)
        p = os.path.join(d, "asmtest-desktop.png")
        master.resize((s, s), Image.LANCZOS).save(p)
        written.append(p)
    # A convenience top-level 256 copy (README / non-theme installs).
    top = os.path.join(base, "asmtest-desktop.png")
    master.resize((256, 256), Image.LANCZOS).save(top)
    written.append(top)
    return written


def main():
    master = draw_master()
    hdr = emit_header(master, os.path.join(ROOT, "desktop", "src", "ui", "app_icon.h"))
    pngs = emit_pngs(master, os.path.join(ROOT, "desktop", "assets"))
    print("wrote %s" % os.path.relpath(hdr, ROOT))
    for p in pngs:
        print("wrote %s" % os.path.relpath(p, ROOT))


if __name__ == "__main__":
    main()
