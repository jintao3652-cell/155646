"""XConnect.xpl 单文件独立性的静态契约测试（不需 X-Plane）。"""
from __future__ import annotations

import importlib.util
import os
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def load_tool():
    p = ROOT / "tools/xconnect_selftest.py"
    spec = importlib.util.spec_from_file_location("xconnect_selftest", p)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class TestXConnectStandalone(unittest.TestCase):
    def test_pe_dependencies_are_minimal(self):
        tool = load_tool()
        xpl = ROOT / "XConnect/build-vs2026/XConnect.xpl"
        if not xpl.exists():
            xpl = ROOT / "XConnect/build/XConnect.xpl"
        if not xpl.exists():
            self.skipTest("未编译 XConnect.xpl")
        info = tool.pe_info(str(xpl))
        self.assertEqual(info["machine"], 0x8664, "必须是 x64")
        self.assertEqual(info["subsystem"], 2, "必须是 DLL（XP 插件）")
        for dll in info["dlls"]:
            self.assertIn(dll.lower(),
                          {"xplm_64.dll", "ws2_32.dll", "opengl32.dll", "kernel32.dll"},
                          f"不应有额外依赖: {dll}")

    def test_entry_has_three_param_signature(self):
        code = (ROOT / "XConnect/src/xconnect.c").read_text(encoding="utf-8")
        self.assertIn(
            "XPluginStart(char* outName, char* outSig, char* outDesc)", code,
            "XPluginStart 必须是 3 参数签名（x64 寄存器错位坑）")

    def test_csl_and_radio_are_optional(self):
        code = (ROOT / "XConnect/src/xconnect.c").read_text(encoding="utf-8")
        self.assertIn("radio_table_load(rp)", code)
        # CSL 缺失时回退 ZZZZ，不阻塞启动
        csl = (ROOT / "XConnect/src/csl.c").read_text(encoding="utf-8")
        self.assertIn("ZZZZ", csl)


if __name__ == "__main__":
    unittest.main()
