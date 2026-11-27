"""XUpdate.exe 入口：客户端 / XConnect / MConnect 的统一更新器。

用法（源码运行把 python xupdate.py 换成 XUpdate.exe）：

    XUpdate.exe --check                     # 只检查，不改动任何文件
    XUpdate.exe --apply                     # 下载 + 校验 + 备份 + 原子替换
    XUpdate.exe --check --json              # 供客户端/脚本解析
    XUpdate.exe --apply --component client --manifest <本地路径或 URL>

默认清单：程序目录下的 ``update/manifest.json``，可用环境变量
``XFLYSIM_UPDATE_MANIFEST`` 覆盖。CSL / FSLTL / AIG 不归本更新器管。

软件中心模式（与清单模式二选一）：

    XUpdate.exe --software-api https://host/api --software-uuid <uuid> --check
    XUpdate.exe --software-api https://host/api --software-uuid <uuid> --apply
    XUpdate.exe --software-api https://host/api --software-uuid <uuid> --software-history

下载包非 zip 直接放根目录；zip 先解压再按包内路径放根目录。
"""
from __future__ import annotations

import argparse
import json
import os
import sys

from xfly_client.logutil import app_base_dir
from xfly_client import software_update as S
from xfly_client.updater import (
    COMPONENTS, DEFAULT_MANIFEST, PreconditionError, UpdateError,
    UPDATER_VERSION, UpdatePlan, apply_plans, build_plans, read_manifest,
    sample_manifest, installed_versions, same_version, default_fetch,
)


def _components(text: str) -> list[str]:
    names = [x.strip() for x in text.split(",") if x.strip()]
    if not names or "all" in names:
        return list(COMPONENTS)
    for n in names:
        if n not in COMPONENTS:
            raise argparse.ArgumentTypeError(f"unknown component: {n}")
    return names


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="XFlySim Client/Plugin updater (XUpdate)")
    p.add_argument("--check", action="store_true",
                   help="只检查更新（默认），不改动任何文件")
    p.add_argument("--apply", action="store_true",
                   help="下载并应用更新；失败自动回滚")
    p.add_argument("--manifest", default=os.environ.get("XFLYSIM_UPDATE_MANIFEST"),
                   help="清单路径或 URL（默认 <程序目录>/update/manifest.json）")
    p.add_argument("--base-dir", help="客户端/清单根目录（默认程序目录）")
    p.add_argument("--xplane-dir", help="X-Plane 根目录（或设 XPLANE_ROOT）")
    p.add_argument("--mconnect-dir", help="MConnect 根目录（默认客户端目录）")
    p.add_argument("--component", type=_components, default=list(COMPONENTS),
                   help="client,xconnect,mconnect 或 all（默认 all）")
    p.add_argument("--force", action="store_true",
                   help="忽略本地哈希，强制重新下载全部文件")
    p.add_argument("--timeout", type=int, default=30, help="单文件下载超时秒数")
    p.add_argument("--json", action="store_true", help="只输出 JSON 报告")
    p.add_argument("--sample-manifest", action="store_true",
                   help="打印清单模板后退出（供服务端/打包脚本参考）")
    # 客户端在线更新：客户端下载校验后写一个作业文件，拉起本 exe 在
    # 客户端退出后完成替换并重启（见 xfly_client/update_apply.py）。
    p.add_argument("--apply-package", metavar="JOB.json",
                   help="按作业文件执行 exe 替换（由客户端拉起，勿手改）")
    p.add_argument("--wait-timeout", type=float, default=60.0,
                   help="等待客户端退出的秒数（默认 60）")
    p.add_argument("--quiet", action="store_true", help="不输出到标准输出")
    # 软件中心模式（与清单模式二选一）：解析发布包并按 zip/非 zip 规则落盘。
    p.add_argument("--software-api",
                   default=os.environ.get("XFLYSIM_SOFTWARE_API", ""),
                   help="飞行员中心 API 根地址（也可用 XFLYSIM_SOFTWARE_API）；指定后走软件中心模式")
    p.add_argument("--software-uuid",
                   default=os.environ.get("XFLYSIM_SOFTWARE_UUID", ""),
                   help="软件标识 uuid（也可用 XFLYSIM_SOFTWARE_UUID）")
    p.add_argument("--software-legacy", action="store_true",
                   help="用 /software/latest 旧路径代替 /softwareVersion/latest")
    p.add_argument("--software-history", action="store_true",
                   help="只打印软件中心版本记录，不检查/更新")
    p.add_argument("--version", action="version",
                   version=f"XUpdate {UPDATER_VERSION}")
    return p


def _plan_report(plans: list[UpdatePlan]) -> dict:
    return {
        "mode": "check",
        "update_available": any(not p.up_to_date for p in plans),
        "plans": [p.summary() for p in plans],
    }


def _software_flow(args, base: str, log, api: str, uuid: str) -> int:
    """软件中心模式：读 latest -> 下载发布包 -> 按 zip/非 zip 规则落根目录。"""
    if not uuid:
        raise PreconditionError(
            "软件中心模式需要 --software-uuid（或 XFLYSIM_SOFTWARE_UUID）")
    api = api.strip().rstrip("/")
    legacy = args.software_legacy

    if args.software_history:
        records = S.fetch_logs(api, uuid, default_fetch, args.timeout,
                               legacy=legacy)
        if args.json:
            print(json.dumps({"mode": "history", "records": records},
                             ensure_ascii=False, indent=2))
        else:
            for r in records:
                log(f"{r['version']} {r['createdTime']} {r['logs']}", "INFO")
        return 0

    release = S.fetch_release(api, uuid, default_fetch, args.timeout,
                              legacy=legacy)
    installed = installed_versions(base).get("client")
    available = not same_version(installed, release.version)

    if not args.apply:
        report = {
            "mode": "check",
            "source": "software-center",
            "software": release.software_name,
            "version": release.version,
            "installed": installed,
            "logs": release.logs,
            "createdTime": release.created_time,
            "md5": release.md5,
            "update_available": available,
        }
        if args.json:
            print(json.dumps(report, ensure_ascii=False, indent=2))
        else:
            log(f"[{release.software_name}] installed={installed} "
                f"target={release.version} md5={release.md5}", "INFO")
            if release.logs:
                log(f"update notes: {release.logs}", "INFO")
            log("update available" if available else "everything up to date",
                "INFO")
        return 1 if available else 0

    data = S.download_package(release, default_fetch, args.timeout, api_base=api)
    result = S.apply_package(release, data, base)
    report = {
        "mode": "apply",
        "source": "software-center",
        "version": release.version,
        **result.summary(),
    }
    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=2))
    else:
        kind = "zip" if result.zipped else "file"
        log(f"applied {kind}: {len(result.replaced)} files, "
            f"version={release.version}", "INFO")
    return 0 if result.ok else 2


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    if args.apply_package:
        # 客户端自更新：等待主进程退出 → 备份/原子替换 → 写版本 → 重启。
        # 只有这一步会改磁盘，且由客户端在退出前拉起。
        from xfly_client.update_apply import run_job_file
        from xfly_client.updater import UpdaterLog
        log = UpdaterLog(app_base_dir(), quiet=args.quiet)
        return run_job_file(args.apply_package, log=log,
                            wait_timeout=args.wait_timeout)

    if args.sample_manifest:
        print(json.dumps(sample_manifest(), ensure_ascii=False, indent=2))
        return 0

    base = os.path.abspath(args.base_dir or app_base_dir())
    from xfly_client.updater import UpdaterLog
    log = UpdaterLog(base, quiet=args.json)

    try:
        api = (args.software_api or "").strip()
        if api:
            return _software_flow(
                args, base, log, api, (args.software_uuid or "").strip())

        source = args.manifest or os.path.join(base, DEFAULT_MANIFEST)
        if not os.path.isfile(source) and "://" not in source:
            raise UpdateError(
                f"找不到更新清单：{source}（用 --manifest 指定路径或 URL）")
        log(f"XUpdate {UPDATER_VERSION}: manifest={source}", "INFO")
        manifest = read_manifest(source, timeout=args.timeout)
        plans = build_plans(
            manifest, base_dir=base, components=args.component,
            xplane_dir=args.xplane_dir, mconnect_dir=args.mconnect_dir,
            force=args.force)

        if not args.apply:
            report = _plan_report(plans)
            if args.json:
                print(json.dumps(report, ensure_ascii=False, indent=2))
            else:
                for p in plans:
                    log(f"[{p.component}] installed={p.current} "
                        f"target={p.version} need={len(p.needed)} "
                        f"({p.summary()['bytes_to_download']} bytes)", "INFO")
                log("update available" if report["update_available"]
                    else "everything up to date", "INFO")
            return 1 if report["update_available"] else 0

        result = apply_plans(manifest, plans, base_dir=base,
                             timeout=args.timeout, log=log)
        report = {"mode": "apply", **result.summary()}
        if args.json:
            print(json.dumps(report, ensure_ascii=False, indent=2))
        else:
            log(f"applied: {len(result.replaced)} files, "
                f"versions={result.versions}", "INFO")
        return 0 if not result.error else 2
    except PreconditionError as e:
        msg = f"更新条件不满足：{e}"
        if args.json:
            print(json.dumps({"ok": False, "error": msg}, ensure_ascii=False))
        else:
            log(msg, "ERROR")
        return 3
    except (UpdateError, OSError, ValueError) as e:
        msg = f"更新失败：{e}"
        if args.json:
            print(json.dumps({"ok": False, "error": msg}, ensure_ascii=False))
        else:
            log(msg, "ERROR")
        return 2


if __name__ == "__main__":
    sys.exit(main())
