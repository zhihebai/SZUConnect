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
feedback=json.loads((root/"shared/links.json").read_text())["project"]["feedbackEmail"]
assert g.validate_feedback_email(feedback)==feedback
assert f"kFeedbackEmail = {g.cpp_string(feedback)}" in files[g.WIN_OUT/"shared_copy.hpp"]
assert f"static let feedbackEmail = {g.swift_string(feedback)}" in files[g.MAC_OUT/"SharedCopy.swift"]
presets=json.loads((root/"shared/links.json").read_text())["websitePresets"]
assert g.validate_website_presets(presets)==presets
for preset in presets:
    assert g.cpp_string(preset["url"]) in files[g.WIN_OUT/"shared_copy.hpp"]
    assert g.swift_string(preset["url"]) in files[g.MAC_OUT/"SharedCopy.swift"]
for unsafe in [[], presets+presets[:1], [presets[0],presets[0]],
               [{"key": "baidu", "url": "http://www.baidu.com/"},presets[1]],
               [{"key": "baidu", "url": "https://www.baidu.com/?"},presets[1]],
               [{"key": "baidu", "url": "https://www.baidu.com:443/"},presets[1]],
               [{"key": "baidu", "url": "https://www.baidu.com.evil.invalid/"},presets[1]],
               [{"key": "baidu", "url": "https://www.baidu.com/", "enabled": True},presets[1]]]:
    try:
        g.validate_website_presets(unsafe)
    except ValueError:
        continue
    raise AssertionError("Website preset whitelist accepted an unsupported target")
sample_address="a"+"@"+"b.invalid"
for unsafe in ["", sample_address+"\n", sample_address+"?bcc=other", sample_address+","+sample_address, "mailto:"+sample_address]:
    try:
        g.validate_feedback_email(unsafe)
    except ValueError:
        continue
    raise AssertionError("Feedback email accepted an unsafe address")
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
