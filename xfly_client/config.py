"""配置读写：config.json（含密码，勿外传）。

路径规则：
- PyInstaller 打包后（sys.frozen）：exe 同目录 —— 该目录可写且持久，
  不会随 onefile 临时解包目录（sys._MEIPASS）被丢弃；
- 源码运行：仓库根目录。
"""

from __future__ import annotations

import json
import os
import sys
from dataclasses import asdict, dataclass, fields


@dataclass
class AppConfig:
    callsign: str = ""
    cid: str = ""
    password: str = ""
    real_name: str = ""
    aircraft_icao: str = "A320"
    airline_icao: str = ""
    remember_password: bool = True
    debug_mode: bool = False
    language: str = "en"
    flight_plan: dict | None = None
    server_host: str = "ol.xflysim.com"
    server_port: int = 6809
    xconnect_host: str = "127.0.0.1"
    xconnect_port: int = 51001

    @classmethod
    def from_dict(cls, d: dict) -> "AppConfig":
        known = {f.name for f in fields(cls)}
        data = {k: v for k, v in d.items() if k in known}
        return cls(**data)


def config_path() -> str:
    """config.json 完整路径（打包后位于 exe 同目录，源码运行时在仓库根）。"""
    if getattr(sys, "frozen", False):
        root = os.path.dirname(os.path.abspath(sys.executable))
    else:
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(root, "config.json")


def load_config(path: str | None = None) -> AppConfig:
    path = path or config_path()
    try:
        with open(path, "r", encoding="utf-8") as f:
            return AppConfig.from_dict(json.load(f))
    except (OSError, json.JSONDecodeError):
        return AppConfig()


def save_config(cfg: AppConfig, path: str | None = None) -> None:
    path = path or config_path()
    with open(path, "w", encoding="utf-8") as f:
        json.dump(asdict(cfg), f, ensure_ascii=False, indent=2)
