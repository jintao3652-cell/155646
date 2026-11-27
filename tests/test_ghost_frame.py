"""断线幽灵帧不得覆盖服务器机型/部件记录（2026-11-27 engines=null 根因）。

SimConnect 断线重连期间，插件会推来一帧全默认 flight_data（位置 0,0、
机型未知、尾号空）。旧代码据此触发 send_aircraft_info() 发出残缺 PI
`PI:GEN:AIRLINE=SJX`，服务器按位置解析后把站点的 equipment/livery/
engines 全覆盖成 null。本测试锁定两层防御：

  ① NetworkModel 帧门控：幽灵帧不触碰 FSD 状态更新
  ② packet.plane_info：机型完全未知时返回空串，_send 跳过
"""
from __future__ import annotations

import unittest
from types import SimpleNamespace
from unittest.mock import MagicMock

from xfly_client.fsd import packet as pk
from xfly_client.fsd.session import STATE_ONLINE
from xfly_client.model import NetworkModel
from xfly_client.report_identity import AircraftIdentity
from xfly_client.xconnect.client import FlightData


def _bare_model() -> NetworkModel:
    """绕过 NetworkModel 的重量级构造（不连网络/不启线程），只装门控所需状态。"""
    m = NetworkModel.__new__(NetworkModel)
    m.on_local_flight_data = None
    m._last_guessed_icao = ""
    m._ghost_frame_logged = False
    m._sim_aircraft_name = ""
    m.my_callsign = "SJX110"
    m.aircraft_manual = ""
    m.aircraft_icao = "A320"     # 已上报的正确机型
    m.engine_count = 2
    m.airline_icao = "SJX"
    m.livery_icao = "SWIFT_A672"
    m.registration = "B-8493"
    m.registration_manual = ""
    m.registration_cache = {}
    m.registration_source = "tailnum"
    m._sim_reg = "B-8493"
    m._sim_reg_src = "tailnum"
    m._sim_reg_icao = "A320"
    m.identity = AircraftIdentity()
    m.on_registration_changed = None
    m.on_registration_resolved = None
    m._log = MagicMock()
    # fsd mock：挂着 STATE_ONLINE，记录所有更新调用
    fsd = MagicMock()
    fsd.state = STATE_ONLINE
    fsd.aircraft_icao = "A320"
    fsd.engines = 2
    m.fsd = fsd
    return m


def _ghost_frame() -> FlightData:
    """断线重连时的全默认帧（fsd.log 18:17:29 实测形状）。

    ⚠️ 坐标不是精确的 0,0 —— MConnect 卡死时给的是
    (0.0004150, 0.0139740)。旧判据 `|lon| < 0.01` 恰好放过它，于是"以为
    修好了、其实还在发残缺 PI"。本用例锁死新阈值。
    """
    return FlightData(
        aircraft_icao="", aircraft_name="", engine_count=1,
        latitude=0.0004150, longitude=0.0139740, altitude_msl=9.0,
        squark="S", transponder=0, registration="",
        parts={"gear_down": False, "on_ground": False,
               "engines": {"1": {"on": False}}})


class TestGhostFrameGuard(unittest.TestCase):
    def test_ghost_frame_does_not_touch_fsd(self):
        m = _bare_model()
        m._on_xconnect_flight_data(_ghost_frame())
        fsd = m.fsd
        fsd.update_flight_state.assert_not_called()
        fsd.send_aircraft_info.assert_not_called()
        fsd.notify_aircraft_parts.assert_not_called()
        # 已上报的机型/尾号保持不变
        self.assertEqual(m.aircraft_icao, "A320")
        self.assertEqual(m.registration, "B-8493")

    def test_ghost_logged_only_once(self):
        m = _bare_model()
        m._on_xconnect_flight_data(_ghost_frame())
        m._on_xconnect_flight_data(_ghost_frame())
        self.assertEqual(m._log.call_count, 1)

    def test_normal_frame_still_updates(self):
        """门控不能误杀正常帧：机型已知 + 位置正常必须照常上报。"""
        m = _bare_model()
        fd = FlightData(
            aircraft_icao="A320", aircraft_name="", engine_count=2,
            latitude=40.85, longitude=111.81, altitude_msl=3500.0,
            squark="N", transponder=2000,
            parts={"on_ground": False, "engines": {"1": {"on": True},
                                                   "2": {"on": True}}})
        m._on_xconnect_flight_data(fd)
        m.fsd.update_flight_state.assert_called_once()
        m.fsd.notify_aircraft_parts.assert_called_once()

    def test_unknown_icao_but_valid_position_still_reports_position(self):
        """机型暂未识别但位置正常时只跳过机型更新，位置照发（不误杀）。"""
        m = _bare_model()
        fd = FlightData(
            aircraft_icao="", aircraft_name="", engine_count=2,
            latitude=40.85, longitude=111.81, altitude_msl=3500.0,
            squark="N", transponder=2000,
            parts={"on_ground": False})
        m._on_xconnect_flight_data(fd)
        m.fsd.update_flight_state.assert_called_once()


class TestPiIntegrity(unittest.TestCase):
    def test_airline_only_pi_is_empty(self):
        self.assertEqual(
            pk.plane_info("SJX110", "SERVER", "", "SJX", "", "B-8493", 2),
            "")

    def test_partial_pi_with_equipment_is_kept(self):
        """有 EQUIPMENT 就算机型信息存在（农机/Experimental 无 livery）。"""
        line = pk.plane_info("SJX110", "SERVER", "A320", "SJX", "", "", 0)
        self.assertIn("EQUIPMENT=A320", line)


if __name__ == "__main__":
    unittest.main()
