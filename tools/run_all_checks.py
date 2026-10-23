#!/usr/bin/env python3
"""Run portable checks and store logs in an explicitly chosen local output directory.

    python3 tools/run_all_checks.py --output-dir DIR

Exit code 1 if any check fails. Native builds, Swift tests and UI acceptance run separately.
"""
import argparse, datetime, os, pathlib, shutil, subprocess, sys, tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
EV = ROOT / "local-reports"
RUNS = [
    ("generate-shared-check", ["python3", "tools/generate_shared.py", "--check"], ROOT),
    ("verify-fixtures", ["python3", "tools/reference/verify_fixtures.py"], ROOT),
    ("core-tests", ["sh", "tests/run-core-tests.sh"], ROOT / "windows/SZUConnect"),
    ("lint-windows", ["python3", "tools/lint_windows_sources.py"], ROOT),
    ("lint-swift", ["python3", "tools/lint_swift_sources.py"], ROOT),
    ("windows-syntax-mock-sdk", ["sh", "tools/check_windows_syntax.sh"], ROOT),
    ("check-ui-resources", ["python3", "tools/check_ui_resources.py"], ROOT),
    ("contrast-audit", ["python3", "tools/check_contrast.py"], ROOT),
    ("html-preview", ["python3", "tools/preview/render_preview.py"], ROOT),
]


def main():
    global EV
    parser = argparse.ArgumentParser(description="Run portable checks; native platform validation is separate.")
    parser.add_argument("--output-dir", type=pathlib.Path,
                        default=pathlib.Path(os.environ.get("SZU_CHECK_OUTPUT_DIR", str(EV))))
    args = parser.parse_args()
    output_root = args.output_dir.expanduser().resolve()
    if output_root == ROOT or ROOT in output_root.parents and output_root != ROOT / "local-reports":
        parser.error("Use an external output directory or local-reports; do not write into source directories.")
    output_root.mkdir(parents=True, exist_ok=True)
    EV = pathlib.Path(tempfile.mkdtemp(prefix="checks-", dir=output_root))
    lines, failed = [], False
    for name, cmd, cwd in RUNS:
        env = dict(os.environ)
        env["PYTHONDONTWRITEBYTECODE"] = "1"
        if name == "core-tests":
            env["OUT"] = str(EV / "core-tests")
        if name == "html-preview":
            env["SZU_PREVIEW_OUTPUT_DIR"] = str(EV / "preview")
        proc = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True)
        out = proc.stdout + proc.stderr
        (EV / f"{name}.log").write_text(" ".join(cmd) + "\n" + out, "utf-8")
        tail = [l for l in out.strip().split("\n") if l.strip()][:1 if name == "contrast-audit" else 10**6]
        tail = (tail[0] if name == "contrast-audit" else tail[-1]) if tail else ""
        lines.append(f"{name}: exit={proc.returncode} | {tail[:170]}")
        failed |= proc.returncode != 0
    tool_lines = []
    for executable, args in [("g++", ["--version"]), ("python3", ["--version"]),
                             ("node", ["--version"]), ("swiftc", ["--version"]),
                             ("xcodebuild", ["-version"]), ("cl", [])]:
        if not shutil.which(executable):
            tool_lines.append(f"{executable}: not found on PATH")
            continue
        probe = subprocess.run([executable, *args], capture_output=True, text=True)
        lines_out = (probe.stdout + probe.stderr).strip().splitlines()
        tool_lines.append(f"{executable}: " + (lines_out[0] if lines_out else f"exit={probe.returncode}"))
    (EV / "environment.txt").write_text(
        f"date: {datetime.datetime.now(datetime.timezone.utc).isoformat()}\n"
        + "\n".join(tool_lines)
        + "\nThis runner does not perform Xcode/MSVC builds or native GUI acceptance.\n", "utf-8")
    (EV / "exit-codes.txt").write_text("\n".join(lines) + "\n", "utf-8")
    print("\n".join(lines))
    print(f"Check reports: {EV}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
