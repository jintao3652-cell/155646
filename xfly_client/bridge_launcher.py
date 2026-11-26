"""随客户端自动拉起 MConnect（MSFS 桥接）插件。

设计要点：
- **默认不弹窗**：启动的是无控制台子系统的 `MConnect.exe`（GUI 子系统），
  诊断请用 `MConnectConsole.exe`。
- **不重复启动**：启动前检查同名进程是否已在运行（按 exe 名匹配），已运行
  就复用，避免抢端口 51002（MConnect 自身也会退到 51003/51004）。
- **不接管停止**：默认不 kill 插件——客户端退出后插件继续监听，下次连线
  不必等它重新初始化 SimConnect（实测重连较慢）。如需随退关掉，把配置
  `mconnect_stop_on_exit` 设为 true。
- **失败静默**：文件不存在、系统不支持、权限不足都只记日志，不影响客户端
  主流程（X-Plane 用户根本不需要 MConnect）。
"""
from __future__ import annotations

import os
import subprocess
import threading
from typing import Callable, Optional


def _is_running(exe_name: str) -> bool:
    """按进程名判断是否已在运行（Windows 用 tasklist，其它平台跳过）。"""
    if os.name != "nt":
        return False
    try:
        out = subprocess.run(
            ["tasklist", "/FI", f"IMAGENAME eq {exe_name}", "/NH"],
            capture_output=True, text=True, timeout=5,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        ).stdout
        return exe_name.lower() in out.lower()
    except Exception:
        return False


class BridgeLauncher:
    """管理 MConnect.exe 的生命周期（启动 / 可选停止）。"""

    def __init__(self, app_dir: str, *,
                 on_log: Optional[Callable[[str], None]] = None,
                 toggle_labels: bool = False):
        self._app_dir = app_dir
        self._on_log = on_log
        self._toggle_labels = bool(toggle_labels)
        self._proc: Optional[subprocess.Popen] = None
        self._lock = threading.Lock()
        self._exe = os.path.join(app_dir, "MConnect", "build", "MConnect.exe")
        # 也认本地 VS2026 构建目录（开发机）
        if not os.path.isfile(self._exe):
            alt = os.path.join(app_dir, "MConnect", "build-vs2026", "MConnect.exe")
            if os.path.isfile(alt):
                self._exe = alt

    def _log(self, msg: str) -> None:
        if self._on_log:
            try:
                self._on_log(msg)
            except Exception:
                pass

    def start(self) -> bool:
        """启动 MConnect；返回是否确实拉起了新进程。"""
        with self._lock:
            if not os.path.isfile(self._exe):
                self._log("MConnect 未编译，跳过自动启动（X-Plane 不需此插件）")
                return False
            if _is_running("MConnect.exe"):
                self._log("MConnect 已在运行，复用现有进程")
                return False
            try:
                flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
                argv = [self._exe]
                if self._toggle_labels:
                    argv.append("--labels")
                self._proc = subprocess.Popen(
                    argv,
                    cwd=os.path.dirname(self._exe),
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    creationflags=flags,
                )
                self._log(f"MConnect 已启动（PID {self._proc.pid}）")
                return True
            except Exception as exc:
                self._log(f"MConnect 自动启动失败：{exc}")
                return False

    def stop(self) -> None:
        """仅在配置要求时终止由本启动器拉起的进程。"""
        with self._lock:
            proc, self._proc = self._proc, None
        if proc is not None and proc.poll() is None:
            try:
                proc.terminate()
                self._log("MConnect 已随客户端退出")
            except Exception:
                pass
