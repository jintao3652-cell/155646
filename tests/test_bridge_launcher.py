"""MConnect 无窗口自动启动的契约测试（不实际拉起进程）。"""
from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


class TestBridgeLauncher(unittest.TestCase):
    def test_mconnect_target_is_gui_subsystem(self):
        cm = read("MConnect/CMakeLists.txt")
        self.assertIn("add_executable(MConnect WIN32", cm,
                      "MConnect 必须是 WIN32 子系统（无控制台窗口）")
        self.assertIn("add_executable(MConnectConsole ", cm,
                      "必须保留控制台版本供诊断")

    def test_gui_build_defines_macro(self):
        cm = read("MConnect/CMakeLists.txt")
        self.assertIn("target_compile_definitions(MConnect PRIVATE MCONNECT_GUI)", cm)
        code = read("MConnect/src/mconnect.c")
        self.assertIn("#ifdef MCONNECT_GUI", code)

    def test_config_switches_exist(self):
        from xfly_client.config import AppConfig
        cfg = AppConfig()
        self.assertTrue(cfg.auto_start_mconnect, "默认应自动启动")
        self.assertFalse(cfg.mconnect_stop_on_exit, "默认不应杀插件进程")

    def test_launcher_uses_no_window_and_avoids_duplicates(self):
        from xfly_client.bridge_launcher import BridgeLauncher
        code = (ROOT / "xfly_client/bridge_launcher.py").read_text(encoding="utf-8")
        self.assertIn("CREATE_NO_WINDOW", code)
        self.assertIn('_is_running("MConnect.exe")', code)
        self.assertIn("stdout=subprocess.DEVNULL", code)

    def test_launcher_skips_when_exe_missing(self):
        from xfly_client.bridge_launcher import BridgeLauncher
        with tempfile.TemporaryDirectory() as d:
            logs = []
            launcher = BridgeLauncher(d, on_log=logs.append)
            self.assertFalse(launcher.start())
            self.assertTrue(any("未编译" in m for m in logs))

    def test_webapp_passes_repo_root_not_parent(self):
        """webapp.py 必须传 app_base_dir()，不能对 __file__ 做两层 dirname。"""
        code = read("webapp.py")
        block = code[code.index("self._bridge = BridgeLauncher("):
                     code.index("self._bridge = BridgeLauncher(") + 400]
        self.assertIn("app_base_dir()", block)
        self.assertNotIn("dirname(os.path.dirname", block)

    def test_find_exe_tolerates_parent_dir_passed_by_mistake(self):
        from xfly_client.bridge_launcher import BridgeLauncher
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "XFlySim-Client"
            exe = root / "MConnect" / "build" / "MConnect.exe"
            exe.parent.mkdir(parents=True)
            exe.touch()
            # 传仓库根、传上一级，都应找到同一个 exe
            self.assertEqual(BridgeLauncher(str(root))._exe, str(exe))
            self.assertEqual(BridgeLauncher(tmp)._exe, str(exe))

    def test_traffic_lib_setting_passthrough(self):
        from xfly_client.config import AppConfig
        from xfly_client.bridge_launcher import BridgeLauncher
        cfg = AppConfig()
        self.assertEqual(cfg.msfs_traffic_lib, "auto",
                         "默认自动（FSLTL/AIG 都可用）")
        launcher = BridgeLauncher("dummy", traffic_lib="fsltl")
        self.assertEqual(launcher._traffic_lib, "fsltl")
        # 非法值回退 auto，不让坏配置污染命令行
        self.assertEqual(BridgeLauncher("dummy", traffic_lib="bogus")._traffic_lib,
                         "auto")
        self.assertIn("--traffic-lib", read("xfly_client/bridge_launcher.py"))
        for path in ("webapp.py", "main.py"):
            self.assertIn("msfs_traffic_lib", read(path),
                          f"{path} 必须把配置透传给启动器")

    def test_traffic_lib_runtime_command_wired(self):
        self.assertIn("Send:TRAFFICLIB", read("xfly_client/xconnect/client.py"))
        self.assertIn("Send:TRAFFICLIB", read("MConnect/src/mconnect.c"))
        self.assertIn("set_msfs_traffic_lib", read("webapp.py"))
        self.assertIn("set_msfs_traffic_lib", read("webui/src/store.js"))

    def test_webapp_wires_launcher(self):
        code = read("webapp.py")
        self.assertIn("BridgeLauncher(", code)
        self.assertIn("self._bridge.start()", code)
        self.assertIn("self._bridge.stop()", code)


if __name__ == "__main__":
    unittest.main()
