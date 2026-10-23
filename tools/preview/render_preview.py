#!/usr/bin/env python3
"""Render an HTML preview from shared design tokens, copy and reference presenters.

The preview checks content hierarchy, wording and palette in Chromium. It does not
validate native controls, fonts, focus, keyboard interaction, scrolling or DPI scaling.

    python3 tools/preview/render_preview.py --output-dir DIR
"""
import argparse, base64, html, json, os, pathlib, re, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/reference"))
import memory_reference, status_reference  # noqa: E402

OUT = ROOT / "preview-output"
TOK = json.loads((ROOT / "shared/design-tokens.json").read_text("utf-8"))
COPY = json.loads((ROOT / "shared/copy.zh-CN.json").read_text("utf-8"))


def flat(d, pre=""):
    for k, v in d.items():
        if isinstance(v, dict):
            yield from flat(v, pre + k + ".")
        else:
            yield pre + k, v


TEXT = dict(flat(COPY))


def t(key, **kw):
    text = str(TEXT.get(key, key))
    for k, v in kw.items():
        text = text.replace("{" + k + "}", str(v))
    return text


TONES = TOK.get("tones") or TOK.get("tone") or {}
DEFAULT_ICON = {"success": "check-circle", "warning": "alert-triangle", "danger": "x-circle", "info": "clock", "busy": "loader", "neutral": "info"}
ICONS = {}
for p in (ROOT / "shared/icons").glob("*.svg"):
    m = re.search(r'\sd="([^"]+)"', p.read_text("utf-8"))
    if m:
        ICONS[p.stem] = m.group(1)


def icon(name, size=18, color="var(--textSecondary)"):
    d = ICONS.get(name) or ICONS.get(re.sub(r"([A-Z])", lambda m: "-" + m.group(1).lower(), name or ""))
    if not d:
        return ""
    return (f'<svg width="{size}" height="{size}" viewBox="0 0 24 24" fill="none" stroke="{color}" stroke-width="1.75" '
            f'stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="{d}"/></svg>')


def tone_icon(tone):
    return (TONES.get(tone) or {}).get("icon") or DEFAULT_ICON.get(tone, "info")


def tone_fg(tone):
    return f"var(--{(TONES.get(tone) or {}).get('foreground', tone)})"


def tone_sf(tone):
    return f"var(--{(TONES.get(tone) or {}).get('surface', tone + 'Surface')})"


def css():
    def block(theme):
        return ";".join(f"--{name}:{pair[theme]}" for name, pair in TOK["color"].items())
    return f"""
:root{{{block('light')}}} [data-theme=dark]{{{block('dark')}}}
*{{box-sizing:border-box}} body{{margin:0;background:var(--background);color:var(--textPrimary);
font:14px/1.45 "Noto Sans CJK SC","PingFang SC","Microsoft YaHei",system-ui,sans-serif}}
.window{{position:relative;display:flex;flex-direction:column;overflow:hidden}}
.bar{{height:56px;flex:none;display:flex;align-items:center;gap:12px;padding:0 20px;background:var(--sidebar);border-bottom:1px solid var(--border)}}
.brand{{display:flex;align-items:center;gap:8px;font-weight:600;font-size:15px}} .brand img{{width:28px;height:28px}}
.sp{{flex:1}} .nav{{display:flex;gap:2px;padding:3px;background:var(--surfaceMuted);border-radius:11px}}
.nav span{{display:flex;align-items:center;gap:6px;min-width:72px;height:34px;justify-content:center;border-radius:8px;color:var(--textSecondary);font-size:13px;padding:0 10px}}
.nav span.on{{background:var(--surface);color:var(--accent);font-weight:600}}
.ibtn{{width:32px;height:32px;display:inline-flex;align-items:center;justify-content:center;border-radius:8px}}
.page{{flex:1;overflow:hidden;background:var(--background)}} .col{{max-width:760px;margin:0 auto;padding:20px 24px;display:flex;flex-direction:column;gap:16px}}
.card{{background:var(--surface);border:1px solid var(--border);border-radius:14px;padding:16px 18px}} .hero{{padding:20px}}
.row{{display:flex;align-items:center;gap:12px}} .grow{{flex:1;min-width:0}}
.disc{{width:56px;height:56px;border-radius:50%;display:flex;align-items:center;justify-content:center;flex:none}}
h1{{margin:0;font-size:{TOK['font']['hero']['size']}px;font-weight:600;line-height:1.25}} .sub{{color:var(--textSecondary);font-size:13px;margin-top:2px}}
.h{{font-size:15px;font-weight:600}} .cap{{font-size:12px;color:var(--textSecondary)}} .cap3{{font-size:12px;color:var(--textTertiary)}}
.btns{{display:flex;gap:10px;flex-wrap:wrap;margin:14px 0}}
.btn{{display:inline-flex;align-items:center;gap:8px;min-height:40px;padding:0 16px;border-radius:10px;font-weight:600;font-size:14px;border:1px solid var(--borderStrong);background:var(--surface);color:var(--textPrimary)}}
.btn.p{{background:var(--accent);border-color:var(--accent);color:var(--onAccent)}} .btn.q{{border-color:transparent;background:transparent;color:var(--accent)}}
hr{{border:0;border-top:1px solid var(--border);margin:2px 0 12px}} .flow{{display:flex;flex-wrap:wrap;gap:8px}}
.chip{{display:inline-flex;align-items:center;gap:6px;height:28px;padding:0 10px;border-radius:14px;font-size:12px;max-width:300px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}}
.chip.m{{background:var(--surfaceMuted)}} .chip b{{font-weight:600}} .chip i{{font-style:normal;color:var(--textSecondary)}}
.foot{{display:flex;align-items:center;gap:8px;margin-top:12px}} .seg{{display:inline-flex;padding:2px;background:var(--surfaceMuted);border:1px solid var(--border);border-radius:8px}}
.seg span{{padding:4px 12px;border-radius:6px;font-size:12px;color:var(--textSecondary)}} .seg span.on{{background:var(--surface);color:var(--textPrimary);font-weight:600;box-shadow:0 0 0 1px var(--border)}}
.seg.wide{{display:flex}} .seg.wide span{{flex:1;text-align:center;padding:6px 8px;font-size:13px}}
.banner{{display:flex;gap:10px;padding:12px 14px;border-radius:10px;margin-top:12px}} .banner .t{{font-weight:600}} .banner .d{{font-size:13px;color:var(--textSecondary)}}
.field{{flex:1;min-width:0;height:34px;border:1px solid var(--borderStrong);border-radius:8px;padding:0 10px;display:flex;align-items:center;color:var(--textSecondary);background:var(--surface);font-size:13px}}
.line{{display:flex;align-items:center;gap:8px;margin-top:10px}} .ck{{width:16px;height:16px;border-radius:4px;border:1.5px solid var(--borderStrong);flex:none;display:inline-flex;align-items:center;justify-content:center}}
.ck.on{{background:var(--accent);border-color:var(--accent)}} .ribbon{{display:flex;gap:3px;margin:10px 0 6px}} .ribbon div{{flex:1;height:22px;border-radius:3px}}
.tag{{position:absolute;left:10px;bottom:8px;font-size:11px;padding:2px 8px;border-radius:10px;background:var(--surfaceMuted);color:var(--textTertiary);border:1px solid var(--border)}}
.detail{{background:var(--surfaceMuted);border-radius:10px;padding:12px;margin-top:8px}} .detail .kv{{display:flex;gap:8px;font-size:13px;margin-top:4px}} .detail .kv span:first-child{{width:72px;color:var(--textSecondary)}}
"""


def chip(text, tone, name=None):
    body = f"<b>{html.escape(name)}</b><i>{html.escape(text)}</i>" if name else html.escape(text)
    return (f'<span class="chip m">{icon(tone_icon(tone), 14, tone_fg(tone))}{body}</span>' if name else
            f'<span class="chip" style="background:{tone_sf(tone)}">{icon(tone_icon(tone), 14, tone_fg(tone))}{body}</span>')


def button(label, kind="", ic=None):
    color = "var(--onAccent)" if kind == "p" else "var(--accent)" if kind == "q" else "var(--textPrimary)"
    return f'<span class="btn {kind}">{icon(ic, 18, color) if ic else ""}{html.escape(label)}</span>'


def card(title, ic, inner, badge=None, info=True):
    b = chip(badge[0], badge[1]) if badge else ""
    i = f'<span class="ibtn">{icon("info", 16)}</span>' if info else ""
    return (f'<div class="card"><div class="row"><span>{icon(ic, 18, "var(--accent)")}</span><span class="h">{html.escape(title)}</span>{i}'
            f'<span class="sp"></span>{b}</div><div style="margin-top:12px">{inner}</div></div>')


def seg(options, on, wide=False):
    return f'<div class="seg{" wide" if wide else ""}">' + "".join(f'<span class="{"on" if i == on else ""}">{html.escape(o)}</span>' for i, o in enumerate(options)) + "</div>"


ACTION_ICON = {"check": "refresh", "login": "log-in", "forceLogin": "log-in", "cancel": "x", "resume": "play", "trust": "shield-check", "account": "user", "network": "globe"}


def action_label(token, can_remember):
    if token == "trust" and can_remember:
        return t("memory.remember")
    return t("action." + token)


STATES = {
    "online-ready": dict(
        title="在线，自动恢复已就绪（记住的网络）",
        status=dict(status="online", diagnosis="online", probeMode="quick", passedCount=1, mode="recover", readOnly=False, hasDraft=True, autoLogin="ready", hold=None, offerForcedLogin=False),
        ready=dict(mode="recover", readOnly=False, hasSaved=True, unsavedChanges=False, autoLogin="ready", hold=None, sessionTrusted=False, remembered=True, memoryConflict=False),
        checks=[("Apple", "通过 24 ms", "success"), ("Microsoft", t("dashboard.notWaited"), "neutral"), ("www.baidu.com", t("dashboard.notWaited"), "neutral")],
        footer="Wi-Fi 无线连接 · 快速检测 14:30:05", probe=0, can_remember=False, remembered=True, wait=None),
    "needs-trust": dict(
        title="离线，宿舍区需要确认网络",
        status=dict(status="needsTrust", diagnosis=None, probeMode="complete", passedCount=0, mode="recover", readOnly=False, hasDraft=True, autoLogin="needsTrust", hold=None, offerForcedLogin=False),
        ready=dict(mode="recover", readOnly=False, hasSaved=True, unsavedChanges=False, autoLogin="needsTrust", hold=None, sessionTrusted=False, remembered=False, memoryConflict=False),
        checks=[("Apple", "未通过", "warning"), ("Microsoft", "未通过", "warning")],
        footer="有线网络 · 完整检测 14:31:40", probe=1, can_remember=True, remembered=False, wait=None),
    "awaiting-tun": dict(
        title="已记住的网络，VPN/TUN 可能接管了校园地址",
        status=dict(status="unconfirmed", diagnosis=None, probeMode="quick", passedCount=0, mode="recover", readOnly=False, hasDraft=True, autoLogin="awaitingProof", hold=None, offerForcedLogin=False),
        ready=dict(mode="recover", readOnly=False, hasSaved=True, unsavedChanges=False, autoLogin="awaitingProof", hold=None, sessionTrusted=False, remembered=True, memoryConflict=False),
        checks=[("Apple", "通过 61 ms", "success"), ("Microsoft", t("dashboard.notWaited"), "neutral")],
        footer="Wi-Fi 无线连接 · VPN / TUN 路由 · 快速检测 09:12:44", probe=0, can_remember=False, remembered=True, wait="tunnel"),
    "paused": dict(
        title="密码被拒绝，自动登录已暂停",
        status=dict(status="paused", diagnosis=None, probeMode="quick", passedCount=0, mode="recover", readOnly=False, hasDraft=True, autoLogin="paused", hold="credentials", offerForcedLogin=False),
        ready=dict(mode="recover", readOnly=False, hasSaved=True, unsavedChanges=False, autoLogin="paused", hold="credentials", sessionTrusted=True, remembered=False, memoryConflict=False),
        checks=[("Apple", "未通过", "warning"), ("Microsoft", "未通过", "warning")],
        footer="Wi-Fi 无线连接 · 快速检测 20:05:31", probe=0, can_remember=False, remembered=False, wait=None),
}


def connection(state):
    p = status_reference.present(state["status"])
    r = memory_reference.readiness(state["ready"])
    banner = ""
    if p["banner"]:
        k = p["banner"]["key"]
        banner = (f'<div class="banner" style="background:{tone_sf(p["banner"]["tone"])}">{icon(tone_icon(p["banner"]["tone"]), 20, tone_fg(p["banner"]["tone"]))}'
                  f'<div><div class="t">{html.escape(t(k + ".title"))}</div><div class="d">{html.escape(t(k + ".detail"))}</div></div></div>')
    wait = f'<div class="cap" style="color:{tone_fg("warning")};margin-top:10px">{html.escape(t("memory.wait." + state["wait"]))}</div>' if state["wait"] else ""
    btns = ""
    for token, kind in ((p["primary"], "p"), (p["secondary"], "")):
        if token:
            btns += button(action_label(token, state["can_remember"]), kind, ACTION_ICON.get(token))
    chips = "".join(chip(text, tone, name) for name, text, tone in state["checks"])
    hero = (f'<div class="card hero"><div class="row"><div class="disc" style="background:{tone_sf(p["tone"])}">{icon(tone_icon(p["tone"]), 28, tone_fg(p["tone"]))}</div>'
            f'<div class="grow"><div class="row" style="gap:4px"><h1>{html.escape(t(p["headline"]))}</h1><span class="ibtn">{icon("info", 16)}</span></div>'
            f'<div class="sub">{html.escape(t(p["detail"]))}</div></div></div><div class="btns">{btns}</div><hr><div class="flow">{chips}</div>'
            f'<div class="foot"><span class="cap grow">{html.escape(state["footer"])}</span>{seg([t("probe.quick.title"), t("probe.complete.title")], state["probe"])}<span class="ibtn">{icon("info", 16)}</span></div></div>')
    ready_chips = "".join(chip(t(i["key"]), i["tone"]) for i in r["items"])
    acts = ""
    if state["status"]["hold"]:
        acts += button(t("action.resume"), "", "play")
    else:
        acts += button(t("action.pause"), "", "pause")
    if state["remembered"]:
        acts += button(t("memory.forget"), "q")
    elif state["status"]["autoLogin"] == "needsTrust":
        acts += button(t("memory.remember"), "", "bookmark") + button(t("memory.once"), "q")
    auto = card(t("dashboard.guard"), "shield-check",
                seg([t("mode.manual.title"), t("mode.observe.title"), t("mode.recover.title")], 2, True) +
                f'<div class="flow" style="margin-top:12px">{ready_chips}</div>{banner}{wait}<div class="btns" style="margin-bottom:0">{acts}<span class="sp"></span><span class="ibtn">{icon("info", 16)}</span></div>',
                badge=(t(r["title"]), r["tone"]))
    account = card(t("account.title"), "user",
                   '<div class="row"><div class="field">2023******17</div><div class="field grow">••••••••••</div></div>'
                   f'<div class="line"><div class="field" style="flex:none;width:230px">{html.escape(t("area.automatic.title"))} ▾</div><span class="ibtn">{icon("info", 16)}</span><span class="sp"></span>'
                   f'<span class="cap">{html.escape(t("dashboard.accountSaved"))}</span></div>')
    sites = card(t("internet.targets.title"), "globe",
                 f'<div class="line" style="margin-top:0"><span class="ck on">{icon("check", 12, "var(--onAccent)")}</span><span class="grow"><b style="font-size:13px">www.baidu.com</b>'
                 f'<div class="cap">https://www.baidu.com</div></span></div><div class="line"><div class="field grow">https://</div>{button(t("internet.add"))}</div>')
    net = (f'<div class="card"><div class="row"><span>{icon("globe", 18, "var(--accent)")}</span><span class="h">{html.escape(t("dashboard.network"))}</span>'
           f'<span class="cap grow">{html.escape(state["footer"].split(" · ")[0])}</span><span class="ibtn">{icon("info", 16)}</span></div></div>')
    return hero + auto + account + sites + net


def records():
    samples = []
    for i in range(24):
        ok = i not in (3, 4, 15)
        samples.append(ok)
    cells = "".join(f'<div style="background:{tone_fg("success" if ok else "warning")};{"outline:2px solid var(--textPrimary);outline-offset:-2px" if i == 18 else ""}"></div>' for i, ok in enumerate(samples))
    inner = (f'<div style="font-weight:600">{html.escape(t("records.health.samples", count=87))} · {html.escape(t("records.health.successRate", percent=94))} · {html.escape(t("records.health.latency", ms=31))}</div>'
             f'<div class="cap" style="margin-top:6px">{html.escape(t("records.range.retention", max=360))}<br>{html.escape(t("records.range.stored", count=87, **{"from": "09-30 06:41:07", "to": "09-30 14:30:05"}))}'
             f'<br>{html.escape(t("records.range.ribbon", count=24, **{"from": "09-30 12:31:22", "to": "09-30 14:30:05"}))}</div>'
             f'<div class="ribbon">{cells}</div><div class="cap3">{html.escape(t("records.ribbon.hint"))}</div>'
             f'<div class="detail"><div class="row"><b>{html.escape(t("records.detail.title"))}</b><span class="sp"></span><span class="btn q" style="min-height:28px">{html.escape(t("records.detail.close"))}</span></div>'
             + "".join(f'<div class="kv"><span>{html.escape(t(k))}</span><span>{html.escape(v)}</span></div>' for k, v in (
                 ("records.detail.time", "09-30 13:58:41"), ("records.detail.mode", t("probe.complete.title")), ("records.detail.result", "完整检测：一项通过、另一项未通过"),
                 ("records.detail.apple", "通过"), ("records.detail.microsoft", "未通过"), ("records.detail.sites", "1 / 1"), ("records.detail.link", "Wi-Fi 无线"), ("records.detail.technical", "en0")))
             + f'</div><div class="cap3" style="margin-top:10px">{html.escape(t("records.health.note"))}</div>')
    return card(t("records.health.title"), "records", inner)


def page(body, nav, theme, w, h, label):
    brand = base64.b64encode((ROOT / "shared/icon/app-icon-1024.png").read_bytes()).decode()
    items = [("signal", "nav.connection"), ("records", "nav.records"), ("settings", "nav.settings")]
    navigation = "".join(f'<span class="{"on" if i == nav else ""}">{icon(ic, 16, "var(--accent)" if i == nav else "var(--textSecondary)")}{html.escape(t(k))}</span>' for i, (ic, k) in enumerate(items))
    height = f"height:{h}px;" if h else ""
    return (f'<!doctype html><html lang="zh-CN" data-theme="{theme}"><head><meta charset="utf-8"><style>{css()}</style></head><body>'
            f'<div class="window" style="width:{w}px;{height}"><div class="bar"><div class="brand"><img alt="" src="data:image/png;base64,{brand}">{html.escape(t("app.name"))}</div>'
            f'<span class="sp"></span><div class="nav">{navigation}</div><span class="sp"></span><span class="ibtn">{icon("help", 20)}</span></div>'
            f'<div class="page"><div class="col">{body}</div></div><span class="tag">{html.escape(label)}</span></div></body></html>')


def main():
    global OUT
    parser = argparse.ArgumentParser(description="Render an HTML layout preview using installed Playwright.")
    parser.add_argument("--output-dir", type=pathlib.Path,
                        default=pathlib.Path(os.environ.get("SZU_PREVIEW_OUTPUT_DIR", str(OUT))))
    args = parser.parse_args()
    OUT = args.output_dir.expanduser().resolve()
    if OUT == ROOT or ROOT in OUT.parents and OUT != ROOT / "preview-output":
        parser.error("Use an external output directory or preview-output; do not write into source directories.")
    (OUT / "html").mkdir(parents=True, exist_ok=True)
    label = "HTML 预览 · 非原生截图"
    jobs = []
    sizes = [(640, 520), (780, 620), (920, 740)]
    for key, state in STATES.items():
        body = connection(state)
        for theme in ("light", "dark"):
            for w, h in sizes:
                name = f"{key}-{theme}-{w}x{h}"
                (OUT / "html" / f"{name}.html").write_text(page(body, 0, theme, w, h, label), "utf-8")
                jobs.append((name, w, h))
    body = records()
    for theme in ("light", "dark"):
        for w, h in sizes:
            name = f"records-{theme}-{w}x{h}"
            (OUT / "html" / f"{name}.html").write_text(page(body, 1, theme, w, h, label), "utf-8")
            jobs.append((name, w, h))
    for theme in ("light", "dark"):  # full page, to show the whole hierarchy once
        name = f"online-ready-{theme}-780-full"
        (OUT / "html" / f"{name}.html").write_text(page(connection(STATES["online-ready"]), 0, theme, 780, 0, label), "utf-8")
        jobs.append((name, 780, 0))
    runner = ROOT / "tools/preview/shoot.js"
    env = dict(os.environ)
    proc = subprocess.run(["node", str(runner), json.dumps(jobs), str(OUT)], env=env, capture_output=True, text=True)
    print("renderer exit", proc.returncode, proc.stderr[-400:])
    print(len(jobs), "images requested;", len(list(OUT.glob("*.png"))), "written")
    return proc.returncode


if __name__ == "__main__":
    sys.exit(main())
