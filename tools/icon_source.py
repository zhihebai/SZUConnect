#!/usr/bin/env python3
"""Original stroke icons for SZUConnect (24x24 grid, 1.75 stroke, round caps/joins).

Every icon is authored with a few geometric primitives and written to
shared/icons/<name>.svg as ONE <path> whose data only uses absolute M/L/C/Z
commands, so both apps can draw it with a tiny parser (SwiftUI Path and
Direct2D path geometry).  Run:  python3 tools/icon_source.py && python3 tools/generate_shared.py
All shapes here are drawn for this project; no third-party icon set is used.
"""
import math
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "shared" / "icons"
K = 0.5522847498


def f(v):
    s = f"{v:.2f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


class Icon:
    def __init__(self):
        self.parts = []

    def move(self, x, y):
        self.parts.append(f"M{f(x)} {f(y)}")

    def line_to(self, x, y):
        self.parts.append(f"L{f(x)} {f(y)}")

    def curve(self, x1, y1, x2, y2, x, y):
        self.parts.append(f"C{f(x1)} {f(y1)} {f(x2)} {f(y2)} {f(x)} {f(y)}")

    def close(self):
        self.parts.append("Z")

    # primitives -----------------------------------------------------------
    def line(self, x1, y1, x2, y2):
        self.move(x1, y1)
        self.line_to(x2, y2)
        return self

    def poly(self, *pts, closed=False):
        self.move(*pts[0])
        for p in pts[1:]:
            self.line_to(*p)
        if closed:
            self.close()
        return self

    def arc(self, cx, cy, r, a0, a1, start=True):
        """Clockwise (screen coordinates) from a0 to a1 degrees, 0 = +x."""
        if a1 < a0:
            a1 += 360
        steps = max(1, math.ceil((a1 - a0) / 90 - 1e-9))
        span = math.radians((a1 - a0) / steps)
        a = math.radians(a0)
        if start:
            self.move(cx + r * math.cos(a), cy + r * math.sin(a))
        for _ in range(steps):
            b = a + span
            k = 4 / 3 * math.tan(span / 4)
            p1 = (cx + r * (math.cos(a) - k * math.sin(a)), cy + r * (math.sin(a) + k * math.cos(a)))
            p2 = (cx + r * (math.cos(b) + k * math.sin(b)), cy + r * (math.sin(b) - k * math.cos(b)))
            self.curve(*p1, *p2, cx + r * math.cos(b), cy + r * math.sin(b))
            a = b
        return self

    def circle(self, cx, cy, r):
        self.arc(cx, cy, r, 0, 360)
        self.close()
        return self

    def rrect(self, x, y, w, h, r):
        self.move(x + r, y)
        self.line_to(x + w - r, y)
        self.curve(x + w - r + K * r, y, x + w, y + r - K * r, x + w, y + r)
        self.line_to(x + w, y + h - r)
        self.curve(x + w, y + h - r + K * r, x + w - r + K * r, y + h, x + w - r, y + h)
        self.line_to(x + r, y + h)
        self.curve(x + r - K * r, y + h, x, y + h - r + K * r, x, y + h - r)
        self.line_to(x, y + r)
        self.curve(x, y + r - K * r, x + r - K * r, y, x + r, y)
        self.close()
        return self

    def dot(self, cx, cy, r=0.6):
        return self.circle(cx, cy, r)

    def data(self):
        return " ".join(self.parts)


def build():
    I = {}

    def icon(name):
        def register(fn):
            ic = Icon()
            fn(ic)
            I[name] = ic.data()
            return fn
        return register

    @icon("wifi")
    def _(i):
        i.arc(12, 19, 12, 225, 315); i.arc(12, 19, 8, 225, 315); i.arc(12, 19, 4, 225, 315); i.dot(12, 19, 0.9)

    @icon("wifi-off")
    def _(i):
        i.arc(12, 19, 12, 225, 315); i.arc(12, 19, 8, 225, 315); i.dot(12, 19, 0.9); i.line(4, 4, 20, 20)

    @icon("ethernet")
    def _(i):
        i.rrect(4, 7, 16, 12, 2); i.poly((9, 7), (9, 4.5), (15, 4.5), (15, 7)); i.line(8.5, 12, 8.5, 15); i.line(12, 12, 12, 15); i.line(15.5, 12, 15.5, 15)

    @icon("records")
    def _(i):
        i.line(9, 7, 20, 7); i.line(9, 12, 20, 12); i.line(9, 17, 20, 17); i.dot(4.8, 7, 0.9); i.dot(4.8, 12, 0.9); i.dot(4.8, 17, 0.9)

    @icon("settings")
    def _(i):
        i.line(4, 7, 20, 7); i.line(4, 12, 20, 12); i.line(4, 17, 20, 17)
        i.circle(9, 7, 2); i.circle(15, 12, 2); i.circle(8, 17, 2)

    @icon("check-circle")
    def _(i):
        i.circle(12, 12, 9); i.poly((8, 12.3), (10.8, 15), (16.2, 9.3))

    @icon("alert-triangle")
    def _(i):
        i.poly((12, 3.8), (21.2, 19.6), (2.8, 19.6), closed=True); i.line(12, 9.5, 12, 14); i.dot(12, 16.9, 0.8)

    @icon("x-circle")
    def _(i):
        i.circle(12, 12, 9); i.line(9, 9, 15, 15); i.line(15, 9, 9, 15)

    @icon("info")
    def _(i):
        i.circle(12, 12, 9); i.line(12, 11, 12, 16.5); i.dot(12, 7.9, 0.8)

    @icon("clock")
    def _(i):
        i.circle(12, 12, 9); i.poly((12, 7), (12, 12), (15.5, 14))

    @icon("loader")
    def _(i):
        i.arc(12, 12, 8.5, 300, 210)

    @icon("pause-circle")
    def _(i):
        i.circle(12, 12, 9); i.line(10, 9, 10, 15); i.line(14, 9, 14, 15)

    @icon("moon")
    def _(i):
        i.arc(12, 12, 8.5, 330, 240)
        i.curve(8.6, 10.6, 14.6, 14.4, 19.36, 7.75)
        i.close()

    @icon("shield")
    def _(i):
        i.move(12, 3); i.line_to(19.5, 6); i.line_to(19.5, 11.5)
        i.curve(19.5, 16, 16.3, 19.3, 12, 21); i.curve(7.7, 19.3, 4.5, 16, 4.5, 11.5); i.line_to(4.5, 6); i.close()

    @icon("shield-check")
    def _(i):
        i.move(12, 3); i.line_to(19.5, 6); i.line_to(19.5, 11.5)
        i.curve(19.5, 16, 16.3, 19.3, 12, 21); i.curve(7.7, 19.3, 4.5, 16, 4.5, 11.5); i.line_to(4.5, 6); i.close()
        i.poly((8.6, 12), (11, 14.4), (15.6, 9.6))

    @icon("user")
    def _(i):
        i.circle(12, 8, 4); i.move(4.5, 20.5); i.curve(4.5, 16.4, 7.8, 13.8, 12, 13.8); i.curve(16.2, 13.8, 19.5, 16.4, 19.5, 20.5)

    @icon("globe")
    def _(i):
        i.circle(12, 12, 9); i.line(3, 12, 21, 12)
        i.move(12, 3); i.curve(14.8, 5.6, 15.8, 8.8, 15.8, 12); i.curve(15.8, 15.2, 14.8, 18.4, 12, 21)
        i.curve(9.2, 18.4, 8.2, 15.2, 8.2, 12); i.curve(8.2, 8.8, 9.2, 5.6, 12, 3); i.close()

    @icon("route")
    def _(i):
        i.circle(6, 18, 2); i.circle(18, 6, 2)
        i.move(6, 16); i.line_to(6, 13); i.curve(6, 10.5, 7.5, 9, 10, 9); i.line_to(14, 9)
        i.curve(16.5, 9, 18, 10.5, 18, 13); i.line_to(18, 20); i.line(18, 8, 18, 9)

    @icon("tunnel")
    def _(i):
        i.move(4, 20); i.line_to(4, 12); i.arc(12, 12, 8, 180, 360, start=False); i.line_to(20, 20)
        i.move(8.5, 20); i.line_to(8.5, 13); i.arc(12, 13, 3.5, 180, 360, start=False); i.line_to(15.5, 20); i.line(2.5, 20, 21.5, 20)

    @icon("layers")
    def _(i):
        i.poly((12, 4), (20.5, 8.5), (12, 13), (3.5, 8.5), closed=True); i.poly((3.5, 12.5), (12, 17), (20.5, 12.5))
        i.poly((3.5, 16.2), (12, 20.7), (20.5, 16.2))

    @icon("refresh")
    def _(i):
        i.arc(12, 12, 8, 320, 270); i.poly((12.6, 1.6), (12, 4), (14.4, 5.2))

    @icon("log-in")
    def _(i):
        i.poly((14, 4), (19, 4), (19, 20), (14, 20)); i.line(4, 12, 14, 12); i.poly((10.5, 8.5), (14, 12), (10.5, 15.5))

    @icon("x")
    def _(i):
        i.line(6, 6, 18, 18); i.line(18, 6, 6, 18)

    @icon("check")
    def _(i):
        i.poly((5, 12.5), (9.5, 17), (19, 7.5))

    @icon("play")
    def _(i):
        i.poly((8, 5), (19, 12), (8, 19), closed=True)

    @icon("pause")
    def _(i):
        i.line(9, 5, 9, 19); i.line(15, 5, 15, 19)

    @icon("key")
    def _(i):
        i.circle(8, 15, 4); i.line(10.9, 12.1, 20, 3); i.line(16.5, 6.5, 19, 9); i.line(14, 9, 16, 11)

    @icon("chevron-right")
    def _(i):
        i.poly((9, 5.5), (15.5, 12), (9, 18.5))

    @icon("chevron-left")
    def _(i):
        i.poly((15, 5.5), (8.5, 12), (15, 18.5))

    @icon("chevron-down")
    def _(i):
        i.poly((5.5, 9), (12, 15.5), (18.5, 9))

    @icon("copy")
    def _(i):
        i.rrect(8.5, 8.5, 11.5, 11.5, 2); i.poly((15.5, 8.5), (15.5, 5.5), (15.5, 5.5)); i.move(15.5, 5.5)
        i.curve(15.5, 4.7, 14.8, 4, 14, 4); i.line_to(5.5, 4); i.curve(4.7, 4, 4, 4.7, 4, 5.5); i.line_to(4, 14)
        i.curve(4, 14.8, 4.7, 15.5, 5.5, 15.5); i.line_to(8.5, 15.5)

    @icon("external-link")
    def _(i):
        i.poly((18, 13.5), (18, 19), (5, 19), (5, 6), (10.5, 6)); i.line(11, 13, 20, 4); i.poly((14.5, 4), (20, 4), (20, 9.5))

    @icon("help")
    def _(i):
        i.circle(12, 12, 9); i.move(9.4, 9.4); i.curve(9.4, 7.9, 10.6, 6.9, 12, 6.9); i.curve(13.5, 6.9, 14.7, 7.9, 14.7, 9.3)
        i.curve(14.7, 11.2, 12, 11.6, 12, 13.6); i.dot(12, 16.8, 0.8)

    @icon("heart")
    def _(i):
        i.move(12, 20); i.curve(6, 16, 3, 12.5, 3, 9); i.curve(3, 6.2, 5.2, 4.2, 7.8, 4.2); i.curve(9.6, 4.2, 11.1, 5.2, 12, 6.8)
        i.curve(12.9, 5.2, 14.4, 4.2, 16.2, 4.2); i.curve(18.8, 4.2, 21, 6.2, 21, 9); i.curve(21, 12.5, 18, 16, 12, 20); i.close()

    @icon("search")
    def _(i):
        i.circle(10.5, 10.5, 6.5); i.line(15.3, 15.3, 20.5, 20.5)

    @icon("filter")
    def _(i):
        i.poly((3.5, 5), (20.5, 5), (14, 12.5), (14, 19), (10, 20.5), (10, 12.5), closed=True)

    @icon("download")
    def _(i):
        i.line(12, 3.5, 12, 15); i.poly((7, 10), (12, 15), (17, 10)); i.poly((4, 16), (4, 20), (20, 20), (20, 16))

    @icon("sparkles")
    def _(i):
        i.move(10, 3); i.curve(10.8, 7.8, 12.2, 9.2, 17, 10); i.curve(12.2, 10.8, 10.8, 12.2, 10, 17)
        i.curve(9.2, 12.2, 7.8, 10.8, 3, 10); i.curve(7.8, 9.2, 9.2, 7.8, 10, 3); i.close()
        i.line(18, 15, 18, 21); i.line(15, 18, 21, 18)

    @icon("building")
    def _(i):
        i.poly((4, 21), (4, 4.5), (14, 3), (14, 21)); i.poly((14, 9), (20, 10), (20, 21)); i.line(2.5, 21, 21.5, 21)
        i.line(7, 8, 11, 8); i.line(7, 12, 11, 12); i.line(7, 16, 11, 16); i.line(16.5, 14, 17.5, 14)

    @icon("home")
    def _(i):
        i.poly((3.5, 11), (12, 3.8), (20.5, 11)); i.poly((6, 9.2), (6, 20), (18, 20), (18, 9.2)); i.poly((10, 20), (10, 14.5), (14, 14.5), (14, 20))

    @icon("power")
    def _(i):
        i.arc(12, 13, 7.5, 300, 240); i.line(12, 3, 12, 11)

    @icon("eye")
    def _(i):
        i.move(2.5, 12); i.curve(5, 7.6, 8.3, 5.5, 12, 5.5); i.curve(15.7, 5.5, 19, 7.6, 21.5, 12)
        i.curve(19, 16.4, 15.7, 18.5, 12, 18.5); i.curve(8.3, 18.5, 5, 16.4, 2.5, 12); i.close(); i.circle(12, 12, 3)

    @icon("eye-off")
    def _(i):
        i.move(2.5, 12); i.curve(5, 7.6, 8.3, 5.5, 12, 5.5); i.curve(15.7, 5.5, 19, 7.6, 21.5, 12)
        i.curve(19, 16.4, 15.7, 18.5, 12, 18.5); i.curve(8.3, 18.5, 5, 16.4, 2.5, 12); i.close(); i.circle(12, 12, 3)
        i.line(4, 20, 20, 4)

    @icon("bookmark")
    def _(i):
        i.poly((6.5, 3.5), (17.5, 3.5), (17.5, 20.5), (12, 16.2), (6.5, 20.5), closed=True)

    @icon("pointer")
    def _(i):
        i.poly((5, 3.5), (18.5, 12.2), (12.4, 13.4), (15.8, 20), (13.2, 21), (9.9, 14.5), (5.6, 18.6), closed=True)

    @icon("lock")
    def _(i):
        i.rrect(5, 10.5, 14, 10, 2); i.move(8, 10.5); i.line_to(8, 7.5); i.arc(12, 7.5, 4, 180, 360, start=False); i.line_to(16, 10.5)
        i.line(12, 14.5, 12, 16.5)

    @icon("book")
    def _(i):
        i.move(12, 6.5); i.curve(9.8, 5, 6.8, 4.5, 3.5, 5); i.line_to(3.5, 18.5); i.curve(6.8, 18, 9.8, 18.5, 12, 20)
        i.curve(14.2, 18.5, 17.2, 18, 20.5, 18.5); i.line_to(20.5, 5); i.curve(17.2, 4.5, 14.2, 5, 12, 6.5); i.close(); i.line(12, 6.5, 12, 20)

    @icon("signal")
    def _(i):
        i.circle(12, 12, 2); i.arc(12, 12, 5.5, 135, 225); i.arc(12, 12, 5.5, 315, 45); i.arc(12, 12, 9, 135, 225); i.arc(12, 12, 9, 315, 45)

    @icon("plus")
    def _(i):
        i.line(5, 12, 19, 12); i.line(12, 5, 12, 19)

    @icon("minus")
    def _(i):
        i.line(6, 12, 18, 12)
    @icon("window-maximize")
    def _(i):
        i.poly((5, 5), (19, 5), (19, 19), (5, 19), closed=True)
    @icon("window-restore")
    def _(i):
        i.poly((5, 8), (16, 8), (16, 19), (5, 19), closed=True)
        i.poly((8, 8), (8, 5), (19, 5), (19, 16), (16, 16))
    return I


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    icons = build()
    for old in OUT.glob("*.svg"):
        if old.stem not in icons:
            old.unlink()
    for name, data in sorted(icons.items()):
        svg = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="24" height="24" fill="none" '
               'stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round">\n'
               f'  <!-- SZUConnect original icon "{name}" generated by tools/icon_source.py -->\n  <path d="{data}"/>\n</svg>\n')
        (OUT / f"{name}.svg").write_text(svg, "utf-8")
    print(f"wrote {len(icons)} icons to {OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
