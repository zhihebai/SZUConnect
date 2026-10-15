#!/usr/bin/env python3
"""Reproducible brand, tray and menu-bar icons for SZUConnect (original artwork).

    python3 tools/make_icons.py          # regenerate every raster from the SVG masters

Masters (written here from the geometry below, then rasterised):
  shared/icon/app-icon.svg          1024 master (macOS style, transparent margin)
  shared/icon/app-icon-small.svg    heavier strokes for 16–32 px
  shared/icon/app-icon-full.svg     full-bleed opaque square (iOS 1024, Windows)
  shared/icon/tray-<state>-<theme>.svg, shared/icon/menubar-<state>.svg
Outputs:
  shared/icon/app-icon-1024.png, shared/icon/SZUConnect.icns
  macos/SZUConnect/Assets.xcassets/AppIcon.appiconset/*.png
  macos/SZUConnect/Assets.xcassets/MenuBar-<state>.imageset (template images)
  macos/SZUConnect/Assets.xcassets/AppIconDisplay.imageset (in-app display of the app icon)
  windows/SZUConnect/resources/icons/app.ico, tray-<state>-<light|dark>.ico
Rasteriser: sharp/librsvg through node (path via SZU_SHARP or the default npm-global path).
ICO/ICNS containers are written by this script (no extra tools). Deterministic output.
"""
import io
import json
import math
import os
import pathlib
import struct
import subprocess
import tempfile

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[1]
ICON = ROOT / "shared" / "icon"
MAC_ASSETS = ROOT / "macos/SZUConnect/Assets.xcassets"
WIN_ICONS = ROOT / "windows/SZUConnect/resources/icons"
SHARP = os.environ.get("SZU_SHARP", "sharp")
STATES = ["idle", "online", "attention", "paused", "busy"]
TILE = "#18397A"          # deep ink blue tile
DOT = "#8DB4FF"           # the signal source, one step lighter than the accent
BADGE = {"online": "#2EA043", "attention": "#D9A02B", "paused": "#D64545", "busy": "#4C82F0"}


def polar(cx, cy, r, deg):
    a = math.radians(deg)
    return cx + r * math.cos(a), cy + r * math.sin(a)


def connection_mark(color, small=False, dot_color=None):
    """Draw two Wi-Fi arcs above a rounded cable cradle, centred on the 1024 canvas."""
    w = 72 if small else 58                        # stroke width; heavier for 16-32 px
    cx = 512
    r_out, r_in = (262, 168) if not small else (270, 172)
    half = 47                                      # each arc spans +-47 degrees around straight up
    dot_r = 44 if small else 38
    cradle_w = 372 if not small else 384           # outer width of the U
    cradle_h = 86
    gap = 92 if small else 96                      # empty band between the source dot and the cradle
    # vertical layout measured from the arc apex, then shifted so the bounding box is centred
    apex = r_out                                   # apex distance above the dot centre
    top = -(apex + w / 2)                          # top of the outer arc stroke, relative to dot centre
    cradle_top = dot_r + gap
    bottom = cradle_top + cradle_h + w / 2
    cy = 512 - (top + bottom) / 2                  # dot centre y
    def arc(r):
        a = math.radians(half)
        x = r * math.sin(a); y = r * math.cos(a)
        return f'M{cx - x:.2f} {cy - y:.2f} A{r} {r} 0 0 1 {cx + x:.2f} {cy - y:.2f}'
    x0, x1 = cx - cradle_w / 2 + w / 2, cx + cradle_w / 2 - w / 2
    yt, yb = cy + cradle_top, cy + cradle_top + cradle_h
    rad = 34
    cradle = (f'M{x0:.2f} {yt:.2f} V{yb - rad:.2f} Q{x0:.2f} {yb:.2f} {x0 + rad:.2f} {yb:.2f} '
              f'H{x1 - rad:.2f} Q{x1:.2f} {yb:.2f} {x1:.2f} {yb - rad:.2f} V{yt:.2f}')
    dot = dot_color or color
    return (f'<g fill="none" stroke="{color}" stroke-width="{w}" stroke-linecap="round" stroke-linejoin="round">'
            f'<path d="{arc(r_out)}"/><path d="{arc(r_in)}"/><path d="{cradle}"/></g>'
            f'<circle cx="{cx}" cy="{cy:.2f}" r="{dot_r}" fill="{dot}"/>')


def app_svg(variant):
    color = TILE
    if variant == "full":
        body = f'<rect width="1024" height="1024" fill="{color}"/>'
    else:
        inset, radius = (100, 185) if variant == "master" else (64, 200)
        size = 1024 - 2 * inset
        body = f'<rect x="{inset}" y="{inset}" width="{size}" height="{size}" rx="{radius}" fill="{color}"/>'
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024" width="1024" height="1024">'
            '<!-- SZUConnect original artwork: Wi-Fi arcs over a cable cradle on an ink-blue tile. -->'
            + body + connection_mark("#FFFFFF", variant == "small", DOT) + '</svg>')


def glyph(state, color, size=32, template=False):
    """Status glyph for the Windows tray (colour badge) and the macOS menu bar (template image).

    It is the same Wi-Fi fan as the app icon; the state is carried by a badge shape, never by colour
    alone: none = online, ! = needs attention, pause bars = paused, open ring = busy, dimmed fan = idle.
    In the template variant the fan is cut away around the badge so it stays legible on any menu bar."""
    s = float(size)
    w = s * (0.100 if template else 0.105)
    cx, cy = s * 0.5, s * 0.70
    r_out, r_in, dot = s * 0.40, s * 0.245, s * 0.075
    half = math.radians(47)

    def arc(r):
        return (f'M{cx - r * math.sin(half):.2f} {cy - r * math.cos(half):.2f} '
                f'A{r:.2f} {r:.2f} 0 0 1 {cx + r * math.sin(half):.2f} {cy - r * math.cos(half):.2f}')

    has_badge = state != "idle" and not (template and state == "online") and not (state == "online" and template)
    if state == "online" and not template:
        has_badge = True
    rb = s * (0.205 if template else 0.22)
    bx, by = s * 0.775, s * 0.775
    opacity = 0.55 if state == "idle" else 1
    defs, group_open, group_close = "", "", ""
    if template and has_badge:
        defs = (f'<defs><mask id="cut" maskUnits="userSpaceOnUse" x="0" y="0" width="{s}" height="{s}">'
                f'<rect width="{s}" height="{s}" fill="#fff"/><circle cx="{bx:.2f}" cy="{by:.2f}" r="{rb + s * 0.075:.2f}" fill="#000"/></mask></defs>')
        group_open, group_close = '<g mask="url(#cut)">', '</g>'
    fan = (f'{group_open}<g fill="none" stroke="{color}" stroke-width="{w:.2f}" stroke-linecap="round" opacity="{opacity}">'
           f'<path d="{arc(r_out)}"/><path d="{arc(r_in)}"/></g>'
           f'<circle cx="{cx:.2f}" cy="{cy:.2f}" r="{dot:.2f}" fill="{color}" opacity="{opacity}"/>{group_close}')
    badge = ""
    if has_badge:
        sw = s * 0.085 if template else rb * 0.30
        ink = color if template else "#FFFFFF"
        if template:
            badge += f'<circle cx="{bx:.2f}" cy="{by:.2f}" r="{rb:.2f}" fill="none" stroke="{color}" stroke-width="{s * 0.07:.2f}"/>'
        else:
            badge += (f'<circle cx="{bx:.2f}" cy="{by:.2f}" r="{rb:.2f}" fill="{BADGE[state]}" '
                      f'stroke="{"#202020" if color == "#FFFFFF" else "#F3F3F3"}" stroke-width="{s * 0.05:.2f}"/>')
        k = rb * (0.62 if template else 0.58)
        common = f'stroke="{ink}" stroke-width="{sw:.2f}" stroke-linecap="round" stroke-linejoin="round" fill="none"'
        if state == "online":
            badge += f'<path d="M{bx - k * 0.85:.2f} {by + k * 0.05:.2f} L{bx - k * 0.2:.2f} {by + k * 0.62:.2f} L{bx + k * 0.9:.2f} {by - k * 0.55:.2f}" {common}/>'
        elif state == "attention":
            badge += (f'<path d="M{bx:.2f} {by - k * 0.9:.2f} L{bx:.2f} {by + k * 0.15:.2f}" {common}/>'
                      f'<circle cx="{bx:.2f}" cy="{by + k * 0.78:.2f}" r="{sw * 0.62:.2f}" fill="{ink}"/>')
        elif state == "paused":
            for dx in (-0.42, 0.42):
                badge += f'<path d="M{bx + k * dx:.2f} {by - k * 0.72:.2f} L{bx + k * dx:.2f} {by + k * 0.72:.2f}" {common}/>'
        elif state == "busy":
            rr = k * 0.95
            badge += (f'<path d="M{bx:.2f} {by - rr:.2f} A{rr:.2f} {rr:.2f} 0 1 1 {bx - rr * 0.95:.2f} {by + rr * 0.32:.2f}" {common}/>')
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {size} {size}" width="{size}" height="{size}">'
            f'<!-- SZUConnect status glyph ({state}), original artwork -->' + defs + fan + badge + '</svg>')


def render(jobs):
    """jobs: list of (svg_text, size) -> list of PIL images, rendered by librsvg at the exact size."""
    with tempfile.TemporaryDirectory() as tmp:
        spec = []
        for index, (svg, size) in enumerate(jobs):
            src = pathlib.Path(tmp) / f"{index}.svg"
            src.write_text(svg, "utf-8")
            spec.append({"src": str(src), "out": str(pathlib.Path(tmp) / f"{index}.png"), "size": size})
        script = ("const sharp=require(process.argv[1]);const jobs=JSON.parse(require('fs').readFileSync(process.argv[2],'utf8'));"
                  "(async()=>{for(const j of jobs){const svg=require('fs').readFileSync(j.src);"
                  "const m=/viewBox=\"0 0 ([0-9.]+)/.exec(svg.toString());const vb=m?parseFloat(m[1]):j.size;"
                  "await sharp(svg,{density:72*j.size/vb}).resize(j.size,j.size).png().toFile(j.out);}})()"
                  ".catch(e=>{console.error(e);process.exit(1)});")
        spec_path = pathlib.Path(tmp) / "jobs.json"
        spec_path.write_text(json.dumps(spec))
        subprocess.run(["node", "-e", script, SHARP, str(spec_path)], check=True)
        return [Image.open(item["out"]).convert("RGBA").copy() for item in spec]


def png_bytes(image):
    buffer = io.BytesIO()
    image.save(buffer, format="PNG", optimize=True)
    return buffer.getvalue()


def dib(image):
    w, h = image.size
    rows = []
    pixels = image.tobytes("raw", "BGRA")
    for y in range(h - 1, -1, -1):
        rows.append(pixels[y * w * 4:(y + 1) * w * 4])
    mask_row = ((w + 31) // 32) * 4
    header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, len(pixels) + mask_row * h, 0, 0, 0, 0)
    return header + b"".join(rows) + b"\x00" * (mask_row * h)


def write_ico(path, images):
    blobs = [png_bytes(img) if img.size[0] >= 256 else dib(img) for img in images]
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for img, blob in zip(images, blobs):
        w = img.size[0]
        out += struct.pack("<BBBBHHII", 0 if w >= 256 else w, 0 if w >= 256 else w, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    path.write_bytes(out + b"".join(blobs))


def write_icns(path, by_size):
    types = [("icp4", 16), ("icp5", 32), ("icp6", 64), ("ic07", 128), ("ic08", 256), ("ic09", 512), ("ic10", 1024),
             ("ic11", 32), ("ic12", 64), ("ic13", 256), ("ic14", 512)]
    body = b""
    for kind, size in types:
        data = png_bytes(by_size[size])
        body += kind.encode("ascii") + struct.pack(">I", len(data) + 8) + data
    path.write_bytes(b"icns" + struct.pack(">I", len(body) + 8) + body)


def main():
    ICON.mkdir(parents=True, exist_ok=True)
    WIN_ICONS.mkdir(parents=True, exist_ok=True)
    masters = {"app-icon.svg": app_svg("master"), "app-icon-small.svg": app_svg("small"), "app-icon-full.svg": app_svg("full")}
    for state in STATES:
        for theme, color in (("light", "#1B1B1B"), ("dark", "#FFFFFF")):
            masters[f"tray-{state}-{theme}.svg"] = glyph(state, color)
        masters[f"menubar-{state}.svg"] = glyph(state, "#000000", size=36, template=True)
    for name, svg in masters.items():
        (ICON / name).write_text(svg + "\n", "utf-8")

    mac_sizes = [16, 32, 64, 128, 256, 512, 1024]
    jobs = [(masters["app-icon-small.svg"] if s <= 32 else masters["app-icon.svg"], s) for s in mac_sizes]
    win_sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    jobs += [(masters["app-icon-small.svg"], s) for s in win_sizes]  # Windows favours the bolder variant at every size
    jobs += [(masters["app-icon-full.svg"], 1024)]
    tray_sizes = [16, 20, 24, 32, 40, 48, 64]
    tray_keys = [(state, theme) for state in STATES for theme in ("light", "dark")]
    for state, theme in tray_keys:
        jobs += [(masters[f"tray-{state}-{theme}.svg"], s) for s in tray_sizes]
    for state in STATES:
        jobs += [(masters[f"menubar-{state}.svg"], 18), (masters[f"menubar-{state}.svg"], 36)]
    display_sizes = (64, 128, 192)  # AppIconDisplay: the icon shown inside the app (About sheet)
    jobs += [(masters["app-icon.svg"], s) for s in display_sizes]
    images = render(jobs)
    cursor = 0
    mac = dict(zip(mac_sizes, images[cursor:cursor + len(mac_sizes)])); cursor += len(mac_sizes)
    win = images[cursor:cursor + len(win_sizes)]; cursor += len(win_sizes)
    ios = images[cursor]; cursor += 1

    mac[1024].save(ICON / "app-icon-1024.png", optimize=True)
    write_icns(ICON / "SZUConnect.icns", mac)
    appicon = MAC_ASSETS / "AppIcon.appiconset"
    names = {"mac-16@1x.png": 16, "mac-16@2x.png": 32, "mac-32@1x.png": 32, "mac-32@2x.png": 64, "mac-128@1x.png": 128,
             "mac-128@2x.png": 256, "mac-256@1x.png": 256, "mac-256@2x.png": 512, "mac-512@1x.png": 512, "mac-512@2x.png": 1024}
    for name, size in names.items():
        mac[size].save(appicon / name, optimize=True)
    ios.convert("RGB").save(appicon / "ios-1024.png", optimize=True)  # iOS icons must be opaque
    write_ico(WIN_ICONS / "app.ico", win)
    for state, theme in tray_keys:
        write_ico(WIN_ICONS / f"tray-{state}-{theme}.ico", images[cursor:cursor + len(tray_sizes)])
        cursor += len(tray_sizes)
    for state in STATES:
        folder = MAC_ASSETS / f"MenuBar-{state}.imageset"
        folder.mkdir(exist_ok=True)
        images[cursor].save(folder / f"menubar-{state}.png", optimize=True)
        images[cursor + 1].save(folder / f"menubar-{state}@2x.png", optimize=True)
        cursor += 2
        (folder / "Contents.json").write_text(json.dumps({
            "images": [{"idiom": "universal", "filename": f"menubar-{state}.png", "scale": "1x"},
                       {"idiom": "universal", "filename": f"menubar-{state}@2x.png", "scale": "2x"}],
            "info": {"author": "xcode", "version": 1},
            "properties": {"template-rendering-intent": "template"}}, indent=2) + "\n")
    display = MAC_ASSETS / "AppIconDisplay.imageset"
    display.mkdir(exist_ok=True)
    entries = []
    for scale, size in zip(("1x", "2x", "3x"), display_sizes):
        name = "app-icon-display.png" if scale == "1x" else f"app-icon-display@{scale}.png"
        images[cursor].save(display / name, optimize=True)
        cursor += 1
        entries.append({"idiom": "universal", "filename": name, "scale": scale})
    (display / "Contents.json").write_text(json.dumps({"images": entries, "info": {"author": "xcode", "version": 1}}, indent=2) + "\n")
    print("icons written:", len(images), "rasters")


if __name__ == "__main__":
    main()
