"""更新助手：等客户端退出 → 备份 → 原子替换 → 写版本 → 重启。

为什么必须独立成一个进程：Windows 上被映射为镜像的 exe 不能在被自己
覆盖，客户端只能"先下载、再拉起助手、然后退出"，由助手在客户端退出后
完成替换。

由 ``XUpdate.exe --apply-package <job.json>`` 调用（作业文件由
``xfly_client.update_service.start_updater`` 生成），也可以直接：

    python xupdate.py --apply-package %TEMP%\\XFlySimUpdate\\1.0.1\\update-job-1234.json

替换顺序（任何一步失败都按逆序回滚，绝不留下半个版本）：

    ① 校验安装包（MZ/PE 头 + MD5）
    ② 等 wait_pid 退出（默认最多 60s）
    ③ 包复制到目标同目录的 .update-new（跨盘也能原子 rename）
    ④ 旧文件改名 .bak
    ⑤ .update-new → 正式路径（同卷 os.replace，原子）
    ⑥ 写回版本号 / 清 .bak
    ⑦ 重启新版本
失败：.bak 还原 → 写结果文件 → 弹窗提示（此时客户端已退出，没有窗口）
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time

from .logutil import redact
from .update_service import (UpdateError, UpdateJob, file_md5, read_job,
                             write_installed_version, write_result)

WAIT_TIMEOUT = 60.0
REPLACE_ATTEMPTS = 24          # 24 × 0.5s ≈ 12s：等杀毒/索引释放句柄
REPLACE_DELAY = 0.5


def log_stub(message: str, level: str = "INFO") -> None:
    print(f"[{level}] {message}", file=sys.stderr if level == "DEBUG" else sys.stdout,
          flush=True)


# ---------------------------------------------------------------- 进程等待

def wait_for_process_exit(pid: int, timeout: float = WAIT_TIMEOUT) -> bool:
    """等指定进程退出；返回 True=已退出（或本来就不存在），False=超时。"""
    pid = int(pid or 0)
    if pid <= 0:
        return True
    if os.name == "nt":
        import ctypes
        SYNCHRONIZE = 0x00100000
        WAIT_OBJECT_0 = 0x00000000
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.OpenProcess(SYNCHRONIZE, False, pid)
        if not handle:
            return True               # 已经是退出状态 / 无此进程
        try:
            rc = kernel32.WaitForSingleObject(handle, int(max(0.0, timeout) * 1000))
            return rc == WAIT_OBJECT_0
        finally:
            kernel32.CloseHandle(handle)
    deadline = time.monotonic() + max(0.0, timeout)
    while time.monotonic() < deadline:
        try:
            os.kill(pid, 0)
        except OSError:
            return True
        time.sleep(0.2)
    return False


# ---------------------------------------------------------------- 安装包校验

def looks_like_pe(path: str) -> bool:
    """粗略校验：MZ 头 + PE 签名偏移合理，避免拿 HTML 错误页覆盖 exe。"""
    try:
        with open(path, "rb") as f:
            if f.read(2) != b"MZ":
                return False
            f.seek(0x3C)
            raw = f.read(4)
            if len(raw) != 4:
                return False
            offset = int.from_bytes(raw, "little")
            if not 0x40 <= offset <= 0x1000:
                return False
            f.seek(offset)
            return f.read(4) == b"PE\0\0"
    except OSError:
        return False


def check_package(path: str, expected_md5: str = "",
                  log=log_stub) -> None:
    """校验安装包存在、像 PE、MD5 对得上（MD5 为空时跳过哈希）。"""
    if not os.path.isfile(path):
        raise UpdateError(f"更新包不存在：{path}", "update_bad_package")
    size = os.path.getsize(path)
    if size <= 0:
        raise UpdateError("更新包为空", "update_bad_package")
    if not looks_like_pe(path):
        raise UpdateError("更新包不是有效的 Windows 程序（PE 头校验失败）",
                          "update_bad_package")
    if expected_md5:
        actual = file_md5(path)
        if actual.lower() != expected_md5.lower():
            raise UpdateError(
                f"更新包 MD5 不一致（期望 {expected_md5}，实际 {actual}）",
                "update_bad_md5")
        log(f"更新包校验通过：{os.path.basename(path)} ({size} 字节)", "INFO")


# ---------------------------------------------------------------- 替换

def _remove_quiet(path: str) -> None:
    try:
        os.remove(path)
    except OSError:
        pass


def _replace_with_retry(src: str, dst: str, attempts: int = REPLACE_ATTEMPTS,
                        delay: float = REPLACE_DELAY) -> None:
    """同卷 rename；被杀毒/索引占用时重试，避免几毫秒的句柄竞争就失败。"""
    last: OSError | None = None
    for i in range(max(1, attempts)):
        try:
            os.replace(src, dst)
            return
        except PermissionError as exc:
            last = exc
        except OSError as exc:
            # 跨卷 / 目标被占用：留最后一次错误信息
            if getattr(exc, "winerror", None) not in (5, 32, 33):
                raise
            last = exc
        time.sleep(delay)
    raise UpdateError(
        f"无法替换 {os.path.basename(dst)}（文件仍被占用，请关闭客户端后重试）："
        f"{last}", "update_locked")


def swap_files(package: str, target: str, log=log_stub) -> None:
    """把 package 换到 target：旧文件留 .bak，失败立即还原。"""
    target_dir = os.path.dirname(target)
    os.makedirs(target_dir, exist_ok=True)
    staged = os.path.join(target_dir, os.path.basename(target) + ".update-new")
    backup = target + ".bak"

    _remove_quiet(staged)             # 上次中断的残留
    log(f"复制安装包到目标目录：{os.path.basename(staged)}", "INFO")
    try:
        shutil.copyfile(package, staged)
    except OSError as exc:
        _remove_quiet(staged)
        raise UpdateError(f"复制安装包失败：{exc}", "update_copy") from exc

    _remove_quiet(backup)
    had_old = os.path.isfile(target)
    if had_old:
        log(f"备份旧版本：{os.path.basename(backup)}", "INFO")
        try:
            _replace_with_retry(target, backup)
        except UpdateError:
            _remove_quiet(staged)
            raise
        except OSError as exc:
            _remove_quiet(staged)
            raise UpdateError(f"备份旧版本失败：{exc}", "update_locked") from exc
    try:
        log("正在替换为新版本…", "INFO")
        _replace_with_retry(staged, target)
    except BaseException:
        _remove_quiet(staged)
        if had_old and os.path.isfile(backup):
            try:
                os.replace(backup, target)      # 回滚
                log("替换失败，已还原旧版本", "ERROR")
            except OSError as exc:
                log(f"回滚失败，备份仍在：{backup}（{exc}）", "ERROR")
        raise
    if had_old:
        _remove_quiet(backup)
    log("替换完成", "INFO")


def restart_client(target: str) -> None:
    """拉起新版本客户端（脱离当前进程，助手随后退出）。"""
    kwargs = {"cwd": os.path.dirname(target) or ".",
              "stdin": subprocess.DEVNULL,
              "stdout": subprocess.DEVNULL,
              "stderr": subprocess.DEVNULL,
              "close_fds": True}
    if os.name == "nt":
        kwargs["creationflags"] = 0x00000008 | 0x00000200  # DETACHED | NEW_GROUP
    else:
        kwargs["start_new_session"] = True
    subprocess.Popen([target], **kwargs)


# ---------------------------------------------------------------- 对外入口

def apply_package(job: UpdateJob, log=log_stub,
                  wait_timeout: float = WAIT_TIMEOUT) -> None:
    """执行一次完整替换；失败抛 :class:`UpdateError` 并已尽力回滚。"""
    log(f"更新作业：{os.path.basename(job.package)} → {job.target} "
        f"(版本 {job.version})", "INFO")
    check_package(job.package, job.md5, log=log)

    if job.wait_pid:
        log(f"等待客户端退出 (PID {job.wait_pid})…", "INFO")
        if not wait_for_process_exit(job.wait_pid, wait_timeout):
            log("等待超时，仍然尝试替换（可能会因占用失败）", "WARN")

    swap_files(job.package, job.target, log=log)
    write_installed_version(job.version, md5=job.md5, package=job.package)

    if job.restart:
        log("启动新版本客户端", "INFO")
        try:
            restart_client(job.target)
        except Exception as exc:
            # 文件已经换好了：启动失败不算更新失败，只提示手动打开
            log(f"自动启动失败：{exc}（更新已完成，请手动打开）", "WARN")


def show_error_dialog(message: str, title: str = "XFlySim 更新失败") -> None:
    """客户端已退出时唯一的可见提示渠道（Windows）。"""
    if os.name != "nt":
        print(message, file=sys.stderr, flush=True)
        return
    try:
        import ctypes
        ctypes.windll.user32.MessageBoxW(None, message, title, 0x10)
    except Exception:
        pass


def run_job_file(job_path: str, log=log_stub,
                 wait_timeout: float = WAIT_TIMEOUT) -> int:
    """助手主流程：读作业 → 应用 → 写结果（成功静默，失败弹窗）。"""
    try:
        job = read_job(job_path)
    except (OSError, ValueError, UpdateError) as exc:
        log(f"读取作业文件失败：{exc}", "ERROR")
        show_error_dialog(f"更新作业文件无法读取：\n{exc}")
        return 2

    try:
        apply_package(job, log=log, wait_timeout=wait_timeout)
    except UpdateError as exc:
        write_result(False, job.version, message=str(exc), target=job.target)
        log(f"更新失败：{redact(str(exc))}", "ERROR")
        show_error_dialog(
            f"更新到 {job.version} 失败：\n{exc}\n\n旧版本已保留，可以继续使用。")
        return 2
    except Exception as exc:            # 兜底：任何异常都要留下结果
        write_result(False, job.version, message=str(exc), target=job.target)
        log(f"更新异常：{redact(str(exc))}", "ERROR")
        show_error_dialog(f"更新到 {job.version} 时发生异常：\n{exc}")
        return 2

    write_result(True, job.version, target=job.target)
    log(f"更新完成：{job.version}", "INFO")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="xupdate", description="XFlySim 客户端更新助手")
    p.add_argument("--apply-package", metavar="JOB.json",
                   help="按作业文件执行替换（由客户端拉起，不建议手动改）")
    p.add_argument("--wait-timeout", type=float, default=WAIT_TIMEOUT,
                   help=f"等待客户端退出的秒数（默认 {WAIT_TIMEOUT:.0f}）")
    p.add_argument("--quiet", action="store_true", help="不输出到标准输出")
    return p


def main(argv: list[str] | None = None) -> int:
    """独立入口（调试用）：XUpdate.exe 缺失时可直接
    ``python -m xfly_client.update_apply --apply-package <job>``。"""
    args = build_parser().parse_args(argv)
    log = (lambda m, lv="INFO": None) if args.quiet else log_stub
    return run_job_file(args.apply_package, log=log,
                        wait_timeout=args.wait_timeout)


if __name__ == "__main__":
    raise SystemExit(main())
