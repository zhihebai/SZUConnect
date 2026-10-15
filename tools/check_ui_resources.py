#!/usr/bin/env python3
"""Check generated copy, icon aliases and project links without opening either app."""
import importlib.util,json,re
from pathlib import Path
root=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location("shared_generator",root/"tools/generate_shared.py")
g=importlib.util.module_from_spec(spec);spec.loader.exec_module(g)
files,_=g.generate()
for path,content in files.items():
    assert path.read_text()==content, f"Stale generator output: {path}"
    if path.name in {"SharedCopy.swift","shared_copy.cpp"}:
        assert not re.search(r"\{(?:store|tray|launch)\}",content), f"Unresolved platform noun: {path}"
swift=files[g.MAC_OUT/"SharedIcons.swift"]
cpp=files[g.WIN_OUT/"shared_icons.cpp"]
for name in g.load_icons():
    for alias in {name,g.camel(name)}:
        assert '"'+alias+'"' in swift and '"'+alias+'"' in cpp, f"Missing shared icon {alias}"
assert "钥匙串" in files[g.MAC_OUT/"SharedCopy.swift"]
assert "Windows 凭据管理器" in files[g.WIN_OUT/"shared_copy.cpp"]
project=json.loads((root/"shared/links.json").read_text())["project"]["url"]
assert g.validate_project_url(project)==project
assert f"static let projectURL = {g.swift_string(project)}" in files[g.MAC_OUT/"SharedCopy.swift"]
assert f"kProjectUrl = {g.cpp_string(project)}" in files[g.WIN_OUT/"shared_copy.hpp"]
assert "SharedLinks.projectURL" in (root/"macos/SZUConnect/Sources/Sheets.swift").read_text()
assert "links::kProjectUrl" in (root/"windows/SZUConnect/app/views_sheets.cpp").read_text()
for unsafe in ["http://github.com/owner/repo", "https://github.com.evil.invalid/owner/repo",
               "https://github.com:443/owner/repo", "https://"+"fixture@"+"github.com/owner/repo",
               "https://github.com/owner/repo?view=1", "https://github.com/owner/repo#readme",
               "https://github.com/owner/repo?", "https://github.com/owner/repo#",
               "https://github.com/owner/repo/issues", "https://github.com/owner/%72epo",
               "https://github.com/owner/repo\n"]:
    try:
        g.validate_project_url(unsafe)
    except ValueError:
        continue
    raise AssertionError("Project URL whitelist accepted an unsafe link")
print("PASS: platform copy, icon aliases and project link resources match; project URL whitelist rejects unsafe forms.")
