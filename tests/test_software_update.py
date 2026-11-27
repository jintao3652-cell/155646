"""软件中心发布包解析器测试：latest/logs 解析、md5、zip/非 zip 落盘。"""
from __future__ import annotations

import hashlib
import io
import json
import os
import tempfile
import unittest
import unittest.mock
import zipfile

from xfly_client import software_update as S
from xfly_client import updater as U


def _md5(data: bytes) -> str:
    return hashlib.md5(data).hexdigest()


def _release(data: bytes, version: str = "1.0.1",
             link: str = "https://example.test/download/XFlySimClient.exe") -> S.Release:
    return S.Release("XFlySim Client", version, "修复若干问题", link,
                     _md5(data), "2026-11-27 15:30:00")


def _zip_bytes(files: dict[str, bytes]) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        for name, content in files.items():
            zf.writestr(name, content)
    return buf.getvalue()


class TestReleaseParsing(unittest.TestCase):
    def test_parse_release(self):
        rel = S.parse_release({
            "softwareName": "XFlySim Client", "version": "1.0.1",
            "logs": "修复若干问题", "link": "https://example.test/pkg.zip",
            "md5": "A" * 32, "createdTime": "2026-11-27 15:30:00",
        })
        self.assertEqual(rel.md5_hex, "a" * 32)
        self.assertEqual(rel.version, "1.0.1")

    def test_parse_release_rejects_missing_and_bad_md5(self):
        base = {"version": "1.0.1", "link": "https://example.test/a.zip",
                "md5": "a" * 32}
        for key in ("version", "link", "md5"):
            data = dict(base)
            data[key] = ""
            with self.assertRaises(S.SoftwareUpdateError):
                S.parse_release(data)
        data = dict(base, md5="xyz")
        with self.assertRaises(S.SoftwareUpdateError):
            S.parse_release(data)

    def test_parse_logs(self):
        records = S.parse_logs({"softwareName": "XFlySim Client", "records": [
            {"version": "1.0.1", "logs": "修复", "createdTime": "2026-11-27"},
        ]})
        self.assertEqual(records[0]["version"], "1.0.1")
        with self.assertRaises(S.SoftwareUpdateError):
            S.parse_logs({"softwareName": "x"})

    def test_fetch_uses_documented_endpoints(self):
        seen: list[str] = []

        def fetch(url: str, timeout: int) -> bytes:
            seen.append(url)
            if "/logs/" in url:
                return json.dumps({"records": []}).encode("utf-8")
            return json.dumps({
                "version": "1.0.1", "link": "https://example.test/a.zip",
                "md5": "a" * 32,
            }).encode("utf-8")

        S.fetch_release("https://host.test/api/", "abc-123", fetch)
        self.assertEqual(seen[-1],
                         "https://host.test/api/softwareVersion/latest/abc-123")
        S.fetch_logs("https://host.test/api", "abc-123", fetch)
        self.assertEqual(seen[-1],
                         "https://host.test/api/softwareVersion/logs/abc-123")
        S.fetch_release("https://host.test/api", "abc-123", fetch, legacy=True)
        self.assertEqual(seen[-1], "https://host.test/api/software/latest/abc-123")

    def test_relative_link_resolved_against_api_base(self):
        self.assertEqual(
            S.resolve_download_url("https://host.test/api", "files/pkg.zip"),
            "https://host.test/api/files/pkg.zip")
        self.assertEqual(
            S.resolve_download_url("https://host.test/api",
                                   "https://cdn.test/pkg.zip"),
            "https://cdn.test/pkg.zip")


class TestDownload(unittest.TestCase):
    def test_download_checks_md5(self):
        data = b"release payload"
        rel = _release(data)
        fetched = lambda url, timeout: data
        self.assertEqual(S.download_package(rel, fetched, 5), data)
        with self.assertRaises(S.SoftwareUpdateError):
            S.download_package(_release(b"expected"), lambda u, t: b"tampered", 5)

    def test_download_rejects_empty(self):
        rel = _release(b"x")
        with self.assertRaises(S.SoftwareUpdateError):
            S.download_package(rel, lambda u, t: b"", 5)


class TestPackageFiles(unittest.TestCase):
    def test_non_zip_uses_link_filename(self):
        data = b"single binary"
        files = S.package_files(
            data, "https://example.test/download/XFlySimClient.exe")
        self.assertEqual(files, [("XFlySimClient.exe", data)])
        self.assertFalse(S.is_zip(data))

    def test_non_zip_without_filename_rejected(self):
        with self.assertRaises(S.SoftwareUpdateError):
            S.package_files(b"data", "https://example.test/?id=42")

    def test_zip_expands_to_root_relative_paths(self):
        data = _zip_bytes({
            "XFlySimClient.exe": b"exe",
            "webui/dist/index.html": b"<html/>",
            "readme.txt": b"hi",
        })
        self.assertTrue(S.is_zip(data))
        files = dict(S.package_files(data, "https://example.test/pkg.zip"))
        self.assertEqual(files["XFlySimClient.exe"], b"exe")
        self.assertEqual(files["webui/dist/index.html"], b"<html/>")
        self.assertEqual(files["readme.txt"], b"hi")

    def test_zip_slip_rejected(self):
        for name in ("../evil.exe", "/etc/passwd", "C:/evil.exe",
                     "ok/../../evil.exe"):
            with self.assertRaises(U.UpdateError, msg=name):
                S.package_files(
                    _zip_bytes({name: b"bad"}),
                    "https://example.test/pkg.zip")


class TestSoftwareCli(unittest.TestCase):
    def test_check_apply_and_history_via_cli(self):
        import pathlib
        import subprocess
        import sys

        root_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        script = os.path.join(root_dir, "xupdate.py")
        with tempfile.TemporaryDirectory() as tmp:
            api = os.path.join(tmp, "api")
            latest_dir = os.path.join(api, "softwareVersion", "latest")
            logs_dir = os.path.join(api, "softwareVersion", "logs")
            os.makedirs(latest_dir)
            os.makedirs(logs_dir)
            base = os.path.join(tmp, "root")
            os.makedirs(base)

            pkg = os.path.join(tmp, "pkg.zip")
            with open(pkg, "wb") as f:
                f.write(_zip_bytes({"XFlySimClient.exe": b"new exe"}))
            with open(pkg, "rb") as f:
                digest = _md5(f.read())
            link = pathlib.Path(pkg).as_uri()
            with open(os.path.join(latest_dir, "uuid-1"), "w",
                      encoding="utf-8") as f:
                json.dump({
                    "softwareName": "XFlySim Client", "version": "9.9.9",
                    "logs": "修复若干问题", "link": link, "md5": digest,
                    "createdTime": "2026-11-27 15:30:00",
                }, f)
            with open(os.path.join(logs_dir, "uuid-1"), "w",
                      encoding="utf-8") as f:
                json.dump({"softwareName": "XFlySim Client", "records": [
                    {"version": "9.9.9", "logs": "修复若干问题",
                     "createdTime": "2026-11-27 15:30:00"},
                ]}, f)

            api_uri = pathlib.Path(api).as_uri()

            def run(*args: str) -> subprocess.CompletedProcess:
                return subprocess.run(
                    [sys.executable, script, "--software-api", api_uri,
                     "--software-uuid", "uuid-1", "--base-dir", base,
                     *args, "--json"],
                    capture_output=True, text=True, cwd=root_dir)

            checked = run("--check")
            self.assertEqual(checked.returncode, 1, checked.stderr)
            report = json.loads(checked.stdout)
            self.assertTrue(report["update_available"])
            self.assertEqual(report["version"], "9.9.9")

            history = run("--software-history")
            self.assertEqual(history.returncode, 0, history.stderr)
            self.assertEqual(
                json.loads(history.stdout)["records"][0]["version"], "9.9.9")

            applied = run("--apply")
            self.assertEqual(applied.returncode, 0, applied.stderr)
            result = json.loads(applied.stdout)
            self.assertTrue(result["ok"])
            self.assertTrue(result["zipped"])
            with open(os.path.join(base, "XFlySimClient.exe"), "rb") as f:
                self.assertEqual(f.read(), b"new exe")


class TestApply(unittest.TestCase):
    def test_non_zip_lands_in_root(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = b"new client binary"
            target = os.path.join(tmp, "XFlySimClient.exe")
            with open(target, "wb") as f:
                f.write(b"old")
            result = S.apply_package(_release(data), data, tmp)
            self.assertTrue(result.ok)
            self.assertFalse(result.zipped)
            with open(target, "rb") as f:
                self.assertEqual(f.read(), data)
            self.assertTrue(os.path.exists(target + ".xupdate-bak"))
            self.assertEqual(U.installed_versions(tmp)["client"], "1.0.1")

    def test_zip_lands_in_root_without_extra_folder(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = _zip_bytes({
                "XFlySimClient.exe": b"exe",
                "webui/dist/index.html": b"<html/>",
            })
            result = S.apply_package(
                _release(data, link="https://example.test/pkg.zip"), data, tmp)
            self.assertTrue(result.ok)
            self.assertTrue(result.zipped)
            self.assertEqual(len(result.replaced), 2)
            self.assertTrue(os.path.isfile(
                os.path.join(tmp, "XFlySimClient.exe")))
            self.assertTrue(os.path.isfile(
                os.path.join(tmp, "webui", "dist", "index.html")))
            self.assertFalse(os.path.exists(os.path.join(tmp, "pkg")))

    def test_md5_mismatch_writes_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            rel = S.Release("XFlySim Client", "1.0.1", "", "https://x/a.exe",
                            "0" * 32, "")
            result = S.apply_package(rel, b"payload", tmp)
            self.assertFalse(result.ok)
            self.assertFalse(result.md5_ok)
            self.assertEqual(os.listdir(tmp), [])

    def test_failure_rolls_back(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = b"payload"
            target = os.path.join(tmp, "XFlySimClient.exe")
            with open(target, "wb") as f:
                f.write(b"old")
            with unittest.mock.patch.object(U, "_atomic_write",
                                            side_effect=OSError("boom")):
                result = S.apply_package(_release(data), data, tmp)
            self.assertFalse(result.ok)
            with open(target, "rb") as f:
                self.assertEqual(f.read(), b"old")
            self.assertEqual(result.replaced, [])


if __name__ == "__main__":
    unittest.main()
