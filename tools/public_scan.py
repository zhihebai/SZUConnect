#!/usr/bin/env python3
"""Check the complete candidate source tree without printing matched content."""
from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
FORBIDDEN_DIRS = {
    "evidence", "history", "private", "local-reports", "outputs", "preview-output",
    "build", "build-core-tests", "DerivedData", ".build", ".swiftpm",
    "node_modules", "Pods", "__pycache__", "xcuserdata", ".vs", "RUN", "patch",
}
FORBIDDEN_NAMES = {
    ".DS_Store", "PROVENANCE.json", "verification-status.json", "PROJECT-RESUME.md",
    "LOCAL-ACCEPTANCE.md", "LOCAL-CHANGES.md", "DELIVERY-REPORT.md", "RETURN-NOTES.md",
    "REQUIREMENT-COVERAGE.md", "CHANGED-FILES.md", "FEATURE-PARITY.md",
    "WINDOWS-RESUME.md", "WEB-" "AI-NEXT-ITERATION.md", "RETURN-MANIFEST.sha256",
    "APP-PATH.txt", "DERIVED-DATA-PATH.txt",
}
FORBIDDEN_SUFFIXES = {
    ".log", ".pyc", ".pyo", ".o", ".obj", ".exe", ".dll", ".pdb", ".ilk",
    ".ipa", ".dmg", ".zip", ".p12", ".pfx", ".key", ".cer",
    ".mobileprovision", ".provisionprofile", ".xcuserstate",
}
PATTERNS = {
    "machine-home-path": re.compile(r"/(?:Users|home)/[^\s/\\\"'<>]+|[A-Z]:[\\/](?:Users|Documents and Settings)[\\/][^\s/\\\"'<>]+", re.I),
    "assistant-conversation": re.compile(r"\b(?:Co[d]ex|Clau[d]e)\b|网页\s*A[I]|WEB[-_ ]A[I]", re.I),
    "personal-acceptance-context": re.compile(r"本[轮].{0,20}(?:验[收]|交[付]|本[机]|对[话])|本地验[收]|本次验[收]|开发者此[前]|首轮.{0,20}沟[通]"),
    "private-permission-record": re.compile(r"开发者[于].{0,40}确[认]"),
    "private-key": re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE K[E]Y-----"),
    "access-token": re.compile(r"\b(?:gh[pousr]_[A-Za-z0-9]{25,}|github_pat_[A-Za-z0-9_]{25,}|sk-(?:proj-)?[A-Za-z0-9_-]{30,})\b"),
    "signing-team-value": re.compile(r"(?:DEVELOPMENT_TEAM|teamID|teamIdentifier)\s*[=:]\s*[\"']?[A-Z0-9]{10}\b"),
    "signing-person-identity": re.compile(r"Developer ID Application:\s*[^\n]+\([A-Z0-9]{10}\)"),
}
EMAIL = re.compile(r"\b[A-Za-z0-9_.+:-]+@([A-Za-z0-9][A-Za-z0-9.-]*\.[A-Za-z]{2,})\b")
SKIP_EMAIL_SUFFIXES = {"png", "svg", "ico", "icns", "swift", "cpp", "h", "txt"}
MAX_FILE_BYTES = 20_000_000


@dataclass(frozen=True)
class Finding:
    path: str
    line: int
    category: str


def is_fixture(path: Path) -> bool:
    return "Tests" in path.parts or "tests" in path.parts or path.as_posix().startswith("shared/fixtures/")


def permitted_email(match: re.Match[str], line: str, path: Path) -> bool:
    """Allow named fake URL-userinfo cases, never every value in a test directory."""
    domain = match.group(1).lower()
    if domain.rsplit(".", 1)[-1] in SKIP_EMAIL_SUFFIXES:
        return True
    feedback = "hizhihebai" + "@" + "163.com"
    if path.as_posix() in {
        "shared/links.json", "windows/SZUConnect/core/generated/shared_copy.hpp",
        "macos/SZUConnect/Sources/Generated/SharedCopy.swift", "README.md", "docs/USER-GUIDE.md",
        "docs/“深大随连”项目说明与使用教程-v1.0.pdf",
    } and match.group(0) in {feedback, "mailto:" + feedback}:
        return True
    # The upstream author address is required in this exact public license notice.
    # This does not allow user addresses or arbitrary emails elsewhere in the tree.
    if path.as_posix() == "docs/licenses/curl-COPYING.txt":
        address = "daniel" + "@" + "haxx.se"
        return match.group(0) == address and line == (
            "Copyright (c) 1996 - 2026, Daniel Stenberg, <" + address + ">, and many")
    if not is_fixture(path):
        return False
    local = match.group(0).rsplit("@", 1)[0].casefold()
    mailto = local.startswith("mailto:")
    if mailto:
        local = local.removeprefix("mailto:")
    fake_local = local in {"u", "u:p", "user", "user:pass", "user:secret", "fixture", "test", "someone"}
    fake_domain = domain in {"example.com", "example.org", "example.net", "net.szu.edu.cn"} or domain.endswith((".test", ".invalid"))
    prefix = line[max(0, match.start() - 8):match.start()]
    return fake_local and fake_domain and (mailto or bool(re.search(r"https?://$", prefix)))


def path_category(path: Path) -> str | None:
    if any(part in FORBIDDEN_DIRS or part.endswith((".app", ".dSYM")) for part in path.parts):
        return "private-or-generated-directory"
    if path.name in FORBIDDEN_NAMES or re.fullmatch(r"CHANGELOG-.*-build\d+\.md", path.name):
        return "private-history-document"
    if path.suffix.lower() in FORBIDDEN_SUFFIXES or path.name == ".env" or path.name.startswith(".env.") and path.name != ".env.example":
        return "artifact-or-signing-material"
    return None


def scan_content(path: Path, raw: bytes, deny_terms: tuple[str, ...] = ()) -> list[Finding]:
    findings: list[Finding] = []
    content = raw.decode("utf-8", errors="replace")
    for number, line in enumerate(content.splitlines(), 1):
        for kind, pattern in PATTERNS.items():
            if pattern.search(line):
                findings.append(Finding(path.as_posix(), number, kind))
        if any(not permitted_email(m, line, path) for m in EMAIL.finditer(line)):
            findings.append(Finding(path.as_posix(), number, "unreviewed-email-address"))
        if any(term.casefold() in line.casefold() for term in deny_terms):
            findings.append(Finding(path.as_posix(), number, "private-deny-term"))
    return findings


def scan_tree(root: Path, deny_terms: tuple[str, ...] = ()) -> tuple[list[Finding], list[Path]]:
    """Inspect every candidate file, rejecting links and unreadable content."""
    root = root.resolve()
    findings: list[Finding] = []
    candidates: list[Path] = []
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root)
        # Version-control metadata is never part of the source package.
        if ".git" in rel.parts:
            continue
        if path.is_symlink():
            findings.append(Finding(rel.as_posix(), 0, "symbolic-link"))
            continue
        if not path.is_file():
            continue
        candidates.append(path)
        category = path_category(rel)
        if category:
            findings.append(Finding(rel.as_posix(), 0, category))
            continue
        try:
            if path.stat().st_size > MAX_FILE_BYTES:
                findings.append(Finding(rel.as_posix(), 0, "oversized-file"))
                continue
            raw = path.read_bytes()
        except OSError:
            findings.append(Finding(rel.as_posix(), 0, "unreadable-file"))
            continue
        # ASCII/UTF-8 metadata in binary assets is checked as well as source text.
        findings.extend(scan_content(rel, raw, deny_terms))
    return findings, candidates


def read_deny_terms(path: Path | None) -> tuple[str, ...]:
    return tuple(line.strip() for line in path.read_text("utf-8").splitlines() if line.strip()) if path else ()


def report(findings: list[Finding], files: list[Path]) -> dict:
    return {"schemaVersion": 1, "status": "failed" if findings else "passed",
            "fileCount": len(files), "findingCount": len(findings),
            "findings": [asdict(item) for item in findings]}


def external_output(path: Path, root: Path) -> Path:
    output = path.expanduser().resolve()
    if output == root or root in output.parents:
        raise ValueError("Reports and packages must be written outside the source tree")
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--deny-file", type=Path, help="External UTF-8 list of private terms; values are never printed")
    parser.add_argument("--json", type=Path, help="Write a report outside the source tree")
    args = parser.parse_args()
    root = args.root.expanduser().resolve()
    if not root.is_dir():
        parser.error("Source root must be an existing directory")
    try:
        if args.deny_file:
            external_output(args.deny_file, root)
        terms = read_deny_terms(args.deny_file)
        findings, files = scan_tree(root, terms)
        result = report(findings, files)
        if args.json:
            output = external_output(args.json, root)
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", "utf-8")
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(f"Public source scan: {result['status']}; {len(files)} files; {len(findings)} findings")
    for item in findings:
        print(f"{item.path}:{item.line}: {item.category}")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
