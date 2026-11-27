import unittest
from collections import deque
from types import SimpleNamespace

import webapp


class _Writer:
    def __init__(self):
        self.lines = []

    def write(self, text):
        self.lines.append(text)


class TestApiLogLevels(unittest.TestCase):
    def setUp(self):
        self.api = object.__new__(webapp.Api)
        self.api._events = deque()
        self.api._log_records = deque(maxlen=200)
        self.api._cfg = SimpleNamespace(language="zh-CN")
        self._orig_writer = webapp._FSD_LOG
        webapp._FSD_LOG = _Writer()

    def tearDown(self):
        webapp._FSD_LOG = self._orig_writer

    def test_error_level_and_redaction_before_dispatch(self):
        self.api._push("log", "[fsd] 登录失败 cid=98765 password=hunter2")
        event, payload = self.api._events[0]
        self.assertEqual(event, "log")
        self.assertTrue(payload.startswith("[ERROR] "))
        for secret in ("98765", "hunter2"):
            self.assertNotIn(secret, payload)
        self.assertEqual(self.api._log_records[0]["level"], "ERROR")
        written = "".join(webapp._FSD_LOG.lines)
        self.assertIn("[ERROR]", written)
        for secret in ("98765", "hunter2"):
            self.assertNotIn(secret, written)

    def test_protocol_line_is_debug(self):
        self.api._push("log", "> $GPCCA1234")
        self.assertEqual(self.api._log_records[0]["level"], "DEBUG")
        self.assertTrue(self.api._events[0][1].startswith("[DEBUG] > "))

    def test_log_buffer_is_bounded(self):
        for i in range(250):
            self.api._push("log", f"[i] 第 {i} 条")
        self.assertEqual(len(self.api._log_records), 200)
        self.assertEqual(self.api._log_records[-1]["text"], "[i] 第 249 条")

    def test_diagnostics_extra_has_structured_log(self):
        self.api._xconnect = None
        self.api._model = None
        self.api._push("log", "[fsd] 超时")
        extra = self.api._diagnostics_extra()
        self.assertEqual(extra["log"]["level_counts"]["WARN"], 1)
        self.assertEqual(extra["log"]["recent"][0]["level"], "WARN")


if __name__ == "__main__":
    unittest.main()
