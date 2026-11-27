"""站点上报身份（equipment/airline/livery/engines）的契约测试。

站点 JSON 的四个字段分属两条通道（PI / ACC），历史上各自被不同分支维护，
任何一个分支给不出值就只表现为"某字段 null"且服务器不报错。本测试锁定
`report_identity.resolve_identity()` 的兜底顺序：

| 字段 | 顺序 |
|---|---|
| equipment | 手填机型 → 桥值（规范化：ATC* 当未知、库外码按机模名重推） |
| airline   | 配置/界面 → 呼号前三字 |
| livery    | swift 机型库 `SWIFT_A<id>`（+ 尾号） |
| engines   | 桥实测 → swift 机型库台数 |
"""

import unittest

from xfly_client.report_identity import (
    AircraftIdentity, airline_from_callsign, engine_mask, engines_from_parts,
    resolve_identity,
)


class TestAirlineFromCallsign(unittest.TestCase):
    def test_three_letter_prefix(self):
        self.assertEqual(airline_from_callsign("SJX110"), "SJX")
        self.assertEqual(airline_from_callsign("cca1378"), "CCA")

    def test_registration_style_callsign_has_no_airline(self):
        # N12345 这类注册号式呼号不能硬凑出航司码
        self.assertEqual(airline_from_callsign("N12345"), "")
        self.assertEqual(airline_from_callsign(""), "")
        self.assertEqual(airline_from_callsign("SJX"), "")


class TestResolveIdentity(unittest.TestCase):
    def test_complete_identity_for_known_aircraft(self):
        ident = resolve_identity(icao="A320", airline="SJX",
                                 registration="B-8493", measured_engines=2,
                                 callsign="SJX110")
        self.assertEqual(ident.equipment, "A320")
        self.assertEqual(ident.airline, "SJX")
        self.assertTrue(ident.livery.startswith("SWIFT_A"), ident.livery)
        self.assertEqual(ident.engines, 2)
        self.assertEqual(ident.missing, ())
        self.assertTrue(ident.pi_sendable)

    def test_engines_fall_back_to_aircraft_db(self):
        """桥报 0（机型未加载完）时必须用库内台数兜底，否则站点 engines=null。"""
        ident = resolve_identity(icao="B744", airline="SJX", measured_engines=0)
        self.assertEqual(ident.engines, 4)
        self.assertEqual(ident.source_of("engines"), "db")

    def test_airline_from_callsign_when_config_empty(self):
        ident = resolve_identity(icao="A320", callsign="SJX110")
        self.assertEqual(ident.airline, "SJX")
        self.assertEqual(ident.source_of("airline"), "callsign")

    def test_manual_icao_wins(self):
        ident = resolve_identity(icao="B738", manual_icao="A359",
                                 callsign="SJX110")
        self.assertEqual(ident.equipment, "A359")
        self.assertEqual(ident.source_of("equipment"), "manual")

    def test_atc_placeholder_is_inferred_from_name(self):
        ident = resolve_identity(icao="ATCCOM",
                                 name="FenixAirbus A320 CFM Lufthansa",
                                 callsign="SJX110", measured_engines=2)
        self.assertEqual(ident.equipment, "A320")

    def test_unknown_aircraft_reports_missing_not_guesses(self):
        # XXXX 不在 swift 机型库（ZZZZ 是库内"未知"占位机型，有 id）
        ident = resolve_identity(icao="XXXX", name="", callsign="SJX110")
        self.assertEqual(ident.equipment, "XXXX")
        self.assertIn("aircraft.livery", ident.missing)
        self.assertIn("aircraft.engines", ident.missing)
        # 有机型码 → 仍允许发 PI（equipment 有价值），但不许伪造 livery
        self.assertTrue(ident.pi_sendable)

    def test_nothing_known_is_not_sendable(self):
        ident = resolve_identity(icao="ATCCOM", name="", airline="SJX",
                                 callsign="SJX110")
        self.assertFalse(ident.pi_sendable)
        self.assertIn("aircraft.equipment", ident.missing)

    def test_audit_line_mentions_all_four_fields(self):
        ident = AircraftIdentity(equipment="A320", airline="SJX",
                                 livery="SWIFT_A672", engines=2)
        line = ident.audit()
        for key in ("equipment=A320", "airline=SJX", "livery=SWIFT_A672",
                    "engines=2"):
            self.assertIn(key, line)
        self.assertNotIn("missing=", line)

    def test_registration_can_be_kept_out_of_livery(self):
        with_reg = resolve_identity(icao="A320", registration="B-8493")
        without = resolve_identity(icao="A320", registration="B-8493",
                                   registration_in_livery=False)
        self.assertTrue(with_reg.livery.endswith("-B-8493"))
        self.assertFalse(without.livery.endswith("-B-8493"))


class TestEngineHelpers(unittest.TestCase):
    def test_engine_mask_shape_matches_server_schema(self):
        mask = engine_mask(2)
        self.assertEqual(mask, {"1": {"on": False}, "2": {"on": False}})
        # 越界保护：模型再离谱也不能产生 100 台引擎
        self.assertEqual(len(engine_mask(100)), 8)
        self.assertEqual(engine_mask(0), {})

    def test_engines_from_parts_both_shapes(self):
        self.assertEqual(engines_from_parts(
            {"engines": {"1": {"on": True}, "2": {"on": False}}}), 2)
        self.assertEqual(engines_from_parts({"engines": [True, False]}), 2)
        self.assertEqual(engines_from_parts({"engine_count": 4}), 4)
        self.assertEqual(engines_from_parts(None), 0)


if __name__ == "__main__":
    unittest.main()