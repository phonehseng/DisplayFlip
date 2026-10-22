"""Generates the app icon and Stream Deck images with no dependencies (pure Python).

    python tools/make_icons.py

Writes assets/displayflip.ico and streamdeck/com.displayflip.sdPlugin/images/*.png.
The generated files are committed, so this only needs to run when the artwork changes.
"""
import math
import os
import struct
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEAL = (45, 212, 191, 255)
TEAL_DIM = (20, 110, 100, 255)
WHITE = (244, 244, 245, 255)
GRAY = (113, 113, 122, 255)
DARK = (24, 24, 27, 255)
BLACK = (9, 9, 11, 255)
SS = 4  # supersampling per axis


# ---- shapes (coordinates in 0..1 of the canvas) -----------------------------
def rrect(x0, y0, x1, y1, r):
    def f(x, y):
        if x < x0 or x > x1 or y < y0 or y > y1:
            return False
        cx = min(max(x, x0 + r), x1 - r)
        cy = min(max(y, y0 + r), y1 - r)
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r
    return f, (x0, y0, x1, y1)


def rrect_ring(x0, y0, x1, y1, r, w):
    outer, bb = rrect(x0, y0, x1, y1, r)
    inner, _ = rrect(x0 + w, y0 + w, x1 - w, y1 - w, max(r - w, 0.0))
    return (lambda x, y: outer(x, y) and not inner(x, y)), bb


def line(ax, ay, bx, by, w):
    dx, dy = bx - ax, by - ay
    ll = dx * dx + dy * dy
    h = w / 2

    def f(x, y):
        t = 0.0 if ll == 0 else max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / ll))
        px, py = ax + t * dx, ay + t * dy
        return (x - px) ** 2 + (y - py) ** 2 <= h * h
    return f, (min(ax, bx) - h, min(ay, by) - h, max(ax, bx) + h, max(ay, by) + h)


def circle(cx, cy, r):
    return (lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r), (cx - r, cy - r, cx + r, cy + r)


def poly(pts):
    def f(x, y):
        inside = False
        n = len(pts)
        for i in range(n):
            x1, y1 = pts[i]
            x2, y2 = pts[(i + 1) % n]
            if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
                inside = not inside
        return inside
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    return f, (min(xs), min(ys), max(xs), max(ys))


# ---- raster ------------------------------------------------------------------
def render(size, layers):
    px = [[0.0, 0.0, 0.0, 0.0] for _ in range(size * size)]
    for (fn, bb), color in layers:
        x0 = max(0, int(bb[0] * size) - 1)
        y0 = max(0, int(bb[1] * size) - 1)
        x1 = min(size, int(math.ceil(bb[2] * size)) + 1)
        y1 = min(size, int(math.ceil(bb[3] * size)) + 1)
        ca = color[3] / 255.0
        for y in range(y0, y1):
            for x in range(x0, x1):
                hit = 0
                for sy in range(SS):
                    v = (y + (sy + 0.5) / SS) / size
                    for sx in range(SS):
                        if fn((x + (sx + 0.5) / SS) / size, v):
                            hit += 1
                if not hit:
                    continue
                a = ca * hit / (SS * SS)
                d = px[y * size + x]
                oa = a + d[3] * (1 - a)
                for i in range(3):
                    d[i] = (color[i] * a + d[i] * d[3] * (1 - a)) / oa if oa else 0
                d[3] = oa
    return px


def png_bytes(size, px):
    raw = bytearray()
    for y in range(size):
        raw.append(0)
        for x in range(size):
            r, g, b, a = px[y * size + x]
            raw += bytes((int(r + 0.5), int(g + 0.5), int(b + 0.5), int(a * 255 + 0.5)))

    def chunk(t, data):
        c = struct.pack(">I", len(data)) + t + data
        return c + struct.pack(">I", zlib.crc32(t + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def bmp_icon_bytes(size, px):
    hdr = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    body = bytearray()
    for y in range(size - 1, -1, -1):
        for x in range(size):
            r, g, b, a = px[y * size + x]
            body += bytes((int(b + 0.5), int(g + 0.5), int(r + 0.5), int(a * 255 + 0.5)))
    mask_row = ((size + 31) // 32) * 4
    return hdr + bytes(body) + bytes(mask_row * size)


def write_png(path, size, layers):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(png_bytes(size, render(size, layers)))


# ---- artwork -----------------------------------------------------------------
def monitor(x0, y0, x1, y1, frame, screen, stroke=0.035, stand=True):
    """A monitor outline with a filled screen, inside the given box (stand below)."""
    h = y1 - y0
    body_bottom = y1 - (h * 0.22 if stand else 0)
    layers = [
        (rrect(x0, y0, x1, body_bottom, stroke * 1.6), frame),
        (rrect(x0 + stroke, y0 + stroke, x1 - stroke, body_bottom - stroke, stroke * 0.6), screen),
    ]
    if stand:
        cx = (x0 + x1) / 2
        layers += [
            (line(cx, body_bottom, cx, y1 - stroke * 0.6, stroke * 1.2), frame),
            (line(cx - (x1 - x0) * 0.18, y1 - stroke * 0.6, cx + (x1 - x0) * 0.18, y1 - stroke * 0.6, stroke * 1.2), frame),
        ]
    return layers


def flip_arrows(x0, y0, x1, y1, color, w):
    """Two opposite arrows (the "flip")."""
    ym1 = y0 + (y1 - y0) * 0.3
    ym2 = y0 + (y1 - y0) * 0.7
    head = (y1 - y0) * 0.22
    return [
        (line(x0, ym1, x1 - head * 0.6, ym1, w), color),
        (poly([(x1, ym1), (x1 - head, ym1 - head * 0.8), (x1 - head, ym1 + head * 0.8)]), color),
        (line(x0 + head * 0.6, ym2, x1, ym2, w), color),
        (poly([(x0, ym2), (x0 + head, ym2 - head * 0.8), (x0 + head, ym2 + head * 0.8)]), color),
    ]


def app_icon():
    layers = [(rrect(0.02, 0.02, 0.98, 0.98, 0.22), DARK)]
    layers += monitor(0.14, 0.18, 0.86, 0.84, WHITE, TEAL, stroke=0.05)
    layers += flip_arrows(0.32, 0.27, 0.68, 0.58, DARK, 0.055)
    return layers


def key_bg():
    return [(rrect(0, 0, 1, 1, 0.0), BLACK)]


def key_monitor(on):
    layers = key_bg()
    if on:
        layers += [(rrect(0.12, 0.16, 0.88, 0.72, 0.08), (45, 212, 191, 60))]  # glow
        layers += monitor(0.17, 0.2, 0.83, 0.82, WHITE, TEAL, stroke=0.04)
    else:
        layers += monitor(0.17, 0.2, 0.83, 0.82, GRAY, DARK, stroke=0.04)
        layers += [(line(0.42, 0.33, 0.58, 0.49, 0.045), GRAY), (line(0.58, 0.33, 0.42, 0.49, 0.045), GRAY)]
    return layers


def triple(on, base=None):
    layers = list(base or key_bg())
    frame = WHITE if on else GRAY
    screen = TEAL if on else DARK
    side = TEAL_DIM if on else DARK
    layers += monitor(0.04, 0.3, 0.34, 0.66, frame, side, stroke=0.03, stand=False)
    layers += monitor(0.66, 0.3, 0.96, 0.66, frame, side, stroke=0.03, stand=False)
    layers += monitor(0.3, 0.24, 0.7, 0.8, frame, screen, stroke=0.035)
    return layers


def toggle(right):
    layers = key_bg()
    layers += [(rrect(0.16, 0.34, 0.84, 0.66, 0.16), TEAL if right else (63, 63, 70, 255))]
    cx = 0.68 if right else 0.32
    layers += [(circle(cx, 0.5, 0.125), WHITE)]
    return layers


def command_key():
    layers = key_bg()
    layers += [(rrect(0.14, 0.2, 0.86, 0.8, 0.08), DARK)]
    layers += [(line(0.3, 0.38, 0.44, 0.5, 0.06), TEAL), (line(0.44, 0.5, 0.3, 0.62, 0.06), TEAL)]
    layers += [(line(0.5, 0.64, 0.7, 0.64, 0.06), WHITE)]
    return layers


def glyph_monitor():
    return monitor(0.08, 0.12, 0.92, 0.9, WHITE, (0, 0, 0, 0), stroke=0.09)


def glyph_triple():
    l = monitor(0.0, 0.28, 0.3, 0.68, WHITE, (0, 0, 0, 0), stroke=0.07, stand=False)
    l += monitor(0.7, 0.28, 1.0, 0.68, WHITE, (0, 0, 0, 0), stroke=0.07, stand=False)
    l += monitor(0.26, 0.16, 0.74, 0.86, WHITE, (0, 0, 0, 0), stroke=0.08)
    return l


def glyph_toggle():
    return [(rrect_ring(0.06, 0.28, 0.94, 0.72, 0.22, 0.08), WHITE), (circle(0.7, 0.5, 0.13), WHITE)]


def glyph_command():
    return [(line(0.12, 0.25, 0.42, 0.5, 0.11), WHITE), (line(0.42, 0.5, 0.12, 0.75, 0.11), WHITE),
            (line(0.52, 0.78, 0.9, 0.78, 0.11), WHITE)]


def glyph_app():
    l = monitor(0.06, 0.1, 0.94, 0.92, WHITE, (0, 0, 0, 0), stroke=0.09)
    l += flip_arrows(0.3, 0.26, 0.7, 0.6, WHITE, 0.08)
    return l


def main():
    img = os.path.join(ROOT, "streamdeck", "com.displayflip.sdPlugin", "images")
    pairs = {
        "monitor_on": key_monitor(True), "monitor_off": key_monitor(False),
        "profile_on": triple(True), "profile_off": triple(False),
        "toggle_a": toggle(False), "toggle_b": toggle(True),
        "command": command_key(),
    }
    for name, layers in pairs.items():
        write_png(os.path.join(img, name + ".png"), 72, layers)
        write_png(os.path.join(img, name + "@2x.png"), 144, layers)
    for name, layers, size in [("action_monitor", glyph_monitor(), 20), ("action_profile", glyph_triple(), 20),
                               ("action_toggle", glyph_toggle(), 20), ("action_command", glyph_command(), 20),
                               ("category", glyph_app(), 28)]:
        write_png(os.path.join(img, name + ".png"), size, layers)
        write_png(os.path.join(img, name + "@2x.png"), size * 2, layers)
    write_png(os.path.join(img, "plugin.png"), 72, app_icon())
    write_png(os.path.join(img, "plugin@2x.png"), 144, app_icon())

    # Windows icon: BMP entries for the small sizes, PNG for 256
    sizes = [16, 20, 24, 32, 40, 48, 64, 256]
    entries = []
    for s in sizes:
        px = render(s, app_icon())
        entries.append((s, png_bytes(s, px) if s >= 256 else bmp_icon_bytes(s, px)))
    out = bytearray(struct.pack("<HHH", 0, 1, len(entries)))
    offset = 6 + 16 * len(entries)
    for s, data in entries:
        out += struct.pack("<BBBBHHII", s if s < 256 else 0, s if s < 256 else 0, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for _, data in entries:
        out += data
    os.makedirs(os.path.join(ROOT, "assets"), exist_ok=True)
    with open(os.path.join(ROOT, "assets", "displayflip.ico"), "wb") as f:
        f.write(out)
    write_png(os.path.join(ROOT, "assets", "displayflip.png"), 256, app_icon())
    print("icons written")


if __name__ == "__main__":
    main()
