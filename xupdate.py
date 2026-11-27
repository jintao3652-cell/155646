"""XUpdate.exe 入口：客户端 / XConnect / MConnect 的统一更新器。

用法（源码运行把 python xupdate.py 换成 XUpdate.exe）：

    XUpdate.exe --check                     # 只检查，不改动任何文件
    XUpdate.exe --apply                     # 下载 + 校验 + 备份 + 原子替换
    XUpdate.exe --check --json              # 供客户端/脚本解析
    XUpdate.exe --apply --component client --manifest <本地路径或 URL>

默认清单：程序目录下的 ``update/manifest.json``，可用环境变量
``XFLYSIM_UPDATE_MANIFEST`` 覆盖。CSL / FSLTL / AIG 不归本更新器管。
"""
from __future__ import annotations

import argparse
import json
import os
import sys

from xfly_client.logutil import app_base_dir
from xfly_client.updater import (
    COMPONENTS, DEFAULT_MANIFEST, PreconditionError, UpdateError,
    UPDATER_VERSION, UpdatePlan, apply_plans, build_plans, read_manifest,
    sample_manifest,
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
    p.add_argument("--version", action="version",
                   version=f"XUpdate {UPDATER_VERSION}")
    return p


def _plan_report(plans: list[UpdatePlan]) -> dict:
    return {
        "mode": "check",
        "update_available": any(not p.up_to_date for p in plans),
        "plans": [p.summary() for p in plans],
    }


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.sample_manifest:
        print(json.dumps(sample_manifest(), ensure_ascii=False, indent=2))
        return 0

    base = os.path.abspath(args.base_dir or app_base_dir())
    from xfly_client.updater import UpdaterLog
    log = UpdaterLog(base, quiet=args.json)

    try:
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
