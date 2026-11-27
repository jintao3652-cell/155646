#!/usr/bin/env python
"""站点 aircraft 四字段（equipment/airline/livery/engines）核对工具。

## 为什么需要它

这些字段**服务器静默丢弃时不会报错**：`$ER` 里看不到任何东西，客户端日志里
报文也长得完全正确。过去几轮排查全靠"贴站点 JSON 给下一个人看"，一次往返
要一天。这个工具把两侧对齐成一张表：

    本地发出   ← 日志里的 `[report] PI …` 行（client 实际发出去的 PI）
    站点存储   ← get_site 接口返回的 aircraft 对象（服务器实际存下来的）

哪个字段 `-` → 空，就锁定在哪一侧断的。

## 用法

    # 1) 只看本机最近一次上报（离线，读 logs/ 日志）
    python tools/verify_site_aircraft.py

    # 2) 和站点对照（需要站点登录态）
    python tools/verify_site_aircraft.py --online --token <Bearer token>
    python tools/verify_site_aircraft.py --online --cookie "SESSION=..."

    # 3) 指定呼号/日志文件
    python tools/verify_site_aircraft.py --callsign SJX110 --log logs/xflysim-2026-11-27.log

站点接口：`/pilot/api/fsd/list/allOnlineUser`（需登录；匿名 403）。
token/cookie 用浏览器开发者工具 → Network 里任意 `/pilot/api/` 请求的头部，
或环境变量 `XFLYSIM_SITE_TOKEN` / `XFLYSIM_SITE_COOKIE`。

## 判读

| 本地 | 站点 | 结论 |
|---|---|---|
| 有 | 有 | 正常 |
| 有 | null | 服务器侧未采纳 → 看下一节"通道" |
| null | null | 本机没算出来 → 日志里那行会带 `missing=` |

通道：`equipment/airline/livery` 走 PI（`#SB…:PI:GEN`），
`engines` 只能走 ACC（`$CQ…:ACC`）。所以"前三个有值、engines 为 null"
= PI 通了、ACC 没被采纳，与 2026-11-27 的症状完全一致。
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import sys
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_API = "https://api.xflysim.com/pilot/api/fsd/list/allOnlineUser"

RE_REPORT = re.compile(r"\[report\] (?P<cs>\S+) PI (?P<rest>.+)$")
FIELDS = ("equipment", "airline", "livery", "engines")


def _force_utf8_stdout() -> None:
    """Windows 控制台默认 GBK，输出中文/符号会 UnicodeEncodeError。"""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


def latest_log() -> str:
    base = os.environ.get("XFLYSIM_LOG_DIR") or os.path.join(ROOT, "logs")
    files = sorted(glob.glob(os.path.join(base, "xflysim-*.log")))
    if files:
        return files[-1]
    old = os.path.join(ROOT, "fsd.log")
    return old if os.path.exists(old) else ""


def read_local(path: str, callsign: str = "") -> dict:
    """从日志里取最后一条 `[report] PI`（按需过滤呼号）。"""
    if not path or not os.path.exists(path):
        return {"error": f"日志不存在：{path or '(无)'}"}
    out: dict = {"path": path}
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            m = RE_REPORT.search(line.strip())
            if not m:
                continue
            cs = m.group("cs")
            if callsign and callsign.upper() != cs.upper():
                continue
            rest = m.group("rest")
            miss: list[str] = []
            if "missing=" in rest:
                rest, _, tail = rest.partition("missing=")
                miss = [t for t in tail.strip().split(",") if t]
            out["callsign"] = cs
            pairs = {}
            for kv in rest.split():
                if "=" in kv:
                    k, _, v = kv.partition("=")
                    pairs[k] = v
            out["pairs"] = pairs
            out["missing_sent"] = miss
            out["raw"] = line.strip()
    if "pairs" not in out:
        out["error"] = "日志里没有 [report] PI 行（客户端需运行到连线状态）"
    return out


def fetch_site(url: str, token: str = "", cookie: str = "", timeout: float = 15.0) -> dict:
    req = urllib.request.Request(url)
    req.add_header("Accept", "application/json, text/plain, */*")
    req.add_header("User-Agent", "XFlySim-Client/verify-aircraft")
    req.add_header("Referer", "https://www.xflysim.com/")
    if token:
        req.add_header("Authorization",
                       token if token.lower().startswith("bearer ") else f"Bearer {token}")
    if cookie:
        req.add_header("Cookie", cookie)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        hint = "（该接口需要登录态：--token / --cookie）" if exc.code in (401, 403) else ""
        return {"error": f"HTTP {exc.code}{hint}"}
    except OSError as exc:
        return {"error": f"连不上站点接口：{exc}"}
    try:
        return {"data": json.loads(body)}
    except ValueError:
        return {"error": "站点返回的不是 JSON（接口地址或登录态有问题）"}


def pick_player(data, callsign: str) -> dict:
    """从站点返回里找目标呼号的 aircraft 对象（兼容 list / {data:[…]} 两种外壳）。"""
    items = data
    if isinstance(items, dict):
        for key in ("data", "rows", "list", "result"):
            if isinstance(items.get(key), list):
                items = items[key]
                break
    if not isinstance(items, list):
        return {"error": "站点返回结构无法识别（预期是玩家列表）"}
    want = callsign.upper()
    for it in items:
        if not isinstance(it, dict):
            continue
        cs = str(it.get("callsign") or it.get("callsignText") or "").upper()
        if want and cs != want:
            continue
        ac = it.get("aircraft")
        if isinstance(ac, dict):
            return {"aircraft": ac, "raw": it}
    return {"error": f"站点列表里没有呼号 {callsign or '(未指定)'} 的在线记录"}


def print_table(local: dict, site: dict | None, callsign: str) -> int:
    print("=" * 74)
    print(f"站点 aircraft 四字段核对  呼号={callsign or '(未知)'}")
    print("=" * 74)
    lp = local.get("pairs") or {}
    sa = (site or {}).get("aircraft") or {}
    print(f"{'字段':10s} {'本地发出':28s} {'站点存储':28s} 判定")
    print("-" * 74)
    bad = 0
    for f in FIELDS:
        if f == "engines":
            lv = lp.get("ENGINES", "")
        else:
            lv = lp.get(f.upper(), "")
        sv = sa.get(f)
        sv_txt = "" if sv in (None, "", {}, 0) else str(sv)
        ok = bool(lv) and bool(sv_txt)
        verdict = "[OK]" if ok else ("[X] 本机没发" if not lv else "[X] 服务器未采纳")
        if not ok:
            bad += 1
        print(f"{f:10s} {lv or '-':28s} {sv_txt or '-':28s} {verdict}")
    print()
    if local.get("error"):
        print(f"[本机] {local['error']}")
    if site and site.get("error"):
        print(f"[站点] {site['error']}")
    if local.get("missing_sent"):
        print(f"[本机] 该条 PI 缺键：{local['missing_sent']}")
    if local.get("raw"):
        print(f"[本机] {local['raw']}")
    print()
    if bad == 0:
        print("结论：四个字段两侧一致，无需处理。")
        return 0
    if lp and not any(lp.get(k) for k in ("EQUIPMENT", "AIRLINE", "LIVERY")):
        print("结论：本机这条 PI 是残缺报文（机型/涂装都没算出来）。"
              "看日志上一行的 `[report]` 与机型推断；残缺 PI 会被服务器"
              "整块覆盖，必须杜绝。")
    elif lp.get("ENGINES") and sa.get("engines") in (None, "", {}):
        print("结论：PI 侧（equipment/airline/livery）正常、engines 为 null —— "
              "ACC 没被服务器采纳。检查日志里有没有 "
              "`[parts] full ACC -> SERVER`，以及是否出现 "
              "`服务器不接受直发 SERVER 的 ACC`。")
    else:
        print("结论：见上表逐字段判定。")
    return 1 if bad else 0


def main() -> int:
    _force_utf8_stdout()
    ap = argparse.ArgumentParser(description="核对站点 aircraft 四字段")
    ap.add_argument("--callsign", default="", help="呼号（默认取日志里最近一条）")
    ap.add_argument("--log", default="", help="日志路径（默认 logs/ 最新）")
    ap.add_argument("--online", action="store_true", help="同时读取站点接口")
    ap.add_argument("--api", default=os.environ.get("XFLYSIM_SITE_API") or DEFAULT_API)
    ap.add_argument("--token", default=os.environ.get("XFLYSIM_SITE_TOKEN", ""))
    ap.add_argument("--cookie", default=os.environ.get("XFLYSIM_SITE_COOKIE", ""))
    ap.add_argument("--json", action="store_true", help="输出原始 JSON")
    args = ap.parse_args()

    log = args.log or latest_log()
    local = read_local(log, args.callsign)
    callsign = args.callsign or local.get("callsign") or os.environ.get(
        "XFLYSIM_CALLSIGN", "")
    site = None
    if args.online:
        if not callsign:
            print("[!] --online 需要呼号：--callsign 或日志里的 [report] 行")
            return 2
        fetched = fetch_site(args.api, args.token, args.cookie)
        site = pick_player(fetched["data"], callsign) if "data" in fetched else fetched

    if args.json:
        print(json.dumps({"local": local, "site": site}, ensure_ascii=False, indent=2))
        return 0
    return print_table(local, site, callsign)


if __name__ == "__main__":
    sys.exit(main())