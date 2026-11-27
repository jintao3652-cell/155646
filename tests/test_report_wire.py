"""上报线序回归：PI 四字段齐全、全量 ACC 同时进服务器、被拒自动降级。

这是"站点 JSON 某字段恒 null"这类问题的**线级**防线：不验证服务器怎么想，
只验证我们真的把四个字段按契约发出去了、并且 ACC 走的是两条通道。
"""

import json
import unittest

from xfly_client.fsd import packet as pk
from xfly_client.fsd.session import (
    ACFG_BROADCAST_TO, ACFG_SERVER_TARGET, STATE_ONLINE, FsdSession,
)


class _FakeLink:
    """最小 FSD 连接桩：只记录发出的行。"""

    def __init__(self) -> None:
        self.connected = True
        self.sent: list[str] = []

    def send(self, line: str) -> None:
        self.sent.append(line)


def _session(callsign: str = "SJX110") -> FsdSession:
    s = FsdSession("127.0.0.1", 6809, callsign, "12345", "pw", "Tester", 16)
    s._link = _FakeLink()
    s.on_log = None
    s.state = STATE_ONLINE
    return s


class TestPlaneInfoWire(unittest.TestCase):
    def test_pi_carries_all_four_fields(self):
        s = _session()
        s.aircraft_icao = "A320"
        s.airline_icao = "SJX"
        s.registration = "B-8493"
        s.engines = 2
        line = s._pi_line("SERVER")
        for key in ("EQUIPMENT=A320", "AIRLINE=SJX", "LIVERY=SWIFT_A672-B-8493",
                    "ENGINES=2", "REG=B-8493"):
            self.assertIn(key, line)

    def test_pi_engines_fall_back_to_aircraft_db(self):
        """桥给 0 台时靠机型库兜底，PI 里不许缺 ENGINES。"""
        s = _session()
        s.aircraft_icao = "B744"
        s.airline_icao = "SJX"
        s.engines = 0
        self.assertIn("ENGINES=4", s._pi_line("SERVER"))

    def test_unknown_aircraft_sends_nothing(self):
        s = _session()
        s.aircraft_icao = ""
        s.airline_icao = "SJX"
        s.engines = 0
        self.assertEqual(s._pi_line("SERVER"), "")

    def test_airline_filled_from_callsign(self):
        s = _session("CCA1378")
        s.aircraft_icao = "A320"
        s.airline_icao = ""
        self.assertIn("AIRLINE=CCA", s._pi_line("SERVER"))


class TestFullAccWire(unittest.TestCase):
    def _prime(self, s: FsdSession) -> None:
        s.engines = 2
        s._acfg_full = {"lights": {"beacon_on": True},
                        "engines": {"1": {"on": True}, "2": {"on": True}},
                        "gear_down": True}

    def test_full_broadcast_targets_server_and_broadcast(self):
        s = _session()
        self._prime(s)
        s._broadcast_full_aircraft_config()
        targets = [line.split(":")[1] for line in s._link.sent]
        self.assertEqual(targets, [ACFG_BROADCAST_TO, ACFG_SERVER_TARGET])

        cfg = json.loads(s._link.sent[1].split("ACC:", 1)[1])["config"]
        self.assertIs(cfg["is_full_data"], True)
        self.assertEqual(cfg["engines"]["1"]["on"], True)
        # swift 形状副本同时保留（服务器实测读这个位置）
        self.assertEqual(cfg["aircraft"]["engines"], cfg["engines"])

    def test_full_broadcast_synthesizes_engines_when_missing(self):
        s = _session()
        s.engines = 2
        s._acfg_full = {"lights": {"beacon_on": True}}   # 桥没给 engines
        s._broadcast_full_aircraft_config()
        cfg = json.loads(s._link.sent[0].split("ACC:", 1)[1])["config"]
        self.assertEqual(cfg["engines"], {"1": {"on": False}, "2": {"on": False}})

    def test_syntax_error_disables_server_target_only(self):
        s = _session()
        self._prime(s)
        s._broadcast_full_aircraft_config()
        self.assertEqual(len(s._link.sent), 2)
        self.assertTrue(s._maybe_disable_unsupported(pk.ERR_SYNTAX))
        self.assertFalse(s._acfg_server_ok)
        s._link.sent.clear()
        s._broadcast_full_aircraft_config()
        self.assertEqual([ln.split(":")[1] for ln in s._link.sent],
                         [ACFG_BROADCAST_TO])
        # 位置类降级不受影响
        self.assertFalse(s._visual_pos_disabled)

    def test_caps_request_answered_with_acconfig(self):
        s = _session()
        s._on_line("$CQSERVER:SJX110:CAPS")
        self.assertTrue(any(":CAPS:" in ln and "ACCONFIG=1" in ln
                            for ln in s._link.sent))


if __name__ == "__main__":
    unittest.main()