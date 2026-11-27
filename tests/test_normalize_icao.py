"""机型 ICAO 规范化（aircraft_db.normalize_icao）回归测试。

背景：站点 livery=null 的根因（2026-11-27）。机型推断原来只在 ICAO
为**空**（或 ATC* 占位符）时触发，而 IniBuilds A350 上报的
`A350-900` 非空、非占位 —— 直接穿透护栏进入 PI：

    EQUIPMENT=A350-900:...        ← 没有 LIVERY 键

`swift_livery("A350-900")` 在 swift 机型库查不到 id → LIVERY 键不发
→ 站点 JSON 里 livery 恒为 null。normalize_icao 把"库外码"也纳入
推断（A350-900 → A359）。
"""

import unittest

from xfly_client import aircraft_db


class TestNormalizeIcao(unittest.TestCase):
    def test_standard_icao_kept(self):
        """标准码（库里有 id）原样保留，不做任何替换。"""
        for icao in ("A320", "A359", "C750", "B738", "F14"):
            self.assertEqual(aircraft_db.normalize_icao(icao), icao)

    def test_nonstandard_350_reinferred(self):
        """IniBuilds A350 的非标准 ICAO 必须重推成 A359（本次 bug 主体）。"""
        self.assertEqual(aircraft_db.normalize_icao("A350-900",
                                                    "Airbus A350-900", 2),
                         "A359")
        # 名称缺失时靠码本身也能推出来
        self.assertEqual(aircraft_db.normalize_icao("A350-900"), "A359")

    def test_atc_placeholder_cleared(self):
        """ATC* 占位符当未知：能推断就推断，推不出保持空。"""
        self.assertEqual(aircraft_db.normalize_icao(
            "ATCCOM", "Airbus A320neo", 2), "A20N")
        self.assertEqual(aircraft_db.normalize_icao("ATCPVT", "???", 1), "")

    def test_empty_falls_back_to_name(self):
        """空 ICAO（F-14 等老行为）按名称推断。"""
        self.assertEqual(aircraft_db.normalize_icao("", "McDonnell Douglas F-14", 2),
                         "F14")

    def test_unknown_icao_kept_as_is(self):
        """库外且推不出的码保留原值：equipment 至少还能显示。"""
        self.assertEqual(aircraft_db.normalize_icao("ZZZZ", "", 0), "ZZZZ")

    def test_result_always_has_swift_id(self):
        """凡是推断出来的结果，必须能生成 swift LIVERY（否则白修）。"""
        for raw, name in (("A350-900", "Airbus A350-900"),
                          ("ATCCOM", "Airbus A320neo"),
                          ("", "Boeing 737-800")):
            got = aircraft_db.normalize_icao(raw, name, 2)
            if got:
                self.assertTrue(aircraft_db.swift_livery(got),
                                f"{got} 应有 swift id")

    def test_lowercase_and_whitespace(self):
        self.assertEqual(aircraft_db.normalize_icao(" a350-900 ",
                                                    "Airbus A350-900", 2),
                         "A359")


if __name__ == "__main__":
    unittest.main()
