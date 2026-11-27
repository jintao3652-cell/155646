"""XUpdate：客户端 / XConnect / MConnect 的统一更新核心。

边界（见 docs/资源层更新设计.md）：
- 本模块只管主程序与两个桥插件——**小文件、强一致、失败即不可用**；
- CSL / FSLTL / AIG 属于独立资源层，不在这里下载、不在这里校验。

设计要点：
- 默认只检查；``apply_plans`` 才改动磁盘。
- 按文件 sha256 + bytes 增量：内容没变就不下载。
- 下载到独立 staging 目录，校验通过后先备份旧文件，再原子替换；
  任何一步失败都按逆序回滚，绝不留下"半个版本"。
- 清单里的相对路径禁止绝对路径、盘符和 ``..``；目标解析后必须仍在
  组件根目录内（realpath 校验，符号链接也挡住）。
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
from dataclasses import dataclass, field
from typing import Callable, Iterable
from urllib.parse import quote, urljoin, urlparse
from urllib.request import urlopen

from .logutil import DailyLogWriter, default_log_dir, format_log_line, redact
from .version import CLIENT_VERSION

UPDATE_SCHEMA = "xflysim-update-manifest"
UPDATE_SCHEMA_VERSION = 1
UPDATER_VERSION = "1.0.0"
INSTALLED_FILE = "xupdate-installed.json"
DEFAULT_MANIFEST = os.path.join("update", "manifest.json")
COMPONENTS = ("client", "xconnect", "mconnect")
_HASH_RE = re.compile(r"^[0-9a-fA-F]{64}$")
_MAX_FILE = 2 * 1024 ** 3          # 单文件上限 2 GB（插件包远小于此）
_CHUNK = 1 << 20

Fetcher = Callable[[str, int], bytes]


class UpdateError(Exception):
    """清单/下载/写入失败。"""


class PreconditionError(UpdateError):
    """环境不满足（根目录缺失、插件正在运行导致文件被占用等）。"""


@dataclass
class FileEntry:
    path: str                 # 相对组件根目录，用 '/' 分隔
    sha256: str
    size: int
    url: str = ""

    @property
    def key(self) -> str:
        return self.path.replace("\\", "/").lstrip("/")


@dataclass
class FileAction:
    entry: FileEntry
    target: str
    action: str               # 'download' | 'skip'
    reason: str = ""


@dataclass
class UpdatePlan:
    component: str
    root: str
    version: str
    current: str | None
    actions: list[FileAction] = field(default_factory=list)

    @property
    def needed(self) -> list[FileAction]:
        return [a for a in self.actions if a.action == "download"]

    @property
    def up_to_date(self) -> bool:
        return not self.needed and same_version(self.current, self.version)

    def summary(self) -> dict:
        return {
            "component": self.component,
            "version": self.version,
            "installed": self.current,
            "up_to_date": self.up_to_date,
            "files_to_download": len(self.needed),
            "files_unchanged": len(self.actions) - len(self.needed),
            "bytes_to_download": sum(a.entry.size for a in self.needed),
        }


@dataclass
class ApplyResult:
    replaced: list[str] = field(default_factory=list)
    downloaded: int = 0
    versions: dict[str, str] = field(default_factory=dict)
    error: str = ""

    def summary(self) -> dict:
        return {
            "ok": not self.error,
            "replaced_files": len(self.replaced),
            "downloaded_files": self.downloaded,
            "versions": dict(self.versions),
            "error": self.error,
        }


# ---------------------------------------------------------------- 工具

def safe_rel(path: str) -> str:
    """校验清单里的相对路径，统一成 '/' 分隔。"""
    if not isinstance(path, str) or not path.strip():
        raise UpdateError("empty file path in manifest")
    p = path.strip().replace("\\", "/")
    if p.startswith("/") or re.match(r"^[A-Za-z]:", p):
        raise UpdateError(f"absolute path in manifest: {path!r}")
    parts = [x for x in p.split("/") if x not in ("", ".")]
    if not parts or any(x == ".." for x in parts):
        raise UpdateError(f"unsafe path in manifest: {path!r}")
    return "/".join(parts)


def safe_join(root: str, rel: str) -> str:
    """把清单相对路径落到根目录内；符号链接逃出根目录也拒绝。"""
    rel = safe_rel(rel)
    root_real = os.path.realpath(root)
    target = os.path.realpath(os.path.join(root_real, *rel.split("/")))
    if target != root_real and not target.startswith(root_real + os.sep):
        raise UpdateError(f"path escapes root: {rel!r}")
    return os.path.join(os.path.abspath(root), *rel.split("/"))


def parse_version(text: str | None) -> tuple[int, ...]:
    """把 ``v29`` / ``1.2.10`` / ``2026-11-27`` 变成可比较的元组。"""
    if not text:
        return ()
    nums = re.findall(r"\d+", str(text))
    return tuple(int(n) for n in nums) if nums else ()


def same_version(current: str | None, target: str) -> bool:
    if current is None:
        return False
    if str(current).strip().lower() == str(target).strip().lower():
        return True
    a, b = parse_version(current), parse_version(target)
    return bool(a) and bool(b) and a == b


def file_sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(_CHUNK), b""):
            h.update(chunk)
    return h.hexdigest()


def _atomic_write(path: str, data: bytes) -> None:
    parent = os.path.dirname(os.path.abspath(path))
    os.makedirs(parent, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".xupdate-", dir=parent)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    except BaseException:
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise


# ---------------------------------------------------------------- 清单

def parse_manifest(data: dict) -> dict:
    """校验清单结构；返回规范化后的副本。"""
    if not isinstance(data, dict):
        raise UpdateError("manifest must be a JSON object")
    if data.get("schema") != UPDATE_SCHEMA:
        raise UpdateError("unexpected manifest schema")
    if int(data.get("schema_version", -1)) != UPDATE_SCHEMA_VERSION:
        raise UpdateError("unsupported manifest schema_version")
    comps = data.get("components")
    if not isinstance(comps, dict):
        raise UpdateError("manifest.components must be an object")

    out = dict(data)
    out["components"] = {}
    for name, comp in comps.items():
        if name not in COMPONENTS:
            raise UpdateError(f"unknown component: {name!r}")
        if not isinstance(comp, dict):
            raise UpdateError(f"component {name} must be an object")
        version = str(comp.get("version", "")).strip()
        if not version:
            raise UpdateError(f"component {name} has no version")
        files = comp.get("files", [])
        if not isinstance(files, list):
            raise UpdateError(f"component {name}.files must be a list")
        entries: list[FileEntry] = []
        seen: set[str] = set()
        for item in files:
            if not isinstance(item, dict):
                raise UpdateError(f"component {name} has invalid file entry")
            key = safe_rel(str(item.get("path", "")))
            if key in seen:
                raise UpdateError(f"duplicate path in {name}: {key}")
            seen.add(key)
            sha = str(item.get("sha256", "")).strip().lower()
            if not _HASH_RE.match(sha):
                raise UpdateError(f"invalid sha256 for {name}:{key}")
            size = item.get("bytes", item.get("size"))
            if not isinstance(size, int) or isinstance(size, bool) or size < 0:
                raise UpdateError(f"invalid bytes for {name}:{key}")
            if size > _MAX_FILE:
                raise UpdateError(f"file too large for {name}:{key}")
            url = str(item.get("url", "")).strip()
            entries.append(FileEntry(key, sha, size, url))
        out["components"][name] = {
            "version": version,
            "files": entries,
            "root": str(comp.get("root", "") or ""),
        }
    return out


def read_manifest(source: str, *, timeout: int = 20,
                  fetch: Fetcher | None = None) -> dict:
    """从本地路径或 URL 读取清单。"""
    fetch = fetch or default_fetch
    if os.path.isfile(source):
        with open(source, "rb") as f:
            raw = f.read()
    else:
        raw = fetch(source, timeout)
    try:
        return parse_manifest(json.loads(raw.decode("utf-8")))
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        raise UpdateError(f"invalid manifest JSON: {e}") from e


def default_fetch(url: str, timeout: int = 20) -> bytes:
    """下载单个 URL；支持 http(s) 与 file://（本地测试/离线分发）。"""
    parsed = urlparse(url)
    if parsed.scheme not in ("http", "https", "file"):
        raise UpdateError(f"unsupported URL scheme: {parsed.scheme!r}")
    if parsed.scheme == "file":
        path = parsed.path
        # file:///C:/x 在 Windows 上多了个前导斜杠；UNC 则用 netloc。
        if os.name == "nt" and re.match(r"^/[A-Za-z]:/", path):
            path = path[1:]
        if parsed.netloc and parsed.netloc.lower() != "localhost":
            path = os.path.join("\\\\" + parsed.netloc, path.lstrip("/"))
        if not os.path.isfile(path):
            raise UpdateError(f"file not found: {path}")
        with open(path, "rb") as f:
            return f.read(_MAX_FILE + 1)
    try:
        with urlopen(url, timeout=timeout) as r:
            data = r.read(_MAX_FILE + 1)
    except Exception as e:
        raise UpdateError(f"download failed: {redact(url)} ({e})") from e
    if len(data) > _MAX_FILE:
        raise UpdateError(f"file too large: {redact(url)}")
    return data


def file_url(manifest: dict, component: str, entry: FileEntry) -> str:
    if entry.url:
        return entry.url
    base = str(manifest.get("base_url", "") or "").strip()
    if not base:
        raise UpdateError(f"no URL for {component}:{entry.key}")
    if not base.endswith("/"):
        base += "/"
    return urljoin(base, quote(entry.key))


def sample_manifest() -> dict:
    """给用户/服务端生成清单模板。"""
    return {
        "schema": UPDATE_SCHEMA,
        "schema_version": UPDATE_SCHEMA_VERSION,
        "generated_at": "2026-01-01T00:00:00Z",
        "base_url": "https://example.com/xflysim/releases/v1/",
        "components": {
            "client": {
                "version": "1.0.0",
                "files": [
                    {"path": "XFlySimClient.exe", "sha256": "0" * 64, "bytes": 0}
                ],
            },
            "xconnect": {
                "version": "v30",
                "root": "xplane",
                "files": [
                    {"path": "Resources/plugins/XConnect/win_x64/XConnect.xpl",
                     "sha256": "0" * 64, "bytes": 0}
                ],
            },
            "mconnect": {
                "version": "v30",
                "root": "mconnect",
                "files": [
                    {"path": "MConnect/build/MConnect.exe",
                     "sha256": "0" * 64, "bytes": 0}
                ],
            },
        },
    }


# ---------------------------------------------------------------- 计划

def installed_versions(base_dir: str) -> dict[str, str]:
    path = os.path.join(base_dir, INSTALLED_FILE)
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        if isinstance(data, dict) and data.get("schema") == "xflysim-update-installed":
            return {k: str(v) for k, v in data.get("versions", {}).items()}
    except (OSError, ValueError, TypeError):
        pass
    return {}


def resolve_root(component: str, *, base_dir: str,
                 xplane_dir: str | None = None,
                 mconnect_dir: str | None = None) -> str:
    if component == "client":
        return os.path.abspath(base_dir)
    if component == "xconnect":
        root = xplane_dir or os.environ.get("XPLANE_ROOT", "")
        if not root:
            raise PreconditionError(
                "X-Plane 根目录未指定：用 --xplane-dir 或环境变量 XPLANE_ROOT")
        return os.path.abspath(root)
    root = mconnect_dir or os.environ.get("XFLYSIM_MCONNECT_DIR", "") or base_dir
    return os.path.abspath(root)


def build_plans(manifest: dict, *, base_dir: str,
                components: Iterable[str] = COMPONENTS,
                xplane_dir: str | None = None,
                mconnect_dir: str | None = None,
                force: bool = False) -> list[UpdatePlan]:
    """生成增量计划：内容一致的文件标 skip，其余标 download。"""
    known = installed_versions(base_dir)
    if not known.get("client") and os.path.isfile(
            os.path.join(base_dir, "xfly_client", "version.py")):
        known["client"] = CLIENT_VERSION   # 源码树运行时可读内置版本

    plans: list[UpdatePlan] = []
    for name in components:
        if name not in COMPONENTS:
            raise UpdateError(f"unknown component: {name!r}")
        comp = manifest["components"].get(name)
        if comp is None:
            continue
        root = resolve_root(name, base_dir=base_dir,
                            xplane_dir=xplane_dir, mconnect_dir=mconnect_dir)
        current = known.get(name)
        actions: list[FileAction] = []
        for entry in comp["files"]:
            target = safe_join(root, entry.key)
            reason = ""
            need = force
            if not need and not os.path.isfile(target):
                need, reason = True, "missing"
            elif not need:
                try:
                    if os.path.getsize(target) != entry.size or \
                            file_sha256(target) != entry.sha256:
                        need, reason = True, "content mismatch"
                except OSError as e:
                    need, reason = True, f"unreadable: {e}"
            actions.append(FileAction(entry, target,
                                      "download" if need else "skip", reason))
        plans.append(UpdatePlan(name, root, comp["version"], current, actions))
    return plans


# ---------------------------------------------------------------- 应用

def _download_all(manifest: dict, plans: list[UpdatePlan], staging: str,
                  fetch: Fetcher, timeout: int, log) -> dict[int, str]:
    """下载并校验所有待更新文件；返回 action 序号 → staging 路径。"""
    staged: dict[int, str] = {}
    total = sum(len(p.needed) for p in plans)
    done = 0
    for plan in plans:
        for idx, action in enumerate(plan.actions):
            if action.action != "download":
                continue
            done += 1
            url = file_url(manifest, plan.component, action.entry)
            log(f"[{plan.component}] ({done}/{total}) downloading "
                f"{action.entry.key} <- {redact(url)}", "INFO")
            data = fetch(url, timeout)
            if len(data) != action.entry.size or \
                    hashlib.sha256(data).hexdigest() != action.entry.sha256:
                raise UpdateError(
                    f"hash/size mismatch after download: {plan.component}:"
                    f"{action.entry.key}")
            path = os.path.join(staging, f"{plan.component}-{done}")
            _atomic_write(path, data)
            staged[id(action)] = path
    return staged


def apply_plans(manifest: dict, plans: list[UpdatePlan], *, base_dir: str,
                fetch: Fetcher | None = None, timeout: int = 30,
                log: Callable[[str, str], None] | None = None) -> ApplyResult:
    """下载 → 逐文件备份+原子替换 → 记录版本；失败整体回滚。"""
    fetch = fetch or default_fetch
    log = log or (lambda msg, level="INFO": None)
    result = ApplyResult()
    to_apply = [p for p in plans if p.needed]
    if not to_apply:
        result.versions = {p.component: p.version for p in plans}
        return result

    staging = tempfile.mkdtemp(prefix="xupdate-")
    committed: list[tuple[str, str | None]] = []   # (target, backup_path)
    try:
        staged = _download_all(manifest, to_apply, staging, fetch, timeout, log)
        result.downloaded = len(staged)

        for plan in to_apply:
            for idx, action in enumerate(plan.actions):
                if action.action != "download":
                    continue
                target = action.target
                backup = target + ".xupdate-bak"
                os.makedirs(os.path.dirname(target), exist_ok=True)
                if os.path.exists(target):
                    try:
                        os.replace(target, backup)   # 被占用时 Windows 会抛错
                    except PermissionError as e:
                        raise PreconditionError(
                            f"文件被占用，无法替换：{target}（请关闭客户端/模拟器后重试）"
                        ) from e
                committed.append((target, backup if os.path.exists(backup) else None))
                tmp = target + ".xupdate-tmp"
                try:
                    shutil.copyfile(staged[id(action)], tmp)
                    os.replace(tmp, target)
                except BaseException:
                    try:
                        os.remove(tmp)
                    except OSError:
                        pass
                    raise
                result.replaced.append(target)
                log(f"[{plan.component}] replaced {action.entry.key}", "INFO")

        versions = installed_versions(base_dir)
        versions.update({p.component: p.version for p in plans})
        payload = {"schema": "xflysim-update-installed", "versions": versions}
        _atomic_write(os.path.join(base_dir, INSTALLED_FILE),
                      json.dumps(payload, ensure_ascii=False, indent=2).encode("utf-8"))
        result.versions = versions
    except BaseException as e:
        for target, backup in reversed(committed):
            try:
                if backup and os.path.exists(backup):
                    os.replace(backup, target)
                elif os.path.exists(target):
                    os.remove(target)
            except OSError:
                pass
        result.error = str(e)
        result.replaced = []
        log(f"update FAILED, rolled back: {redact(str(e))}", "ERROR")
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    return result


class UpdaterLog:
    """同时输出到控制台与 xupdate 分日日志（不污染客户端 fsd 日志）。"""

    def __init__(self, base_dir: str, quiet: bool = False) -> None:
        self.quiet = quiet
        self._writer: DailyLogWriter | None = None
        try:
            self._writer = DailyLogWriter(default_log_dir(base_dir), prefix="xupdate")
        except OSError:
            self._writer = None

    def __call__(self, message: str, level: str = "INFO") -> None:
        line = format_log_line(level, redact(str(message)))
        if not self.quiet:
            stream = sys.stderr if level == "DEBUG" else sys.stdout
            print(line, file=stream, flush=True)
        if self._writer:
            try:
                self._writer.write(line)
            except OSError:
                pass
