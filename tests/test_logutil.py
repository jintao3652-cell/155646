import os
import tempfile
import unittest

from xfly_client.logutil import RotatingFileWriter, redact


class TestRedact(unittest.TestCase):
    def test_ap_password_masked(self):
        raw = "#APSJX110:SERVER:12345:hunter2:1:9:1:Test Pilot"
        out = redact(raw)
        self.assertNotIn("hunter2", out)
        self.assertIn("#APSJX110:SERVER:12345:***:", out)

    def test_non_ap_lines_unchanged(self):
        raw = "@N:SJX110:2000:1:31.2:121.3:47:0:123:0"
        self.assertEqual(redact(raw), raw)

    def test_empty_safe(self):
        self.assertEqual(redact(""), "")


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
