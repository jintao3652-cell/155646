import json
import os
import tempfile
import unittest

from xfly_client.diagnostics import collect, export


class TestDiagnostics(unittest.TestCase):
    def test_log_tail_is_redacted(self):
        with tempfile.TemporaryDirectory() as d:
            log = os.path.join(d, "fsd.log")
            with open(log, "w", encoding="utf-8") as f:
                f.write("#APSJX110:SERVER:12345:hunter2:1:9:1:Pilot\n")
                f.write("@N:SJX110:2000:1:31.2:121.3:47:0:123:0\n")
            info = collect(log_path=log)
            text = json.dumps(info, ensure_ascii=False)
            self.assertNotIn("hunter2", text)
            self.assertIn("***", text)
            self.assertEqual(len(info["log_tail"]), 2)

    def test_extra_is_scrubbed(self):
        extra = {
            "cid": "98765",
            "account": {"password": "hunter2", "nested": [{"token": "abc123"}]},
            "note": "#APSJX110:SERVER:12345:hunter2:1:9:1:Test Pilot",
            "accident_count": 3,
        }
        info = collect(log_path=None, extra=extra)
        text = json.dumps(info, ensure_ascii=False)
        for secret in ("98765", "hunter2", "abc123", "12345"):
            self.assertNotIn(secret, text)
        self.assertEqual(info["extra"]["account"]["password"], "***")
        self.assertEqual(info["extra"]["account"]["nested"][0]["token"], "***")
        # 普通键名里的 cid 子串不应误伤
        self.assertEqual(info["extra"]["accident_count"], 3)

    def test_export_writes_json(self):
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "diag.json")
            # 显式空日志：否则会读本机真实日志，断言受历史内容影响
            empty_log = os.path.join(d, "empty.log")
            with open(empty_log, "w", encoding="utf-8"):
                pass
            path = export(out, log_path=empty_log)
            self.assertEqual(path, out)
            with open(out, encoding="utf-8") as f:
                data = json.load(f)
            self.assertIn("client_version", data)
            self.assertEqual(data["schema"], "xflysim-diagnostics")
            self.assertEqual(data["schema_version"], 1)
            self.assertTrue(data["redaction"]["enabled"])
            self.assertNotIn("password", json.dumps(data).lower())

    def test_export_no_cid_or_password(self):
        with tempfile.TemporaryDirectory() as d:
            log = os.path.join(d, "fsd.log")
            with open(log, "w", encoding="utf-8") as f:
                f.write("#APSJX110:SERVER:12345:hunter2:1:9:1:Test Pilot\n")
                f.write("#DPSJX110:12345\n")
                f.write("cid=98765 password=s3cr3t\n")
            out = os.path.join(d, "diag.json")
            export(out, log_path=log, extra={"cid": "98765",
                                             "password": "s3cr3t"})
            with open(out, encoding="utf-8") as f:
                text = f.read()
            for secret in ("12345", "hunter2", "98765", "s3cr3t"):
                self.assertNotIn(secret, text)
            self.assertIn("***", text)


if __name__ == "__main__":
    unittest.main()
