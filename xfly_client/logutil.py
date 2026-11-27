"""日志脱敏、按北京时间分日写入与过期清理。

FSD 的 `#AP` 登录报文明文携带 CID 和密码，`#DP` 也带 CID；任何落盘/UI
回显/诊断导出前都必须先经过 :func:`redact`。

客户端日志写入独立的 ``logs/`` 目录，每天一个 ``xflysim-YYYY-MM-DD.log``
（日期按北京时间 UTC+8 切分），默认只保留最近 7 个自然日；每次写入会检查
日切并顺带清理过期文件。旧的 :class:`RotatingFileWriter` 保留供工具脚本用。
"""
from __future__ import annotations

import os
import re
import sys
import threading
from datetime import date, datetime, timedelta, timezone

# #AP<呼号>:<目标>:<CID>:<密码>:<rating>:... —— 遮蔽第 3/4 字段。
# 目标不一定是 SERVER，统一处理。
_AP_RE = re.compile(
    r"(#AP[^:\r\n]*:[^:\r\n]*:)([^:\r\n]*)(:)([^:\r\n]*)(:)",
    re.IGNORECASE,
)
# #DP<呼号>:<CID> —— 退出登录也带 CID。
_DP_RE = re.compile(r"(#DP[^:\r\n]*:)([^:\r\n]+)", re.IGNORECASE)
# 通用 key=value / JSON 键值：cid=12345、"password": "xxx" 等。
_KV_RE = re.compile(
    r"(?i)(\b(?:cid|clientid|password|passwd|pwd|secret|token|api[_-]?key|"
    r"credential|authorization|auth|cookie)\b[\"']?\s*[:=]\s*)([^\s,;}\]\r\n]+)"
)


def redact(text: str) -> str:
    """遮蔽 FSD 登录 CID/密码及常见凭据字段，保留其它字段便于排查。"""
    if not text:
        return text
    text = _AP_RE.sub(r"\1***\3***\5", text)
    text = _DP_RE.sub(r"\1***", text)
    return _KV_RE.sub(r"\1***", text)


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


# ---------------- 按北京时间分日的日志 ----------------

BEIJING_TZ = timezone(timedelta(hours=8), "UTC+8")
_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_LOG_NAME_RE = re.compile(r"^([\w-]+)-(\d{4}-\d{2}-\d{2})\.log$")
DEFAULT_PREFIX = "xflysim"
DEFAULT_KEEP_DAYS = 7


def _safe_prefix(prefix: str) -> str:
    safe = re.sub(r"[^A-Za-z0-9_-]", "_", prefix or "")
    return safe or DEFAULT_PREFIX


def beijing_date(now: datetime | None = None) -> date:
    """北京时间自然日；传入 naive datetime 时按其字面值当作北京时间。"""
    dt = now or datetime.now(BEIJING_TZ)
    if dt.tzinfo is None:
        return dt.date()
    return dt.astimezone(BEIJING_TZ).date()


def beijing_clock(now: datetime | None = None) -> str:
    """北京时间 HH:MM:SS，用于日志行内时间戳。"""
    dt = now or datetime.now(BEIJING_TZ)
    if dt.tzinfo is None:
        return dt.strftime("%H:%M:%S")
    return dt.astimezone(BEIJING_TZ).strftime("%H:%M:%S")


def app_base_dir() -> str:
    """稳定的程序目录：打包后用 exe 所在目录，源码运行时用仓库根目录。

    不能用 ``sys._MEIPASS``：那是 PyInstaller 的临时解包目录，退出即删。
    """
    if getattr(sys, "frozen", False):
        return os.path.dirname(os.path.abspath(sys.executable))
    return _REPO_ROOT


def default_log_dir(base_dir: str | None = None) -> str:
    """日志根目录：优先环境变量，其次程序目录下的 ``logs/``。"""
    env = os.environ.get("XFLYSIM_LOG_DIR")
    if env:
        return env
    return os.path.join(base_dir or app_base_dir(), "logs")


def daily_log_path(log_dir: str, prefix: str = DEFAULT_PREFIX,
                   day: date | None = None) -> str:
    """某一天对应的日志文件完整路径。"""
    d = day or beijing_date()
    return os.path.join(log_dir, f"{_safe_prefix(prefix)}-{d.isoformat()}.log")


def default_log_path(base_dir: str | None = None) -> str:
    """今天（北京时间）的日志文件路径。"""
    return daily_log_path(default_log_dir(base_dir), DEFAULT_PREFIX)


def cleanup_daily_logs(log_dir: str, prefix: str = DEFAULT_PREFIX,
                       keep_days: int = DEFAULT_KEEP_DAYS,
                       today: date | None = None) -> list[str]:
    """删除早于保留窗口的分日日志，返回被删除的文件名。

    ``keep_days=7`` 表示保留今天及前 6 天（共 7 个自然日），删除日期更早的
    ``<prefix>-YYYY-MM-DD.log``。其它文件（含旧 fsd.log、诊断包）一律不动。
    """
    day = today or beijing_date()
    cutoff = day - timedelta(days=max(1, int(keep_days)) - 1)
    safe = _safe_prefix(prefix)
    removed: list[str] = []
    try:
        names = os.listdir(log_dir)
    except OSError:
        return removed
    for name in names:
        m = _LOG_NAME_RE.match(name)
        if not m or m.group(1) != safe:
            continue
        try:
            file_day = date.fromisoformat(m.group(2))
        except ValueError:
            continue
        if file_day >= cutoff:
            continue
        try:
            os.remove(os.path.join(log_dir, name))
            removed.append(name)
        except OSError:
            pass
    return removed


class DailyLogWriter:
    """按北京时间日切、自动清理过期日志的线程安全写入器。

    ``now_fn`` 仅供测试注入；传入 naive datetime 视为已经是北京时间。
    """

    def __init__(self, log_dir: str, prefix: str = DEFAULT_PREFIX,
                 keep_days: int = DEFAULT_KEEP_DAYS,
                 now_fn=None) -> None:
        self.log_dir = log_dir
        self.prefix = _safe_prefix(prefix)
        self.keep_days = max(1, int(keep_days))
        self._now_fn = now_fn
        self._lock = threading.Lock()
        self._day = beijing_date(now_fn() if now_fn else None)
        self.path = daily_log_path(log_dir, self.prefix, self._day)
        cleanup_daily_logs(log_dir, self.prefix, self.keep_days, self._day)

    def _roll_if_needed(self) -> None:
        today = beijing_date(self._now_fn() if self._now_fn else None)
        if today == self._day:
            return
        self._day = today
        self.path = daily_log_path(self.log_dir, self.prefix, today)
        cleanup_daily_logs(self.log_dir, self.prefix, self.keep_days, today)

    def write(self, text: str) -> None:
        with self._lock:
            try:
                self._roll_if_needed()
                os.makedirs(self.log_dir, exist_ok=True)
                with open(self.path, "a", encoding="utf-8") as f:
                    f.write(text)
            except OSError:
                pass

    def cleanup(self) -> list[str]:
        """手动触发清理（供设置页/维护脚本调用）。"""
        with self._lock:
            return cleanup_daily_logs(self.log_dir, self.prefix,
                                      self.keep_days, self._day)


# 客户端 / UI / 诊断导出共用的四级日志级别。
LEVELS = ("DEBUG", "INFO", "WARN", "ERROR")
_LEVEL_TAG_RE = re.compile(r"^\[(DEBUG|INFO|WARN|ERROR)\]\s*", re.IGNORECASE)

_ERROR_WORDS = ("error", "exception", "failed", "failure", "错误", "失败",
                "异常", "crash", "traceback")
_WARN_WORDS = ("warn", "断开", "断线", "离线", "未发现", "未连接", "未上线",
               "超时", "timeout", "disconnect", "offline", "not found",
               "busy", "占用", "退化", "回退", "fallback")
_DEBUG_PREFIXES = ("> ", "< ")


def classify_level(text: str) -> str:
    """按内容判定级别：已有标签优先，其次 ERROR → WARN → DEBUG → INFO。"""
    s = str(text or "").strip()
    low = s.lower()
    tagged = _LEVEL_TAG_RE.match(s)
    if tagged:
        return tagged.group(1).upper()
    if any(k in low for k in _ERROR_WORDS):
        return "ERROR"
    if any(k in low for k in _WARN_WORDS):
        return "WARN"
    if s.startswith(_DEBUG_PREFIXES) or low.startswith("[debug]"):
        return "DEBUG"
    return "INFO"


def tag_level(level: str, text: str) -> str:
    """给日志正文加统一级别前缀（不重复添加）。"""
    lv = level if level in LEVELS else "INFO"
    body = _LEVEL_TAG_RE.sub("", str(text or ""))
    return f"[{lv}] {body}"


def strip_level(text: str) -> tuple[str, str]:
    """拆出 (级别, 正文)；无标签时级别为空串。"""
    s = str(text or "")
    m = _LEVEL_TAG_RE.match(s)
    return (m.group(1).upper(), s[m.end():]) if m else ("", s)


def format_log_line(level: str, text: str, ts: str = "") -> str:
    """落盘/诊断用的一行日志：时间戳 + 级别 + 已脱敏正文。"""
    stamp = ts or beijing_clock()
    return f"{stamp} {tag_level(level, redact(str(text or '')))}"
