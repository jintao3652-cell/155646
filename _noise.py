"""过滤 pywebview 6.x + pythonnet 3.x 在本机的内部噪音日志（不影响功能）。

这些行由 pywebview 后台线程打印到 stderr，是 WebView2 COM 对象跨线程
属性同步失败的诊断信息（pythonnet 3.2 行为），不是程序错误。
"""

from __future__ import annotations

import sys


class _NoiseFilter:
    def __init__(self, real) -> None:
        self._real = real

    @staticmethod
    def _noisy(msg: str) -> bool:
        return ("[pywebview] Error while processing" in msg
                or "CoreWebView2Controller members can only be accessed"
                in msg or "CoreWebView2 can only be accessed" in msg
                or "maximum recursion depth exceeded" in msg)

    def write(self, msg: str) -> int:
        if not self._noisy(msg):
            self._real.write(msg)
        return len(msg)

    def flush(self) -> None:
        self._real.flush()

    def __getattr__(self, name):
        return getattr(self._real, name)


def suppress_noise() -> None:
    if not isinstance(sys.stderr, _NoiseFilter):
        sys.stderr = _NoiseFilter(sys.stderr)
