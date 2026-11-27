"""XUpdate 更新器单元测试：清单校验、增量计划、原子替换与回滚。"""
from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
import tempfile
import unittest
import unittest.mock

from xfly_client import updater as U


def _entry(path: str, data: bytes, url: str = "") -> dict:
    return {
        "path": path,
        "sha256": hashlib.sha256(data).hexdigest(),
        "bytes": len(data),
        "url": url,
    }


def _manifest(files: list[dict], version: str = "2.0.0",
              component: str = "client") -> dict:
    return U.parse_manifest({
        "schema": U.UPDATE_SCHEMA,
        "schema_version": U.UPDATE_SCHEMA_VERSION,
        "base_url": "https://example.test/",
        "components": {component: {"version": version, "files": files}},
    })


class TestManifest(unittest.TestCase):
    def test_sample_manifest_is_valid(self):
        parsed = U.parse_manifest(U.sample_manifest())
        self.assertEqual(set(parsed["components"]), set(U.COMPONENTS))

    def test_rejects_unsafe_paths_and_bad_hash(self):
        base = {
            "schema": U.UPDATE_SCHEMA,
            "schema_version": U.UPDATE_SCHEMA_VERSION,
            "components": {"client": {"version": "1", "files": []}},
        }
        for bad_path in ("../evil.exe", "/etc/passwd", "C:/evil.exe", ".."):
            data = json.loads(json.dumps(base))
            data["components"]["client"]["files"] = [
                {"path": bad_path, "sha256": "0" * 64, "bytes": 1}]
            with self.assertRaises(U.UpdateError, msg=bad_path):
                U.parse_manifest(data)
        data = json.loads(json.dumps(base))
        data["components"]["client"]["files"] = [
            {"path": "ok.exe", "sha256": "xyz", "bytes": 1}]
        with self.assertRaises(U.UpdateError):
            U.parse_manifest(data)

    def test_read_manifest_from_file_url(self):
        import pathlib
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "manifest.json")
            with open(path, "w", encoding="utf-8") as f:
                json.dump({
                    "schema": U.UPDATE_SCHEMA,
                    "schema_version": U.UPDATE_SCHEMA_VERSION,
                    "components": {"client": {"version": "3.0.0", "files": []}},
                }, f)
            parsed = U.read_manifest(pathlib.Path(path).as_uri())
            self.assertEqual(parsed["components"]["client"]["version"], "3.0.0")

    def test_version_comparison(self):
        self.assertEqual(U.parse_version("v29"), (29,))
        self.assertEqual(U.parse_version("1.2.10"), (1, 2, 10))
        self.assertTrue(U.same_version("v29", "29"))
        self.assertTrue(U.same_version("1.2.10", "v1.2.10"))
        self.assertFalse(U.same_version(None, "1.0.0"))

    def test_safe_join_rejects_symlink_escape(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = os.path.join(tmp, "root")
            outside = os.path.join(tmp, "outside")
            os.makedirs(root)
            os.makedirs(outside)
            try:
                os.symlink(outside, os.path.join(root, "link"))
            except OSError:
                self.skipTest("symlink not permitted")
            with self.assertRaises(U.UpdateError):
                U.safe_join(root, "link/evil.exe")


class TestPlan(unittest.TestCase):
    def test_skips_unchanged_and_plans_changed(self):
        with tempfile.TemporaryDirectory() as tmp:
            old = b"old client"
            path = os.path.join(tmp, "XFlySimClient.exe")
            with open(path, "wb") as f:
                f.write(old)
            manifest = _manifest([_entry("XFlySimClient.exe", old)])
            plan = U.build_plans(manifest, base_dir=tmp,
                                 components=["client"])[0]
            # 本地版本未知（没装过 xupdate-installed.json）：内容不变也不宣称"已是最新"
            self.assertFalse(plan.up_to_date)
            self.assertEqual(len(plan.needed), 0)
            self.assertEqual(plan.actions[0].action, "skip")

            new = b"new client"
            manifest = _manifest([_entry("XFlySimClient.exe", new)])
            plan = U.build_plans(manifest, base_dir=tmp,
                                 components=["client"])[0]
            self.assertEqual(len(plan.needed), 1)
            self.assertEqual(plan.actions[0].reason, "content mismatch")


class TestApply(unittest.TestCase):
    def test_apply_replaces_backs_up_and_records_version(self):
        with tempfile.TemporaryDirectory() as tmp:
            old = b"old"
            new = b"brand new binary"
            target = os.path.join(tmp, "XFlySimClient.exe")
            with open(target, "wb") as f:
                f.write(old)
            manifest = _manifest([_entry("XFlySimClient.exe", new)], "2.0.0")
            plan = U.build_plans(manifest, base_dir=tmp, components=["client"])

            fetched = []
            def fetch(url, timeout):
                fetched.append(url)
                return new

            result = U.apply_plans(manifest, plan, base_dir=tmp, fetch=fetch)
            self.assertFalse(result.error)
            self.assertEqual(len(fetched), 1)
            with open(target, "rb") as f:
                self.assertEqual(f.read(), new)
            with open(target + ".xupdate-bak", "rb") as f:
                self.assertEqual(f.read(), old)
            versions = U.installed_versions(tmp)
            self.assertEqual(versions["client"], "2.0.0")

            # 第二次 apply：内容已一致，不再下载
            plan2 = U.build_plans(manifest, base_dir=tmp, components=["client"])
            result2 = U.apply_plans(manifest, plan2, base_dir=tmp, fetch=fetch)
            self.assertEqual(result2.downloaded, 0)
            self.assertEqual(len(fetched), 1)

    def test_apply_rolls_back_when_download_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            old_a, old_b = b"a-old", b"b-old"
            new_a, new_b = b"a-new", b"b-new"
            a = os.path.join(tmp, "a.exe")
            b = os.path.join(tmp, "b.dll")
            for path, data in ((a, old_a), (b, old_b)):
                with open(path, "wb") as f:
                    f.write(data)
            manifest = _manifest([_entry("a.exe", new_a, "https://x/a"),
                                  _entry("b.dll", new_b, "https://x/b")])
            plan = U.build_plans(manifest, base_dir=tmp, components=["client"])

            def fetch(url, timeout):
                if url.endswith("/b"):
                    raise U.UpdateError("simulated network failure")
                return new_a

            result = U.apply_plans(manifest, plan, base_dir=tmp, fetch=fetch)
            self.assertTrue(result.error)
            self.assertEqual(result.replaced, [])
            with open(a, "rb") as f:
                self.assertEqual(f.read(), old_a)
            with open(b, "rb") as f:
                self.assertEqual(f.read(), old_b)
            self.assertFalse(os.path.exists(os.path.join(tmp, U.INSTALLED_FILE)))

    def test_xplane_root_precondition(self):
        with tempfile.TemporaryDirectory() as tmp:
            manifest = _manifest([_entry("Resources/plugins/XConnect/win_x64/XConnect.xpl",
                                         b"x")], component="xconnect")
            with unittest.mock.patch.dict(os.environ, {"XPLANE_ROOT": ""}, clear=False):
                with self.assertRaises(U.PreconditionError):
                    U.build_plans(manifest, base_dir=tmp, components=["xconnect"])


class TestCli(unittest.TestCase):
    def test_check_json_reports_update(self):
        import xupdate
        with tempfile.TemporaryDirectory() as tmp:
            new = b"v2"
            manifest_path = os.path.join(tmp, "m.json")
            with open(manifest_path, "w", encoding="utf-8") as f:
                json.dump({
                    "schema": U.UPDATE_SCHEMA,
                    "schema_version": U.UPDATE_SCHEMA_VERSION,
                    "base_url": "https://example.test/",
                    "components": {"client": {
                        "version": "9.9.9", "files": [_entry("XFlySimClient.exe", new)]}},
                }, f)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = xupdate.main(["--check", "--json", "--manifest",
                                     manifest_path, "--base-dir", tmp])
            self.assertEqual(code, 1)
            data = json.loads(out.getvalue())
            self.assertTrue(data["update_available"])
            self.assertEqual(data["plans"][0]["component"], "client")


if __name__ == "__main__":
    unittest.main()
