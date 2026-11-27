import os
import tempfile
import unittest

from xfly_client.logutil import (RotatingFileWriter, classify_level,
                                 format_log_line, redact, strip_level,
                                 tag_level)


class TestRedact(unittest.TestCase):
    def test_ap_cid_and_password_masked(self):
        raw = "#APSJX110:SERVER:12345:hunter2:1:9:1:Test Pilot"
        out = redact(raw)
        self.assertNotIn("12345", out)
        self.assertNotIn("hunter2", out)
        self.assertIn("#APSJX110:SERVER:***:***:", out)

    def test_dp_cid_masked(self):
        out = redact("#DPSJX110:12345")
        self.assertNotIn("12345", out)
        self.assertIn("#DPSJX110:***", out)

    def test_generic_credentials_masked(self):
        out = redact('cid=98765 password="s3cr3t" token: abc123')
        for secret in ("98765", "s3cr3t", "abc123"):
            self.assertNotIn(secret, out)
        self.assertEqual(out.count("***"), 3)

    def test_non_ap_lines_unchanged(self):
        raw = "@N:SJX110:2000:1:31.2:121.3:47:0:123:0"
        self.assertEqual(redact(raw), raw)

    def test_empty_safe(self):
        self.assertEqual(redact(""), "")


class TestLogLevels(unittest.TestCase):
    def test_classify_levels(self):
        self.assertEqual(classify_level("连接失败: exception"), "ERROR")
        self.assertEqual(classify_level("链路断开 / timeout"), "WARN")
        self.assertEqual(classify_level("> $GPCCA1234"), "DEBUG")
        self.assertEqual(classify_level("连接成功"), "INFO")
        self.assertEqual(classify_level("[WARN] 已降级"), "WARN")

    def test_tag_strip_roundtrip(self):
        tagged = tag_level("ERROR", "[fsd] 登录失败")
        self.assertEqual(tagged, "[ERROR] [fsd] 登录失败")
        self.assertEqual(strip_level(tagged), ("ERROR", "[fsd] 登录失败"))
        self.assertEqual(strip_level("无标签"), ("", "无标签"))
        # 已有标签不重复添加（同级别幂等；换级别时只保留新标签）
        self.assertEqual(tag_level("ERROR", tagged), tagged)
        self.assertEqual(tag_level("INFO", tagged), "[INFO] [fsd] 登录失败")

    def test_format_log_line_redacts(self):
        line = format_log_line("ERROR", "cid=12345 password=hunter2", "12:00:00")
        self.assertEqual(line, "12:00:00 [ERROR] cid=*** password=***")


class TestRotatingWriter(unittest.TestCase):
    def test_rotates_and_keeps_backups(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "app.log")
            w = RotatingFileWriter(path, max_bytes=10, backups=2)
            w.write("a" * 8)
            w.write("b" * 8)
            w.write("c" * 8)   # 第三次写入时超过 10 字节，触发轮转
            self.assertTrue(os.path.exists(path))
            self.assertTrue(os.path.exists(path + ".1"))
            with open(path + ".1", "r", encoding="utf-8") as f:
                self.assertGreaterEqual(len(f.read()), 10)


if __name__ == "__main__":
    unittest.main()
