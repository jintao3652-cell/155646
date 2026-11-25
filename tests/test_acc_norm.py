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


if __name__ == "__main__":
    unittest.main()
