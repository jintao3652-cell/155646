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

import io
import json
import os
import pickle
import sys
import uuid
from dataclasses import asdict, dataclass, fields

# 文件头：XFC2 = JSON 载荷；XFC1 = 旧版 pickle 载荷（仅用于一次性安全迁移）
_MAGIC = b"XFC2"
_LEGACY_MAGIC = b"XFC1"


@dataclass
class AppConfig:
    callsign: str = ""
    cid: str = ""
    password: str = ""
    real_name: str = ""
    aircraft_icao: str = "A320"
    airline_icao: str = ""
    # 注册号（尾号）：用户手填值，最高优先；空串 = 自动识别（模拟器 → 上次记录）
    registration: str = ""
    # 自动识别结果缓存：机型 ICAO → 尾号（仅同机型复用，见 registration.py）
    registration_cache: dict | None = None
    remember_password: bool = True
    debug_mode: bool = False
    language: str = "en"
    flight_plan: dict | None = None
    # SimBrief 用户名/用户 ID：飞行计划页「SimBrief 导入」的上次输入值
    simbrief_user: str = ""
    server_host: str = "ol.xflysim.com"
    server_port: int = 6809
    xconnect_host: str = "127.0.0.1"
    # X-Plane 的 XConnect 插件端口
    xconnect_port: int = 51001
    # MSFS 的 MConnect 端口。必须与 X-Plane 错开：两者同时开时 51001 会被
    # X-Plane 占住（实测 X-Plane 12 独占），MConnect 用 51002。
    # 客户端同时探测 xconnect_port 与本端口（含退避 51003/51004），
    # XP11/XP12/MSFS2020/2024 任一在线即可连线 FSD。
    mconnect_port: int = 51002
    # 客户端启动时自动拉起 MConnect.exe（无窗口模式）。X-Plane 用户可关掉。
    auto_start_mconnect: bool = True
    # 客户端退出时是否一并终止 MConnect（默认否：保留 SimConnect 会话便于重连）
    mconnect_stop_on_exit: bool = False
    # MConnect 启动时带 --labels：翻转一次 MSFS 内置"交通铭牌"（Traffic
    # Nameplates）。铭牌显示的是 AI 的 ATC ID（MConnect 已写成远端呼号）。
    # 注意 toggle 语义：铭牌原本已开启时会被关闭。
    # 限制（模拟器硬约束，代码无法绕过）：
    #   1. 铭牌只能整体开/关，位置由模拟器固定绘制（锚在飞机包围球上方一段
    #      屏幕偏移），做不到贴机身；
    #   2. TOGGLE_AIRCRAFT_LABELS 事件在 MSFS 2020 已废弃（官方确认无替代），
    #      2020 上此开关无效，需在 选项→常规选项→交通→显示交通铭牌 手动设置；
    #   3. 事件只在 MConnect 启动时发一次，改配置后需重启 MConnect。
    #   4. X-Plane 侧呼号标签由 XConnect 自绘（贴机身），无需此开关。
    msfs_toggle_labels: bool = False
    # MSFS 他机 AI 模型库偏好：auto（两套库按匹配质量选用）/ fsltl / aig。
    # 仅影响 MConnect 的 AI 涂装匹配；X-Plane 侧固定使用 X-CSL 模型库，
    # 不提供该项选择。切换后 MConnect 运行时立即生效（下一帧重建 AI 模型）。
    msfs_traffic_lib: str = "auto"

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
    # 用 JSON 而不是 pickle：配置是纯数据，JSON 不存在反序列化执行代码的风险。
    blob = json.dumps(payload, ensure_ascii=False, separators=(",", ":"),
                      allow_nan=False).encode("utf-8")
    return _MAGIC + _xor(blob, _machine_key())


class _SafeUnpickler(pickle.Unpickler):
    """仅允许基础容器/标量的 unpickler，用于迁移旧版配置。

    正常 pickle 数据不会调用 find_class；任何试图引用其它类（REDUCE/GLOBAL）
    的恶意载荷都会被拒绝，从而避免 pickle 反序列化任意代码执行。
    """

    _ALLOWED = {
        ("builtins", n) for n in (
            "dict", "list", "tuple", "set", "frozenset", "str", "bytes",
            "bytearray", "int", "float", "bool", "complex", "NoneType",
        )
    }

    def find_class(self, module, name):  # type: ignore[override]
        if (module, name) in self._ALLOWED:
            return super().find_class(module, name)
        raise pickle.UnpicklingError(f"unsafe class {module}.{name}")


def _load_blob(data: bytes) -> dict:
    if not data:
        raise ValueError("empty config file")
    if data.startswith(_MAGIC):
        raw = _xor(data[len(_MAGIC):], _machine_key())
        obj = json.loads(raw.decode("utf-8"))
    elif data.startswith(_LEGACY_MAGIC):
        raw = _xor(data[len(_LEGACY_MAGIC):], _machine_key())
        obj = _SafeUnpickler(io.BytesIO(raw)).load()
    else:
        raise ValueError("not a config file")
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
            data = f.read()
        cfg = AppConfig.from_dict(_load_blob(data))
        if not data.startswith(_MAGIC):
            # 旧版 pickle 配置：加载成功后立即以新格式重写
            try:
                _write(path, cfg)
            except OSError:
                pass
        return cfg
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
