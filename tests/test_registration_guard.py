"""注册号来源链的护栏：呼号绝不能被当成尾号。

真实场景（MSFS）：机模/涂装的 `ATC ID` 常被写成呼号（`SJX110`），或被留空。
旧代码直接把它当 registration 上报 →
  - 站点 `registration` 显示呼号；
  - PI 变成 `LIVERY=SWIFT_A672-SJX110`，对端按这个变体码匹配涂装必然失败；
  - CSL 匹配掉到"同航司同机型"甚至默认白机。

本测试锁定两道防线：
  ① 客户端 `NetworkModel._apply_registration`：模拟器值 == 本机呼号 → 丢弃；
  ② `registration.resolve_registration` 的优先级不被破坏（手动 > 模拟器 > 缓存）。
"""
from __future__ import annotations

import unittest
from unittest.mock import MagicMock

from xfly_client.model import NetworkModel


def _bare_model(callsign: str = "SJX110") -> NetworkModel:
    m = NetworkModel.__new__(NetworkModel)
    m.my_callsign = callsign
    m.registration = ""
    m.registration_manual = ""
    m.registration_cache = {}
    m.registration_source = ""
    m._sim_reg = ""
    m._sim_reg_src = ""
    m._sim_reg_icao = ""
    m.aircraft_icao = "A320"
    m.on_registration_resolved = None
    m.fsd = None
    m._log = MagicMock()
    return m


class TestRegistrationGuard(unittest.TestCase):
    def test_callsign_as_atc_id_is_rejected(self):
        m = _bare_model()
        changed = m._apply_registration("SJX110", "atc_id", "A320")
        self.assertEqual(m.registration, "")
        self.assertFalse(changed)

    def test_callsign_rejected_case_insensitive(self):
        m = _bare_model()
        m._apply_registration("sjx110", "atc_id", "A320")
        self.assertEqual(m.registration, "")

    def test_real_tail_number_accepted(self):
        m = _bare_model()
        m._apply_registration("B-8493", "atc_id", "A320")
        self.assertEqual(m.registration, "B-8493")
        self.assertEqual(m.registration_source, "sim")   # 插件来源 → sim 标签

    def test_manual_still_wins_over_sim(self):
        m = _bare_model()
        m.registration_manual = "B-6419"
        m._apply_registration("B-8493", "atc_id", "A320")
        self.assertEqual(m.registration, "B-6419")
        self.assertEqual(m.registration_source, "manual")

    def test_cache_used_when_sim_empty(self):
        m = _bare_model()
        m.registration_cache["A320"] = "B-8493"
        m._apply_registration("", "", "A320")
        self.assertEqual(m.registration, "B-8493")
        self.assertEqual(m.registration_source, "cache")


if __name__ == "__main__":
    unittest.main()