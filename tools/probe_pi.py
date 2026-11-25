#!/usr/bin/env python
"""PI 上报对照实验 —— 隔离变量，定位"服务器丢弃 aircraft 段"的真正原因。

## 背景

2026-11-25 发现：XFlySim 站点 API 返回的 `aircraft` 全部为 null
（equipment/airline/livery/engines 一起丢），但位置与飞行计划正常。
而已知成功案例之间互相矛盾：

| 时间 | 客户端 | equipment | livery | 结果 |
|---|---|---|---|---|
| 11-24 20:06 | 我们(?) | A333 | D-AIKI（尾号） | ✅ |
| 11-25 现在 | swift | F14 | SWIFT_L181A... | ✅ |
| 11-25 09:40 | 我们 | F14 | N988DR（尾号） | ❌ |

于是产生了两个互相矛盾的假设，都被对方的证据推翻过：
  A. "必须 4 位机型码"        ← 被 swift 的 F14（3 位）推翻
  B. "LIVERY 必须是 swift 格式" ← 被 20:06 的 D-AIKI（尾号）推翻

## 本工具做什么

用**同一个合法账号**依次连服务器，每次只发一条 PI，然后
**向服务器请求自己的机型信息**（PIR），看它回显什么。
回显的字段就是服务器实际存下来的值 —— 这是唯一不依赖外部
网站/缓存/记忆的判据。

因为 PIR 的应答来自服务器本地的玩家表，所以：
  - 回显完整 → 服务器收下了
  - 回显缺失 → 服务器丢弃了

## 用法

    python tools/probe_pi.py                 # 跑全部用例
    python tools/probe_pi.py --only livery   # 只跑指定组
    python tools/probe_pi.py --dry           # 只看会发什么，不连服务器

⚠️ 这会用你的账号反复登录/登出（每个用例约 3 秒）。
   跑之前请确认：XFlySim 客户端**已关闭**，否则两个进程抢同一个
   呼号会被服务器踢。
"""
from __future__ import annotations

import argparse
import socket
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from xfly_client.config import load_config          # noqa: E402
from xfly_client.fsd import packet as pk            # noqa: E402
from xfly_client.fsd.link import FsdLink            # noqa: E402
from xfly_client.fsd.parser import parse            # noqa: E402


# 用例：(组名, 说明, PI 报文构造参数)
# equipment/airline/livery/registration/engines 直接给 pi 函数用
CASES: list[tuple[str, str, dict]] = [
    # ---- 组 1：机型码长度（验证假设 A）----
    ("length", "4 位机型 A333 + 尾号 livery", dict(
        equipment="A333", airline="SJX", livery="D-AIKI",
        registration="D-AIKI", engines=2)),
    ("length", "3 位机型 F14 + 尾号 livery", dict(
        equipment="F14", airline="SJX", livery="N988DR",
        registration="N988DR", engines=2)),
    ("length", "3 位机型 F14 + swift livery", dict(
        equipment="F14", airline="SJX", livery="SWIFT_A4278",
        registration="N988DR", engines=2)),

    # ---- 组 2：LIVERY 格式（验证假设 B）----
    ("livery", "A333 + swift livery", dict(
        equipment="A333", airline="SJX", livery="SWIFT_A685",
        registration="D-AIKI", engines=2)),
    ("livery", "A333 + 无 livery", dict(
        equipment="A333", airline="SJX", livery="",
        registration="D-AIKI", engines=2)),

    # ---- 组 3：ENGINES 字段（位置敏感：必须紧跟 LIVERY）----
    ("engines", "A333 不带 ENGINES", dict(
        equipment="A333", airline="SJX", livery="D-AIKI",
        registration="D-AIKI", engines=0)),
]


def build(d: dict) -> str:
    return pk.plane_info("PROBE", "SERVER", d["equipment"], d["airline"],
                         d["livery"], d["registration"], d["engines"])


def probe_once(cfg, body: dict, wait: float = 2.5) -> dict:
    """登录 → 发 PI → 请求自己的 PI → 收集回显。

    返回 {"pi_raw": 服务器回显的原文, "error": 服务器错误, "online": 是否登录成功}
    """
    callsign = (cfg.callsign or "").strip().upper()
    out = {"pi_raw": None, "error": None, "online": False}
    if not callsign:
        out["error"] = "配置里没有呼号"
        return out

    link = FsdLink(cfg.server_host, cfg.server_port)
    got: list[str] = []

    def on_line(line: str) -> None:
        print(f"    << {line}")
        p = parse(line)
        # 收集所有 PI 应答（服务器可能在 PIR 后回显，也可能直接广播）
        if p.type == "PI":
            got.append(line)
        if p.type == "ER":
            out["error"] = line

    link.on_line = on_line
    try:
        link.connect(timeout=5.0)
    except OSError as e:
        out["error"] = f"连不上 {cfg.server_host}:{cfg.server_port} ({e})"
        return out

    try:
        # 1. 登录
        link.send(pk.add_pilot(callsign, cfg.cid, cfg.password,
                               real_name=cfg.real_name or "probe"))
        time.sleep(0.8)
        out["online"] = True

        # 2. 发目标 PI（只发给服务器，不发别人的）
        msg = pk.plane_info(callsign, "SERVER", body["equipment"],
                            body["airline"], body["livery"],
                            body["registration"], body["engines"])
        print(f"    >> {msg}")
        link.send(msg)
        time.sleep(0.6)

        # 3. 请服务器回答"我是什么机型"。
        #    PIR 是"机型信息请求"，服务器应从它自己的玩家表里取数据回显。
        link.send(pk.plane_info_request(callsign, "SERVER"))
        time.sleep(wait)
    finally:
        try:
            link.send(pk.delete_pilot(callsign, cfg.cid))
            time.sleep(0.2)
        except Exception:
            pass
        try:
            link.close()
        except Exception:
            pass

    # 取最后一条含 EQUIPMENT 的 PI 作为回显
    for line in reversed(got):
        if "EQUIPMENT" in line or "PI:GEN" in line:
            out["pi_raw"] = line
            break
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="", help="只跑某一组：length/livery/engines")
    ap.add_argument("--dry", action="store_true", help="只打印报文，不连服务器")
    args = ap.parse_args()

    cfg = load_config()
    print("=" * 78)
    print("PI 上报对照实验")
    print("=" * 78)
    print(f"  服务器 : {cfg.server_host}:{cfg.server_port}")
    print(f"  呼号   : {cfg.callsign!r}  CID: {cfg.cid!r}")
    print(f"  姓名   : {cfg.real_name!r}")
    if not cfg.cid or not cfg.password:
        print("  [!] 配置里缺 CID 或密码，无法登录 —— 请先在客户端里填好再跑")
    print()

    groups: dict[str, list] = {}
    for g, desc, body in CASES:
        if args.only and g != args.only:
            continue
        groups.setdefault(g, []).append((desc, body))

    if args.dry:
        for g, items in groups.items():
            print(f"[{g}]")
            for desc, body in items:
                print(f"  {desc}")
                print(f"    {build(body)}")
        return 0

    results = []
    for g, items in groups.items():
        print(f"\n{'─' * 78}\n[{g}] {len(items)} 个用例\n{'─' * 78}")
        for desc, body in items:
            print(f"\n  ▸ {desc}")
            print(f"    {build(body)}")
            r = probe_once(cfg, body)
            results.append((g, desc, body, r))
            print(f"    → 登录: {'成功' if r['online'] else '失败'}"
                  f"  回显: {r['pi_raw'] or '(无)'}")
            if r["error"]:
                print(f"    ! {r['error']}")
            time.sleep(1.0)

    # ---- 汇总 ----
    print(f"\n{'=' * 78}\n汇总\n{'=' * 78}")
    print(f"{'用例':34s} {'EQUIP':7s} {'AIRLINE':8s} {'LIVERY':18s} 回显")
    print("─" * 78)
    for g, desc, body, r in results:
        echo = r["pi_raw"] or "(无回显)"
        # 从回显里抠字段
        import re
        def fld(k: str) -> str:
            m = re.search(rf"{k}=([^:]*)", echo)
            return m.group(1) if m else "—"
        print(f"{desc:34s} {fld('EQUIPMENT'):7s} {fld('AIRLINE'):8s} "
              f"{fld('LIVERY'):18s} {'✅' if r['pi_raw'] else '❌'}")
    print()
    print("判读方法：")
    print("  · 回显里有 EQUIPMENT → 服务器收下了这条 PI")
    print("  · 回显是 (无) 或字段缺失 → 服务器丢弃了")
    print("  · 同一 equipment 在两个 livery 下结果不同 → 是 livery 的问题")
    print("  · 4 位和 3 位结果不同 → 是机型码长度的问题")
    return 0


if __name__ == "__main__":
    sys.exit(main())