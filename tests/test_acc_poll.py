import unittest

from xfly_client.fsd.session import STATE_ONLINE
from xfly_client.model import (
    ACC_FULL_FAR_PER_TICK,
    ACC_FULL_POLL_FAR_S,
    ACC_FULL_POLL_NEAR_S,
    NetworkModel,
    PlayerTrack,
)


class FakeFsd:
    state = STATE_ONLINE
    _lat = 31.0
    _lon = 121.0

    def __init__(self):
        self.calls = []

    def request_aircraft_config(self, callsign):
        self.calls.append(callsign)


class TestAccPoll(unittest.TestCase):
    def setUp(self):
        self.m = NetworkModel.__new__(NetworkModel)
        self.m.fsd = FakeFsd()
        self.m._parts_req_at = {}

    def test_near_interval(self):
        trs = [PlayerTrack(callsign="N0", lat=31.1, lon=121.0)]  # ~11km
        self.assertEqual(self.m._poll_aircraft_configs(trs, 100.0), 1)
        self.assertEqual(
            self.m._poll_aircraft_configs(
                trs, 100.0 + ACC_FULL_POLL_NEAR_S - 0.1), 0)
        self.assertEqual(
            self.m._poll_aircraft_configs(
                trs, 100.0 + ACC_FULL_POLL_NEAR_S), 1)

    def test_far_interval(self):
        trs = [PlayerTrack(callsign="F0", lat=32.0, lon=121.0)]  # ~111km
        self.assertEqual(self.m._poll_aircraft_configs(trs, 100.0), 1)
        self.assertEqual(
            self.m._poll_aircraft_configs(
                trs, 100.0 + ACC_FULL_POLL_FAR_S - 0.1), 0)
        self.assertEqual(
            self.m._poll_aircraft_configs(
                trs, 100.0 + ACC_FULL_POLL_FAR_S), 1)

    def test_near_all_and_far_budget(self):
        near = [PlayerTrack(callsign=f"N{i}", lat=31.1, lon=121.0)
                for i in range(15)]
        far = [PlayerTrack(callsign=f"F{i}", lat=32.0, lon=121.0)
               for i in range(15)]
        trs = near + far
        # 15 个近距全部发送 + 本轮最多 10 个远距
        self.assertEqual(self.m._poll_aircraft_configs(trs, 100.0),
                         15 + ACC_FULL_FAR_PER_TICK)
        # 101s：近距未到 2s；剩余 5 个远距到期
        self.assertEqual(self.m._poll_aircraft_configs(trs, 101.0), 5)
        # 102s：近距 15 个到期
        self.assertEqual(self.m._poll_aircraft_configs(trs, 102.0), 15)


if __name__ == "__main__":
    unittest.main()
