"""日志脱敏与滚动写入。

FSD 的 `#AP` 登录报文里带明文密码；任何落盘/UI 回显前都必须先经过
:func:`redact`。滚动写入用于限制 `fsd.log` / 插件日志体积。
"""
from __future__ import annotations

import os
import re
import threading

# #AP<呼号>:<目标>:<CID>:<密码>:<rating>:...
# 只替换第 4 个字段（密码）；目标不一定是 SERVER，统一处理。
_AP_RE = re.compile(
    r"(#AP[^:\r\n]*:[^:\r\n]*:[^:\r\n]*:)([^:\r\n]*)(:)",
    re.IGNORECASE,
)


def redact(text: str) -> str:
    """遮蔽 FSD 登录密码，保留其它字段便于排查。"""
    if not text:
        return text
    return _AP_RE.sub(r"\1***\3", text)


class RotatingFileWriter:
    """简单的大小滚动写入器（线程安全）。

    超过 max_bytes 时：path -> path.1，path.1 -> path.2，……，删除最旧的。
    """

    def __init__(self, path: str, max_bytes: int = 10 * 1024 * 1024,
                 backups: int = 3) -> None:
        self.path = path
        self.max_bytes = max_bytes
        self.backups = max(1, backups)
        self._lock = threading.Lock()

    def write(self, text: str) -> None:
        with self._lock:
            try:
                self._rotate_if_needed()
                with open(self.path, "a", encoding="utf-8") as f:
                    f.write(text)
            except OSError:
                pass

    def _rotate_if_needed(self) -> None:
        try:
            size = os.path.getsize(self.path)
        except OSError:
            return
        if size < self.max_bytes:
            return
        for i in range(self.backups - 1, 0, -1):
            src = f"{self.path}.{i}"
            dst = f"{self.path}.{i + 1}"
            if os.path.exists(src):
                try:
                    os.replace(src, dst)
                except OSError:
                    pass
        try:
            os.replace(self.path, f"{self.path}.1")
        except OSError:
            pass


def classify_level(text: str) -> str:
    """按内容粗略分级，供 UI/诊断报告使用（不改变原有日志格式）。"""
    s = (text or "").lower()
    if any(k in s for k in ("error", "exception", "failed", "失败",
                            "crash", "断开", "disconnect")):
        return "WARN"
    if any(k in s for k in ("未发现", "未连接", "超时", "timeout",
                            "offline", "not found")):
        return "WARN"
    return "INFO"
