"""按北京时间分日日志与 7 天过期清理测试。"""
from __future__ import annotations

import os
import tempfile
import unittest
from datetime import date, datetime, timedelta, timezone
from unittest.mock import patch

from xfly_client import logutil as L


class TestBeijingTime(unittest.TestCase):
    def test_utc_evening_is_next_beijing_day(self):
        # UTC 16:30 = 北京时间次日 00:30
        dt = datetime(2026, 11, 27, 16, 30, tzinfo=timezone.utc)
        self.assertEqual(L.beijing_date(dt), date(2026, 11, 28))
        self.assertEqual(L.beijing_clock(dt), "00:30:00")

    def test_naive_datetime_treated_as_beijing_wall_clock(self):
        dt = datetime(2026, 11, 27, 23, 59)
        self.assertEqual(L.beijing_date(dt), date(2026, 11, 27))
        self.assertEqual(L.beijing_clock(dt), "23:59:00")

    def test_daily_path_and_prefix_sanitized(self):
        p = L.daily_log_path("logs", "my app/x", date(2026, 11, 27))
        self.assertEqual(os.path.basename(p), "my_app_x-2026-11-27.log")

    def test_log_dir_env_override(self):
        with tempfile.TemporaryDirectory() as d:
            with L_env("XFLYSIM_LOG_DIR", d):
                self.assertEqual(L.default_log_dir(), d)
                self.assertEqual(os.path.dirname(L.default_log_path()), d)

    def test_frozen_app_uses_exe_dir_not_meipass(self):
        with tempfile.TemporaryDirectory() as d:
            exe = os.path.join(d, "XFlySim.exe")
            with patch.object(L.sys, "frozen", True, create=True), \
                    patch.object(L.sys, "executable", exe):
                self.assertEqual(L.app_base_dir(), d)
                self.assertEqual(L.default_log_dir(), os.path.join(d, "logs"))


class L_env:
    """临时环境变量，兼容 py3.8+（无需 patch.dict）。"""

    def __init__(self, key, value):
        self.key, self.value = key, value

    def __enter__(self):
        self.old = os.environ.get(self.key)
        os.environ[self.key] = self.value
        return self

    def __exit__(self, *_):
        if self.old is None:
            os.environ.pop(self.key, None)
        else:
            os.environ[self.key] = self.old


class TestDailyLogWriter(unittest.TestCase):
    def test_writes_to_current_day_file(self):
        with tempfile.TemporaryDirectory() as d:
            now = datetime(2026, 11, 27, 23, 30)
            w = L.DailyLogWriter(d, now_fn=lambda: now)
            w.write("hello\n")
            self.assertEqual(os.path.basename(w.path),
                             "xflysim-2026-11-27.log")
            with open(w.path, encoding="utf-8") as f:
                self.assertEqual(f.read(), "hello\n")

    def test_rolls_over_at_beijing_midnight(self):
        with tempfile.TemporaryDirectory() as d:
            state = {"now": datetime(2026, 11, 27, 23, 59)}
            w = L.DailyLogWriter(d, now_fn=lambda: state["now"])
            w.write("before\n")
            state["now"] = datetime(2026, 11, 28, 0, 1)
            w.write("after\n")
            self.assertEqual(os.path.basename(w.path),
                             "xflysim-2026-11-28.log")
            with open(os.path.join(d, "xflysim-2026-11-27.log"),
                      encoding="utf-8") as f:
                self.assertEqual(f.read(), "before\n")
            with open(w.path, encoding="utf-8") as f:
                self.assertEqual(f.read(), "after\n")

    def test_cleanup_keeps_seven_days_and_removes_older(self):
        with tempfile.TemporaryDirectory() as d:
            today = date(2026, 11, 27)
            for i in range(12):
                day = today - timedelta(days=i)
                path = os.path.join(d, f"xflysim-{day.isoformat()}.log")
                with open(path, "w", encoding="utf-8") as f:
                    f.write("x")
            with open(os.path.join(d, "fsd.log"), "w") as f:
                f.write("legacy")
            with open(os.path.join(d, "notes.txt"), "w") as f:
                f.write("keep")

            removed = L.cleanup_daily_logs(d, today=today)
            self.assertEqual(len(removed), 5)   # 8~12 天前，共 5 个
            remain = sorted(n for n in os.listdir(d) if n.startswith("xflysim"))
            self.assertEqual(len(remain), 7)
            self.assertEqual(remain[0], "xflysim-2026-11-21.log")
            self.assertEqual(remain[-1], "xflysim-2026-11-27.log")
            self.assertTrue(os.path.exists(os.path.join(d, "fsd.log")))
            self.assertTrue(os.path.exists(os.path.join(d, "notes.txt")))

    def test_cleanup_runs_on_writer_init_and_rollover(self):
        with tempfile.TemporaryDirectory() as d:
            old = date(2026, 10, 1)
            path = os.path.join(d, f"xflysim-{old.isoformat()}.log")
            with open(path, "w") as f:
                f.write("old")
            state = {"now": datetime(2026, 11, 2)}
            w = L.DailyLogWriter(d, now_fn=lambda: state["now"])
            self.assertFalse(os.path.exists(path))  # init 时就清理过期文件
            w.write("x")
            state["now"] = datetime(2026, 11, 20)
            w.write("y")
            self.assertEqual(len([n for n in os.listdir(d)
                                  if n.startswith("xflysim-")]), 1)

    def test_other_prefix_logs_are_not_deleted(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "other-2020-01-01.log")
            with open(p, "w") as f:
                f.write("x")
            L.cleanup_daily_logs(d, today=date(2026, 11, 27))
            self.assertTrue(os.path.exists(p))


if __name__ == "__main__":
    unittest.main()
