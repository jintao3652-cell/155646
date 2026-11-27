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


if __name__ == "__main__":
    unittest.main()
