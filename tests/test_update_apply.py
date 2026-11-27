"""更新助手（文件替换/回滚/重启）的单元测试。

不启动真实 exe：用伪造的 PE 文件 + 临时目录验证"备份 → 原子替换 → 回滚"，
以及作业结果文件与版本号落盘。
"""
from __future__ import annotations

import hashlib
import os
import shutil
import struct
import tempfile
import unittest
import unittest.mock

from xfly_client import update_apply as UA
from xfly_client import update_service as US


def make_pe(path: str, payload: bytes = b"fake-pe-body") -> None:
    """造一个能过 MZ/PE 头校验的最小文件。"""
    data = bytearray(b"MZ" + b"\0" * 62)
    struct.pack_into("<I", data, 0x3C, 0x40)
    data.extend(b"\0" * (0x40 - len(data)))
    data.extend(b"PE\0\0")
    data.extend(payload)
    with open(path, "wb") as f:
        f.write(bytes(data))


def read(path: str) -> bytes:
    with open(path, "rb") as f:
        return f.read()


class TestPeCheck(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="xfu-apply-")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_looks_like_pe(self):
        good = os.path.join(self.tmp, "good.exe")
        make_pe(good)
        self.assertTrue(UA.looks_like_pe(good))

        html = os.path.join(self.tmp, "bad.exe")
        with open(html, "w", encoding="utf-8") as f:
            f.write("<html>404</html>")
        self.assertFalse(UA.looks_like_pe(html))

        stub = os.path.join(self.tmp, "stub.exe")
        with open(stub, "wb") as f:
            f.write(b"MZ")
        self.assertFalse(UA.looks_like_pe(stub))
        self.assertFalse(UA.looks_like_pe(os.path.join(self.tmp, "nope.exe")))

    def test_check_package_rejects_non_pe_and_bad_md5(self):
        bad = os.path.join(self.tmp, "bad.exe")
        with open(bad, "w", encoding="utf-8") as f:
            f.write("not a program")
        with self.assertRaises(US.UpdateError) as ctx:
            UA.check_package(bad)
        self.assertEqual(ctx.exception.code, "update_bad_package")

        good = os.path.join(self.tmp, "good.exe")
        make_pe(good)
        good_md5 = hashlib.md5(read(good)).hexdigest()
        UA.check_package(good, good_md5, log=lambda *a, **k: None)   # 不抛
        with self.assertRaises(US.UpdateError) as ctx:
            UA.check_package(good, "0" * 32, log=lambda *a, **k: None)
        self.assertEqual(ctx.exception.code, "update_bad_md5")

    def test_check_package_missing_or_empty(self):
        with self.assertRaises(US.UpdateError):
            UA.check_package(os.path.join(self.tmp, "nope.exe"))
        empty = os.path.join(self.tmp, "empty.exe")
        open(empty, "wb").close()
        with self.assertRaises(US.UpdateError):
            UA.check_package(empty)


class TestWait(unittest.TestCase):
    def test_pid_zero_means_no_wait(self):
        self.assertTrue(UA.wait_for_process_exit(0))

    def test_current_process_times_out(self):
        self.assertFalse(UA.wait_for_process_exit(os.getpid(), timeout=0.3))

    def test_dead_pid(self):
        self.assertTrue(UA.wait_for_process_exit(0x7FFFFFF, timeout=0.3))


class TestSwap(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="xfu-swap-")
        self.target = os.path.join(self.tmp, "XFlySimClient.exe")
        self.package = os.path.join(self.tmp, "pkg.exe")
        make_pe(self.target, b"old-version")
        make_pe(self.package, b"new-version")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_swap_replaces_and_drops_backup(self):
        UA.swap_files(self.package, self.target, log=lambda *a, **k: None)
        self.assertIn(b"new-version", read(self.target))
        self.assertFalse(os.path.exists(self.target + ".bak"))
        self.assertFalse(os.path.exists(self.target + ".update-new"))

    def test_swap_creates_missing_target(self):
        os.remove(self.target)
        UA.swap_files(self.package, self.target, log=lambda *a, **k: None)
        self.assertTrue(os.path.isfile(self.target))
        self.assertFalse(os.path.exists(self.target + ".bak"))

    def test_swap_rolls_back_when_rename_fails(self):
        calls = []

        def flaky(src, dst, *a, **kw):
            calls.append((src, dst))
            if len(calls) == 2:            # 第二次 = staged → target 失败
                raise US.UpdateError("被占用", "update_locked")
            os.replace(src, dst)

        with unittest.mock.patch.object(UA, "_replace_with_retry", flaky):
            with self.assertRaises(US.UpdateError):
                UA.swap_files(self.package, self.target,
                              log=lambda *a, **k: None)
        self.assertIn(b"old-version", read(self.target))    # 旧版回来了
        self.assertFalse(os.path.exists(self.target + ".bak"))
        self.assertFalse(os.path.exists(self.target + ".update-new"))


class TestRunJob(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="xfu-run-")
        self.appdir = os.path.join(self.tmp, "app")
        os.makedirs(self.appdir)
        self.target = os.path.join(self.appdir, "XFlySimClient.exe")
        self.package = os.path.join(self.tmp, "pkg.exe")
        make_pe(self.target, b"old")
        make_pe(self.package, b"new")
        self._env = unittest.mock.patch.dict(
            os.environ, {"APPDATA": os.path.join(self.tmp, "appdata"),
                         "XDG_CONFIG_HOME": os.path.join(self.tmp, "xdg")})
        self._env.start()

    def tearDown(self):
        self._env.stop()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _job(self, **over):
        data = {"package": self.package, "target": self.target,
                "version": "1.0.1",
                "md5": hashlib.md5(read(self.package)).hexdigest(),
                "wait_pid": 0, "restart": False}
        data.update(over)
        return US.UpdateJob(**data)

    def test_success_writes_result_and_version(self):
        job_path = os.path.join(self.tmp, "job.json")
        US.write_job(self._job(), job_path)
        rc = UA.run_job_file(job_path, log=lambda *a, **k: None)
        self.assertEqual(rc, 0)
        self.assertIn(b"new", read(self.target))
        res = US.read_result()
        self.assertTrue(res["ok"])
        self.assertEqual(res["version"], "1.0.1")
        self.assertEqual(US.read_installed_version(), "1.0.1")

    def test_failure_keeps_old_file_and_reports(self):
        bad_pkg = os.path.join(self.tmp, "bad.exe")
        with open(bad_pkg, "w", encoding="utf-8") as f:
            f.write("<html>404</html>")
        job_path = os.path.join(self.tmp, "job.json")
        US.write_job(self._job(package=bad_pkg, md5=""), job_path)
        with unittest.mock.patch.object(UA, "show_error_dialog", lambda *a, **k: None):
            rc = UA.run_job_file(job_path, log=lambda *a, **k: None)
        self.assertEqual(rc, 2)
        self.assertIn(b"old", read(self.target))     # 原文件没动
        res = US.read_result()
        self.assertFalse(res["ok"])
        self.assertIn("PE", res["message"])

    def test_missing_job_file(self):
        with unittest.mock.patch.object(UA, "show_error_dialog", lambda *a, **k: None):
            rc = UA.run_job_file(os.path.join(self.tmp, "nope.json"),
                                 log=lambda *a, **k: None)
        self.assertEqual(rc, 2)

    def test_xupdate_cli_delegates_to_helper(self):
        """XUpdate.exe --apply-package 是助手入口，CLI 层也要真的替换。"""
        import xupdate
        job_path = os.path.join(self.tmp, "job.json")
        US.write_job(self._job(), job_path)
        rc = xupdate.main(["--apply-package", job_path, "--quiet"])
        self.assertEqual(rc, 0)
        self.assertIn(b"new", read(self.target))
        self.assertEqual(US.read_installed_version(), "1.0.1")


if __name__ == "__main__":
    unittest.main()
