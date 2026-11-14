#!/usr/bin/env python3
"""Generate the README feature strips in the gizmo-card style.

Usage: python3 docs/images/readme/generate.py docs/images/readme
Writes <strip>.svg (English) and <strip>.pt-BR.svg for each strip.
"""
import math
import os
import sys
from xml.sax.saxutils import escape

RED, GREEN, BLUE = "#E62929", "#29C738", "#335CF2"
SEL = "#4FB6FF"
TOP, LEFT, RIGHT = "#c9c4b8", "#8e8a80", "#a9a498"
FONT = "Inter, 'Segoe UI', Helvetica, Arial, sans-serif"
MONO = "'JetBrains Mono', Consolas, 'DejaVu Sans Mono', monospace"

CARD_W, CARD_H, GAP, PAD = 560, 440, 40, 20
C30 = math.cos(math.radians(30))


def f(v):
    return f"{v:.1f}"


def pts(ps):
    return " ".join(f"{f(x)},{f(y)}" for x, y in ps)


class Iso:
    def __init__(self, cx, cy, s):
        self.cx, self.cy, self.s = cx, cy, s

    def p(self, x, y, z=0.0):
        return (self.cx + (x - y) * C30 * self.s,
                self.cy + (x + y) * 0.5 * self.s - z * self.s)


def poly(ps, fill, stroke="none", sw=0, op=1.0, extra=""):
    o = f' opacity="{op}"' if op != 1.0 else ""
    return f'<polygon points="{pts(ps)}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}" stroke-linejoin="round"{o}{extra}/>'


def line(a, b, col, w, op=1.0, dash=None):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    o = f' opacity="{op}"' if op != 1.0 else ""
    return (f'<line x1="{f(a[0])}" y1="{f(a[1])}" x2="{f(b[0])}" y2="{f(b[1])}" '
            f'stroke="{col}" stroke-width="{w}" stroke-linecap="round"{d}{o}/>')


def circle(c, r, fill, stroke="none", sw=0, op=1.0):
    o = f' opacity="{op}"' if op != 1.0 else ""
    return f'<circle cx="{f(c[0])}" cy="{f(c[1])}" r="{f(r)}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}"{o}/>'


def text(x, y, s, size, col, weight=400, anchor="start", family=FONT, op=1.0):
    o = f' opacity="{op}"' if op != 1.0 else ""
    return (f'<text x="{f(x)}" y="{f(y)}" font-family="{family}" font-size="{size}" '
            f'font-weight="{weight}" fill="{col}" text-anchor="{anchor}"{o}>{escape(s)}</text>')


def shade(hexcol, k):
    h = hexcol.lstrip("#")
    r, g, b = (int(h[i:i + 2], 16) for i in (0, 2, 4))
    r, g, b = (max(0, min(255, int(c * k))) for c in (r, g, b))
    return f"#{r:02x}{g:02x}{b:02x}"


def box(iso, x, y, z, w, d, h, top=TOP, left=LEFT, right=RIGHT, outline=None, op=1.0, sw=2.2):
    """Visible faces: top, the x=max face (screen right) and the y=max face (screen left, darker)."""
    P = iso.p
    t = [P(x, y, z + h), P(x + w, y, z + h), P(x + w, y + d, z + h), P(x, y + d, z + h)]
    l = [P(x + w, y, z), P(x + w, y + d, z), P(x + w, y + d, z + h), P(x + w, y, z + h)]
    r = [P(x, y + d, z), P(x + w, y + d, z), P(x + w, y + d, z + h), P(x, y + d, z + h)]
    out = [poly(l, right, op=op), poly(r, left, op=op), poly(t, top, op=op)]
    if outline:
        out += [poly(q, "none", outline, sw, op=op) for q in (l, r, t)]
    return "".join(out)


def tinted(col):
    return dict(top=shade(col, 1.12), left=shade(col, 0.78), right=shade(col, 0.94))


def face_matrix(o, u, v):
    """SVG matrix mapping the unit square onto the parallelogram o, o+u, o+v."""
    return f"matrix({f(u[0])} {f(u[1])} {f(v[0])} {f(v[1])} {f(o[0])} {f(o[1])})".replace(".0 ", " ")


def floor_grid(iso, n, col="#ffffff", op=0.13):
    out = []
    for i in range(-n, n + 1):
        out.append(line(iso.p(i, -n), iso.p(i, n), col, 1, op))
        out.append(line(iso.p(-n, i), iso.p(n, i), col, 1, op))
    return "".join(out)


def shadow(iso, x, y, rx, op=0.18):
    c = iso.p(x, y)
    return f'<ellipse cx="{f(c[0])}" cy="{f(c[1])}" rx="{f(rx * iso.s)}" ry="{f(rx * iso.s * 0.5)}" fill="#0b1830" opacity="{op}"/>'


def arrow(a, b, col, w=5, head=14):
    dx, dy = b[0] - a[0], b[1] - a[1]
    L = math.hypot(dx, dy)
    ux, uy = dx / L, dy / L
    nx, ny = -uy, ux
    base = (b[0] - ux * head * 2.4, b[1] - uy * head * 2.4)
    tri = [b, (base[0] + nx * head, base[1] + ny * head), (base[0] - nx * head, base[1] - ny * head)]
    return line(a, base, col, w) + poly(tri, col)


# ---------------------------------------------------------------- scenes
# Each takes (cx, cy) = the centre of the drawing area of a card.

def scene_objects(cx, cy):
    iso = Iso(cx, cy + 10, 52)
    o = [floor_grid(iso, 5), shadow(iso, 0, 0, 1.6)]
    o.append(box(iso, -1, -1, 0, 2, 2, 1.1, outline=SEL))
    c = iso.p(0, 0, 0.55)
    for vec, col in (((1, 0, 0), RED), ((0, 1, 0), BLUE), ((0, 0, 1), GREEN)):
        for sgn in (1, -1):
            a = iso.p(*(0.9 * sgn * v for v in vec))
            b = iso.p(*(2.6 * sgn * v for v in vec))
            a = (a[0], a[1] - 0.55 * iso.s)
            b = (b[0], b[1] - 0.55 * iso.s)
            o.append(arrow(a, b, col, 5, 11))
    o.append(circle(c, 6, "#ffffff", SEL, 2))
    return "".join(o)


def scene_vehicles(cx, cy):
    iso = Iso(cx - 10, cy + 20, 44)
    o = [floor_grid(iso, 6), shadow(iso, 0, 0, 2.6, 0.22)]
    body = "#d6453d"
    # rear wheels (hidden side) first
    for wx in (-1.3, 1.3):
        c = iso.p(wx, -1.0, 0.38)
        o.append(f'<ellipse cx="{f(c[0])}" cy="{f(c[1])}" rx="17" ry="20" fill="#1d1f24" transform="rotate(-30 {f(c[0])} {f(c[1])})"/>')
    o.append(box(iso, -2.2, -1.0, 0.3, 4.4, 2.0, 0.65, **tinted(body)))
    o.append(box(iso, -1.0, -0.9, 0.95, 2.0, 1.8, 0.6, top=shade(body, 1.12),
                 left="#9fc4e6", right="#b9d6f0"))
    # plate on the front (x = max) face
    P = iso.p
    plate = [P(2.2, -0.4, 0.42), P(2.2, 0.4, 0.42), P(2.2, 0.4, 0.66), P(2.2, -0.4, 0.66)]
    o.append(poly(plate, "#f4f4ee", "#2a2a2a", 1.2))
    for k in range(4):
        a = P(2.2, -0.28 + k * 0.17, 0.48)
        b = P(2.2, -0.28 + k * 0.17 + 0.09, 0.6)
        o.append(line(a, (a[0], b[1]), "#30353f", 2))
    # lights
    for ly in (-0.85, 0.65):
        o.append(poly([P(2.2, ly, 0.72), P(2.2, ly + 0.2, 0.72), P(2.2, ly + 0.2, 0.86), P(2.2, ly, 0.86)], "#fff6c8"))
    for wx in (-1.3, 1.3):
        c = iso.p(wx, 1.0, 0.38)
        o.append(f'<ellipse cx="{f(c[0])}" cy="{f(c[1])}" rx="17" ry="20" fill="#1d1f24" transform="rotate(-30 {f(c[0])} {f(c[1])})"/>')
        o.append(f'<ellipse cx="{f(c[0])}" cy="{f(c[1])}" rx="8" ry="9.5" fill="#c9ccd2" transform="rotate(-30 {f(c[0])} {f(c[1])})"/>')
    # colour swatches
    sx, sy = cx + 150, cy - 110
    for i, col in enumerate((body, "#f1f1f1", "#2f6fd6", "#1f1f22")):
        o.append(circle((sx + i * 28, sy), 10, col, "#ffffff", 2 if i == 0 else 1.2))
    return "".join(o)


def scene_elements(cx, cy):
    iso = Iso(cx, cy + 30, 44)
    P = iso.p
    o = [floor_grid(iso, 6)]
    zone = [P(-3, -3), P(1.5, -3), P(1.5, 1.5), P(-3, 1.5)]
    o.append(poly(zone, "#9b5de5", "#c9a6ff", 2, op=0.55))
    # pickup: a floating octahedron (heart pickups are rounder, this reads at a glance)
    px, py, pz = -0.8, -0.8, 1.6
    o.append(shadow(iso, px, py, 0.5, 0.25))
    c = P(px, py, pz)
    r = 26
    top, bot = (c[0], c[1] - r * 1.3), (c[0], c[1] + r * 1.3)
    lft, rgt, frt = (c[0] - r, c[1]), (c[0] + r, c[1]), (c[0] + r * 0.15, c[1] + r * 0.35)
    o += [poly([top, lft, frt], "#7be38a"), poly([top, frt, rgt], "#45c157"),
          poly([bot, lft, frt], "#2f9a3f"), poly([bot, frt, rgt], "#237a31")]
    # 3D text label
    lc = P(1.6, 1.4, 2.6)
    o.append(f'<rect x="{f(lc[0] - 70)}" y="{f(lc[1] - 18)}" width="140" height="34" rx="8" fill="#0d1526" opacity="0.72"/>')
    o.append(text(lc[0], lc[1] + 5, "Grove Street", 15, "#ffd75a", 600, "middle"))
    a = P(1.6, 1.4, 0)
    o.append(line((lc[0], lc[1] + 16), a, "#ffffff", 1.5, 0.6, "3 4"))
    o.append(circle(a, 4, "#ffffff"))
    # map icon pin
    mc = P(2.4, -1.8, 0)
    o.append(f'<path d="M{f(mc[0])} {f(mc[1])} l-14 -22 a16 16 0 1 1 28 0 z" fill="#ffffff"/>')
    o.append(circle((mc[0], mc[1] - 32), 9, BLUE))
    return "".join(o)


def figure(x, y, shirt, s=1.0, skin="#e0b48c"):
    o = [f'<ellipse cx="{f(x)}" cy="{f(y)}" rx="{f(16 * s)}" ry="{f(7 * s)}" fill="#0b1830" opacity="0.22"/>',
         f'<rect x="{f(x - 9 * s)}" y="{f(y - 34 * s)}" width="{f(7 * s)}" height="{f(34 * s)}" rx="3" fill="#2d3550"/>',
         f'<rect x="{f(x + 2 * s)}" y="{f(y - 34 * s)}" width="{f(7 * s)}" height="{f(34 * s)}" rx="3" fill="#252c45"/>',
         f'<rect x="{f(x - 13 * s)}" y="{f(y - 70 * s)}" width="{f(26 * s)}" height="{f(40 * s)}" rx="{f(9 * s)}" fill="{shirt}"/>',
         circle((x, y - 82 * s), 12 * s, skin)]
    return "".join(o)


def scene_npcs(cx, cy):
    iso = Iso(cx, cy + 20, 40)
    P = iso.p
    o = [floor_grid(iso, 6)]
    nodes = [(-4, 2), (-2.5, -0.5), (0, -1), (1.8, 0.8), (4, 0.5)]
    for a, b in zip(nodes, nodes[1:]):
        o.append(line(P(*a), P(*b), "#ffd75a", 3, 0.9, "8 7"))
    for i, n in enumerate(nodes):
        o.append(circle(P(*n), 7, "#ffd75a" if i != 2 else "#ffffff", SEL if i == 2 else "#8a6a00", 2))
    fx, fy = P(-1.1, -0.8)
    o.append(figure(fx, fy, GREEN))
    nx, ny = P(*nodes[2])
    o.append(arrow((fx + 26, fy - 8), (nx - 14, ny - 4), "#ffffff", 3, 7))
    return "".join(o)


def scene_materials(cx, cy):
    iso = Iso(cx, cy + 10, 56)
    P = iso.p
    o = [floor_grid(iso, 5), shadow(iso, 0, 0, 1.7)]
    o.append(box(iso, -1.2, -1.2, 0, 2.4, 2.4, 2.0, top="#d9d4c8"))
    # brick texture on the y=max face (screen left)
    rows, cols = 6, 4
    for r in range(rows):
        off = 0.5 if r % 2 else 0.0
        for c in range(-1, cols + 1):
            x0 = -1.2 + (c + off) * 2.4 / cols
            x1 = x0 + 2.4 / cols
            x0, x1 = max(x0, -1.2), min(x1, 1.2)
            if x1 - x0 < 0.05:
                continue
            z0 = r * 2.0 / rows
            q = [P(x0 + 0.03, 1.2, z0 + 0.03), P(x1 - 0.03, 1.2, z0 + 0.03),
                 P(x1 - 0.03, 1.2, z0 + 2.0 / rows - 0.03), P(x0 + 0.03, 1.2, z0 + 2.0 / rows - 0.03)]
            o.append(poly(q, "#b4553e" if (r + c) % 3 else "#a14a36"))
    # material text on the x=max face (screen right), mapped with a matrix
    oo, ux, vx = P(1.2, 1.2, 2.0), P(1.2, -1.2, 2.0), P(1.2, 1.2, 0.0)
    u = (ux[0] - oo[0], ux[1] - oo[1])
    v = (vx[0] - oo[0], vx[1] - oo[1])
    o.append(poly([P(1.2, -1.2, 0), P(1.2, 1.2, 0), P(1.2, 1.2, 2.0), P(1.2, -1.2, 2.0)], "#1c2b4a"))
    o.append(f'<g transform="{face_matrix(oo, u, v)}">'
             f'<text x="0.5" y="0.45" font-family="{FONT}" font-size="0.2" font-weight="800" fill="#ffd75a" text-anchor="middle">OPEN</text>'
             f'<text x="0.5" y="0.68" font-family="{FONT}" font-size="0.2" font-weight="800" fill="#ffffff" text-anchor="middle">24/7</text></g>')
    for q in ([P(1.2, -1.2, 0), P(1.2, 1.2, 0), P(1.2, 1.2, 2.0), P(1.2, -1.2, 2.0)],
              [P(-1.2, 1.2, 0), P(1.2, 1.2, 0), P(1.2, 1.2, 2.0), P(-1.2, 1.2, 2.0)],
              [P(-1.2, -1.2, 2), P(1.2, -1.2, 2), P(1.2, 1.2, 2), P(-1.2, 1.2, 2)]):
        o.append(poly(q, "none", SEL, 2.2))
    sx, sy = cx + 160, cy - 120
    for i, col in enumerate(("#b4553e", "#1c2b4a", "#ffd75a")):
        o.append(f'<rect x="{f(sx + i * 30)}" y="{f(sy)}" width="22" height="22" rx="5" fill="{col}" stroke="#ffffff" stroke-width="1.5"/>')
    return "".join(o)


def scene_textdraws(cx, cy):
    w, h = 400, 240
    x, y = cx - w / 2, cy - h / 2 + 10
    o = [f'<rect x="{f(x)}" y="{f(y)}" width="{w}" height="{h}" rx="12" fill="#2b3a52" stroke="#ffffff" stroke-opacity="0.35" stroke-width="2"/>']
    # a little skyline in the "game"
    o.append(f'<clipPath id="tdclip"><rect x="{f(x)}" y="{f(y)}" width="{w}" height="{h}" rx="12"/></clipPath>')
    o.append('<g clip-path="url(#tdclip)">')
    o.append(f'<rect x="{f(x)}" y="{f(y)}" width="{w}" height="{h}" fill="url(#tdsky)"/>')
    for i, (bw, bh) in enumerate(((50, 80), (36, 120), (60, 70), (44, 140), (70, 90), (40, 110), (64, 60), (50, 95))):
        bx = x + 10 + sum(v[0] for v in ((50, 80), (36, 120), (60, 70), (44, 140), (70, 90), (40, 110), (64, 60), (50, 95))[:i])
        o.append(f'<rect x="{f(bx)}" y="{f(y + h - bh)}" width="{bw - 4}" height="{bh}" fill="#1a2436" opacity="0.8"/>')
    o.append('</g>')
    # HUD: money, health, armour
    o.append(text(x + w - 18, y + 44, "$00012345", 26, "#5fbf5a", 800, "end", MONO))
    for i, (col, frac) in enumerate((("#b4191d", 0.8), ("#e1e1e1", 0.45))):
        by = y + 60 + i * 20
        o.append(f'<rect x="{f(x + w - 158)}" y="{f(by)}" width="140" height="12" fill="#000000" opacity="0.6"/>')
        o.append(f'<rect x="{f(x + w - 155)}" y="{f(by + 3)}" width="{f(134 * frac)}" height="6" fill="{col}"/>')
    # a selected box with handles
    bx, by, bw, bh = x + 22, y + 140, 170, 76
    o.append(f'<rect x="{f(bx)}" y="{f(by)}" width="{bw}" height="{bh}" fill="#000000" opacity="0.55"/>')
    o.append(text(bx + 12, by + 30, "MISSION PASSED", 16, "#ffd75a", 800))
    o.append(text(bx + 12, by + 56, "Respect +", 14, "#ffffff", 600))
    o.append(f'<rect x="{f(bx)}" y="{f(by)}" width="{bw}" height="{bh}" fill="none" stroke="{SEL}" stroke-width="2"/>')
    for hx in (bx, bx + bw / 2, bx + bw):
        for hy in (by, by + bh / 2, by + bh):
            if hx == bx + bw / 2 and hy == by + bh / 2:
                continue
            o.append(f'<rect x="{f(hx - 5)}" y="{f(hy - 5)}" width="10" height="10" rx="2" fill="#ffffff" stroke="{SEL}" stroke-width="2"/>')
    return "".join(o)


def scene_maptools(cx, cy):
    iso = Iso(cx, cy + 20, 34)
    P = iso.p
    o = [floor_grid(iso, 7)]
    n, R = 10, 3.6
    ring = [P(R * math.cos(t), R * math.sin(t)) for t in (i * 2 * math.pi / 64 for i in range(64))]
    o.append(poly(ring, "none", "#ffffff", 1.5, 0.45, ' stroke-dasharray="6 6"'))
    items = []
    for i in range(n):
        t = i * 2 * math.pi / n
        items.append((R * math.cos(t), R * math.sin(t), i))
    items.sort(key=lambda it: it[0] + it[1])
    for x, y, i in items:
        sel = i == 0
        o.append(box(iso, x - 0.45, y - 0.45, 0, 0.9, 0.9, 1.2 if sel else 0.9,
                     outline=SEL if sel else None, **({} if sel else tinted("#b9b3a5"))))
    c = P(0, 0)
    o.append(circle(c, 6, "#ffffff", SEL, 2))
    return "".join(o)


def scene_radar(cx, cy):
    R = 135
    o = [f'<defs><clipPath id="radarclip"><circle cx="{f(cx)}" cy="{f(cy + 10)}" r="{R}"/></clipPath></defs>']
    o.append(circle((cx, cy + 10), R + 8, "#0d1526"))
    o.append('<g clip-path="url(#radarclip)">')
    o.append(f'<rect x="{f(cx - R)}" y="{f(cy + 10 - R)}" width="{2 * R}" height="{2 * R}" fill="#5c7a54"/>')
    # roads
    o.append(f'<rect x="{f(cx - R)}" y="{f(cy + 52)}" width="{2 * R}" height="16" fill="#3f4448"/>')
    o.append(f'<rect x="{f(cx + 48)}" y="{f(cy + 10 - R)}" width="16" height="{2 * R}" fill="#3f4448"/>')
    # gang-zone pixel art: a little house with a heart
    art = [
        "....rr....",
        "...rrrr...",
        "..rrrrrr..",
        ".rrrrrrrr.",
        "..wwwwww..",
        "..wbwwbw..",
        "..wwwwww..",
        "..wwddww..",
        "..wwddww..",
    ]
    cols = {"r": "#d6453d", "w": "#f0e6d0", "b": "#4FB6FF", "d": "#6b4a2b"}
    cell = 14
    ox, oy = cx - 5 * cell - 30, cy + 10 - 4.5 * cell - 14
    for r, row in enumerate(art):
        for c, ch in enumerate(row):
            if ch in cols:
                o.append(f'<rect x="{f(ox + c * cell)}" y="{f(oy + r * cell)}" width="{cell}" height="{cell}" fill="{cols[ch]}" opacity="0.9"/>')
    o.append('</g>')
    o.append(circle((cx, cy + 10), R + 4, "none", "#0d1526", 8))
    o.append(circle((cx, cy + 10), R + 9, "none", "#ffffff", 1.5, 0.4))
    # player arrow
    px, py = cx + 56, cy + 60
    o.append(poly([(px, py - 14), (px + 10, py + 10), (px, py + 4), (px - 10, py + 10)], "#ffffff", "#0d1526", 2))
    o.append(circle((cx, cy + 10 - R - 4), 14, "#0d1526", "#ffffff", 1.5))
    o.append(text(cx, cy + 10 - R + 1, "N", 14, "#ffffff", 800, "middle"))
    return "".join(o)


def scene_pathnodes(cx, cy):
    iso = Iso(cx, cy + 20, 40)
    P = iso.p
    o = [floor_grid(iso, 6)]
    nodes = {"a": (-4, -1), "b": (-1, -1), "c": (2, -1), "d": (4.5, -1), "e": (-1, 2), "f": (2, 2),
             "g": (-1, -4), "h": (2, 4.5)}
    edges = ["ab", "bc", "cd", "be", "ef", "cf", "bg", "fh"]
    for e in edges:
        a, b = P(*nodes[e[0]]), P(*nodes[e[1]])
        o.append(line(a, b, "#3f4448", 16, 0.85))
    for e in edges:
        a, b = P(*nodes[e[0]]), P(*nodes[e[1]])
        o.append(line(a, b, "#e8e2c8", 1.6, 0.8, "6 8"))
    for k, n in nodes.items():
        sel = k == "c"
        o.append(circle(P(*n), 9 if sel else 7, "#ffffff" if sel else "#ffd75a", SEL if sel else "#8a6a00", 3 if sel else 2))
    # little car on the road
    o.append(box(iso, -2.9, -1.35, 0, 1.0, 0.7, 0.4, **tinted("#2f6fd6")))
    return "".join(o)


def badge(c, col, glyph):
    o = [circle(c, 18, col, "#ffffff", 2.5)]
    if glyph == "!":
        o.append(line((c[0], c[1] - 9), (c[0], c[1] + 2), "#ffffff", 4))
        o.append(circle((c[0], c[1] + 9), 2.6, "#ffffff"))
    else:
        o.append(f'<path d="M{f(c[0] - 8)} {f(c[1])} l6 6 l11 -12" fill="none" stroke="#ffffff" stroke-width="4" stroke-linecap="round" stroke-linejoin="round"/>')
    return "".join(o)


def scene_mapcheck(cx, cy):
    iso = Iso(cx, cy + 30, 44)
    P = iso.p
    o = [floor_grid(iso, 6)]
    # a floating object: the shadow far below it
    o.append(shadow(iso, -1.6, 0.6, 1.0, 0.25))
    o.append(line(P(-1.6, 0.6, 0), P(-1.6, 0.6, 1.4), "#ffffff", 1.5, 0.7, "4 5"))
    o.append(box(iso, -2.4, -0.2, 1.4, 1.6, 1.6, 1.0, outline=RED))
    o.append(badge(P(-1.6, 0.6, 3.4), RED, "!"))
    # a grounded one
    o.append(shadow(iso, 1.4, -1.2, 1.0))
    o.append(box(iso, 0.6, -2.0, 0, 1.6, 1.6, 1.4, outline="#ffffff", sw=1.4))
    o.append(badge(P(1.4, -1.2, 2.4), "#2fa84a", "ok"))
    return "".join(o)


def scene_playerview(cx, cy):
    iso = Iso(cx, cy + 20, 32)
    P = iso.p
    o = [floor_grid(iso, 8)]
    R = 4.2
    ring = [P(R * math.cos(t), R * math.sin(t)) for t in (i * 2 * math.pi / 72 for i in range(72))]
    o.append(poly(ring, "#4FB6FF", SEL, 2.5, 0.28))
    o.append(poly(ring, "none", SEL, 2.5))
    items = [(-6.2, -1.5), (-2.6, -2.2), (1.8, -2.6), (5.6, -3.4), (-3.4, 1.4), (2.5, 1.3), (6.3, 2.2), (-1, 6.3)]
    items.sort(key=lambda it: it[0] + it[1])
    for x, y in items:
        inside = math.hypot(x, y) < R
        o.append(box(iso, x - 0.5, y - 0.5, 0, 1.0, 1.0, 1.0 if inside else 0.8,
                     op=1.0 if inside else 0.28))
    fx, fy = P(0, 0)
    o.append(figure(fx, fy, "#f2a33a", 0.72))
    return "".join(o)


def scene_test(cx, cy):
    iso = Iso(cx - 60, cy + 40, 46)
    P = iso.p
    o = [floor_grid(iso, 6), shadow(iso, 0, 0, 1.6)]
    o.append(box(iso, -0.8, -1.2, 0, 1.6, 2.4, 3.2, top="#4a5468", left="#2a3142", right="#353d50"))
    for k in range(4):
        z = 0.5 + k * 0.7
        q = [P(-0.65, 1.2, z), P(0.65, 1.2, z), P(0.65, 1.2, z + 0.4), P(-0.65, 1.2, z + 0.4)]
        o.append(poly(q, "#20263a"))
        led = P(0.5, 1.2, z + 0.2)
        o.append(circle(led, 3.4, "#5bf06a" if k != 1 else "#ffd75a"))
    c = (cx + 120, cy - 30)
    o.append(circle(c, 52, "#ffffff", op=0.95))
    o.append(poly([(c[0] - 15, c[1] - 24), (c[0] - 15, c[1] + 24), (c[0] + 25, c[1])], "#2fa84a"))
    o.append(f'<rect x="{f(c[0] - 34)}" y="{f(c[1] + 70)}" width="68" height="30" rx="8" fill="#0d1526" opacity="0.75"/>')
    o.append(text(c[0], c[1] + 91, "F5", 17, "#ffffff", 700, "middle", MONO))
    return "".join(o)


def scene_code(cx, cy):
    w, h = 440, 250
    x, y = cx - w / 2, cy - h / 2 + 12
    o = [f'<rect x="{f(x)}" y="{f(y)}" width="{w}" height="{h}" rx="12" fill="#151a24" opacity="0.94"/>',
         f'<rect x="{f(x)}" y="{f(y)}" width="{w}" height="30" rx="12" fill="#1f2633"/>',
         f'<rect x="{f(x)}" y="{f(y + 18)}" width="{w}" height="12" fill="#1f2633"/>']
    for i, col in enumerate(("#ff5f57", "#febc2e", "#28c840")):
        o.append(circle((x + 20 + i * 18, y + 15), 5.5, col))
    o.append(text(x + w / 2, y + 20, "grove.pwn", 13, "#9aa6b8", 500, "middle", MONO))
    K, F, N, C, P_, S = "#c792ea", "#82aaff", "#f78c6c", "#6b7a90", "#d6deeb", "#c3e88d"
    lines_ = [
        [("// Grove Street", C)],
        [("#include ", K), ("<open.mp>", S)],
        [],
        [("public ", K), ("OnGameModeInit", F), ("()", P_)],
        [("{", P_)],
        [("    ", P_), ("CreateObject", F), ("(", P_), ("19353", N), (", ", P_), ("2495.1", N), (", ", P_), ("-1687.4", N), (", ...);", P_)],
        [("    ", P_), ("AddStaticVehicle", F), ("(", P_), ("492", N), (", ", P_), ("2501.0", N), (", ...);", P_)],
        [("    ", P_), ("return ", K), ("1", N), (";", P_)],
        [("}", P_)],
    ]
    for i, segs in enumerate(lines_):
        ly = y + 56 + i * 21
        o.append(text(x + 30, ly, str(i + 1), 12.5, "#4a566a", 400, "end", MONO))
        if segs:
            spans = "".join(f'<tspan fill="{c}">{escape(s)}</tspan>' for s, c in segs)
            o.append(f'<text x="{f(x + 44)}" y="{f(ly)}" font-family="{MONO}" font-size="13" xml:space="preserve">{spans}</text>')
    o.append(f'<rect x="{f(x + 44)}" y="{f(y + 56 + 5 * 21 - 15)}" width="2" height="19" fill="{SEL}"/>')
    return "".join(o)


def scene_merge(cx, cy):
    o = []
    for (dx, dy), col in (((-150, -60), "#d6453d"), ((-150, 90), "#2f6fd6")):
        iso = Iso(cx + dx, cy + dy, 16)
        P = iso.p
        o.append(poly([P(-3, -3), P(3, -3), P(3, 3), P(-3, 3)], "#ffffff", "#ffffff", 1.5, 0.22))
        o.append(box(iso, -2, -1.5, 0, 1.5, 1.5, 1.8, **tinted(col)))
        o.append(box(iso, 0.6, 0.4, 0, 1.6, 1.2, 1.0))
    iso = Iso(cx + 120, cy + 30, 24)
    P = iso.p
    o.append(poly([P(-3.5, -3.5), P(3.5, -3.5), P(3.5, 3.5), P(-3.5, 3.5)], "#ffffff", "#ffffff", 1.5, 0.25))
    o.append(box(iso, -2.6, -2.2, 0, 1.4, 1.4, 1.6, **tinted("#d6453d")))
    o.append(box(iso, -0.2, -2.6, 0, 1.4, 1.0, 0.9))
    o.append(box(iso, -2.4, 0.6, 0, 1.6, 1.2, 0.9))
    o.append(box(iso, 0.8, 0.8, 0, 1.3, 1.3, 1.6, **tinted("#2f6fd6")))
    for sy in (-60, 90):
        a = (cx - 70, cy + sy - 10)
        b = (cx + 20, cy + 20 + (sy / 4))
        o.append(arrow(a, b, "#ffffff", 3.5, 8))
    return "".join(o)


def scene_namaka(cx, cy):
    iso = Iso(cx - 20, cy + 40, 46)
    P = iso.p
    o = [floor_grid(iso, 6), shadow(iso, -1.2, 0.6, 1.0)]
    o.append(box(iso, -2, -0.2, 0, 1.6, 1.6, 1.4))
    # ghost of the object Namaka is about to place
    o.append(box(iso, 0.4, -2.0, 0, 1.6, 1.6, 1.0, top="#ffffff", left="#ffffff", right="#ffffff",
                 outline=SEL, op=0.35))
    o.append(line(P(1.2, -1.2, 1.0), P(1.2, -1.2, 3.0), SEL, 2, 0.8, "5 5"))
    # the marker: a sparkle
    c = P(1.2, -1.2, 3.0)
    r1, r2 = 30, 9
    star = []
    for i in range(8):
        a = -math.pi / 2 + i * math.pi / 4
        r = r1 if i % 2 == 0 else r2
        star.append((c[0] + r * math.cos(a), c[1] + r * math.sin(a)))
    o.append(circle(c, 40, "#ffffff", op=0.18))
    o.append(poly(star, "#ffffff", SEL, 2))
    # chat bubble
    bx, by = c[0] - 250, c[1] - 70
    o.append(f'<rect x="{f(bx)}" y="{f(by)}" width="190" height="44" rx="12" fill="#ffffff" opacity="0.95"/>')
    o.append(poly([(bx + 150, by + 44), (bx + 170, by + 44), (bx + 186, by + 60)], "#ffffff", op=0.95))
    o.append(text(bx + 16, by + 28, "add a bench here", 15, "#1d2a44", 600))
    return "".join(o)


# ---------------------------------------------------------------- strips

STRIPS = {
    "build": [
        (scene_objects, ("Objects", "Find, place, move and rotate"), ("Objetos", "Encontre, coloque, mova e gire")),
        (scene_vehicles, ("Vehicles", "Colours, plates, paintjobs and tuning"), ("Veículos", "Cores, placas, paintjobs e tuning")),
        (scene_elements, ("Server elements", "Pickups, labels, gang zones, icons"), ("Elementos do servidor", "Pickups, labels, gang zones, ícones")),
    ],
    "world": [
        (scene_npcs, ("NPCs", "Bots that walk a path (open.mp)"), ("NPCs", "Bots que seguem um caminho (open.mp)")),
        (scene_materials, ("Materials", "Textures, colours and text"), ("Materiais", "Texturas, cores e textos")),
        (scene_textdraws, ("TextDraws", "Design them on the game screen"), ("TextDraws", "Desenhe-os na tela do jogo")),
    ],
    "tools": [
        (scene_maptools, ("Map tools", "Lines, rings, mirror, align, scatter"), ("Ferramentas de mapa", "Linhas, círculos, espelhar, alinhar")),
        (scene_radar, ("Radar art", "Gang zones that draw on the radar"), ("Radar art", "Gang zones que desenham no radar")),
        (scene_pathnodes, ("Path nodes", "The roads open.mp NPCs walk"), ("Nós de caminho", "As ruas que os NPCs do open.mp andam")),
    ],
    "check": [
        (scene_mapcheck, ("Map Check", "Find what breaks before players do"), ("Verificação do mapa", "Ache o que quebra antes dos jogadores")),
        (scene_playerview, ("Player view", "Draw distances and streamer limits"), ("Visão do jogador", "Distâncias de desenho e limites")),
        (scene_test, ("Test in game", "A private open.mp server, one key"), ("Testar no jogo", "Um servidor open.mp privado, uma tecla")),
    ],
    "files": [
        (scene_code, ("Pawn maps", "Native or streamer code, ready to include"), ("Mapas Pawn", "Código nativo ou streamer, pronto")),
        (scene_merge, ("Other editors", "Open Texture Studio, merge MTA and IPL"), ("Outros editores", "Abra Texture Studio, junte MTA e IPL")),
        (scene_namaka, ("Namaka", "An AI assistant that edits with you"), ("Namaka", "Um assistente de IA que edita com você")),
    ],
}


def strip_svg(cards, lang):
    W = 3 * CARD_W + 2 * GAP + 2 * PAD
    H = CARD_H + 2 * PAD
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">',
         '<defs>'
         '<linearGradient id="sky" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#9cc7e8"/><stop offset="1" stop-color="#5f7fa8"/></linearGradient>'
         '<linearGradient id="tdsky" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#f2a65a"/><stop offset="0.6" stop-color="#c56a7d"/><stop offset="1" stop-color="#4d4f7d"/></linearGradient>'
         '</defs>']
    for i, (fn, en, pt) in enumerate(cards):
        title, sub = en if lang == "en" else pt
        x = PAD + i * (CARD_W + GAP)
        y = PAD
        cid = f"card{i}"
        o.append(f'<clipPath id="{cid}"><rect x="{x}" y="{y}" width="{CARD_W}" height="{CARD_H}" rx="14"/></clipPath>')
        o.append(f'<g clip-path="url(#{cid})">')
        o.append(f'<rect x="{x}" y="{y}" width="{CARD_W}" height="{CARD_H}" fill="url(#sky)"/>')
        o.append(fn(x + CARD_W / 2, y + CARD_H / 2 + 40))
        o.append('</g>')
        o.append(text(x + 24, y + 46, title, 28, "#ffffff", 700))
        o.append(text(x + 24, y + 76, sub, 17, "#eef4fb"))
    o.append('</svg>')
    return "\n".join(o)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name, cards in STRIPS.items():
        for lang, suffix in (("en", ""), ("pt", ".pt-BR")):
            with open(os.path.join(out, f"{name}{suffix}.svg"), "w", encoding="utf-8") as fh:
                fh.write(strip_svg(cards, lang))


if __name__ == "__main__":
    main()
