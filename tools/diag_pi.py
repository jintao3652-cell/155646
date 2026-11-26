#!/usr/bin/env python
"""PI 上报诊断：对比不同机型码的上报结果。

背景：服务器站点 API 返回的 `aircraft.{equipment,airline,livery,engines}`
在飞 F14（3 位 ICAO）时全为 null，而 swift 用户（全 4 位）正常。

假设：服务器对 EQUIPMENT 做了 **4 位长度校验**，非 4 位时静默丢弃整条 PI。

用法（先跑一次客户端，切换机型后重新运行本脚本）：
    python tools/diag_pi.py

它会从 fsd.log 里抽取每次「登录 → PI → 服务器响应」的序列，按机型码
长度分组统计：哪些机型码后面服务器给了正常欢迎语（说明 PI 被接受），
哪些没有任何回应（说明被丢弃）。
"""
from __future__ import annotations

import re
import sys
from collections import defaultdict
from pathlib import Path

LOG = Path(__file__).resolve().parent.parent / "fsd.log"

# 登录 / PI / 欢迎语
RE_AP = re.compile(r"^(\d{2}:\d{2}:\d{2}) > #AP(\S+?):SERVER:")
RE_PI = re.compile(r"^(\d{2}:\d{2}:\d{2}) > #SB(\S+?):SERVER:PI:GEN:(.*)$")
RE_WELCOME = re.compile(r"^(\d{2}:\d{2}:\d{2}) < #TMserver:(\S+?):We?clome")
RE_ERR = re.compile(r"^(\d{2}:\d{2}:\d{2}) < \$ERserver:(\S*?):(\d+):(\S*?):(.*)$")
RE_ACC = re.compile(r"^(\d{2}:\d{2}:\d{2}) > \$CQ(\S+?):@\d+:ACC:")


def parse_pi(body: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for kv in body.split(":"):
        if "=" in kv:
            k, _, v = kv.partition("=")
            out[k] = v
    return out


def main() -> int:
    if not LOG.exists():
        print(f"[x] 找不到日志：{LOG}")
        return 1

    lines = LOG.read_text(encoding="utf-8", errors="ignore").splitlines()

    # 按"呼号 + 登录时刻"分组，把同一会话里的 PI 收在一起
    sessions: list[dict] = []
    cur: dict | None = None
    for ln in lines:
        m = RE_AP.match(ln)
        if m:
            cur = {"t": m.group(1), "callsign": m.group(2),
                   "pis": [], "welcomed": False, "err": None, "acc": False}
            sessions.append(cur)
            continue
        m = RE_PI.match(ln)
        if m and cur is not None:
            cur["pis"].append((m.group(1), parse_pi(m.group(3))))
            continue
        m = RE_WELCOME.match(ln)
        if m and cur is not None:
            cur["welcomed"] = True
            continue
        m = RE_ERR.match(ln)
        if m and cur is not None and cur["err"] is None:
            cur["err"] = f"{m.group(3)} {m.group(5)}"
            continue
        m = RE_ACC.match(ln)
        if m and cur is not None:
            cur["acc"] = True

    if not sessions:
        print("[!] 日志里没有登录记录")
        return 1

    # 按机型码长度汇总
    stats: dict[int, dict[str, list]] = defaultdict(
        lambda: {"ok": [], "fail": []})

    print("=" * 74)
    print("会话明细（登录 → PI → 服务器是否回应）")
    print("=" * 74)
    for s in sessions:
        if not s["pis"]:
            continue
        eq = ""
        for _, p in s["pis"]:
            if p.get("EQUIPMENT"):
                eq = p["EQUIPMENT"]
                break
        if not eq:
            continue
        n = len(eq)
        verdict = "接受" if s["welcomed"] else "疑似丢弃"
        bucket = stats[n]["ok" if s["welcomed"] else "fail"]
        bucket.append(eq)
        print(f"  {s['t']}  {s['callsign']:<9s} EQUIPMENT={eq:<5s}"
              f"({n}位)  PI×{len(s['pis'])}  ACC={'有' if s['acc'] else '无'}"
              f"  → {verdict}")

    print()
    print("=" * 74)
    print("按机型码长度汇总")
    print("=" * 74)
    for n in sorted(stats):
        ok = stats[n]["ok"]
        bad = stats[n]["fail"]
        total = len(ok) + len(bad)
        rate = f"{len(ok)}/{total}"
        verdict = "✅ 正常" if not bad else (
            "❌ 全部失败" if not ok else "⚠️ 部分失败")
        samples = ", ".join(sorted(set(ok + bad))[:6])
        print(f"  {n} 位: {rate:>6s}  {verdict:<10s}  {samples}")

    print()
    print("=" * 74)
    print("结论")
    print("=" * 74)
    print("  ⚠️ 注意：本脚本用「收到 Weclome 欢迎语」当作 PI 被接受的近似判据，")
    print("     但它并不等价 —— 欢迎语只说明**登录成功**，")
    print("     PI 里的 aircraft 段仍可能被服务器单独丢弃。")
    print("     真正的判据要看站点 API 返回的 aircraft.equipment 是否为 null。")
    print()
    four = stats.get(4, {"ok": [], "fail": []})
    short = {n: v for n, v in stats.items() if n != 4}
    if four:
        print(f"  4 位机型码（{', '.join(sorted(set(four['ok'] + four['fail'])))}）："
              f"{len(four['ok'])}/{len(four['ok']) + len(four['fail'])} 登录成功")
    if short:
        for n, v in sorted(short.items()):
            codes = ", ".join(sorted(set(v["ok"] + v["fail"])))
            print(f"  {n} 位机型码（{codes}）："
                  f"{len(v['ok'])}/{len(v['ok']) + len(v['fail'])} 登录成功")
    print()
    if four and short and len(four["fail"]) == 0:
        print("  ⚠️ 目前只能确认「短机型码更容易出问题」，")
        print("     需要你去站点 API 核对 F14 那次的 aircraft.equipment 是否 null，")
        print("     才能确认是**长度校验**而不是别的因素。")
    return 0


if __name__ == "__main__":
    sys.exit(main())