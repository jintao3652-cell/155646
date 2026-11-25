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

    def test_export_writes_json(self):
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "diag.json")
            path = export(out, log_path=None)
            self.assertEqual(path, out)
            with open(out, encoding="utf-8") as f:
                data = json.load(f)
            self.assertIn("client_version", data)
            self.assertNotIn("password", json.dumps(data).lower())


if __name__ == "__main__":
    unittest.main()
