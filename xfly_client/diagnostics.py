"""诊断信息收集/导出（自动脱敏，不含密码）。

用法：
    python -m xfly_client.diagnostics
    python -m xfly_client.diagnostics --out diag.json
"""
from __future__ import annotations

import json
import os
import platform
import sys
import time
from typing import Optional

from .logutil import redact
from .version import BRIDGE_PROTOCOL, CLIENT_NAME, CLIENT_VERSION

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_LOG = os.path.join(_REPO_ROOT, "fsd.log")


def _log_tail(path: Optional[str], limit: int = 200) -> list[str]:
    if not path:
        return []
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()[-limit:]
    except OSError:
        return []
    return [redact(x.rstrip("\r\n")) for x in lines]


def collect(log_path: Optional[str] = DEFAULT_LOG,
            extra: Optional[dict] = None) -> dict:
    """收集便于排障的非敏感信息（日志尾部已脱敏）。"""
    info = {
        "client": CLIENT_NAME,
        "client_version": CLIENT_VERSION,
        "bridge_protocol": BRIDGE_PROTOCOL,
        "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
        "python": sys.version.split()[0],
        "platform": platform.platform(),
        "executable": sys.executable,
        "cwd": os.getcwd(),
        "log_path": log_path or "",
        "log_tail": _log_tail(log_path),
    }
    if extra:
        info["extra"] = extra
    return info


def export(path: Optional[str] = None, log_path: Optional[str] = DEFAULT_LOG,
           extra: Optional[dict] = None) -> str:
    """把诊断信息写成 JSON，返回实际路径。"""
    if not path:
        path = os.path.join(
            _REPO_ROOT,
            f"xflysim-diagnostics-{time.strftime('%Y%m%d-%H%M%S')}.json")
    data = collect(log_path=log_path, extra=extra)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    return path


def main() -> None:
    import argparse
    ap = argparse.ArgumentParser(description="XFlySim-Client diagnostics")
    ap.add_argument("--log", default=DEFAULT_LOG, help="日志路径")
    ap.add_argument("--out", default=None, help="导出 JSON 路径")
    args = ap.parse_args()
    if args.out:
        print(export(args.out, log_path=args.log))
    else:
        print(json.dumps(collect(log_path=args.log), ensure_ascii=False,
                         indent=2))


if __name__ == "__main__":
    main()
