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

    def test_swift_shape_nests_engines(self):
        """engines 必须嵌进 config.aircraft.engines（服务器只认 swift 形状）。

        实测 api.xflysim.com：swift 客户端 engines 全部入库，顶层 engines
        被忽略（站点 JSON engines=null）。顶层副本保留给自家客户端 roster。"""
        cfg = {
            "lights": {"beacon_on": True},
            "engines": {"1": {"on": True}, "2": {"on": False}},
            "gear_down": True,
        }
        out = FsdSession._acfg_swift_shape(cfg)

        self.assertEqual(out["aircraft"]["engines"],
                         {"1": {"on": True}, "2": {"on": False}})
        self.assertEqual(out["engines"],
                         {"1": {"on": True}, "2": {"on": False}})

        # 原字典不被修改（send 端在 diff 之后才做形状变换）
        self.assertNotIn("aircraft", cfg)

        # 无引擎时不产生空 aircraft 对象（避免覆盖服务器的机型字段）
        empty = FsdSession._acfg_swift_shape({"lights": {"beacon_on": True}})
        self.assertNotIn("aircraft", empty)

    def test_acfg_swift_shape_synthesizes_engines(self):
        """桥当前帧没给 engines 时按台数补"全关"掩码。

        站点 `aircraft.engines` 只来自 ACC；缺这个键就是 null。宁可先报
        全关（下一帧真实值覆盖），也不留空。
        """
        out = FsdSession._acfg_swift_shape({"lights": {"beacon_on": True}},
                                           engine_count=2)
        self.assertEqual(out["engines"],
                         {"1": {"on": False}, "2": {"on": False}})
        self.assertEqual(out["aircraft"]["engines"],
                         {"1": {"on": False}, "2": {"on": False}})
        # 台数未知（0）时不伪造
        self.assertNotIn("engines",
                         FsdSession._acfg_swift_shape({"lights": {}}))

    def test_acfg_targets_include_server(self):
        """全量 ACC 必须同时发给服务器本体：站点 engines 的唯一来源。

        PI 里没有 engines（swift 的 PlaneInformation 只发 EQUIPMENT/AIRLINE/
        LIVERY），而 PI 我们本来就是发给 SERVER 的；ACC 过去只发 @94836。
        """
        fsd = FsdSession.__new__(FsdSession)
        fsd._acfg_server_ok = True
        self.assertEqual(fsd._acfg_targets(), ("@94836", "SERVER"))
        fsd._acfg_server_ok = False          # 服务器拒绝后自动退回
        self.assertEqual(fsd._acfg_targets(), ("@94836",))

    def test_flatten_swift_acfg(self):
        """接收侧：swift 客户端的嵌套 engines / flapsDown 归一化为内部形状。"""
        swift_cfg = {
            "aircraft": {"icaoCode": "A388", "engineCount": 4,
                         "engines": {"1": {"on": True}, "2": {"on": True},
                                     "3": {"on": True}, "4": {"on": True}}},
            "flapsDown": 15,
            "gearDown": False,
            "lights": {"beacon": True, "nav": True},
        }
        out = FsdSession._flatten_swift_acfg(swift_cfg)

        self.assertEqual(out["engines"]["4"]["on"], True)
        self.assertEqual(out["flaps_pct"], 15)
        self.assertEqual(out["gearDown"], False)   # 别名由 from_raw 处理

        # 入参不被修改；顶层 engines 优先（自家客户端形状）
        self.assertNotIn("engines", swift_cfg)
        both = FsdSession._flatten_swift_acfg({
            "engines": {"1": {"on": False}},
            "aircraft": {"engines": {"1": {"on": True}}},
        })
        self.assertIs(both["engines"]["1"]["on"], False)

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
