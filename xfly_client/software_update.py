"""软件中心（飞行员中心）发布包解析器。

与 ``updater.py`` 的“清单逐文件更新”不同，这里处理软件中心托管的
**单个发布包**：先取版本信息，再按包内容落盘。

接口（``api_base`` 为服务根地址，``uuid`` 为软件标识）：

    GET {api_base}/softwareVersion/latest/{uuid}
        -> {"softwareName", "version", "logs", "link", "md5", "createdTime"}
    GET {api_base}/softwareVersion/logs/{uuid}
        -> {"softwareName", "records": [{"version", "logs", "createdTime"}]}

    ``/software/latest/{uuid}`` 与 ``/software/logs/{uuid}`` 作为旧路径别名。

落盘规则（用户需求，不可改）：

1. 下载文件**不是 zip**：直接放进根目录（文件名取下载链接末段）；
2. 下载文件**是 zip**：先解压，按包内相对路径放进根目录，不额外套一层
   以压缩包命名的子目录。

安全边界：校验服务端 md5、单包大小上限、zip slip（绝对路径/盘符/``..``
以及符号链接逃出根目录），落盘前先备份、失败整体回滚。不碰清单里没有
列出的文件。
"""
from __future__ import annotations

import hashlib
import io
import json
import os
import re
import zipfile
from dataclasses import dataclass, field
from typing import Callable
from urllib.parse import quote, unquote, urljoin, urlparse

from . import updater as U
from .logutil import redact

Fetcher = Callable[[str, int], bytes]

_MD5_RE = re.compile(r"^[0-9a-fA-F]{32}$")
_LATEST_PATH = "softwareVersion/latest/{uuid}"
_LOGS_PATH = "softwareVersion/logs/{uuid}"
_LEGACY_LATEST_PATH = "software/latest/{uuid}"
_LEGACY_LOGS_PATH = "software/logs/{uuid}"


class SoftwareUpdateError(U.UpdateError):
    """软件中心响应/发布包不合法。"""


@dataclass
class Release:
    software_name: str
    version: str
    logs: str
    link: str
    md5: str
    created_time: str

    @property
    def md5_hex(self) -> str:
        return self.md5.lower()


@dataclass
class PackageResult:
    """落盘结果；语义与 updater.ApplyResult 对齐。"""

    replaced: list[str] = field(default_factory=list)
    md5_ok: bool = False
    zipped: bool = False
    error: str = ""

    @property
    def ok(self) -> bool:
        return not self.error

    def summary(self) -> dict:
        return {
            "ok": self.ok,
            "md5_ok": self.md5_ok,
            "zipped": self.zipped,
            "replaced_files": len(self.replaced),
            "error": self.error,
        }


# ---------------------------------------------------------------- 解析

def parse_release(data: dict) -> Release:
    """校验并规范化 ``latest`` 接口响应。"""
    if not isinstance(data, dict):
        raise SoftwareUpdateError("latest response must be a JSON object")
    version = str(data.get("version", "") or "").strip()
    link = str(data.get("link", "") or "").strip()
    md5 = str(data.get("md5", "") or "").strip()
    if not version:
        raise SoftwareUpdateError("latest response has no version")
    if not link:
        raise SoftwareUpdateError("latest response has no download link")
    if not _MD5_RE.match(md5):
        raise SoftwareUpdateError("latest response has invalid md5")
    return Release(
        software_name=str(data.get("softwareName", "") or "").strip(),
        version=version,
        logs=str(data.get("logs", "") or "").strip(),
        link=link,
        md5=md5,
        created_time=str(data.get("createdTime", "") or "").strip(),
    )


def parse_logs(data: dict) -> list[dict]:
    """校验 ``logs`` 接口响应，返回版本记录列表。"""
    if not isinstance(data, dict) or not isinstance(data.get("records"), list):
        raise SoftwareUpdateError("logs response must contain records[]")
    out: list[dict] = []
    for item in data["records"]:
        if not isinstance(item, dict):
            raise SoftwareUpdateError("invalid record in logs response")
        out.append({
            "version": str(item.get("version", "") or "").strip(),
            "logs": str(item.get("logs", "") or "").strip(),
            "createdTime": str(item.get("createdTime", "") or "").strip(),
        })
    return out


def _endpoint(api_base: str, path: str) -> str:
    return f"{api_base.rstrip('/')}/{path}"


def fetch_json(url: str, fetch: Fetcher, timeout: int) -> dict:
    raw = fetch(url, timeout)
    try:
        data = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        raise SoftwareUpdateError(
            f"invalid JSON from {redact(url)}: {e}") from e
    if not isinstance(data, dict):
        raise SoftwareUpdateError(f"expected JSON object from {redact(url)}")
    return data


def fetch_release(api_base: str, uuid: str, fetch: Fetcher,
                  timeout: int = 20, *, legacy: bool = False) -> Release:
    """读取最新版本信息；legacy=True 用 ``/software/latest`` 旧路径。"""
    path = _LEGACY_LATEST_PATH if legacy else _LATEST_PATH
    url = _endpoint(api_base, path.format(uuid=quote(uuid, safe="")))
    return parse_release(fetch_json(url, fetch, timeout))


def fetch_logs(api_base: str, uuid: str, fetch: Fetcher,
               timeout: int = 20, *, legacy: bool = False) -> list[dict]:
    path = _LEGACY_LOGS_PATH if legacy else _LOGS_PATH
    url = _endpoint(api_base, path.format(uuid=quote(uuid, safe="")))
    return parse_logs(fetch_json(url, fetch, timeout))


def resolve_download_url(api_base: str, link: str) -> str:
    """link 为绝对 URL 时原样使用，相对路径按 API 根地址解析。"""
    if urlparse(link).scheme in ("http", "https", "file"):
        return link
    return urljoin(api_base.rstrip("/") + "/", link.lstrip("/"))


def download_package(release: Release, fetch: Fetcher, timeout: int,
                     *, api_base: str = "") -> bytes:
    """下载发布包并按服务端 md5 校验；错误统一转 SoftwareUpdateError。"""
    url = resolve_download_url(api_base, release.link)
    try:
        data = fetch(url, timeout)
    except Exception as e:
        raise SoftwareUpdateError(
            f"download failed: {redact(url)} ({e})") from e
    if not data:
        raise SoftwareUpdateError("downloaded package is empty")
    if len(data) > U._MAX_FILE:
        raise SoftwareUpdateError("downloaded package too large")
    if hashlib.md5(data).hexdigest().lower() != release.md5_hex:
        raise SoftwareUpdateError(
            f"md5 mismatch: expected {release.md5_hex}, "
            f"got {hashlib.md5(data).hexdigest()}")
    return data


# ---------------------------------------------------------------- 落盘

def is_zip(data: bytes) -> bool:
    """按文件内容识别 zip，不依赖 URL 后缀（下载接口常不带扩展名）。"""
    return zipfile.is_zipfile(io.BytesIO(data))


def package_files(data: bytes, link: str) -> list[tuple[str, bytes]]:
    """把发布包拆成 ``(相对根目录路径, 内容)`` 列表。

    - 非 zip：只有一个文件，文件名取下载链接末段；
    - zip：按包内相对路径展开，目录项忽略，名字重复时后者覆盖。

    两类都经过 :func:`updater.safe_rel` 校验，杜绝 zip slip 与绝对路径。
    """
    if not is_zip(data):
        name = os.path.basename(unquote(urlparse(link).path)).strip()
        if not name:
            raise SoftwareUpdateError(
                "download link has no filename; cannot place non-zip package")
        return [(U.safe_rel(name), data)]

    out: dict[str, bytes] = {}
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as zf:
            for info in zf.infolist():
                if info.is_dir():
                    continue
                rel = U.safe_rel(info.filename)
                out[rel] = zf.read(info)
    except (zipfile.BadZipFile, OSError, RuntimeError) as e:
        raise SoftwareUpdateError(f"invalid zip package: {e}") from e
    if not out:
        raise SoftwareUpdateError("zip package contains no files")
    return list(out.items())


def apply_package(release: Release, data: bytes, root: str) -> PackageResult:
    """落盘：非 zip 直接放根目录；zip 解压后按包内路径放根目录。

    与 updater.apply_plans 一样先备份再原子替换，任何一步失败按逆序回滚。
    """
    result = PackageResult()
    result.zipped = is_zip(data)
    if hashlib.md5(data).hexdigest().lower() == release.md5_hex:
        result.md5_ok = True
    else:
        result.error = "md5 mismatch before apply"
        return result

    try:
        files = package_files(data, release.link)
    except (SoftwareUpdateError, U.UpdateError) as e:
        result.error = str(e)
        return result

    committed: list[tuple[str, str | None]] = []
    try:
        root_abs = os.path.abspath(root)
        for rel, payload in files:
            target = U.safe_join(root_abs, rel)
            backup = target + ".xupdate-bak"
            os.makedirs(os.path.dirname(target), exist_ok=True)
            if os.path.exists(target):
                try:
                    os.replace(target, backup)
                except PermissionError as e:
                    raise U.PreconditionError(
                        f"文件被占用，无法替换：{target}"
                        "（请关闭客户端/模拟器后重试）") from e
            committed.append((target, backup if os.path.exists(backup) else None))
            tmp = target + ".xupdate-tmp"
            try:
                with open(tmp, "wb") as f:
                    f.write(payload)
                    f.flush()
                    os.fsync(f.fileno())
                os.replace(tmp, target)
            except BaseException:
                try:
                    os.remove(tmp)
                except OSError:
                    pass
                raise
            result.replaced.append(target)

        versions = U.installed_versions(root_abs)
        versions["client"] = release.version
        payload = {
            "schema": "xflysim-update-installed",
            "versions": versions,
        }
        U._atomic_write(os.path.join(root_abs, U.INSTALLED_FILE),
                        json.dumps(payload, ensure_ascii=False,
                                   indent=2).encode("utf-8"))
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
    return result
