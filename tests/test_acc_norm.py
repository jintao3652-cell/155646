import unittest

from xfly_client.fsd.session import FsdSession


class TestAccNorm(unittest.TestCase):
    def test_server_schema_and_booleans(self):
        raw = {
            "lights": {
                "beacon_on": 1, "landing_on": 0, "taxi_on": 1,
                "nav_on": 1, "strobe_on": 0, "logo_on": 1,
                "recognition_on": 1, "cabin_on": 0, "wing_on": 1,
            },
            "gear_down": 1,
            "spoilers_out": 0,
            "flaps_pct": 20,
            "on_ground": 1,
            "engines": {"1": {"on": 1}, "2": {"on": 0}},
        }
        out = FsdSession._norm_parts(raw)

        self.assertEqual(out["lights"], {
            "beacon_on": True, "landing_on": False, "taxi_on": True,
            "nav_on": True, "strobe_on": False, "logo_on": True,
        })
        self.assertIs(out["gear_down"], True)
        self.assertIs(out["spoilers_out"], False)
        self.assertIs(out["on_ground"], True)
        self.assertEqual(out["flaps_pct"], 20)
        self.assertIs(out["engines"]["1"]["on"], True)
        self.assertIs(out["engines"]["2"]["on"], False)

    def test_does_not_mutate(self):
        raw = {"lights": {"beacon_on": 1}}
        FsdSession._norm_parts(raw)
        self.assertEqual(raw, {"lights": {"beacon_on": 1}})

    def test_throttle_ratio_in_acc(self):
        """油门进 ACC（功能对等）：归一化后带 5% 量化值，随增量上报。"""
        raw = {
            "engines": {"1": {"on": 1}, "2": {"on": 1}},
            "throttle_ratio": 0.634,   # → 0.65（5% 量化）
        }
        out = FsdSession._norm_parts(raw)
        self.assertEqual(out["throttle_ratio"], 0.65)
        # 增量 diff 能看到油门变化（客户端 notify_aircraft_parts 的比较基准）
        prev = {"engines": {"1": {"on": True}, "2": {"on": True}},
                "lights": {}, "throttle_ratio": 0.6, "is_full_data": False}
        diff = {k: v for k, v in out.items() if prev.get(k) != v}
        self.assertEqual(diff, {"throttle_ratio": 0.65})

    def test_throttle_ratio_out_of_range(self):
        out = FsdSession._norm_parts({"throttle_ratio": 7.5})
        self.assertEqual(out["throttle_ratio"], 1.0)
        out = FsdSession._norm_parts({"throttle_ratio": -2})
        self.assertEqual(out["throttle_ratio"], 0.0)
        # 非法值（NaN/字符串）不产生该键
        out = FsdSession._norm_parts({"throttle_ratio": float("nan")})
        self.assertNotIn("throttle_ratio", out)


if __name__ == "__main__":
    unittest.main()
