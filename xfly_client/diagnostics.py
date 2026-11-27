"""诊断信息收集/导出（自动脱敏，不含密码 / CID / 凭据）。

用法：
    python -m xfly_client.diagnostics
    python -m xfly_client.diagnostics --out diag.json
"""
from __future__ import annotations

import json
import os
import platform
import re
import sys
import time
from typing import Optional

from .logutil import (DEFAULT_PREFIX, app_base_dir, default_log_dir,
                      default_log_path, redact)
from .version import BRIDGE_PROTOCOL, CLIENT_NAME, CLIENT_VERSION

# 旧版单一大日志：只作为历史回退读取，新日志一律写 logs/ 下的分日文件。
LEGACY_LOG = os.path.join(app_base_dir(), "fsd.log")


def resolve_log_path() -> str:
    """诊断默认读取的日志：今天的分日文件 → 最近分日文件 → 旧 fsd.log。"""
    today = default_log_path()
    if os.path.isfile(today):
        return today
    log_dir = default_log_dir()
    try:
        recent = sorted(
            n for n in os.listdir(log_dir)
            if n.startswith(DEFAULT_PREFIX + "-") and n.endswith(".log")
        )
        if recent:
            return os.path.join(log_dir, recent[-1])
    except OSError:
        pass
    return LEGACY_LOG if os.path.isfile(LEGACY_LOG) else today

# 键名按非字母数字切分后精确匹配，避免 "accident" 这类词里的 "cid" 误伤。
_SENSITIVE_KEY_PARTS = {
    "password", "passwd", "pwd", "cid", "clientid", "token", "secret",
    "key", "apikey", "credential", "credentials", "authorization", "auth",
    "cookie", "sessionid",
}
_HOME = os.path.expanduser("~")


def _is_sensitive_key(key: object) -> bool:
    parts = re.split(r"[^a-z0-9]+", str(key).lower())
    return any(p and p in _SENSITIVE_KEY_PARTS for p in parts)


def _scrub(value):
    """递归脱敏：敏感键置为 ***，字符串过 redact 并隐藏用户目录。"""
    if isinstance(value, dict):
        return {
            k: ("***" if _is_sensitive_key(k) else _scrub(v))
            for k, v in value.items()
        }
    if isinstance(value, (list, tuple)):
        return [_scrub(v) for v in value]
    if isinstance(value, str):
        text = redact(value)
        if _HOME:
            text = re.sub(re.escape(_HOME), "~", text, flags=re.IGNORECASE)
        return text
    return value


def _log_tail(path: Optional[str], limit: int = 200) -> list[str]:
    if not path:
        return []
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()[-limit:]
    except OSError:
        return []
    return [redact(x.rstrip("\r\n")) for x in lines]


def collect(log_path: Optional[str] = None,
            extra: Optional[dict] = None) -> dict:
    """收集便于排障的非敏感信息（整棵结构递归脱敏）。"""
    log_path = log_path or resolve_log_path()
    info = {
        # 固定 schema，便于用户反馈时自动解析；v1 兼容早期只有 log_tail 的包。
        "schema": "xflysim-diagnostics",
        "schema_version": 1,
        "redaction": {
            "enabled": True,
            "method": "redact + recursive key scrub",
        },
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
    return _scrub(info)


def export(path: Optional[str] = None, log_path: Optional[str] = None,
           extra: Optional[dict] = None) -> str:
    """把诊断信息写成 JSON，返回实际路径。"""
    if not path:
        path = os.path.join(
            app_base_dir(),
            f"xflysim-diagnostics-{time.strftime('%Y%m%d-%H%M%S')}.json")
    data = collect(log_path=log_path, extra=extra)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    return path


def main() -> None:
    import argparse
    ap = argparse.ArgumentParser(description="XFlySim-Client diagnostics")
    ap.add_argument("--log", default=None,
                    help="日志路径（默认取 logs/ 下今天或最近的分日日志）")
    ap.add_argument("--out", default=None, help="导出 JSON 路径")
    args = ap.parse_args()
    if args.out:
        print(export(args.out, log_path=args.log))
    else:
        print(json.dumps(collect(log_path=args.log), ensure_ascii=False,
                         indent=2))


if __name__ == "__main__":
    main()
