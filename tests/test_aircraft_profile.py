"""统一机型档案 / 部件中间层回归测试（对应 swift CAircraftParts 契约）。"""

import math
import unittest

from xfly_client.aircraft_profile import (
    AircraftParts, AircraftProfile, EngineType,
    SERVER_LIGHT_KEYS, SOURCE_FSD, SOURCE_GUESS,
)
from xfly_client.model import _merge_parts


class TestAircraftProfile(unittest.TestCase):
    def test_profile_engine_count_and_type(self):
        a346 = AircraftProfile.from_designator("A346")
        c172 = AircraftProfile.from_designator("C172")
        atr72 = AircraftProfile.from_designator("AT72")
        self.assertEqual(a346.engines, 4)
        self.assertEqual(a346.engine_type, EngineType.JET)
        self.assertEqual(c172.engine_type, EngineType.PISTON)
        self.assertEqual(atr72.engine_type, EngineType.TURBOPROP)

    def test_profile_unknown_designator_uses_measured(self):
        p = AircraftProfile.from_designator("XXXX", measured_engines=3)
        self.assertEqual(p.engines, 3)

    def test_variable_engine_count_1_to_4(self):
        raw = {"engines": {"1": {"on": 1}, "2": {"on": 1},
                           "3": {"on": 0}, "4": {"on": 1}}}
        parts = AircraftParts.from_raw(raw, source=SOURCE_FSD)
        eff = parts.effective(AircraftProfile.from_designator("A346"))
        self.assertEqual(eff["engines"],
                         {1: True, 2: True, 3: False, 4: True})
        self.assertEqual(eff["engine_count"], 4)
        # 报告端保留 1..n 完整掩码，不能被压成一个 bool
        payload = parts.to_full_json()
        self.assertEqual(set(payload["engines"]), {"1", "2", "3", "4"})


class TestCanonicalParts(unittest.TestCase):
    def test_msfs_five_lights_and_camel_aliases(self):
        raw = {"beaconOn": 1, "landingOn": 0, "taxiOn": 1,
               "navOn": 1, "strobeOn": 0, "onGround": 1}
        parts = AircraftParts.from_raw(raw, source=SOURCE_GUESS)
        self.assertEqual(parts.lights["beacon_on"], True)
        self.assertEqual(parts.lights["strobe_on"], False)
        self.assertNotIn("logo_on", parts.lights)   # 未知 ≠ 关
        self.assertIs(parts.on_ground, True)

    def test_nan_is_unknown_not_false(self):
        parts = AircraftParts.from_raw({
            "gear_down": float("nan"),
            "lights": {"logo_on": math.inf},
        })
        self.assertIsNone(parts.gear_down)
        self.assertNotIn("logo_on", parts.lights)

    def test_gear_fallback_follows_swift(self):
        parts = AircraftParts.from_raw({"on_ground": 1})
        self.assertIs(parts.is_fixed_gear_down(), True)
        parts2 = AircraftParts.from_raw({"gear_down": 0, "on_ground": 0})
        self.assertIs(parts2.is_fixed_gear_down(), False)

    def test_adjusted_lights_from_engines(self):
        parts = AircraftParts.from_raw({"engines": [True, False]})
        adj = parts.get_adjusted_lights()
        self.assertTrue(adj["cabin_on"])
        self.assertTrue(adj["recognition_on"])

    def test_server_schema_filters_six_lights(self):
        raw = {"lights": {k: 1 for k in (
            "strobe_on", "landing_on", "taxi_on", "beacon_on",
            "nav_on", "logo_on", "recognition_on", "cabin_on", "wing_on")}}
        out = AircraftParts.from_raw(raw).to_incremental_json()
        self.assertEqual(set(out["lights"]), set(SERVER_LIGHT_KEYS))
        self.assertIs(out["is_full_data"], False)

    def test_incremental_then_full_merge(self):
        inc = AircraftParts.from_raw({"gear_down": 1}, is_full_data=False)
        base = AircraftParts.from_raw(
            {"lights": {"nav_on": 1}, "flaps_pct": 30}, is_full_data=True)
        merged = base.merge(inc)
        self.assertIs(merged.gear_down, True)
        self.assertEqual(merged.flaps_pct, 30)
        self.assertEqual(merged.lights, {"nav_on": True})

        full = AircraftParts.from_raw({"lights": {"strobe_on": 1}},
                                      is_full_data=True)
        replaced = merged.merge(full)
        self.assertIsNone(replaced.gear_down)   # 旧值不得残留
        self.assertIsNone(replaced.flaps_pct)
        self.assertEqual(replaced.lights, {"strobe_on": True})

    def test_dict_merge_honors_full_flag(self):
        dst = {"gear_down": True, "lights": {"nav_on": True}}
        _merge_parts(dst, {"is_full_data": True,
                           "lights": {"strobe_on": False}})
        self.assertEqual(dst, {"lights": {"strobe_on": False}})


if __name__ == "__main__":
    unittest.main()
