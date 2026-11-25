import unittest

from xfly_client.fsd import packet as pk
from xfly_client.fsd.parser import parse, try_get_pilot_position


class TestPositionPackets(unittest.TestCase):
    def test_interim_roundtrip(self):
        line = pk.interim_position("SJX110", 31.2, 121.34, 47, 12, 123456)
        self.assertTrue(line.startswith("#SBSJX110:ALL:VI:"))
        p = parse(line)
        self.assertEqual(p.kind, "#SB")
        self.assertEqual(p.type, "VI")
        pp = try_get_pilot_position(p)
        self.assertIsNotNone(pp)
        self.assertEqual(pp.callsign, "SJX110")
        self.assertAlmostEqual(pp.lat, 31.2)
        self.assertAlmostEqual(pp.lon, 121.34)
        self.assertEqual(pp.alt_ft, 47)
        self.assertEqual(pp.gs_kt, 12)
        self.assertEqual(pp.pbh, 123456)

    def test_visual_roundtrip(self):
        line = pk.visual_position(
            "SJX110", 31.2, 121.34, 47.0, 0.0, 2,
            44.2, -1.8, 0.0, nose_gear=1.0)
        self.assertTrue(line.startswith("^SJX110:"))
        p = parse(line)
        self.assertEqual(p.kind, "^")
        pp = try_get_pilot_position(p)
        self.assertIsNotNone(pp)
        self.assertEqual(pp.callsign, "SJX110")
        self.assertAlmostEqual(pp.lat, 31.2)
        self.assertAlmostEqual(pp.lon, 121.34)
        self.assertEqual(pp.alt_ft, 47)
        self.assertAlmostEqual(pp.agl_ft, 0.0)
        self.assertEqual(pp.pbh, 2)
        self.assertTrue(pp.on_ground)


if __name__ == "__main__":
    unittest.main()
