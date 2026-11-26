"""原生插件与 Python 中间层的 schema / 索引一致性检查。

这些检查在 CI（tests.yml）里跑，用来堵住"Python 改了 canonical parts，
C 插件没跟上"这类断层——本机没有完整 VS 环境时尤其重要。
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

LIGHT_KEYS = (
    "beacon_on", "landing_on", "taxi_on",
    "nav_on", "strobe_on", "logo_on",
)


def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8", errors="replace")


def macro_int(text, name):
    m = re.search(r"#define\s+" + re.escape(name) + r"\s+(\d+)", text)
    if not m:
        raise AssertionError("missing macro " + name)
    return int(m.group(1))


class NativeSchemaTest(unittest.TestCase):
    def test_xconnect_csl_capacity_and_engine_groups(self):
        header = read("XConnect/src/csl.h")
        source = read("XConnect/src/csl.c")

        cap = macro_int(header, "CSL_MAX_DATAREFS")
        base_angle = macro_int(header, "CSL_DREF_ENG_ANGLE_N1")
        base_speed = macro_int(header, "CSL_DREF_ENG_SPEED_N1")
        base_prop = macro_int(header, "CSL_DREF_PROP_ANGLE_N1")

        block = re.search(r"kCslDataRefs\[CSL_MAX_DATAREFS\]\s*=\s*\{(.*?)\};",
                          source, re.S)
        self.assertIsNotNone(block, "kCslDataRefs table not found")
        names = re.findall(r'"([^"]+)"', block.group(1))
        self.assertLessEqual(len(names), cap, "dataref table overflows array")
        self.assertEqual(base_angle, 36)
        self.assertEqual(base_speed, base_angle + 7)
        self.assertEqual(base_prop, base_speed + 7)

        # 三组各 7 项（引擎 1..7），必须连续且顺序一致；
        # 旧代码把 speed[4] 写到 prop[1] 上，并让第 8 台越界写 anim[48]。
        for i in range(7):
            n = i + 1
            self.assertEqual(
                names[base_angle + i],
                "libxplanemp/engines/engine_rotation_angle_deg[%d]" % n)
            self.assertEqual(
                names[base_speed + i],
                "libxplanemp/engines/engine_rotation_speed_rad_sec[%d]" % n)
            self.assertEqual(
                names[base_prop + i],
                "libxplanemp/engines/prop_rotation_angle_deg[%d]" % n)
        self.assertGreater(base_prop + 7, 35)   # logo 之后
        self.assertLessEqual(base_prop + 7, cap)

    def test_xconnect_uses_group_macros_not_hardcoded_offsets(self):
        code = read("XConnect/src/players.c")
        for stale in ("ai = 36 +", "si = 39 +", "pi = 42 +"):
            self.assertNotIn(stale, code, "stale hardcoded dataref index")
        for macro in ("CSL_DREF_ENG_ANGLE_N1", "CSL_DREF_ENG_SPEED_N1",
                      "CSL_DREF_PROP_ANGLE_N1"):
            self.assertIn(macro, code)

    def test_mconnect_definition_ids_are_unique(self):
        header = read("MConnect/src/datadefs.h")
        defs = dict((k, int(v)) for k, v in
                    re.findall(r"#define\s+(DEF_[A-Z_]+)\s+(\d+)", header))
        reqs = dict((k, int(v)) for k, v in
                    re.findall(r"#define\s+(REQ_[A-Z_]+)\s+(\d+)", header))
        self.assertEqual(len(defs), len(set(defs.values())), "duplicate DEF_ id")
        # DEF_FLIGHT/REQ_FLIGHT 复用 1 是历史约定；新增定义绝不能撞请求 ID
        self.assertNotIn(defs["DEF_AI_PARTS"], reqs.values())
        self.assertNotIn(defs["DEF_XPNDR_STATE"], reqs.values())

    def test_both_plugins_parse_all_six_lights(self):
        xp = read("XConnect/src/xconnect.c")
        ms = read("MConnect/src/mconnect.c")
        for key in LIGHT_KEYS:
            self.assertIn('"%s"' % key, xp, "XConnect missing " + key)
            self.assertIn('"%s"' % key, ms, "MConnect missing " + key)

    def test_mconnect_has_realtime_parts_probe(self):
        code = read("MConnect/src/mconnect.c")
        players = read("MConnect/src/players.c")
        self.assertIn('"--parts-probe"', code)
        self.assertIn("players_parts_status(", code)
        self.assertIn("void players_parts_status(", players)
        self.assertIn("g_ai_parts_write_tried", players)

    def test_all_packed_databases_are_bundled(self):
        # 新增 b64 数据文件却忘了加 --add-data 时，打包版会在真机上
        # 静默退回旧的引擎类型猜测，CI 的纯 Python 测试却不会发现。
        spec = read("build_exe.ps1")
        for db in (ROOT / "xfly_client").glob("*.b64"):
            self.assertIn(db.name, spec, "%s not bundled" % db.name)


if __name__ == "__main__":
    unittest.main()
