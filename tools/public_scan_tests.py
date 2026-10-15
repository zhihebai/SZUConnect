"""Safety regression fixtures use invented values and never real user data."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from public_package import MANIFEST, public_package, sha256
from public_scan import report, scan_tree


class PublicSourceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="public-source-fixture-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.base / "source"
        self.root.mkdir()

    def write(self, name, content):
        target = self.root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")
        return target

    def test_nested_documents_and_binary_metadata_are_checked(self):
        fake_path = "/" + "Users" + "/fixture-person/Documents/sample"
        self.write("platform/docs/notes.md", fake_path)
        image = self.root / "asset.png"
        image.write_bytes(b"\x89PNG\x00" + fake_path.encode())
        findings, files = scan_tree(self.root)
        self.assertEqual({item.path for item in findings}, {"platform/docs/notes.md", "asset.png"})
        self.assertEqual(len(files), 2)

    def test_generated_and_history_files_fail_even_when_empty(self):
        self.write("evidence/local/notes.txt", "")
        self.write("platform/DELIVERY-REPORT.md", "")
        self.write("build/product.o", "")
        self.write("distribution/identity.p12", "")
        findings, _ = scan_tree(self.root)
        self.assertEqual(len(findings), 4)

    def test_windows_home_paths_with_either_separator_are_checked(self):
        for separator in ("/", "\\"):
            home = "C:" + separator + "Users" + separator + "fixture-person" + separator + "data"
            self.write("notes.txt", home)
            findings, _ = scan_tree(self.root)
            self.assertEqual([item.category for item in findings], ["machine-home-path"])

    def test_named_fake_credentials_and_school_addresses_are_allowed(self):
        fake_url = "https://user" + "@net.szu.edu.cn/cgi-bin/srun_portal"
        self.write("macos/SZUConnect/Tests/FixtureTests.swift", fake_url + '\npassword="preview-only"\n')
        self.write("shared/fixtures/protocol.json", '{"account":"2023123456","password":"fixture-password"}')
        self.write("network.md", "https://net.szu.edu.cn/ http://172.30.255.42:801/eportal")
        self.write("icon.json", '{"name":"mac-16' + '@2x.png"}')
        self.write("shared/fixtures/website.json", '"mailto:someone' + '@example.com"')
        self.write("shared/fixtures/round.json", '"本轮改用已确认的宿舍区"')
        findings, _ = scan_tree(self.root)
        self.assertEqual(findings, [])

    def test_tests_do_not_blanket_allow_private_email(self):
        email = "@".join(("fixture-person", "mail.invalid"))
        self.write("macos/SZUConnect/Tests/FixtureTests.swift", email)
        findings, _ = scan_tree(self.root)
        self.assertEqual([item.category for item in findings], ["unreviewed-email-address"])

    def test_public_curl_license_email_exception_is_exact(self):
        address = "daniel" + "@" + "haxx.se"
        notice = "Copyright (c) 1996 - 2026, Daniel Stenberg, <" + address + ">, and many"
        self.write("docs/licenses/curl-COPYING.txt", notice)
        self.assertEqual(scan_tree(self.root)[0], [])
        self.write("notes.txt", notice)
        self.assertEqual(len(scan_tree(self.root)[0]), 1)
        self.write("docs/licenses/curl-COPYING.txt", "private-person" + "@" + "haxx.se")
        self.assertEqual(len(scan_tree(self.root)[0]), 2)

    def test_private_permission_note_is_not_public_license_metadata(self):
        note = "开发者" + "于2000-01-01确认"
        self.write("shared/links.json", json.dumps({"license": note}, ensure_ascii=False))
        findings, _ = scan_tree(self.root)
        self.assertEqual([item.category for item in findings], ["private-permission-record"])

    def test_deny_terms_and_reports_never_print_matched_values(self):
        private_term = "fixture-sensitive-campus"
        self.write("data.txt", private_term)
        findings, files = scan_tree(self.root, (private_term,))
        output = json.dumps(report(findings, files))
        self.assertNotIn(private_term, output)
        self.assertIn("private-deny-term", output)

    def test_links_cannot_smuggle_external_files(self):
        outside = self.base / "outside.txt"
        outside.write_text("fixture")
        (self.root / "link.txt").symlink_to(outside)
        findings, _ = scan_tree(self.root)
        self.assertEqual(findings[0].category, "symbolic-link")
        with self.assertRaises(ValueError):
            public_package(self.root, self.base / "refused.zip")

    def test_zip_is_reproducible_and_manifest_is_verified(self):
        self.write("README.md", "Public fixture\n")
        self.write("src/core.cpp", "int main() { return 0; }\n")
        first, second = self.base / "first.zip", self.base / "second.zip"
        self.assertEqual(public_package(self.root, first), 3)
        public_package(self.root, second)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        with zipfile.ZipFile(first) as archive:
            manifest = archive.read("SZUConnect-source/" + MANIFEST).decode()
            for line in manifest.splitlines():
                digest, relative = line.split("  ", 1)
                self.assertEqual(digest, sha256(archive.read("SZUConnect-source/" + relative)))

    def test_package_rejects_overwrite_and_source_output(self):
        self.write("README.md", "fixture")
        output = self.base / "result.zip"
        public_package(self.root, output)
        with self.assertRaises(ValueError):
            public_package(self.root, output)
        with self.assertRaises(ValueError):
            public_package(self.root, self.root / "output.zip")
        with self.assertRaises(ValueError):
            public_package(self.root, self.base / "invalid.zip", "../escape")

    def test_changed_source_content_is_checked_again(self):
        file = self.write("README.md", "clean")
        def initially_clean(root, deny_terms):
            file.write_text("/" + "home" + "/fixture-person/data")
            return [], [file]
        with patch("public_package.scan_tree", side_effect=initially_clean):
            with self.assertRaises(ValueError):
                public_package(self.root, self.base / "changed.zip")


if __name__ == "__main__":
    unittest.main()
