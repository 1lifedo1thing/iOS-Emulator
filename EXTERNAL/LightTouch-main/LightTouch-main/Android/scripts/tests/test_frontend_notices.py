# SPDX-License-Identifier: GPL-2.0-or-later
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import re
import tempfile
import unittest
from unittest import mock
import warnings
import zipfile

spec = importlib.util.spec_from_file_location("frontend_notices", Path(__file__).resolve().parents[1] / "package-frontend-notices.py")
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)
REFERENCE = "https://publisher.example/corresponding-source"
REVISION = "a" * 40


def archive(entries):
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w") as z:
        for name, value in entries:
            z.writestr(name, value)
    return output.getvalue()


class FrontendNotices(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.cache = self.root / "cache"
        self.inventory = self.root / "private-inventory.json"

    def tearDown(self):
        self.tmp.cleanup()

    def selected(self, value, copies=1):
        path = self.root / "input.aar"
        path.write_bytes(value)
        row = {"coordinate": "example.runtime:ui-android:1.2.3", "file": "ui-release.aar",
               "sha256": hashlib.sha256(value).hexdigest(), "artifact_path": str(path)}
        self.inventory.write_text(json.dumps({"schema": 1, "configuration": "debugRuntimeClasspath", "artifacts": [row] * copies}))
        return row

    def run_package(self, destination="assets", **kwargs):
        return tool.package(self.inventory, self.root / destination, self.cache, REFERENCE, REVISION, **kwargs)

    def test_original_nested_bytes_scope_duplicates_and_relocation(self):
        original = b"Synthetic fixture license\r\nCopyright fixture\r\n"
        self.selected(archive([("classes.jar", archive([("META-INF/LICENSE.txt", original)])),
                               ("../../NOTICE.txt", b"Synthetic outer fixture notice\n")]), copies=2)
        # This unrelated build-tool cache member must never enter selected assets.
        unrelated = self.cache / "unselected-tool.jar"
        unrelated.parent.mkdir(parents=True)
        unrelated.write_bytes(archive([("NOTICE.txt", b"UNSELECTED")]))
        a = self.run_package("a"); b = self.run_package("b")
        self.assertEqual(a, b)
        self.assertEqual(a, self.run_package("a"))
        manifest = json.loads((self.root / "a/manifest.json").read_text())
        self.assertEqual(manifest["selected_artifact_rows"], 2)
        self.assertEqual(manifest["selected_unique_artifacts"], 1)
        self.assertEqual(manifest["components"][0]["selection_occurrences"], 2)
        self.assertNotIn(str(self.root), json.dumps(manifest))
        for row in manifest["files"]:
            self.assertTrue(all(re.fullmatch(r"[A-Za-z0-9._-]+", p) and p not in (".", "..") for p in row["path"].split('/')))
            data = (self.root / "a" / row["path"]).read_bytes()
            self.assertEqual(hashlib.sha256(data).hexdigest(), row["sha256"])
            self.assertNotEqual(data, b"UNSELECTED")
        nested = next(r for r in manifest["components"][0]["notices"] if r.get("archive_entry", "").startswith("classes.jar!/"))
        self.assertEqual((self.root / "a" / nested["path"]).read_bytes(), original)
        # Local path relocation must not affect public bundle bytes.
        doc = json.loads(self.inventory.read_text())
        relocated = self.root / "different-local-cache.aar"
        relocated.write_bytes((self.root / "input.aar").read_bytes())
        for row in doc["artifacts"]:
            row["artifact_path"] = str(relocated)
        self.inventory.write_text(json.dumps(doc))
        self.assertEqual(a, self.run_package("relocated"))

    def test_missing_notice_is_not_an_invented_grant(self):
        self.selected(archive([("classes.jar", archive([("LicenseService.class", b"synthetic class")]))]))
        with self.assertRaisesRegex(ValueError, "Missing primary"):
            self.run_package()
        self.assertFalse((self.root / "assets").exists())
        self.assertFalse(list(self.root.glob(".frontend-notices-*")))
        self.assertEqual(self.run_package(allow_missing=True)["notice_byte_coverage"], "INCOMPLETE")

    def test_notice_directory_member_is_preserved(self):
        payload = b"Synthetic original notice from a licenses directory\n"
        self.selected(archive([("META-INF/licenses/component.txt", payload)]))
        self.run_package()
        m = json.loads((self.root / "assets/manifest.json").read_text())
        entry = m["components"][0]["notices"][0]
        self.assertEqual((self.root / "assets" / entry["path"]).read_bytes(), payload)

    def test_input_checksum_rejects_before_output(self):
        self.selected(archive([("LICENSE.txt", b"synthetic fixture")]))
        (self.root / "input.aar").write_bytes(b"changed input")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            self.run_package()
        self.assertFalse((self.root / "assets").exists())

    def test_explicit_checked_source_supplement_and_tamper(self):
        row = self.selected(archive([("Example.class", b"synthetic class")]))
        notice = self.root / "public-source-license.txt"
        notice.write_bytes(b"Synthetic upstream license fixture\r\n")
        supplement = self.root / "private-supplements.json"
        supplement.write_text(json.dumps({"schema": 1, "components": [{"coordinate": row["coordinate"],
            "artifact_sha256": row["sha256"], "notices": [{"name": "LICENSE.txt", "role": "license",
            "notice_path": str(notice), "sha256": hashlib.sha256(notice.read_bytes()).hexdigest(),
            "source_url": "https://upstream.example/tag-1.2.3/LICENSE", "upstream_reference": "1.2.3"}]}]}))
        self.assertEqual(self.run_package(supplements=supplement)["notice_byte_coverage"], "ALL_SELECTED_INPUTS_REPRESENTED")
        notice.write_bytes(b"tampered")
        with self.assertRaisesRegex(ValueError, "Supplemental notice checksum"):
            self.run_package("bad", supplements=supplement)
        self.assertFalse((self.root / "bad").exists())

    def test_original_gradle_platform_alias_metadata(self):
        row = self.selected(archive([("LICENSE.txt", b"synthetic license")]))
        path = self.cache / "example.runtime/ui-android/1.2.3/hash/ui-android-1.2.3.module"
        path.parent.mkdir(parents=True)
        path.write_text(json.dumps({"component": {"group": "example.runtime", "module": "ui", "version": "1.2.3",
            "url": "../../ui/1.2.3/ui-1.2.3.module"}, "variants": [{"files": [{"name": row["file"],
            "url": "ui-android-1.2.3.aar", "sha256": row["sha256"]}]}]}))
        self.run_package()
        m = json.loads((self.root / "assets/manifest.json").read_text())
        self.assertTrue(m["components"][0]["artifact_source_url"].endswith("ui-android-1.2.3.aar"))
        self.assertEqual(m["components"][0]["module_component"]["module"], "ui")

    def test_duplicate_zip_entries_rejected(self):
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            self.selected(archive([("LICENSE.txt", b"first"), ("LICENSE.txt", b"second")]))
        with self.assertRaisesRegex(ValueError, "duplicate ZIP"):
            self.run_package()
        self.assertFalse((self.root / "assets").exists())

    def test_declared_license_requires_original_pom_hash_and_url(self):
        row = self.selected(archive([("Example.class", b"synthetic class")]))
        pom = self.cache / "example.runtime/ui-android/1.2.3/hash/ui-android-1.2.3.pom"
        pom.parent.mkdir(parents=True)
        pom.write_bytes(b'<project xmlns="http://maven.apache.org/POM/4.0.0"><groupId>example.runtime</groupId>'
            b'<artifactId>ui-android</artifactId><version>1.2.3</version><licenses><license><name>Apache 2.0</name>'
            b'<url>http://www.apache.org/licenses/LICENSE-2.0.txt</url></license></licenses></project>')
        legal = self.root / "upstream"
        legal.mkdir()
        payload = b"Synthetic declared-license body fixture\n"
        (legal / "Apache-LICENSE-2.0.txt").write_bytes(payload)
        pins = {"Apache-LICENSE-2.0.txt": ("https://www.apache.org/licenses/LICENSE-2.0.txt", hashlib.sha256(payload).hexdigest())}
        with mock.patch.object(tool, "PINNED_UPSTREAM_TEXTS", pins):
            self.run_package(upstream_cache=legal)
            m = json.loads((self.root / "assets/manifest.json").read_text())
            notice = next(n for n in m["components"][0]["notices"] if n.get("role") == "license")
            self.assertEqual(notice["pom_basis"]["sha256"], hashlib.sha256(pom.read_bytes()).hexdigest())
            self.assertEqual(notice["pom_basis"]["declared_license_url"], "http://www.apache.org/licenses/LICENSE-2.0.txt")
            (legal / "Apache-LICENSE-2.0.txt").write_bytes(b"corrupt source text")
            with self.assertRaisesRegex(ValueError, "Pinned upstream source text checksum"):
                self.run_package("corrupt", upstream_cache=legal)
            self.assertFalse((self.root / "corrupt").exists())

    def test_original_source_header_byte_range(self):
        row = self.selected(archive([("Example.class", b"synthetic class")]))
        source = self.root / "public-source.cpp"
        header = b"/* Synthetic copyright/license header fixture. */"
        source.write_bytes(header + b"\nvoid source_fixture() {}\n")
        notice = self.root / "source-notice.txt"
        notice.write_bytes(header)
        item = {"name": "source-header.txt", "role": "license", "notice_path": str(notice),
                "sha256": hashlib.sha256(header).hexdigest(), "source_url": "https://upstream.example/v1.2.3/source.cpp",
                "upstream_reference": "v1.2.3", "source_file_path": str(source),
                "source_file_sha256": hashlib.sha256(source.read_bytes()).hexdigest(), "source_byte_range": [0, len(header)]}
        supplement = self.root / "supplement.json"
        def write():
            supplement.write_text(json.dumps({"schema": 1, "components": [{"coordinate": row["coordinate"],
                                  "artifact_sha256": row["sha256"], "notices": [item]}]}))
        write()
        self.run_package(supplements=supplement)
        item["source_byte_range"][0] = 1
        write()
        with self.assertRaisesRegex(ValueError, "checked original byte range"):
            self.run_package("bad-source-range", supplements=supplement)
        self.assertFalse((self.root / "bad-source-range").exists())


if __name__ == "__main__":
    unittest.main()
