#!/usr/bin/env python3
"""Portable static checks for Windows source structure and shared-resource references.

Not a compiler. It catches: unbalanced (), [], {}; local #include targets that do not exist;
tokens::k* names missing from the generated header; std:: facilities used without the
standard header in the file or its local headers; icon names that are not in shared/icons.
Exit status 1 when anything is found.  Usage: python3 tools/lint_windows_sources.py
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
WIN = ROOT / "windows/SZUConnect"
INCLUDE_DIRS = [WIN / "core/include", WIN / "core/generated", WIN / "platform/include", WIN / "app", WIN / "resources"]
STD_HEADERS = {
    "atoi": "cstdlib", "strtof": "cstdlib", "fmod": "cmath", "floor": "cmath", "ceil": "cmath", "memcpy": "cstring",
    "snprintf": "cstdio", "swprintf": "cstdio", "strftime": "ctime", "time_t": "ctime", "tm": "ctime", "wcscmp": "cwchar",
    "function": "functional", "thread": "thread", "mutex": "mutex", "lock_guard": "mutex", "unique_lock": "mutex",
    "condition_variable": "condition_variable", "optional": "optional", "nullopt": "optional", "map": "map", "set": "set",
    "deque": "deque", "vector": "vector", "string": "string", "wstring": "string", "string_view": "string_view",
    "wstring_view": "string_view", "unique_ptr": "memory", "make_unique": "memory", "shared_ptr": "memory",
    "atomic": "atomic", "clamp": "algorithm", "find": "algorithm", "find_if": "algorithm", "remove_if": "algorithm",
    "transform": "algorithm", "sort": "algorithm", "chrono": "chrono", "towlower": "cwctype", "uint64_t": "cstdint",
    "uint32_t": "cstdint", "pair": "utility", "move": "utility",
}
TRANSITIVE_OK = {  # headers that are guaranteed to provide others
    "string": {"string_view"}, "map": {"utility"}, "vector": {"utility"}, "optional": {"utility"},
}


def strip(code):
    out, i, n = [], 0, len(code)
    while i < n:
        c = code[i]
        if code.startswith("//", i):
            j = code.find("\n", i)
            i = n if j < 0 else j
        elif code.startswith("/*", i):
            j = code.find("*/", i + 2)
            out.append("\n" * code.count("\n", i, n if j < 0 else j))
            i = n if j < 0 else j + 2
        elif c == 'R' and code.startswith('R"', i):
            m = re.match(r'R"([^(]*)\(', code[i:])
            end = code.find(")" + m.group(1) + '"', i)
            out.append('""' + "\n" * code.count("\n", i, end))
            i = end + len(m.group(1)) + 2
        elif c in "\"'":
            j = i + 1
            while j < n and code[j] != c:
                j += 2 if code[j] == "\\" else 1
            out.append(c + c)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def local_includes(path, seen):
    if path in seen:
        return
    seen.add(path)
    for inc in re.findall(r'#include\s+"([^"]+)"', path.read_text("utf-8")):
        for base in [path.parent] + INCLUDE_DIRS:
            target = (base / inc).resolve()
            if target.exists():
                local_includes(target, seen)
                break


def main():
    problems = []
    tokens_header = (WIN / "core/generated/shared_tokens.hpp").read_text("utf-8")
    icon_names = {p.stem for p in (ROOT / "shared/icons").glob("*.svg")}
    files = sorted(p for d in ("core", "platform", "app", "tests") for p in (WIN / d).rglob("*") if p.suffix in (".cpp", ".hpp", ".h"))
    for path in files:
        raw = path.read_text("utf-8")
        code = strip(raw)
        rel = path.relative_to(ROOT)
        stack = []
        pairs = {")": "(", "]": "[", "}": "{"}
        for line_no, line in enumerate(code.split("\n"), 1):
            for ch in line:
                if ch in "([{":
                    stack.append((ch, line_no))
                elif ch in ")]}":
                    if not stack or stack[-1][0] != pairs[ch]:
                        problems.append(f"{rel}:{line_no}: unbalanced '{ch}'")
                        stack = []
                        break
                    stack.pop()
        for ch, line_no in stack:
            problems.append(f"{rel}:{line_no}: '{ch}' never closed")
        for inc in re.findall(r'#include\s+"([^"]+)"', raw):
            if not any((base / inc).exists() for base in [path.parent] + INCLUDE_DIRS):
                problems.append(f"{rel}: missing local include \"{inc}\"")
        for name in sorted(set(re.findall(r"tokens::(k[A-Za-z0-9]+)", code))):
            if not re.search(rf"\b{name}\b", tokens_header):
                problems.append(f"{rel}: tokens::{name} not in shared_tokens.hpp")
        closure = set()
        local_includes(path.resolve(), closure)
        included = set()
        for header in closure:
            included |= set(re.findall(r"#include\s+<([^>]+)>", header.read_text("utf-8")))
        for std_name in sorted(set(re.findall(r"std::([a-z_0-9]+)", code))):
            needed = STD_HEADERS.get(std_name)
            if needed and needed not in included and not any(needed in TRANSITIVE_OK.get(h, ()) for h in included):
                problems.append(f"{rel}: std::{std_name} used without <{needed}>")
        if path.parent.name == "app":
            for icon in set(re.findall(r'(?:icon\s*=\s*|icon_row\(\s*|"icon":\s*)"([a-z][a-z-]*)"', raw)) | set(
                    re.findall(r'c\.icon\("([a-z-]+)"', raw)):
                if icon not in icon_names:
                    problems.append(f"{rel}: icon \"{icon}\" not in shared/icons")
    for p in problems:
        print("  -", p)
    print(f"windows sources: {len(files)} files, {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
