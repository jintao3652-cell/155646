import time
import unittest

from xfly_client.fsd import packet as pk
from xfly_client.fsd.session import FsdSession


class TestUnsupportedPosition(unittest.TestCase):
    def make_session(self):
        s = FsdSession.__new__(FsdSession)
        s._visual_pos_disabled = False
        s._interim_pos_disabled = False
        s._last_visual_sent = 0.0
        s._last_interim_sent = 0.0
        s.logs = []
        s.errors = []
        s._log = s.logs.append
        s.on_error_text = s.errors.append
        return s

    def test_syntax_error_disables_visual(self):
        s = self.make_session()
        s._last_visual_sent = time.monotonic()
        self.assertTrue(s._maybe_disable_unsupported(pk.ERR_SYNTAX))
        self.assertTrue(s._visual_pos_disabled)
        self.assertFalse(s._interim_pos_disabled)
        self.assertTrue(any("^" in x for x in s.logs))

    def test_syntax_error_disables_interim(self):
        s = self.make_session()
        s._last_interim_sent = time.monotonic()
        self.assertTrue(s._maybe_disable_unsupported(pk.ERR_SYNTAX))
        self.assertTrue(s._interim_pos_disabled)
        self.assertFalse(s._visual_pos_disabled)

    def test_non_syntax_ignored(self):
        s = self.make_session()
        s._last_visual_sent = time.monotonic()
        self.assertFalse(s._maybe_disable_unsupported(pk.ERR_SERVER_FULL))
        self.assertFalse(s._visual_pos_disabled)


if __name__ == "__main__":
    unittest.main()
