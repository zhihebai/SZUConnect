#!/usr/bin/env python3
"""WCAG contrast audit of shared/design-tokens.json (both themes).

Text pairs must reach 4.5:1 (this script exits 1 otherwise). Non-text pairs (control borders, ribbon fills)
are reported against 3:1 as advisory lines only. Nothing here measures a rendered screen.
"""
import json, pathlib, re, sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
TOK = json.loads((ROOT / "shared/design-tokens.json").read_text("utf-8"))
COLORS = TOK["color"]
TONES = TOK.get("tones") or TOK.get("tone") or {}


def parse(value):
    v = value.strip()
    if v.startswith("#"):
        h = v[1:]
        if len(h) in (3, 4):
            h = "".join(c * 2 for c in h)
        r, g, b = (int(h[i:i + 2], 16) for i in (0, 2, 4))
        return r, g, b, (int(h[6:8], 16) / 255 if len(h) >= 8 else 1.0)
    m = re.match(r"rgba?\(([^)]+)\)", v)
    parts = [float(x) for x in m.group(1).replace("/", ",").replace(" ", ",").split(",") if x != ""]
    return parts[0], parts[1], parts[2], parts[3] if len(parts) > 3 else 1.0


def blend(fg, bg):
    a = fg[3]
    return tuple(fg[i] * a + bg[i] * (1 - a) for i in range(3))


def lum(rgb):
    def ch(c):
        c /= 255
        return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4
    r, g, b = (ch(x) for x in rgb)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def ratio(theme, fg_name, bg_name):
    bg = parse(COLORS[bg_name][theme])
    bg_rgb = bg[:3]
    fg_rgb = blend(parse(COLORS[fg_name][theme]), bg_rgb)
    a, b = lum(fg_rgb), lum(bg_rgb)
    return (max(a, b) + 0.05) / (min(a, b) + 0.05)


def have(*names):
    return all(n in COLORS for n in names)


text_pairs, advisory = [], []
surfaces = [s for s in ("background", "surface", "surfaceMuted", "sidebar", "accentSurface") if s in COLORS]
for fg in ("textPrimary", "textSecondary", "textTertiary", "accent"):
    for bg in surfaces:
        if have(fg, bg):
            text_pairs.append((fg, bg))
for bg in ("accent", "accentPressed", "accentHover"):
    if have("onAccent", bg):
        text_pairs.append(("onAccent", bg))
if have("onSelection", "selectionFill"):
    text_pairs.append(("onSelection", "selectionFill"))
for tone in ("success", "warning", "danger", "info"):
    fg = (TONES.get(tone) or {}).get("foreground", tone)
    sf = (TONES.get(tone) or {}).get("surface", tone + "Surface")
    for bg in ("surface", "background", sf):
        if have(fg, bg):
            text_pairs.append((fg, bg))
    for txt in ("textPrimary", "textSecondary"):
        if have(txt, sf):
            text_pairs.append((txt, sf))
    if have(fg, "surface"):
        advisory.append((fg, "surface"))
if have("borderStrong", "surface"):
    advisory.append(("borderStrong", "surface"))

failures, checked, lowest = [], 0, (99, "")
for theme in ("light", "dark"):
    for fg, bg in text_pairs:
        r = ratio(theme, fg, bg)
        checked += 1
        if r < lowest[0]:
            lowest = (r, f"{theme}: {fg} on {bg}")
        if r < 4.5:
            failures.append(f"{theme}: {fg} on {bg} = {r:.2f}")
print(f"text pairs checked: {checked} (both themes); below 4.5:1: {len(failures)}; lowest {lowest[0]:.2f} ({lowest[1]})")
for line in failures:
    print("FAIL", line)
for theme in ("light", "dark"):
    for fg, bg in advisory:
        r = ratio(theme, fg, bg)
        print(f"advisory (non-text, 3:1) {theme}: {fg} on {bg} = {r:.2f}{'' if r >= 3 else '  <3:1'}")
sys.exit(1 if failures else 0)
