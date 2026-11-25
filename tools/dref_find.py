#!/usr/bin/env python3
"""XPlane dataref 查询工具：让插件实时枚举 dataref，并把结果按关键字过滤出来。

背景：`Resources/plugins/DataRefs.txt` 只是 Laminar 原生 dataref 的静态快照，
**不含第三方机模的私有 dataref**（实测 AirbusFBW / laminar/B738 匹配数均为 0）。
机模私有 dataref 是在加载 .acf 时才注册到 XPLM 的，只有运行时才看得到。

用法:
    python tools/dref_find.py                # 触发重扫，列出全部匹配项
    python tools/dref_find.py com VHF        # 只看名字/值含这些关键字的
    python tools/dref_find.py -k AirbusFBW   # 同上（-k 可省略）
    python tools/dref_find.py --no-scan      # 只读日志，不触发重扫

输出会打印 dataref 名、当前值、以及是否可写（[writable]）。
找 radio.json 要填的名字时：**先看哪个 dataref 的值和你在座舱里看到的一致**，
同型机常常同时存在通用和机模私有两套，只有值对得上的那个才是机模真正在用的。
"""
import os
import re
import socket
import sys
import time

HOST = "127.0.0.1"
PORT = 51001
XP_ROOT = r"D:\steam\steamapps\common\X-Plane 12"
LOG_PATH = os.path.join(XP_ROOT, "Resources", "plugins", "XConnect", "xconnect.log")


def parse_args(argv):
    keywords = []
    do_scan = True
    for a in argv:
        if a in ("-h", "--help"):
            print(__doc__)
            sys.exit(0)
        if a == "--no-scan":
            do_scan = False
        elif a in ("-k", "--keyword"):
            continue
        elif a.startswith("-"):
            print(f"未知参数: {a}")
        else:
            keywords.append(a)
    return keywords, do_scan


def trigger_scan():
    """向插件发 Send:SCAN，让它立刻重新枚举 dataref。"""
    try:
        s = socket.create_connection((HOST, PORT), timeout=5)
    except OSError as exc:
        print(f"[失败] 连不上插件 {HOST}:{PORT}: {exc}")
        print("       请确认 X-Plane 已启动插件，且没有其他客户端占用 51001")
        return False
    try:
        # 插件只服务单个客户端；读完首包就断开，避免占住连接
        s.settimeout(1.0)
        try:
            s.recv(4096)
        except OSError:
            pass
        s.sendall(b'{"type":"Send:SCAN"}\n')
        time.sleep(0.5)
    except OSError as exc:
        print(f"[警告] 发送 SCAN 失败: {exc}")
        return False
    finally:
        s.close()
    return True


def read_dref_lines(since_offset=0):
    if not os.path.exists(LOG_PATH):
        print(f"[失败] 找不到日志: {LOG_PATH}")
        return []
    with open(LOG_PATH, "rb") as f:
        f.seek(since_offset)
        data = f.read()
    text = data.decode("utf-8", "replace")
    return [ln for ln in text.splitlines() if " dref: " in ln or ln.lstrip().startswith("dref:")]


LINE_RE = re.compile(
    r"dref:\s+(?P<name>\S+)\s+=\s+(?P<value>.*?)(?P<writable>\s+\[writable\])?\s*$"
)


def parse_line(line):
    """把 `dref: sim/xxx = 121.800  [writable]` 解成 (name, value, writable)。"""
    idx = line.find("dref: ")
    if idx < 0:
        return None
    body = line[idx + len("dref: "):]
    # 跳过统计行 `dref: scanned=.. matched=..`
    if body.startswith("scanned="):
        return None
    if " = " not in body:
        return None
    name, rest = body.split(" = ", 1)
    writable = rest.rstrip().endswith("[writable]")
    if writable:
        rest = rest.rstrip()[: -len("[writable]")].rstrip()
    return name.strip(), rest.strip(), writable


def main():
    keywords, do_scan = parse_args(sys.argv[1:])

    start = os.path.getsize(LOG_PATH) if os.path.exists(LOG_PATH) else 0

    if do_scan:
        print("向插件请求重新扫描 dataref ...")
        if not trigger_scan():
            print("提示：可以用 --no-scan 只读日志里已有的结果")
        else:
            print("已发送。等插件写日志（约 1 秒）...")
            time.sleep(1.2)

    lines = read_dref_lines(start)
    if not lines:
        # 本次没有新结果，回退到读整个日志的最后一次扫描
        print("本次无新结果，读取日志里最后一次扫描 ...")
        lines = read_dref_lines(0)

    entries = []
    stats = ""
    for ln in lines:
        if "scanned=" in ln:
            stats = ln[ln.find("dref:"):].strip()
            continue
        p = parse_line(ln)
        if p:
            entries.append(p)

    # 去重（保留最后一次）
    seen = {}
    for name, value, writable in entries:
        seen[name] = (value, writable)
    rows = [(n, v, w) for n, (v, w) in seen.items()]

    if keywords:
        kw = [k.lower() for k in keywords]
        rows = [r for r in rows if any(k in r[0].lower() or k in r[1].lower() for k in kw)]

    if stats:
        print(stats)
    print(f"匹配 {len(rows)} 个 dataref" + (f"（过滤: {', '.join(keywords)}）" if keywords else ""))
    print("-" * 78)

    if not rows:
        print("没有匹配项。可尝试：")
        print("  1. 确认已进入飞行场景并加载了目标机模")
        print("  2. 不带关键字跑一次，看全部结果")
        print("  3. 关键字换成 COM / radio / audio / AirbusFBW 等")
        return

    w_name = max(len(r[0]) for r in rows)
    w_val = max(len(r[1]) for r in rows)
    for name, value, writable in sorted(rows):
        flag = " [W]" if writable else ""
        print(f"{name:<{w_name}}  {value:<{w_val}}{flag}")

    print("-" * 78)
    print("提示：[W] = 可写。找 radio.json 名字时，挑**值和你座舱显示一致**的那个；")
    print("      同型机常有通用与私有两套 dataref，值对得上的才是机模真正在用的。")


if __name__ == "__main__":
    main()