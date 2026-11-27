"""资源层启动零哈希、主动校验与包级过滤测试。"""
from __future__ import annotations

import hashlib
import json
import os
import tempfile
import unittest
from unittest.mock import patch

from xfly_client import resources as R


def _write_json(path, obj):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False)


def _make_resource(root, kind=R.CSL, version="v1", packages=("B738",),
                   with_index=True):
    os.makedirs(root, exist_ok=True)
    manifest = {
        "schema": R.SCHEMA_MANIFEST,
        "version": R.SCHEMA_VERSION,
        "kind": kind,
        "resource_version": version,
        "packages": [{"id": p, "version": "1", "files": 0, "bytes": 0}
                     for p in packages],
        "index": R.VERIFY_NAME,
    }
    _write_json(os.path.join(root, R.MANIFEST_NAME), manifest)
    index = {
        "schema": R.SCHEMA_VERIFY,
        "version": R.SCHEMA_VERSION,
        "kind": kind,
        "resource_version": version,
        "files": {},
    }
    if with_index:
        _write_json(os.path.join(root, R.VERIFY_NAME), index)
    return manifest, index


class TestQuickCheck(unittest.TestCase):
    def test_missing_root(self):
        with tempfile.TemporaryDirectory() as d:
            st = R.quick_check(R.CSL, os.path.join(d, "nope"))
        self.assertEqual(st.state, "missing")
        self.assertFalse(st.present)

    def test_unmanaged_directory_is_not_touched(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "B738"))
            st = R.quick_check(R.CSL, d)
        self.assertEqual(st.state, "unmanaged")
        self.assertTrue(st.present)
        self.assertIn("不覆盖", st.reason)

    def test_valid_manifest_reports_version_without_hashing(self):
        with tempfile.TemporaryDirectory() as d:
            _make_resource(d, version="2026-11-01", packages=("B738", "A320"))
            os.makedirs(os.path.join(d, "B738"), exist_ok=True)
            with open(os.path.join(d, "B738", "xsb_aircraft.txt"), "w") as f:
                f.write("original")
            # 启动路径上任何哈希调用都应视为回归
            with patch.object(R.hashlib, "sha256", side_effect=AssertionError):
                st = R.quick_check(R.CSL, d)
            self.assertEqual(st.state, "ok")
            self.assertEqual(st.resource_version, "2026-11-01")
            self.assertEqual(st.package_count, 2)
            self.assertTrue(st.has_verify_index)

            # 篡改文件不改变启动检查结论 —— 证明没有逐文件校验
            with open(os.path.join(d, "B738", "xsb_aircraft.txt"), "w") as f:
                f.write("tampered")
            st2 = R.quick_check(R.CSL, d)
            self.assertEqual(st2.state, "ok")

    def test_expected_version_marks_outdated_not_download(self):
        with tempfile.TemporaryDirectory() as d:
            _make_resource(d, version="v1")
            st = R.quick_check(R.CSL, d, expected_version="v2")
        self.assertEqual(st.state, "outdated")
        self.assertIn("v2", st.reason)

    def test_invalid_manifest(self):
        with tempfile.TemporaryDirectory() as d:
            _write_json(os.path.join(d, R.MANIFEST_NAME),
                        {"schema": R.SCHEMA_MANIFEST, "version": 99})
            st = R.quick_check(R.CSL, d)
        self.assertEqual(st.state, "invalid_manifest")

    def test_discover_root_from_env(self):
        with tempfile.TemporaryDirectory() as d:
            with patch.dict(os.environ, {"XFLYSIM_FSLTL_ROOT": d}):
                self.assertEqual(R.discover_root(R.FSLTL), d)
                st = R.quick_check(R.FSLTL)
            self.assertEqual(st.state, "unmanaged")
            self.assertEqual(st.root, d)


class TestVerify(unittest.TestCase):
    def _resource_with_files(self, root):
        p1 = os.path.join(root, "B738", "xsb_aircraft.txt")
        p2 = os.path.join(root, "A320", "xsb_aircraft.txt")
        os.makedirs(os.path.dirname(p1), exist_ok=True)
        os.makedirs(os.path.dirname(p2), exist_ok=True)
        with open(p1, "wb") as f:
            f.write(b"B738 model\n")
        with open(p2, "wb") as f:
            f.write(b"A320 model\n")
        manifest, index = _make_resource(
            root, packages=("B738", "A320"))
        for rel, data in (("B738/xsb_aircraft.txt", b"B738 model\n"),
                          ("A320/xsb_aircraft.txt", b"A320 model\n")):
            index["files"][rel] = {
                "sha256": hashlib.sha256(data).hexdigest(),
                "bytes": len(data),
                "pkg": rel.split("/", 1)[0],
            }
        _write_json(os.path.join(root, R.VERIFY_NAME), index)
        return manifest, index

    def test_verify_ok(self):
        with tempfile.TemporaryDirectory() as d:
            self._resource_with_files(d)
            rep = R.verify(R.CSL, d)
        self.assertTrue(rep.ok)
        self.assertFalse(rep.cancelled)
        self.assertEqual(rep.files, 2)
        self.assertEqual(set(rep.packages), {"B738", "A320"})
        self.assertTrue(all(p.ok for p in rep.packages.values()))

    def test_verify_detects_missing_and_tampered(self):
        with tempfile.TemporaryDirectory() as d:
            self._resource_with_files(d)
            os.remove(os.path.join(d, "A320", "xsb_aircraft.txt"))
            with open(os.path.join(d, "B738", "xsb_aircraft.txt"), "wb") as f:
                f.write(b"tampered")
            rep = R.verify(R.CSL, d)
        self.assertFalse(rep.ok)
        self.assertEqual(rep.packages["A320"].missing, 1)
        self.assertEqual(rep.packages["B738"].mismatch, 1)

    def test_only_packages_filter(self):
        with tempfile.TemporaryDirectory() as d:
            self._resource_with_files(d)
            progress = []
            rep = R.verify(R.CSL, d, only_packages={"A320"},
                           on_progress=lambda done, total, pkg:
                           progress.append((done, total, pkg)))
        self.assertEqual(set(rep.packages), {"A320"})
        self.assertEqual(rep.files, 1)
        self.assertEqual(progress, [(1, 1, "A320")])

    def test_cancel_returns_partial_report(self):
        with tempfile.TemporaryDirectory() as d:
            self._resource_with_files(d)
            state = {"n": 0}

            def cancel():
                state["n"] += 1
                return state["n"] > 2

            rep = R.verify(R.CSL, d, should_cancel=cancel,
                           chunk_size=4)
        self.assertTrue(rep.cancelled)
        self.assertFalse(rep.ok)
        self.assertIn("取消", rep.reason)

    def test_throttle_still_verifies(self):
        with tempfile.TemporaryDirectory() as d:
            self._resource_with_files(d)
            rep = R.verify(R.CSL, d, max_bytes_per_sec=10_000_000,
                           chunk_size=4)
        self.assertTrue(rep.ok)

    def test_path_traversal_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            _, index = self._resource_with_files(d)
            index["files"]["../../secret.txt"] = {
                "sha256": hashlib.sha256(b"x").hexdigest(),
                "bytes": 1, "pkg": "B738",
            }
            _write_json(os.path.join(d, R.VERIFY_NAME), index)
            rep = R.verify(R.CSL, d)
        self.assertFalse(rep.ok)
        self.assertEqual(rep.packages["B738"].missing, 1)

    def test_bad_index(self):
        with tempfile.TemporaryDirectory() as d:
            _make_resource(d)
            _write_json(os.path.join(d, R.VERIFY_NAME), {"schema": "wrong"})
            rep = R.verify(R.CSL, d)
        self.assertFalse(rep.ok)
        self.assertIn("索引无效", rep.reason)


class TestDiagnostics(unittest.TestCase):
    def test_summary_has_no_absolute_paths(self):
        with tempfile.TemporaryDirectory() as d:
            _make_resource(d, version="v1")
            with patch.dict(os.environ, {"XFLYSIM_FSLTL_ROOT": d,
                                         "XFLYSIM_AIG_ROOT": d,
                                         "XFLYSIM_CSL_ROOT": d}):
                summary = R.diagnostics_summary()
        self.assertEqual(len(summary), 3)
        self.assertTrue(all("root" not in x for x in summary))
        self.assertTrue(all(x["state"] == "ok" for x in summary))
        self.assertTrue(all("path" not in str(x).lower() for x in summary))


if __name__ == "__main__":
    unittest.main()
