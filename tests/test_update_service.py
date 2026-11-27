"""客户端在线更新的单元测试：版本比较、接口解析、续传下载、MD5 校验、作业与拉起。

全部离线：下载用本机 http.server（支持/不支持 Range 两种），不访问外网。
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import tempfile
import threading
import unittest
import unittest.mock
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from xfly_client import update_service as US

PAYLOAD = (b"MZ" + bytes(64) + b"PE\0\0" + b"unit-test-payload-" * 8192)
PAYLOAD_MD5 = hashlib.md5(PAYLOAD).hexdigest()


def _sample(**over):
    data = {
        "softwareName": "XFlySim Client",
        "version": "1.0.1",
        "logs": "修复若干问题",
        "link": "https://your.host/xflysim/releases/XFlySimClient.exe",
        "md5": PAYLOAD_MD5,
        "createdTime": "2026-11-27 15:30:00",
    }
    data.update(over)
    return data


class _PkgHandler(BaseHTTPRequestHandler):
    """最小 HTTP 文件服务：可开关 Range，用于验证续传分支。"""

    payload = PAYLOAD
    honor_range = True
    fail_status = 0
    seen_ranges: list[str | None] = []

    def do_GET(self):                      # noqa: N802 (http.server 约定)
        _PkgHandler.seen_ranges.append(self.headers.get("Range"))
        if _PkgHandler.fail_status:
            self.send_error(_PkgHandler.fail_status)
            return
        data = _PkgHandler.payload
        start = 0
        rng = self.headers.get("Range")
        if rng and _PkgHandler.honor_range:
            m = re.match(r"bytes=(\d+)-", rng)
            if m:
                start = int(m.group(1))
            if start >= len(data):
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{len(data)}")
                self.end_headers()
                return
            self.send_response(206)
            self.send_header(
                "Content-Range", f"bytes {start}-{len(data) - 1}/{len(data)}")
            body = data[start:]
        else:
            self.send_response(200)
            body = data
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):           # 静音
        pass


class _ServerMixin:
    payload = PAYLOAD
    honor_range = True

    @classmethod
    def setUpClass(cls):
        _PkgHandler.payload = cls.payload
        _PkgHandler.honor_range = cls.honor_range
        _PkgHandler.fail_status = 0
        _PkgHandler.seen_ranges = []
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), _PkgHandler)
        cls.port = cls.server.server_address[1]
        cls.thread = threading.Thread(target=cls.server.serve_forever,
                                      daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def url(self, path="/pkg.exe"):
        return f"http://127.0.0.1:{self.port}{path}"


# ---------------------------------------------------------------- 版本

class TestVersion(unittest.TestCase):
    def test_version_key(self):
        self.assertEqual(US.version_key("1.2.10"), (1, 2, 10))
        self.assertEqual(US.version_key("v1.0"), (1, 0))
        self.assertEqual(US.version_key("1.0.1-beta"), (1, 0, 1))
        self.assertEqual(US.version_key("2026-11-27"), (2026,))  # 非语义版本
        self.assertEqual(US.version_key(""), ())
        self.assertEqual(US.version_key(None), ())
        self.assertEqual(US.version_key("abc"), ())

    def test_compare_and_is_newer(self):
        self.assertEqual(US.compare_versions("1.0.1", "1.0.0"), 1)
        self.assertEqual(US.compare_versions("1.0.0", "1.0.1"), -1)
        self.assertEqual(US.compare_versions("1.0.0", "v1.0.0"), 0)
        self.assertEqual(US.compare_versions("1.10", "1.9"), 1)   # 数字而非字典序
        self.assertEqual(US.compare_versions("1.0", "1"), 0)      # 补零对齐
        self.assertTrue(US.is_newer("1.0.1", "1.0.0"))
        self.assertFalse(US.is_newer("1.0.0", "1.0.0"))
        self.assertFalse(US.is_newer("0.9.9", "1.0.0"))           # 不降级
        with self.assertRaises(US.UpdateError):
            US.compare_versions("", "1.0.0")


# ---------------------------------------------------------------- 接口解析

class TestParseInfo(unittest.TestCase):
    def test_valid_payload(self):
        info = US.parse_update_info(_sample(md5=PAYLOAD_MD5.upper()))
        self.assertEqual(info.version, "1.0.1")
        self.assertEqual(info.software_name, "XFlySim Client")
        self.assertEqual(info.logs, "修复若干问题")
        self.assertEqual(info.md5, PAYLOAD_MD5)          # 统一小写
        self.assertEqual(info.created_time, "2026-11-27 15:30:00")
        self.assertEqual(info.as_dict()["softwareName"], "XFlySim Client")

    def test_missing_or_bad_version(self):
        for bad in ({"version": ""}, {"version": "latest"}, {"version": None}):
            with self.assertRaises(US.UpdateError) as ctx:
                US.parse_update_info(_sample(**bad))
            self.assertEqual(ctx.exception.code, "update_bad_response")

    def test_bad_md5_and_link(self):
        with self.assertRaises(US.UpdateError) as ctx:
            US.parse_update_info(_sample(md5="not-a-md5"))
        self.assertEqual(ctx.exception.code, "update_bad_response")
        with self.assertRaises(US.UpdateError) as ctx:
            US.parse_update_info(_sample(link="ftp://x/pkg.exe"))
        self.assertEqual(ctx.exception.code, "update_bad_link")

    def test_logs_may_be_object(self):
        info = US.parse_update_info(_sample(logs={"a": 1}))
        self.assertIn('"a": 1', info.logs)

    def test_not_a_dict(self):
        with self.assertRaises(US.UpdateError):
            US.parse_update_info([])

    def test_package_filename(self):
        self.assertEqual(
            US.package_filename("https://h/x/XFlySimClient.exe?t=1", "1.0.1"),
            "XFlySimClient.exe")
        self.assertEqual(
            US.package_filename("https://h/x/%E5%AE%89%E8%A3%85.exe", "1"),
            "__.exe")
        self.assertTrue(US.package_filename("https://h/x/", "1.0.1").endswith(".exe"))


# ---------------------------------------------------------------- 下载

class TestDownload(_ServerMixin, unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="xfu-test-")
        _PkgHandler.seen_ranges = []
        _PkgHandler.honor_range = True
        _PkgHandler.fail_status = 0
        self._env = unittest.mock.patch.dict(
            os.environ, {"no_proxy": "127.0.0.1,localhost",
                         "NO_PROXY": "127.0.0.1,localhost"})
        self._env.start()

    def tearDown(self):
        self._env.stop()
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def dest(self):
        return os.path.join(self.tmp, "pkg.exe")

    def test_full_download_and_md5(self):
        seen = []
        dl = US.Downloader(self.url(), self.dest(), PAYLOAD_MD5)
        path = dl.download(on_progress=lambda rcv, total: seen.append((rcv, total)))
        self.assertTrue(os.path.isfile(path))
        with open(path, "rb") as f:
            self.assertEqual(f.read(), PAYLOAD)
        self.assertFalse(os.path.exists(dl.part), ".part 应已改名")
        self.assertTrue(seen and seen[-1][0] == len(PAYLOAD))

    def test_resume_from_part(self):
        # 模拟上次下到一半：先写前 1/3 到 .part
        dl = US.Downloader(self.url(), self.dest(), PAYLOAD_MD5)
        os.makedirs(os.path.dirname(dl.part), exist_ok=True)
        cut = len(PAYLOAD) // 3
        with open(dl.part, "wb") as f:
            f.write(PAYLOAD[:cut])
        dl.download()
        self.assertTrue(_PkgHandler.seen_ranges)
        self.assertEqual(_PkgHandler.seen_ranges[0], f"bytes={cut}-")
        with open(dl.dest, "rb") as f:
            self.assertEqual(f.read(), PAYLOAD)

    def test_server_ignores_range_restarts_clean(self):
        dl = US.Downloader(self.url(), self.dest(), PAYLOAD_MD5)
        os.makedirs(os.path.dirname(dl.part), exist_ok=True)
        with open(dl.part, "wb") as f:
            f.write(b"garbage" * 100)         # 比真实包短且内容错误
        _PkgHandler.honor_range = False
        dl.download()
        with open(dl.dest, "rb") as f:
            self.assertEqual(f.read(), PAYLOAD)

    def test_md5_mismatch_removes_part(self):
        dl = US.Downloader(self.url(), self.dest(), "0" * 32)
        with self.assertRaises(US.UpdateError) as ctx:
            dl.download()
        self.assertEqual(ctx.exception.code, "update_bad_md5")
        self.assertFalse(os.path.exists(dl.part))
        self.assertFalse(os.path.exists(dl.dest))

    def test_http_error(self):
        _PkgHandler.fail_status = 500
        dl = US.Downloader(self.url(), self.dest(), PAYLOAD_MD5)
        with self.assertRaises(US.UpdateError) as ctx:
            dl.download()
        self.assertEqual(ctx.exception.code, "update_download")

    def test_cancel(self):
        cancel = threading.Event()
        cancel.set()
        dl = US.Downloader(self.url(), self.dest(), PAYLOAD_MD5)
        with self.assertRaises(US.DownloadCancelled):
            dl.download(cancel=cancel)

    def test_reuse_existing_complete_file(self):
        with open(self.dest(), "wb") as f:
            f.write(PAYLOAD)
        dl = US.Downloader(self.url(), self.dest(), PAYLOAD_MD5)
        path = dl.download()
        self.assertEqual(path, dl.dest)
        self.assertFalse(_PkgHandler.seen_ranges)   # 没走网络


# ---------------------------------------------------------------- 作业 / 命令

class TestJob(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="xfu-job-")

    def tearDown(self):
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _job(self, **over):
        base = {"package": os.path.join(self.tmp, "new.exe"),
                "target": os.path.join(self.tmp, "app.exe"),
                "version": "1.0.1", "md5": PAYLOAD_MD5,
                "wait_pid": 4321, "restart": True}
        base.update(over)
        return US.UpdateJob(**base)

    def test_roundtrip(self):
        path = os.path.join(self.tmp, "job.json")
        US.write_job(self._job(), path)
        job = US.read_job(path)
        self.assertEqual(job.version, "1.0.1")
        self.assertEqual(job.wait_pid, 4321)
        self.assertTrue(job.restart)

    def test_rejects_relative_and_bad_schema(self):
        path = os.path.join(self.tmp, "job.json")
        US.write_job(self._job(target="app.exe"), path)
        with self.assertRaises(US.UpdateError):
            US.read_job(path)
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"schema": "something-else"}, f)
        with self.assertRaises(US.UpdateError):
            US.read_job(path)

    def test_updater_command_prefers_exe(self):
        with open(os.path.join(self.tmp, "XUpdate.exe"), "wb") as f:
            f.write(b"MZ")
        self.assertEqual(US.updater_command(self.tmp),
                         [os.path.join(self.tmp, "XUpdate.exe")])

    def test_updater_command_falls_back_to_script(self):
        with open(os.path.join(self.tmp, "xupdate.py"), "w") as f:
            f.write("")
        cmd = US.updater_command(self.tmp)
        self.assertEqual(cmd[1], os.path.join(self.tmp, "xupdate.py"))

    def test_updater_command_missing(self):
        with self.assertRaises(US.UpdateError) as ctx:
            US.updater_command(self.tmp)
        self.assertEqual(ctx.exception.code, "update_updater_missing")


# ---------------------------------------------------------------- 管理器

class TestManager(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="xfu-mgr-")
        self.events: list[dict] = []
        self._env = unittest.mock.patch.dict(
            os.environ, {"APPDATA": os.path.join(self.tmp, "appdata"),
                         "XDG_CONFIG_HOME": os.path.join(self.tmp, "xdg")})
        self._env.start()

    def tearDown(self):
        self._env.stop()
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _wait(self, mgr, timeout=5.0):
        thread = getattr(mgr, "_thread", None)
        if thread is not None:
            thread.join(timeout)

    def test_disabled_without_url(self):
        mgr = US.UpdateManager(base_dir=self.tmp, api_url="",
                               on_event=self.events.append)
        self.assertEqual(mgr.state()["state"], "disabled")
        r = mgr.check(manual=True)
        self.assertFalse(r["ok"])
        self.assertEqual(r["code"], "update_no_url")

    def test_check_available(self):
        mgr = US.UpdateManager(
            base_dir=self.tmp, api_url="http://x/api", current="1.0.0",
            on_event=self.events.append,
            fetch=lambda url: US.parse_update_info(_sample(version="1.0.1")))
        mgr.check(manual=False)
        self._wait(mgr)
        st = mgr.state()
        self.assertEqual(st["state"], "available")
        self.assertEqual(st["latest"], "1.0.1")
        self.assertEqual(st["notes"], "修复若干问题")

    def test_check_uptodate(self):
        mgr = US.UpdateManager(
            base_dir=self.tmp, api_url="http://x/api", current="1.0.1",
            on_event=self.events.append,
            fetch=lambda url: US.parse_update_info(_sample(version="1.0.1")))
        mgr.check()
        self._wait(mgr)
        self.assertEqual(mgr.state()["state"], "uptodate")

    def test_check_network_error_is_silent_on_auto(self):
        def boom(url):
            raise US.UpdateError("连不上", "update_network")
        mgr = US.UpdateManager(base_dir=self.tmp, api_url="http://x/api",
                               on_event=self.events.append, fetch=boom)
        mgr.check(manual=False)
        self._wait(mgr)
        st = mgr.state()
        self.assertEqual(st["state"], "error")
        self.assertTrue(st["silent"])            # 启动自动检查不打扰用户
        self.assertEqual(st["errorCode"], "update_network")

    def test_apply_unsupported_in_source_run(self):
        mgr = US.UpdateManager(base_dir=self.tmp, api_url="http://x/api",
                               on_event=self.events.append)
        with unittest.mock.patch.dict(os.environ,
                                      {"XFLYSIM_UPDATE_TARGET": ""}):
            mgr._info = US.parse_update_info(_sample())
            r = mgr.apply()
        self.assertFalse(r["ok"])
        self.assertEqual(r["code"], "update_unsupported")

    def test_apply_requires_download_link(self):
        mgr = US.UpdateManager(base_dir=self.tmp, api_url="http://x/api",
                               on_event=self.events.append)
        target = os.path.join(self.tmp, "app.exe")
        with open(target, "wb") as f:
            f.write(b"old")
        with unittest.mock.patch.dict(
                os.environ, {"XFLYSIM_UPDATE_TARGET": target}):
            mgr._info = US.parse_update_info(_sample(link=""))
            r = mgr.apply()
        self.assertFalse(r["ok"])
        self.assertEqual(r["code"], "update_bad_link")

    def test_apply_downloads_and_launches_helper(self):
        pkg = os.path.join(self.tmp, "pkg.exe")
        with open(pkg, "wb") as f:
            f.write(PAYLOAD)
        launched = {}

        def fake_launcher(job, base_dir):
            launched.update(job=job, base_dir=base_dir)
            return os.path.join(self.tmp, "job.json")

        target = os.path.join(self.tmp, "app.exe")
        with open(target, "wb") as f:
            f.write(b"old")
        mgr = US.UpdateManager(
            base_dir=self.tmp, api_url="http://x/api", current="1.0.0",
            on_event=self.events.append, launcher=fake_launcher)
        with unittest.mock.patch.dict(
                os.environ, {"XFLYSIM_UPDATE_TARGET": target}):
            mgr._info = US.parse_update_info(_sample(link="file:///" +
                                                     pkg.replace("\\", "/")))
            r = mgr.apply()
            self.assertTrue(r["ok"])
            self._wait(mgr)
        self.assertEqual(launched["job"].version, "1.0.1")
        self.assertEqual(launched["job"].target, target)
        self.assertEqual(launched["job"].wait_pid, os.getpid())
        self.assertEqual(mgr.state()["state"], "restarting")

    def test_apply_bad_md5_goes_to_error(self):
        pkg = os.path.join(self.tmp, "pkg.exe")
        with open(pkg, "wb") as f:
            f.write(PAYLOAD)

        def fake_launcher(job, base_dir):       # 不应被调用
            raise AssertionError("launcher must not run on bad md5")

        mgr = US.UpdateManager(base_dir=self.tmp, api_url="http://x/api",
                               on_event=self.events.append,
                               launcher=fake_launcher)
        target = os.path.join(self.tmp, "app.exe")
        with open(target, "wb") as f:
            f.write(b"old")
        with unittest.mock.patch.dict(
                os.environ, {"XFLYSIM_UPDATE_TARGET": target}):
            mgr._info = US.parse_update_info(_sample(
                link="file:///" + pkg.replace("\\", "/"), md5="0" * 32))
            mgr.apply()
            self._wait(mgr)
        st = mgr.state()
        self.assertEqual(st["state"], "error")
        self.assertEqual(st["errorCode"], "update_bad_md5")


# ---------------------------------------------------------------- 结果文件

class TestResultFiles(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="xfu-res-")
        self._env = unittest.mock.patch.dict(
            os.environ, {"APPDATA": os.path.join(self.tmp, "appdata"),
                         "XDG_CONFIG_HOME": os.path.join(self.tmp, "xdg")})
        self._env.start()

    def tearDown(self):
        self._env.stop()
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_result_read_once(self):
        self.assertIsNone(US.read_and_clear_result())
        US.write_result(True, "1.0.1", target=r"C:\x\app.exe")
        data = US.read_and_clear_result()
        self.assertTrue(data["ok"])
        self.assertEqual(data["version"], "1.0.1")
        self.assertIsNone(US.read_and_clear_result())   # 只提示一次

    def test_installed_version(self):
        self.assertEqual(US.read_installed_version(), "")
        US.write_installed_version("1.0.1", md5=PAYLOAD_MD5, package="pkg.exe")
        self.assertEqual(US.read_installed_version(), "1.0.1")


if __name__ == "__main__":
    unittest.main()
