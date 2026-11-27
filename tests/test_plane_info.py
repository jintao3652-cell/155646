"""PI:GEN 报文（packet.plane_info）的槽位回归测试。

背景：XFlySim 服务器对 PI 组的 livery/engines **按位置**解析
（第 3 键 = LIVERY，第 4 键 = ENGINES）。两种实测过的 engines=null 路径：

  ① REG 插在 ENGINES 前面 → 第 4 键是 REG → 解析不出数字 → engines null；
  ② LIVERY 缺失（机型反查不到 swift id，如 MSFS 的 ATCCOM 占位符）时
     仍发 ENGINES → ENGINES 顶到 LIVERY 的槽位 → 第 4 键是 REG →
     engines null（fsd.log 23:13 会话实证）。

所以规则是：ENGINES 只能出现在 LIVERY 之后；LIVERY 缺失时连带省略。
"""

import unittest

from xfly_client.fsd import packet


def _keys(line: str) -> list[str]:
    """取 PI:GEN 报文的键列表（不含头三段 #SB<cs>:<to>:PI:GEN）。"""
    return line.split(":PI:GEN:", 1)[1].split(":")


class TestPlaneInfoEnginesSlot(unittest.TestCase):
    def test_engines_follows_livery(self):
        line = packet.plane_info("T1", "SERVER", "A320", "SJX",
                                 "SWIFT_A672", "B-6419", 2)
        keys = _keys(line)
        self.assertEqual(keys,
                         ["EQUIPMENT=A320", "AIRLINE=SJX",
                          "LIVERY=SWIFT_A672", "ENGINES=2", "REG=B-6419"])

    def test_no_livery_no_engines(self):
        """LIVERY 缺失时 ENGINES 必须连带省略，否则顶错槽位 → engines null。"""
        line = packet.plane_info("T1", "SERVER", "ATCCOM", "SJX",
                                 "", "B-6419", 2)
        keys = _keys(line)
        self.assertNotIn("ENGINES=2", keys)
        self.assertNotIn("LIVERY=", keys)
        # REG 仍在（按名解析，registration 不受位置影响）
        self.assertIn("REG=B-6419", keys)

    def test_zero_engines_omitted(self):
        line = packet.plane_info("T1", "SERVER", "A320", "SJX",
                                 "SWIFT_A672", "B-6419", 0)
        self.assertNotIn("ENGINES=0", _keys(line))

    def test_empty_equipment_omitted(self):
        """EQUIPMENT 有键无值会导致整条 PI 被服务器丢弃 —— 空值必须省键。"""
        line = packet.plane_info("T1", "SERVER", "", "SJX",
                                 "SWIFT_A672", "B-6419", 2)
        keys = _keys(line)
        self.assertNotIn("EQUIPMENT=", keys)
        self.assertIn("ENGINES=2", keys)
        # LIVERY 仍在 ENGINES 之前
        self.assertLess(keys.index("LIVERY=SWIFT_A672"),
                        keys.index("ENGINES=2"))

    def test_no_equipment_no_livery_returns_empty(self):
        """机型完全未知时整条不发送：残缺 PI 会覆盖服务器已有的正确记录。

        实测 fsd.log 18:17:29：`PI:GEN:AIRLINE=SJX` 让站点 equipment/livery/
        engines 全变 null（只剩 airline）。返回空串由 _send 跳过。
        """
        self.assertEqual(
            packet.plane_info("SJX110", "SERVER", "", "SJX", "", "B-8493", 2),
            "")
        # 只有尾号、连航司都没有，同样不发送
        self.assertEqual(
            packet.plane_info("SJX110", "SERVER", "", "", "", "B-8493", 0),
            "")

    def test_airline_only_with_no_registration_returns_empty(self):
        """只有 AIRLINE 一个键时绝不能发出（用户贴的 null JSON 的直接来源）。"""
        self.assertEqual(
            packet.plane_info("SJX110", "atis_collector", "", "SJX", "", "", 0),
            "")


class TestCapsResponse(unittest.TestCase):
    def test_caps_declares_acconfig(self):
        """CAPS 必须声明 ACCONFIG=1 —— 不声明对方就不发机模配置，
        `aircraft.engines`（唯一来源是 ACC）也就永远拿不到。"""
        line = packet.caps_response("SJX110", "SERVER")
        self.assertTrue(line.startswith("$CRSJX110:SERVER:CAPS:"), line)
        self.assertIn("ACCONFIG=1", line)
        self.assertIn("MODELDESC=1", line)

    def test_caps_accepts_custom_list(self):
        line = packet.caps_response("A1", "B2", ("ACCONFIG=1",))
        self.assertEqual(line, "$CRA1:B2:CAPS:ACCONFIG=1")


if __name__ == "__main__":
    unittest.main()
