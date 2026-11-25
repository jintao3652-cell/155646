import unittest

from xfly_client.fsd import packet as pk
from xfly_client.fsd.parser import parse
from xfly_client.model import NetworkModel


class TestReplay(unittest.TestCase):
    """回放一条 FSD 序列：@ -> VI -> ^ -> PI -> ACC -> #DP。"""

    def setUp(self):
        self.m = NetworkModel()

    def tearDown(self):
        self.m.shutdown()

    def feed(self, line: str) -> None:
        self.m._on_fsd_packet(parse(line))

    def test_position_and_parts_sequence(self):
        pbh = pk.encode_pbh(5.0, -2.0, 90.0, True)
        self.feed(pk.pilot_position(
            "CCA90", "N", "2000", pk.NETWORK_RATING,
            31.2, 121.34, 47, 12, pbh))
        tr = self.m._players["CCA90"]
        self.assertAlmostEqual(tr.lat, 31.2)
        self.assertAlmostEqual(tr.lon, 121.34)
        self.assertEqual(tr.gs_kt, 12)
        self.assertTrue(tr.on_ground)
        self.assertAlmostEqual(tr.heading, 90.0, delta=1.0)

        # 中间位置：更新经纬度，地速/姿态沿用
        self.feed(pk.interim_position("CCA90", 31.201, 121.341, 47, 12, pbh))
        self.assertAlmostEqual(tr.lat, 31.201)
        self.assertEqual(tr.gs_kt, 12)

        # 视觉位置：AGL 生效；没有地速字段时保留原值
        self.feed(pk.visual_position(
            "CCA90", 31.202, 121.342, 48.0, 5.0, 2, 10.0, 0.0, 0.0))
        self.assertEqual(tr.alt_ft, 48)
        self.assertAlmostEqual(tr.agl_ft, 5.0)
        self.assertEqual(tr.gs_kt, 12)

        # PI：机型/航司/尾号
        self.m._on_plane_info_received(
            "CCA90", "PI",
            ["GEN", "EQUIPMENT=A321", "AIRLINE=CCA",
             "LIVERY=SWIFT_A1", "REG=B-1234"], "")
        self.assertEqual(tr.aircraft, "A321")
        self.assertEqual(tr.airline, "CCA")
        self.assertEqual(tr.registration, "B-1234")

        # ACC：部件合并
        self.m._on_aircraft_config_received("CCA90", {
            "flaps_pct": 20, "gear_down": True,
            "lights": {"beacon_on": True},
            "engines": {"1": {"on": True}},
            "on_ground": True,
        })
        self.assertEqual(tr.parts["flaps_pct"], 20)
        self.assertIs(tr.parts["gear_down"], True)
        self.assertIs(tr.parts["lights"]["beacon_on"], True)

        # 离线包移除
        self.feed("#DPCCA90:123")
        self.assertNotIn("CCA90", self.m._players)


if __name__ == "__main__":
    unittest.main()
