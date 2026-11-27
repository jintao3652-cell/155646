"""客户端在线更新：检查接口 → 断点续传下载 → 校验 → 拉起更新助手。

与 ``xfly_client/updater.py``（XUpdate 清单 + sha256 多组件）是**两条独立通道**：
本模块只负责"客户端整包 exe 自更新"，服务端只要一个 JSON 接口：

    {
      "softwareName": "XFlySim Client",
      "version": "1.0.1",
      "logs": "修复若干问题",
      "link": "https://your.host/xflysim/releases/XFlySimClient.exe",
      "md5": "d41d8cd98f00b204e9800998ecf8427e",
      "createdTime": "2026-11-27 15:30:00"
    }

流程（Windows 上正在运行的 exe 不能自己覆盖自己）：

    ① 只比版本号（启动时自动查一次，失败静默）
    ② 下载到 %TEMP%\\XFlySimUpdate\\<version>\\*.part，支持 HTTP Range 续传
    ③ 边下边算 MD5，下完核对，不一致就删包重来
    ④ 先拉起独立更新助手 XUpdate.exe，再退出主程序
       → 助手：等本进程退出 → 旧文件改名 .bak → 原子替换 → 写版本
         → 重启新版本 → 自己退出；任何一步失败按逆序回滚

插件（XConnect.xpl / MConnect.exe）**不在**本通道内：它们被模拟器占用，
更新时机与客户端不同，仍走 XUpdate 的清单通道。
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass
from typing import Any, Callable
from urllib.error import HTTPError, URLError
from urllib.parse import unquote, urlparse
from urllib.request import Request, urlopen

from .config import config_dir
from .logutil import app_base_dir, beijing_clock, redact
from .version import CLIENT_NAME, CLIENT_VERSION

# ---------------------------------------------------------------------------
# 更新接口地址：把下面这个空字符串填成你的检查更新接口即可（例如
# "https://api.xflysim.com/client/version"）。留空 = 关闭启动自动检查，
# 界面上也不会出现任何更新提示。
#
# 调试/私有部署可用环境变量 XFLYSIM_UPDATE_URL 覆盖（优先级更高）。
# ---------------------------------------------------------------------------
UPDATE_API_URL = ""
UPDATE_URL_ENV = "XFLYSIM_UPDATE_URL"

UPDATE_USER_AGENT = f"XFlySim-Client-Updater/{CLIENT_VERSION}"
_CHUNK = 256 * 1024
_MAX_JSON = 1 << 20        # 更新接口响应上限 1 MB
_MAX_PACKAGE = 1 << 30     # 安装包上限 1 GB
_CONNECT_TIMEOUT = 8.0
_DOWNLOAD_TIMEOUT = 30.0
_MD5_RE = re.compile(r"^[0-9a-fA-F]{32}$")
# 版本号只取"数字.数字"前缀，后缀（-beta / +build / 空格加日期）忽略：
# 检查更新只比版本号大小，不应因为服务端多写了预发布标记就判定为非法。
_VERSION_RE = re.compile(r"^[vV]?(\d+(?:\.\d+)*)")
_SAFE_NAME_RE = re.compile(r"[^A-Za-z0-9._-]")

JOB_SCHEMA = "xflysim-update-job"
JOB_SCHEMA_VERSION = 1
RESULT_FILE = "update-result.json"
INSTALLED_FILE = "update-installed.json"


class UpdateError(Exception):
    """检查/下载/替换失败。``code`` 供前端映射本地化文案。"""

    def __init__(self, message: str, code: str = "update_failed") -> None:
        super().__init__(message)
        self.code = code
        self.message = message


class DownloadCancelled(UpdateError):
    def __init__(self, message: str = "下载已取消") -> None:
        super().__init__(message, "update_cancelled")


# ---------------------------------------------------------------- 版本比较

def version_key(text: str | None) -> tuple[int, ...]:
    """``v1.2.10`` / ``1.2.10`` → ``(1, 2, 10)``；非法返回空元组。"""
    m = _VERSION_RE.match(str(text or "").strip())
    if not m:
        return ()
    return tuple(int(x) for x in m.group(1).split("."))


def compare_versions(a: str | None, b: str | None) -> int:
    """比较两个版本号：a>b 返回 1，相等 0，a<b 返回 -1（补零对齐）。"""
    ka, kb = version_key(a), version_key(b)
    if not ka or not kb:
        raise UpdateError(
            f"版本号无法比较：{a!r} / {b!r}", "update_bad_version")
    n = max(len(ka), len(kb))
    ka += (0,) * (n - len(ka))
    kb += (0,) * (n - len(kb))
    return (ka > kb) - (ka < kb)


def is_newer(latest: str | None, current: str | None) -> bool:
    """远端版本是否比本地新（相同或更旧都不算更新，避免降级）。"""
    return compare_versions(latest, current) > 0


# ---------------------------------------------------------------- 接口解析

@dataclass(frozen=True)
class UpdateInfo:
    """更新接口的一次响应。"""

    software_name: str = ""
    version: str = ""
    logs: str = ""
    link: str = ""
    md5: str = ""
    created_time: str = ""

    def as_dict(self) -> dict:
        return {
            "softwareName": self.software_name,
            "version": self.version,
            "logs": self.logs,
            "link": self.link,
            "md5": self.md5,
            "createdTime": self.created_time,
        }


def parse_update_info(payload: Any) -> UpdateInfo:
    """校验更新接口返回的 JSON 结构（字段名按服务端约定，camelCase）。"""
    if not isinstance(payload, dict):
        raise UpdateError("更新接口返回的不是 JSON 对象", "update_bad_response")

    version = str(payload.get("version") or "").strip()
    if not version_key(version):
        raise UpdateError(f"更新接口的 version 无效：{version!r}",
                          "update_bad_response")

    md5 = str(payload.get("md5") or "").strip()
    if md5 and not _MD5_RE.match(md5):
        raise UpdateError(f"更新接口的 md5 不是 32 位十六进制：{md5!r}",
                          "update_bad_response")

    link = str(payload.get("link") or "").strip()
    if link:
        scheme = urlparse(link).scheme.lower()
        if scheme not in ("http", "https", "file"):
            raise UpdateError(f"更新包地址协议不支持：{scheme or '(空)'}",
                              "update_bad_link")

    logs = payload.get("logs")
    if logs is None:
        logs = ""
    elif not isinstance(logs, str):
        logs = json.dumps(logs, ensure_ascii=False, indent=2)

    return UpdateInfo(
        software_name=str(payload.get("softwareName")
                          or payload.get("software_name") or "").strip(),
        version=version,
        logs=logs.strip(),
        link=link,
        md5=md5.lower(),
        created_time=str(payload.get("createdTime")
                         or payload.get("created_time") or "").strip(),
    )


def configured_api_url() -> str:
    """生效的更新接口地址（环境变量优先，便于本地联调）。"""
    return str(os.environ.get(UPDATE_URL_ENV) or UPDATE_API_URL or "").strip()


def fetch_update_info(url: str, timeout: float = _CONNECT_TIMEOUT) -> UpdateInfo:
    """请求检查更新接口并解析。"""
    if not url:
        raise UpdateError("未配置更新接口地址", "update_no_url")
    scheme = urlparse(url).scheme.lower()
    if scheme not in ("http", "https", "file"):
        raise UpdateError(f"更新接口地址协议不支持：{scheme or '(空)'}",
                          "update_no_url")

    req = Request(url, headers={
        "User-Agent": UPDATE_USER_AGENT,
        "Accept": "application/json",
        "Cache-Control": "no-cache",
    })
    try:
        with urlopen(req, timeout=timeout) as resp:
            raw = resp.read(_MAX_JSON + 1)
    except HTTPError as exc:
        # HTTPError 本身是 response，不 close 会漏 socket（单元测试会报 ResourceWarning）
        try:
            exc.close()
        except Exception:
            pass
        raise UpdateError(f"更新接口返回 HTTP {exc.code}",
                          "update_http") from exc
    except URLError as exc:
        raise UpdateError(f"无法连接更新接口：{exc.reason}",
                          "update_network") from exc
    except OSError as exc:      # 超时 / 连接被重置
        raise UpdateError(f"更新接口请求失败：{exc}", "update_network") from exc

    if len(raw) > _MAX_JSON:
        raise UpdateError("更新接口响应过大", "update_bad_response")
    try:
        payload = json.loads(raw.decode("utf-8-sig"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise UpdateError(f"更新接口返回无法解析：{exc}",
                          "update_bad_response") from exc
    return parse_update_info(payload)


# ---------------------------------------------------------------- 下载

def staging_dir(version: str) -> str:
    """安装包暂存目录：%TEMP%/XFlySimUpdate/<version>（可被环境变量覆盖）。"""
    base = os.environ.get("XFLYSIM_UPDATE_DIR") or os.path.join(
        tempfile.gettempdir(), "XFlySimUpdate")
    safe = _SAFE_NAME_RE.sub("_", str(version or "latest")) or "latest"
    return os.path.join(base, safe)


def package_filename(link: str, version: str = "") -> str:
    """从 link 取文件名；没有可用文件名时用客户端名兜底。

    非 ASCII 字符（中文包名）会被替换成 ``_``，避免临时目录里的编码问题。
    """
    name = unquote(os.path.basename(urlparse(link).path.replace("\\", "/")))
    name = _SAFE_NAME_RE.sub("_", name)
    if not name.strip("._-"):
        name = ""
    if not name:
        name = f"{_SAFE_NAME_RE.sub('_', CLIENT_NAME)}-{version or 'update'}.exe"
    if not os.path.splitext(name)[1]:
        name += ".exe"
    return name


def file_md5(path: str, chunk: int = _CHUNK) -> str:
    h = hashlib.md5()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(chunk), b""):
            h.update(block)
    return h.hexdigest()


class Downloader:
    """支持断点续传、边下边算 MD5 的单文件下载器。

    - ``.part`` 留存未完成数据，下次调用自动从断点续传（HTTP Range 206）；
      服务端不支持 Range（返回 200）则清空重下，不会拼出坏包。
    - 校验失败（长度/MD5）删除 ``.part``，避免"永远续传坏包"。
    """

    def __init__(self, url: str, dest: str, expected_md5: str = "",
                 timeout: float = _DOWNLOAD_TIMEOUT,
                 chunk: int = _CHUNK) -> None:
        self.url = url
        self.dest = os.path.abspath(dest)
        self.part = self.dest + ".part"
        self.expected_md5 = (expected_md5 or "").strip().lower()
        self.timeout = timeout
        self.chunk = chunk

    # -- 对外 ---------------------------------------------------------
    def download(self,
                 on_progress: Callable[[int, int], None] | None = None,
                 cancel: threading.Event | None = None) -> str:
        """下载并校验，返回最终文件路径；失败抛 :class:`UpdateError`。"""
        os.makedirs(os.path.dirname(self.dest), exist_ok=True)

        # 已下完且校验通过：直接复用（用户上次更新到一半退出也能省流量）
        if os.path.isfile(self.dest):
            try:
                if (not self.expected_md5
                        or file_md5(self.dest) == self.expected_md5):
                    return self.dest
            except OSError:
                pass
            try:
                os.remove(self.dest)
            except OSError:
                pass

        if urlparse(self.url).scheme.lower() == "file":
            self._copy_local(on_progress, cancel)
            self._verify()
            os.replace(self.part, self.dest)
            return self.dest

        self._download_http(on_progress, cancel)
        self._verify()
        os.replace(self.part, self.dest)
        return self.dest

    # -- 本地 file:// -------------------------------------------------
    def _copy_local(self, on_progress, cancel) -> None:
        src = urlparse(self.url).path
        if os.name == "nt" and re.match(r"^/[A-Za-z]:/", src):
            src = src[1:]
        if not os.path.isfile(src):
            raise UpdateError(f"找不到更新包：{src}", "update_network")
        total = os.path.getsize(src)
        if total > _MAX_PACKAGE:
            raise UpdateError("更新包过大", "update_bad_package")
        done = 0
        hasher = hashlib.md5()
        with open(src, "rb") as fin, open(self.part, "wb") as fout:
            while True:
                if cancel is not None and cancel.is_set():
                    raise DownloadCancelled()
                block = fin.read(self.chunk)
                if not block:
                    break
                fout.write(block)
                hasher.update(block)
                done += len(block)
                if on_progress:
                    on_progress(done, total)
        self._local_md5 = hasher.hexdigest()

    # -- HTTP ---------------------------------------------------------
    def _download_http(self, on_progress, cancel) -> None:
        start = 0
        hasher = hashlib.md5()
        if os.path.isfile(self.part):
            start = os.path.getsize(self.part)
            if start > _MAX_PACKAGE:
                start = 0
            if start:
                # 续传前先把已有部分吃进 MD5，保证"边下边算"而不是最后再算
                with open(self.part, "rb") as f:
                    for block in iter(lambda: f.read(self.chunk), b""):
                        hasher.update(block)

        headers = {"User-Agent": UPDATE_USER_AGENT,
                   "Accept": "application/octet-stream"}
        if start:
            headers["Range"] = f"bytes={start}-"
        req = Request(self.url, headers=headers)

        try:
            resp = urlopen(req, timeout=self.timeout)
        except HTTPError as exc:
            try:
                exc.close()
            except Exception:
                pass
            if exc.code == 416:      # 断点越界（对端包变了）→ 清空重下
                self._reset_part()
                return self._download_http(on_progress, cancel)
            raise UpdateError(f"更新包下载失败：HTTP {exc.code}",
                              "update_download") from exc
        except URLError as exc:
            raise UpdateError(f"更新包下载失败：{exc.reason}",
                              "update_download") from exc
        except OSError as exc:
            raise UpdateError(f"更新包下载失败：{exc}", "update_download") from exc

        with resp:
            status = getattr(resp, "status", 200)
            if start and status != 206:
                # 服务端忽略了 Range，返回整个文件 → 只能清空重下
                start = 0
                hasher = hashlib.md5()
                self._reset_part()

            total = self._total_size(resp, start)
            mode = "ab" if start else "wb"
            done = start
            last_emit = 0.0
            try:
                fout = open(self.part, mode)
            except OSError as exc:
                raise UpdateError(f"无法写入临时文件：{exc}",
                                  "update_staging") from exc
            try:
                with fout:
                    while True:
                        if cancel is not None and cancel.is_set():
                            raise DownloadCancelled()
                        block = resp.read(self.chunk)
                        if not block:
                            break
                        fout.write(block)
                        hasher.update(block)
                        done += len(block)
                        if done > _MAX_PACKAGE:
                            raise UpdateError("更新包过大", "update_bad_package")
                        now = time.monotonic()
                        if on_progress and now - last_emit >= 0.15:
                            last_emit = now
                            on_progress(done, total)
            except OSError as exc:
                raise UpdateError(f"下载中断：{exc}", "update_download") from exc

        if on_progress:
            on_progress(done, total)
        self._http_md5 = hasher.hexdigest()

    @staticmethod
    def _total_size(resp, start: int) -> int:
        """总字节数：优先 Content-Range 的 total，其次 Content-Length+起点。"""
        content_range = resp.headers.get("Content-Range", "") if resp.headers else ""
        m = re.search(r"/(\d+)\s*$", content_range or "")
        if m:
            return int(m.group(1))
        length = resp.headers.get("Content-Length") if resp.headers else None
        try:
            return start + int(length) if length is not None else 0
        except (TypeError, ValueError):
            return 0

    def _reset_part(self) -> None:
        try:
            os.remove(self.part)
        except OSError:
            pass

    # -- 校验 ---------------------------------------------------------
    def _verify(self) -> None:
        """核对长度与 MD5；不一致删除 .part（避免续传坏包）。"""
        try:
            size = os.path.getsize(self.part)
        except OSError as exc:
            raise UpdateError(f"更新包不存在：{exc}", "update_download") from exc
        if size <= 0:
            raise UpdateError("更新包为空", "update_bad_package")
        if self.expected_md5:
            actual = getattr(self, "_local_md5", None) or \
                getattr(self, "_http_md5", None) or file_md5(self.part)
            if actual != self.expected_md5:
                self._reset_part()
                raise UpdateError(
                    f"更新包校验失败（MD5 不一致：期望 {self.expected_md5}，"
                    f"实际 {actual}），已删除临时文件",
                    "update_bad_md5")


# ---------------------------------------------------------------- 更新作业

@dataclass
class UpdateJob:
    """交给独立更新助手执行的一次替换任务。"""

    package: str
    target: str
    version: str
    md5: str = ""
    wait_pid: int = 0
    restart: bool = True
    created_at: str = ""

    def as_dict(self) -> dict:
        return {
            "schema": JOB_SCHEMA,
            "schema_version": JOB_SCHEMA_VERSION,
            "package": self.package,
            "target": self.target,
            "version": self.version,
            "md5": self.md5,
            "wait_pid": int(self.wait_pid or 0),
            "restart": bool(self.restart),
            "created_at": self.created_at or time.strftime("%Y-%m-%d %H:%M:%S"),
        }


def write_job(job: UpdateJob, path: str | None = None) -> str:
    path = path or os.path.join(
        staging_dir(job.version), f"update-job-{os.getpid()}.json")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(job.as_dict(), f, ensure_ascii=False, indent=2)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    return path


def read_job(path: str) -> UpdateJob:
    """读取并校验作业文件（助手端用；拒绝相对路径/非法 schema）。"""
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, dict) or data.get("schema") != JOB_SCHEMA:
        raise UpdateError("更新作业文件格式不正确", "update_bad_job")
    if int(data.get("schema_version", -1)) != JOB_SCHEMA_VERSION:
        raise UpdateError("更新作业版本不支持", "update_bad_job")
    package = str(data.get("package") or "")
    target = str(data.get("target") or "")
    version = str(data.get("version") or "")
    if not package or not target or not version:
        raise UpdateError("更新作业缺少 package/target/version", "update_bad_job")
    if not os.path.isabs(package) or not os.path.isabs(target):
        raise UpdateError("更新作业路径必须是绝对路径", "update_bad_job")
    return UpdateJob(
        package=package, target=target, version=version,
        md5=str(data.get("md5") or "").lower(),
        wait_pid=int(data.get("wait_pid") or 0),
        restart=bool(data.get("restart", True)),
        created_at=str(data.get("created_at") or ""),
    )


def updater_command(base_dir: str) -> list[str]:
    """定位更新助手：优先同目录 XUpdate.exe，源码树回退 python xupdate.py。"""
    exe = os.path.join(base_dir, "XUpdate.exe")
    if os.path.isfile(exe):
        return [exe]
    script = os.path.join(base_dir, "xupdate.py")
    if os.path.isfile(script):
        return [sys.executable, script]
    raise UpdateError(
        "找不到更新助手：请把 XUpdate.exe 放在客户端同目录（打包脚本会一并产出）",
        "update_updater_missing")


def launch_detached(args: list[str], cwd: str) -> None:
    """以脱离进程启动助手：主程序退出后它继续跑，且不弹控制台窗口。"""
    kwargs: dict[str, Any] = {
        "cwd": cwd,
        "stdin": subprocess.DEVNULL,
        "stdout": subprocess.DEVNULL,
        "stderr": subprocess.DEVNULL,
        "close_fds": True,
    }
    if os.name == "nt":
        # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
        kwargs["creationflags"] = 0x00000008 | 0x00000200
    else:
        kwargs["start_new_session"] = True
    subprocess.Popen(args, **kwargs)


def start_updater(job: UpdateJob, base_dir: str) -> str:
    """写作业文件并拉起助手；返回作业文件路径。"""
    base_dir = os.path.abspath(base_dir)
    if not os.path.isfile(job.package):
        raise UpdateError(f"更新包已不存在：{job.package}", "update_bad_package")
    cmd = updater_command(base_dir)
    job_path = write_job(job)
    launch_detached(cmd + ["--apply-package", job_path], cwd=base_dir)
    return job_path


# ---------------------------------------------------------------- 结果 / 版本落盘

def _write_json_atomic(path: str, payload: dict) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def result_path() -> str:
    return os.path.join(config_dir(), RESULT_FILE)


def installed_path() -> str:
    return os.path.join(config_dir(), INSTALLED_FILE)


def write_result(ok: bool, version: str, message: str = "",
                 target: str = "") -> None:
    """助手把结果写这里；客户端下次启动读一次并清除，用于提示用户。"""
    try:
        _write_json_atomic(result_path(), {
            "ok": bool(ok),
            "version": version,
            "message": message,
            "target": target,
            "finished_at": beijing_clock(),
        })
    except OSError:
        pass


def read_result() -> dict | None:
    try:
        with open(result_path(), "r", encoding="utf-8") as f:
            data = json.load(f)
        return data if isinstance(data, dict) else None
    except (OSError, ValueError):
        return None


def clear_result() -> None:
    try:
        os.remove(result_path())
    except OSError:
        pass


def read_and_clear_result() -> dict | None:
    data = read_result()
    if data is not None:
        clear_result()
    return data


def write_installed_version(version: str, md5: str = "",
                            package: str = "") -> None:
    """写回新版本号（客户端自己的版本以 exe 内置为准，这里只作记录）。"""
    try:
        _write_json_atomic(installed_path(), {
            "version": version,
            "md5": md5,
            "package": os.path.basename(package) if package else "",
            "updated_at": time.strftime("%Y-%m-%d %H:%M:%S"),
            "beijing_time": beijing_clock(),
        })
    except OSError:
        pass


def read_installed_version() -> str:
    try:
        with open(installed_path(), "r", encoding="utf-8") as f:
            data = json.load(f)
        return str(data.get("version") or "") if isinstance(data, dict) else ""
    except (OSError, ValueError):
        return ""


# ---------------------------------------------------------------- 更新管理器

def target_executable() -> str:
    """要被替换的客户端 exe：打包版 = 自身；源码运行无目标（不可自更新）。"""
    override = str(os.environ.get("XFLYSIM_UPDATE_TARGET") or "").strip()
    if override:
        return os.path.abspath(override)
    if getattr(sys, "frozen", False):
        return os.path.abspath(sys.executable)
    return ""


class UpdateManager:
    """检查/下载/替换的状态机，供 pywebview Api 调用。

    所有网络与磁盘操作都在后台线程，状态通过 ``on_event`` 回调推给前端：

        {"state": "checking" | "uptodate" | "available" | "downloading"
                  | "ready" | "restarting" | "error" | "disabled",
         "current": "1.0.0", "latest": "1.0.1", "notes": "...",
         "created": "...", "softwareName": "...",
         "percent": 0~100, "received": 字节, "total": 字节,
         "error": "失败原因", "errorCode": "update_xxx", "manual": true/false}
    """

    def __init__(self, *, base_dir: str | None = None,
                 api_url: str | None = None,
                 current: str | None = None,
                 on_event: Callable[[dict], None] | None = None,
                 log: Callable[[str, str], None] | None = None,
                 fetch: Callable[[str], UpdateInfo] | None = None,
                 launcher: Callable[[UpdateJob, str], str] | None = None) -> None:
        self.base_dir = os.path.abspath(base_dir or app_base_dir())
        self._api_url = (api_url if api_url is not None
                         else configured_api_url())
        self.current = str(current or CLIENT_VERSION)
        self._on_event = on_event
        self._log = log
        self._fetch = fetch or (lambda url: fetch_update_info(url))
        self._launcher = launcher or start_updater
        self._lock = threading.Lock()
        self._thread: threading.Thread | None = None
        self._cancel = threading.Event()
        self._info: UpdateInfo | None = None
        self._path: str = ""
        self._state: dict[str, Any] = {
            "state": "idle" if self._api_url else "disabled",
            "current": self.current,
            "latest": "", "notes": "", "created": "", "softwareName": "",
            "percent": 0, "received": 0, "total": 0,
            "error": "", "errorCode": "", "manual": False, "silent": False,
        }

    # -- 查询 ---------------------------------------------------------
    def state(self) -> dict:
        with self._lock:
            return dict(self._state)

    @property
    def info(self) -> UpdateInfo | None:
        return self._info

    def busy(self) -> bool:
        thread = self._thread
        return bool(thread and thread.is_alive())

    # -- 检查 ---------------------------------------------------------
    def check(self, manual: bool = False) -> dict:
        """后台检查更新；返回当前状态（不阻塞）。"""
        if self.busy():
            return {"ok": True, "busy": True}
        if not self._api_url:
            self._emit(state="disabled", manual=bool(manual))
            return {"ok": False, "code": "update_no_url",
                    "error": "未配置更新接口地址"}
        self._set_state("checking", manual=bool(manual), error="",
                        errorCode="")
        self._thread = threading.Thread(
            target=self._check_worker, args=(bool(manual),), daemon=True,
            name="update-check")
        self._thread.start()
        return {"ok": True, "started": True}

    def _check_worker(self, manual: bool) -> None:
        try:
            info = self._fetch(self._api_url)
            self._info = info
            newer = is_newer(info.version, self.current)
            if newer:
                self._logf(f"[update] 发现新版本 {info.version}"
                           f"（当前 {self.current}）", "INFO")
                self._set_state("available", latest=info.version,
                                notes=info.logs, created=info.created_time,
                                softwareName=info.software_name,
                                manual=manual, percent=0)
            else:
                self._logf(f"[update] 已是最新版本 {self.current}", "DEBUG")
                self._set_state("uptodate", latest=info.version,
                                notes=info.logs, created=info.created_time,
                                softwareName=info.software_name,
                                manual=manual)
        except UpdateError as exc:
            self._fail(exc, manual=manual, quiet=not manual)
        except Exception as exc:            # 兜底：绝不让更新线程打崩应用
            self._fail(UpdateError(f"检查更新异常：{exc}", "update_failed"),
                       manual=manual, quiet=not manual)

    # -- 下载 + 拉起助手 ----------------------------------------------
    def apply(self) -> dict:
        """下载（含续传/校验）后拉起更新助手；前端收到 restarting 即退出。"""
        info = self._info
        if info is None:
            return {"ok": False, "code": "update_not_checked",
                    "error": "还没有检查更新"}
        if self.busy():
            return {"ok": True, "busy": True}
        target = target_executable()
        if not target:
            return {"ok": False, "code": "update_unsupported",
                    "error": "源码运行模式不支持自动更新（请使用打包版客户端）"}
        if not info.link:
            return {"ok": False, "code": "update_bad_link",
                    "error": "更新接口没有提供下载地址（link）"}
        self._set_state("downloading", percent=0, received=0, total=0,
                        error="", errorCode="")
        self._thread = threading.Thread(
            target=self._apply_worker, args=(info, target), daemon=True,
            name="update-apply")
        self._thread.start()
        return {"ok": True, "started": True}

    def _apply_worker(self, info: UpdateInfo, target: str) -> None:
        self._cancel.clear()
        try:
            dest = os.path.join(staging_dir(info.version),
                                package_filename(info.link, info.version))
            dl = Downloader(info.link, dest, info.md5,
                            timeout=_DOWNLOAD_TIMEOUT)
            path = dl.download(on_progress=self._on_progress,
                               cancel=self._cancel)
            self._path = path
            self._logf(f"[update] 已下载并校验 {os.path.basename(path)}", "INFO")

            job = UpdateJob(package=path, target=target,
                            version=info.version, md5=info.md5,
                            wait_pid=os.getpid(), restart=True)
            self._launcher(job, self.base_dir)
            self._logf("[update] 已拉起更新助手，客户端即将退出", "INFO")
            self._set_state("restarting", percent=100, latest=info.version)
        except DownloadCancelled:
            self._fail(DownloadCancelled(), manual=True)
        except UpdateError as exc:
            self._fail(exc, manual=True)
        except Exception as exc:
            self._fail(UpdateError(f"更新失败：{exc}", "update_failed"),
                       manual=True)

    def _on_progress(self, received: int, total: int) -> None:
        percent = int(received * 100 / total) if total else 0
        st = self.state()
        # 百分比没变就不推，避免前端事件队列被刷爆
        if st.get("percent") == percent and st.get("state") == "downloading":
            return
        self._set_state("downloading", received=received, total=total,
                        percent=percent)

    def cancel(self) -> None:
        self._cancel.set()

    # -- 内部 ---------------------------------------------------------
    def _set_state(self, state: str, **fields: Any) -> None:
        with self._lock:
            self._state.update(fields)
            self._state["state"] = state
            snapshot = dict(self._state)
        self._emit_payload(snapshot)

    def _emit(self, **fields: Any) -> None:
        with self._lock:
            self._state.update(fields)
            snapshot = dict(self._state)
        self._emit_payload(snapshot)

    def _emit_payload(self, snapshot: dict) -> None:
        if self._on_event is None:
            return
        try:
            self._on_event(snapshot)
        except Exception:
            pass

    def _fail(self, exc: UpdateError, manual: bool = False,
              quiet: bool = False) -> None:
        """失败：静默（启动自动检查）只记 DEBUG，手动检查/下载才提示。"""
        self._logf(f"[update] {redact(str(exc))}",
                   "DEBUG" if quiet else "WARN")
        self._set_state("error", error=str(exc), errorCode=exc.code,
                        manual=manual, percent=0, silent=quiet)

    def _logf(self, message: str, level: str = "INFO") -> None:
        if self._log is None:
            return
        try:
            self._log(message, level)
        except Exception:
            pass
