"""配置读写：二进制文件 + 用户目录 + 机器绑定（防整份拷贝给他人）。

- 位置：Windows 在 %APPDATA%\\XFlySim-Client\\config.bin，
  其它平台在 ~/.config/xflysim-client/config.bin（独立于软件目录，
  分发/升级 exe 不影响配置，也避免用户把软件目录整个拷贝带走配置）。
- 格式：pickle 序列化字典后按「机器唯一密钥」做 XOR 混淆，前缀魔数。
  密钥来自本机 MachineGuid（Windows 注册表），因此把 config.bin 复制到
  其他机器后无法解密 → 加载失败自动回退默认配置（防共享/盗用）。
  注：属混淆而非加密，防"随手复制"级风险。
- 兼容：首次运行时若检测到旧版 config.json（软件根目录），自动迁移并删除。
"""

from __future__ import annotations

import json
import os
import pickle
import sys
import uuid
from dataclasses import asdict, dataclass, fields

_MAGIC = b"XFC1"   # 文件头，识别合法二进制配置


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


def config_dir() -> str:
    """配置文件目录（用户级，随系统账户持久）。"""
    if os.name == "nt":
        base = os.environ.get("APPDATA") or os.path.expanduser("~")
        return os.path.join(base, "XFlySim-Client")
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    return os.path.join(base, "xflysim-client")


def config_path() -> str:
    return os.path.join(config_dir(), "config.bin")


def _machine_key() -> bytes:
    """机器唯一密钥：Windows 取注册表 MachineGuid，其它平台取 MAC 地址。"""
    try:
        if os.name == "nt":
            import winreg
            with winreg.OpenKey(
                    winreg.HKEY_LOCAL_MACHINE,
                    r"SOFTWARE\Microsoft\Cryptography") as k:
                val, _ = winreg.QueryValueEx(k, "MachineGuid")
                return str(val).encode("utf-8")
    except OSError:
        pass
    return str(uuid.getnode()).encode("utf-8")


def _xor(data: bytes, key: bytes) -> bytes:
    if not key:
        return data
    out = bytearray(len(data))
    kl = len(key)
    for i, b in enumerate(data):
        out[i] = b ^ key[i % kl]
    return bytes(out)


def _dump(payload: dict) -> bytes:
    blob = pickle.dumps(payload, protocol=pickle.HIGHEST_PROTOCOL)
    return _MAGIC + _xor(blob, _machine_key())


def _load_blob(data: bytes) -> dict:
    if not data or not data.startswith(_MAGIC):
        raise ValueError("not a config file")
    obj = pickle.loads(_xor(data[len(_MAGIC):], _machine_key()))
    if not isinstance(obj, dict):
        raise ValueError("bad payload")
    return obj


def _legacy_json_path() -> str:
    """旧版 config.json（软件根目录）：源码运行时为仓库根，打包后为 exe 同目录。"""
    if getattr(sys, "frozen", False):
        return os.path.join(os.path.dirname(os.path.abspath(sys.executable)),
                            "config.json")
    return os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "config.json")


def _write(path: str, cfg: AppConfig) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(_dump(asdict(cfg)))
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)   # 原子替换：崩溃/被杀不会留下损坏配置


def load_config(path: str | None = None) -> AppConfig:
    """读取配置；不存在/损坏/机器不匹配 → 回退默认；并尝试迁移旧 config.json。"""
    path = path or config_path()
    try:
        with open(path, "rb") as f:
            return AppConfig.from_dict(_load_blob(f.read()))
    except Exception:
        pass
    legacy = _legacy_json_path()
    if legacy and os.path.exists(legacy):
        try:
            with open(legacy, "r", encoding="utf-8") as f:
                cfg = AppConfig.from_dict(json.load(f))
            _write(path, cfg)
            try:
                os.remove(legacy)
            except OSError:
                pass
            return cfg
        except Exception:
            pass
    return AppConfig()


def save_config(cfg: AppConfig, path: str | None = None) -> None:
    _write(path or config_path(), cfg)
