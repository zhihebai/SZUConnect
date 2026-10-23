#!/usr/bin/env python3
"""Portable static checks for Swift source structure and shared-resource references.

Not a compiler. It checks: balanced (), [], {} after lexing strings (including multi-line
strings and interpolation) and nested comments; `model.<member>` references resolve to a
declaration in Sources; SharedTokens members, token colour names, tone names and icon names
exist. Exit status 1 when anything is found.  Usage: python3 tools/lint_swift_sources.py
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCES = ROOT / "macos/SZUConnect/Sources"
TESTS = ROOT / "macos/SZUConnect/Tests"


def lex(code):
    """Returns code with string contents blanked and comments removed; interpolations kept."""
    out, i, n = [], 0, len(code)

    def string(i, multiline):
        # i points just after the opening quote(s); returns index after the closing quote(s)
        while i < n:
            if code.startswith('"""', i) if multiline else code[i] == '"':
                return i + (3 if multiline else 1)
            if code[i] == "\\":
                if i + 1 < n and code[i + 1] == "(":
                    out.append("(")
                    i = expression(i + 2)
                    out.append(")")
                    continue
                i += 2
                continue
            if code[i] == "\n":
                out.append("\n")
            i += 1
        return i

    def expression(i):
        # inside \( ... ): lex code until the matching ')'
        depth = 1
        while i < n:
            c = code[i]
            if code.startswith('"""', i):
                out.append('""'); i = string(i + 3, True); continue
            if c == '"':
                out.append('""'); i = string(i + 1, False); continue
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    return i + 1
            out.append(c)
            i += 1
        return i

    while i < n:
        c = code[i]
        if code.startswith("//", i):
            j = code.find("\n", i)
            i = n if j < 0 else j
        elif code.startswith("/*", i):
            depth, i = 1, i + 2
            while i < n and depth:
                if code.startswith("/*", i): depth += 1; i += 2
                elif code.startswith("*/", i): depth -= 1; i += 2
                else:
                    if code[i] == "\n": out.append("\n")
                    i += 1
        elif code.startswith('#"', i):
            j = code.find('"#', i + 2)
            out.append('""' + "\n" * code.count("\n", i, j))
            i = j + 2
        elif code.startswith('"""', i):
            out.append('""'); i = string(i + 3, True)
        elif c == '"':
            out.append('""'); i = string(i + 1, False)
        else:
            out.append(c); i += 1
    return "".join(out)


def main():
    problems = []
    files = sorted(list(SOURCES.rglob("*.swift")) + list(TESTS.glob("*.swift")))
    declared = set()
    for path in SOURCES.rglob("*.swift"):
        text = lex(path.read_text("utf-8"))
        declared |= set(re.findall(r"\b(?:var|let|func|case)\s+([A-Za-z_]\w*)", text))
        declared |= set(re.findall(r"\bcase\s+(?:[a-z]\w*\s*,\s*)*([a-z]\w*)", text))
    tokens_swift = (SOURCES / "Generated/SharedTokens.swift").read_text("utf-8")
    token_members = set(re.findall(r"static let (\w+)", tokens_swift))
    tokens_json = json.loads((ROOT / "shared/design-tokens.json").read_text("utf-8"))
    colour_names = set(re.findall(r'^\s*"(\w+)": Pair\(', tokens_swift, re.M)) or set(tokens_json.get("colors", {}).keys())
    tone_names = set(re.findall(r'^\s*"(\w+)": \(foreground', tokens_swift, re.M))
    icon_names = {p.stem for p in (ROOT / "shared/icons").glob("*.svg")}
    for path in files:
        raw = path.read_text("utf-8")
        code = lex(raw)
        rel = path.relative_to(ROOT)
        stack, pairs = [], {")": "(", "]": "[", "}": "{"}
        for line_no, line in enumerate(code.split("\n"), 1):
            for ch in line:
                if ch in "([{": stack.append((ch, line_no))
                elif ch in ")]}":
                    if not stack or stack[-1][0] != pairs[ch]:
                        problems.append(f"{rel}:{line_no}: unbalanced '{ch}'"); stack = []; break
                    stack.pop()
        for ch, line_no in stack:
            problems.append(f"{rel}:{line_no}: '{ch}' never closed")
        if path.parent == TESTS:
            continue
        for member in sorted(set(re.findall(r"\bmodel\.([A-Za-z_]\w*)", code))):
            if member not in declared:
                problems.append(f"{rel}: model.{member} has no declaration in Sources")
        for member in sorted(set(re.findall(r"\bSharedTokens\.([A-Za-z_]\w*)", code))):
            if member not in token_members:
                problems.append(f"{rel}: SharedTokens.{member} does not exist")
        for name in sorted(set(re.findall(r'\.token\(\s*"(\w+)"', raw))):
            if colour_names and name not in colour_names:
                problems.append(f"{rel}: colour token \"{name}\" does not exist")
        for name in sorted(set(re.findall(r'\.tone(?:Surface)?\(\s*"(\w+)"', raw)) | set(re.findall(r'\btone:\s*"(\w+)"', raw))):
            if tone_names and name not in tone_names:
                problems.append(f"{rel}: tone \"{name}\" does not exist")
        for name in sorted(set(re.findall(r'(?:AppIcon\(name:|icon:|IconShape\(name:)\s*"([a-z][a-z0-9-]*)"', raw))):
            if name not in icon_names:
                problems.append(f"{rel}: icon \"{name}\" not in shared/icons")
    for p in problems:
        print("  -", p)
    print(f"swift sources: {len(files)} files, {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
