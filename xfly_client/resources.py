"""他机模型资源层：CSL / FSLTL / AIG 的存在性检查与主动校验。

设计背景见 ``docs/资源层更新设计.md``。核心约束：

* 启动路径只做「目录存在性 + 读一个小清单」，**绝不遍历文件、绝不计算哈希**；
* 逐文件校验只在用户主动触发时执行，支持按包过滤、限速、取消；
* 用户自行安装但没有我们清单的资源标记为 ``unmanaged``，不覆盖、不删除。

本模块不访问网络；服务器清单下载与包替换属于后续资源管理器 UI 的职责。
"""
from __future__ import annotations

import hashlib
import json
import ntpath
import os
import time
from dataclasses import dataclass, field
from typing import Callable, Optional

CSL = "csl"
FSLTL = "fsltl"
AIG = "aig"
KINDS = (CSL, FSLTL, AIG)

MANIFEST_NAME = "xflysim-resource-manifest.json"
VERIFY_NAME = "xflysim-resource-verify-index.json"
SCHEMA_MANIFEST = "xflysim-resource-manifest"
SCHEMA_VERIFY = "xflysim-resource-verify-index"
SCHEMA_VERSION = 1

_CHUNK = 1024 * 1024
_STATE_OK = "ok"
_STATE_MISSING = "missing"
_STATE_UNMANAGED = "unmanaged"
_STATE_INVALID = "invalid_manifest"
_STATE_OUTDATED = "outdated"


@dataclass
class ResourceStatus:
    """启动路径上的轻量检查结果（不含任何逐文件信息）。"""

    kind: str
    root: str = ""
    present: bool = False
    state: str = _STATE_MISSING
    resource_version: str = ""
    package_count: int = 0
    has_verify_index: bool = False
    reason: str = ""
    checked_at: float = 0.0

    def to_dict(self) -> dict:
        """诊断导出用的摘要；刻意不含绝对路径以外的资源细节。"""
        return {
            "kind": self.kind,
            "state": self.state,
            "present": self.present,
            "resource_version": self.resource_version,
            "package_count": self.package_count,
            "has_verify_index": self.has_verify_index,
            "reason": self.reason,
        }


@dataclass
class PackageResult:
    """一个包内的校验汇总。"""

    ok: bool = True
    files: int = 0
    bytes: int = 0
    missing: int = 0
    mismatch: int = 0

    def to_dict(self) -> dict:
        return {
            "ok": self.ok,
            "files": self.files,
            "bytes": self.bytes,
            "missing": self.missing,
            "mismatch": self.mismatch,
        }


@dataclass
class VerifyReport:
    """用户主动校验的结果。``cancelled`` 为真时其余字段为已完成部分。"""

    ok: bool = False
    cancelled: bool = False
    reason: str = ""
    kind: str = ""
    root: str = ""
    files: int = 0
    bytes: int = 0
    elapsed: float = 0.0
    packages: dict[str, PackageResult] = field(default_factory=dict)

    def to_dict(self) -> dict:
        return {
            "ok": self.ok,
            "cancelled": self.cancelled,
            "reason": self.reason,
            "kind": self.kind,
            "files": self.files,
            "bytes": self.bytes,
            "elapsed": round(self.elapsed, 3),
            "packages": {k: v.to_dict() for k, v in self.packages.items()},
        }


def _read_json(path: str) -> Optional[dict]:
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError):
        return None
    return data if isinstance(data, dict) else None


def _valid_manifest(data: Optional[dict]) -> bool:
    if not data:
        return False
    if data.get("schema") != SCHEMA_MANIFEST:
        return False
    if data.get("version") != SCHEMA_VERSION:
        return False
    pkgs = data.get("packages")
    return isinstance(pkgs, list) and all(
        isinstance(p, dict) and isinstance(p.get("id"), str)
        for p in pkgs
    )


def _valid_index(data: Optional[dict]) -> bool:
    if not data:
        return False
    if data.get("schema") != SCHEMA_VERIFY:
        return False
    if data.get("version") != SCHEMA_VERSION:
        return False
    return isinstance(data.get("files"), dict)


def discover_root(kind: str, *,
                  root: str | None = None,
                  msfs_community: str | None = None,
                  xp_root: str | None = None) -> str:
    """定位资源根目录；不硬编码盘符，优先用显式参数 / 环境变量。

    找不到时返回首个预期路径（供 UI 显示与用户配置），由调用方判断存在性。
    """
    if kind not in KINDS:
        raise ValueError(f"unknown resource kind: {kind}")
    if root:
        return root

    env_name = {
        CSL: "XFLYSIM_CSL_ROOT",
        FSLTL: "XFLYSIM_FSLTL_ROOT",
        AIG: "XFLYSIM_AIG_ROOT",
    }[kind]
    env = os.environ.get(env_name)
    if env:
        return env

    candidates: list[str] = []
    if kind == CSL:
        base = xp_root or os.environ.get("XPLANE_ROOT") or ""
        if base:
            candidates.append(os.path.join(
                base, "Resources", "plugins", "XConnect", "CSL"))
    else:
        community = msfs_community or os.environ.get("MSFS_COMMUNITY") or ""
        pkg = "fsltl-traffic-base" if kind == FSLTL else "aig-aitraffic-oci"
        if community:
            candidates.append(os.path.join(community, pkg))

    for c in candidates:
        if os.path.isdir(c):
            return c
    return candidates[0] if candidates else ""


def quick_check(kind: str, root: str | None = None, *,
                expected_version: str | None = None,
                now: float | None = None,
                **discover_kwargs) -> ResourceStatus:
    """启动路径检查：目录在不在 + 清单版本是什么。不 stat 文件、不哈希。

    ``expected_version`` 来自缓存的服务器清单版本（异步刷新）；不匹配只标记
    ``outdated``，绝不在此处触发下载。
    """
    if kind not in KINDS:
        raise ValueError(f"unknown resource kind: {kind}")
    path = root if root is not None else discover_root(kind, **discover_kwargs)
    st = ResourceStatus(kind=kind, root=path or "",
                        checked_at=time.time() if now is None else now)

    if not path or not os.path.isdir(path):
        st.state = _STATE_MISSING
        st.reason = "资源根目录不存在"
        return st

    st.present = True
    manifest = _read_json(os.path.join(path, MANIFEST_NAME))
    st.has_verify_index = os.path.isfile(os.path.join(path, VERIFY_NAME))

    if manifest is None:
        st.state = _STATE_UNMANAGED
        st.reason = "已安装但未纳入资源清单（用户自行安装，不覆盖）"
        return st
    if not _valid_manifest(manifest):
        st.state = _STATE_INVALID
        st.reason = "资源清单缺失字段或版本不认识"
        return st

    st.resource_version = str(manifest.get("resource_version") or "")
    st.package_count = len(manifest["packages"])
    if expected_version and st.resource_version != expected_version:
        st.state = _STATE_OUTDATED
        st.reason = f"本地清单 {st.resource_version}，可用版本 {expected_version}"
    else:
        st.state = _STATE_OK
    return st


def _safe_path(root: str, rel: str) -> str | None:
    """拒绝索引里的绝对路径与 ``..`` 逃逸，防止恶意清单读取任意文件。"""
    if not isinstance(rel, str) or not rel:
        return None
    rel = rel.replace("\\", "/").replace("/", os.sep)
    if ntpath.isabs(rel) or os.path.isabs(rel):
        return None
    norm = os.path.normpath(rel)
    parts = norm.split(os.sep)
    if ".." in parts:
        return None
    return os.path.join(root, norm)


def _file_digest(path: str, pace: Callable[[int], None],
                 cancel: Callable[[], bool], chunk_size: int) -> Optional[str]:
    h = hashlib.sha256()
    try:
        with open(path, "rb") as f:
            while True:
                if cancel():
                    return None
                block = f.read(chunk_size)
                if not block:
                    break
                h.update(block)
                pace(len(block))
    except OSError:
        return None
    return h.hexdigest()


def verify(kind: str, root: str, *,
           only_packages: Optional[set[str] | list[str]] = None,
           max_bytes_per_sec: int = 0,
           should_cancel: Optional[Callable[[], bool]] = None,
           on_progress: Optional[Callable[[int, int, str], None]] = None,
           chunk_size: int = _CHUNK) -> VerifyReport:
    """主动校验（用户点击「校验资源」时调用）。

    * ``only_packages`` 按包过滤，例如 CSL 只校验 ``{"B738"}``；
    * ``max_bytes_per_sec`` > 0 时按已读字节数限速；
    * ``should_cancel`` 每块检查一次；取消时返回已完成部分；
    * 只报告，不删除、不修复任何文件。
    """
    rep = VerifyReport(kind=kind, root=root)
    cancel = should_cancel or (lambda: False)
    if cancel():
        rep.cancelled = True
        rep.reason = "已取消"
        return rep

    manifest = _read_json(os.path.join(root, MANIFEST_NAME))
    index = _read_json(os.path.join(root, VERIFY_NAME))
    if not _valid_manifest(manifest) or not _valid_index(index):
        rep.reason = "清单或校验索引无效，无法校验"
        return rep
    if manifest.get("kind") and manifest["kind"] != kind:
        rep.reason = "清单类型与资源类型不符"
        return rep
    if index.get("resource_version") and \
            index["resource_version"] != manifest.get("resource_version"):
        rep.reason = "校验索引版本与清单版本不一致"
        return rep

    wanted = set(only_packages) if only_packages else None
    groups: dict[str, list[tuple[str, dict]]] = {}
    for rel, meta in index["files"].items():
        if not isinstance(meta, dict):
            continue
        pkg = meta.get("pkg") or ""
        if wanted is not None and pkg not in wanted:
            continue
        groups.setdefault(pkg, []).append((rel, meta))

    total = sum(len(v) for v in groups.values())
    done = 0
    started = time.monotonic()
    bytes_seen = 0

    def pace(n: int) -> None:
        nonlocal bytes_seen
        bytes_seen += n
        if max_bytes_per_sec and max_bytes_per_sec > 0:
            target = bytes_seen / float(max_bytes_per_sec)
            delay = target - (time.monotonic() - started)
            if delay > 0:
                time.sleep(delay)

    for pkg in sorted(groups):
        result = rep.packages.setdefault(pkg, PackageResult())
        for rel, meta in groups[pkg]:
            if cancel():
                rep.cancelled = True
                rep.reason = "已取消"
                rep.elapsed = time.monotonic() - started
                return rep
            full = _safe_path(root, rel)
            result.files += 1
            rep.files += 1
            if full is None or not os.path.isfile(full):
                result.missing += 1
                result.ok = False
                done += 1
                if on_progress:
                    on_progress(done, total, pkg)
                continue
            digest = _file_digest(full, pace, cancel, chunk_size)
            if digest is None:
                if cancel():
                    rep.cancelled = True
                    rep.reason = "已取消"
                    rep.elapsed = time.monotonic() - started
                    return rep
                result.missing += 1
                result.ok = False
            else:
                size = int(meta.get("bytes") or 0)
                result.bytes += size
                rep.bytes += size
                expected = str(meta.get("sha256") or "")
                if expected and digest.lower() != expected.lower():
                    result.mismatch += 1
                    result.ok = False
            done += 1
            if on_progress:
                on_progress(done, total, pkg)

    rep.ok = all(p.ok for p in rep.packages.values())
    rep.elapsed = time.monotonic() - started
    return rep


def diagnostics_summary(**discover_kwargs) -> list[dict]:
    """给诊断 JSON 的 ``extra.resources`` 用；不触发任何哈希或下载。"""
    return [quick_check(k, **discover_kwargs).to_dict() for k in KINDS]
