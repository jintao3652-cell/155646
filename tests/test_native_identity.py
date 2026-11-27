"""原生桥"本机身份/灯光/引擎数"采集口径的静态一致性检查。

这些断言只读 C 源码，不需要模拟器，用来堵住三类已发生过的回归：

  ① 把呼号当尾号上报（MSFS `ATC ID` 被机模写成 `SJX110`）
  ② 深度机模的灯光只维护 `LIGHT ON STATES` 总线，逐灯 SimVar 恒 false
  ③ 冷舱多发动机机型被兜底成"1 台"（站点/对端据此误判机型）
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8", errors="replace")


MC = "MConnect/src/datadefs.c"
MC_H = "MConnect/src/datadefs.h"
XC = "XConnect/src/datarefs.c"


class RegistrationChainTest(unittest.TestCase):
    def test_mconnect_filters_registration_shape(self):
        src = read(MC)
        self.assertIn("static int reg_like", src,
                      "MSFS 尾号必须做形状判定")
        self.assertIn("effective_registration", src,
                      "必须走 ATC ID → TITLE 的尾号来源链")
        # JSON 里不能再直接塞 ATC ID（那就是把呼号当尾号的老 bug）
        self.assertIsNone(
            re.search(r"^\s*g_flight\.atc_id,\s*$", src, re.M),
            "flight_data 的 registration 不能直连 g_flight.atc_id")

    def test_reg_like_rejects_type_codes(self):
        """B738 / A320NEO 这类机型码不能被认成尾号（源码断言其判据存在）。"""
        src = read(MC)
        block = src[src.index("static int reg_like"):]
        block = block[:block.index("\n}\n") + 3]
        self.assertIn("dash", block)
        self.assertIn("'N'", block)      # N 字头尾号（N829PL）
        self.assertIn("n < 5", block)    # 长度下限挡 B738

    def test_client_guard_against_own_callsign(self):
        model = read("xfly_client/model.py")
        self.assertIn("sim_reg.upper() == my_cs", model.replace(" ", " "))


class LightSourceTest(unittest.TestCase):
    def test_mconnect_reads_light_states_mask(self):
        src = read(MC)
        self.assertIn('"LIGHT ON STATES"', src,
                      "深度机模只有灯光总线可靠，必须读掩码")
        self.assertIn("light_or_mask", src)
        # 掩码必须与逐灯读数做并集，不能反过来把逐灯值覆盖掉
        self.assertRegex(src, r"return \(g_flight\.light_mask >> bit\) & 1u")

    def test_light_mask_bits_match_msfs_sdk(self):
        src = read(MC)
        expect = {
            "LT_BIT_NAV": 0, "LT_BIT_BEACON": 1, "LT_BIT_LANDING": 2,
            "LT_BIT_TAXI": 3, "LT_BIT_STROBE": 4, "LT_BIT_RECOG": 6,
            "LT_BIT_WING": 7, "LT_BIT_LOGO": 8, "LT_BIT_CABIN": 9,
        }
        for name, bit in expect.items():
            m = re.search(r"#define\s+" + name + r"\s+(\d+)", src)
            self.assertIsNotNone(m, f"缺少 {name}")
            self.assertEqual(int(m.group(1)), bit, f"{name} 位定义与 SDK 不符")

    def test_light_mask_lives_in_aux_packet(self):
        """掩码必须放在 AUX 包里：主包有字节数硬校验，不能随意加字段。"""
        src = read(MC)
        aux = re.search(r"typedef struct \{(.*?)\} FlightAuxPkt;", src, re.S)
        self.assertIsNotNone(aux)
        self.assertIn("lt_states", aux.group(1))
        self.assertIn("light_mask", read(MC_H))


class EngineCountTest(unittest.TestCase):
    def test_no_phantom_single_engine_fallback(self):
        """两侧都不许再把"台数未知"兜底成 1 台。"""
        for path in (MC, XC):
            src = read(path)
            self.assertIsNone(
                re.search(r"if\s*\(en\s*<\s*1\)\s*en\s*=\s*1\s*;", src),
                f"{path} 仍有 en<1 → 1 台的兜底（冷舱多发动机机型会报错台数）")

    def test_unknown_engine_count_omits_key(self):
        for path in (MC, XC):
            src = read(path)
            self.assertRegex(
                src, r"if\s*\(en\s*<=\s*0\)\s*eng\[0\]\s*=\s*'\\0'\s*;",
                f"{path} 台数未知时应整键不发，交给客户端用机型库补")

    def test_max_engines_seen_memory_exists(self):
        for path in (MC, XC):
            src = read(path)
            self.assertIn("g_max_engines_seen", src,
                          f"{path} 需要台数记忆，避免冷舱推断成 1 台")


if __name__ == "__main__":
    unittest.main()