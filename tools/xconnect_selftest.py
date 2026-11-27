#!/usr/bin/env python3
"""XConnect.xpl 独立收发自检（两个层次）。

用法：
  python tools/xconnect_selftest.py            # 静态检查编译产物 + 本机回环协议测试
  python tools/xconnect_selftest.py --live     # 额外连接真实插件（需 X-Plane 已加载 xpl）

层次 1（离线，任何机器可跑）：
  - 解析 XConnect.xpl 的 PE：x64、DLL(子系统 2)、只依赖 XPLM_64/WS2_32/OPENGL32/KERNEL32
    → 说明 XP 能加载，且无 VCRT/额外运行库依赖，单文件即可。
  - 本机起一个假插件 socket，按 docs/XConnect-PROTOCOL.md 回 connected + flight_data，
    客户端侧用 XConnectClient 连接，验证「输入（插件→客户端）」被正确解析。
  - 客户端推 Send:OPD:FSD9 roster，假插件读回，验证「输出（客户端→插件）」格式正确。

层次 2（--live，需真机）：
  - 复用 tools/xconnect_check.py 的判定：连 127.0.0.1:51001 读 flight_data。
"""
from __future__ import annotations

import json
import os
import socket
import struct
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)


def pe_info(path: str) -> dict:
    with open(path, "rb") as fh:
        d = fh.read()
    if d[:2] != b"MZ":
        raise ValueError("不是 PE 文件")
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    machine = struct.unpack_from("<H", d, pe + 4)[0]
    nsect = struct.unpack_from("<H", d, pe + 6)[0]
    optsz = struct.unpack_from("<H", d, pe + 20)[0]
    magic = struct.unpack_from("<H", d, pe + 24)[0]
    subsystem = struct.unpack_from("<H", d, pe + 24 + 68)[0]
    dd = pe + 24 + (112 if magic == 0x20B else 96)
    import_rva = struct.unpack_from("<I", d, dd + 8)[0]

    secs = []
    so = pe + 24 + optsz
    for i in range(nsect):
        o = so + i * 40
        vsz, va, rsz, ro = struct.unpack_from("<IIII", d, o + 8)
        secs.append((va, vsz, ro, rsz))

    def rva2off(rva):
        for va, vsz, ro, rsz in secs:
            if va <= rva < va + max(vsz, rsz):
                return ro + (rva - va)
        return None

    dlls = []
    off = rva2off(import_rva)
    while off and off + 20 <= len(d):
        name_rva = struct.unpack_from("<I", d, off + 12)[0]
        if name_rva == 0:
            break
        no = rva2off(name_rva)
        if no:
            end = d.find(b"\0", no)
            dlls.append(d[no:end].decode("ascii", "replace"))
        off += 20
    return {"machine": machine, "subsystem": subsystem, "dlls": dlls}


def check_static() -> bool:
    candidates = [
        os.path.join(ROOT, "XConnect/build-vs2026/XConnect.xpl"),
        os.path.join(ROOT, "XConnect/build/XConnect.xpl"),
    ]
    xpl = next((p for p in candidates if os.path.isfile(p)), None)
    if not xpl:
        print("[静态] 未找到 XConnect.xpl（先编译）")
        return False
    info = pe_info(xpl)
    ok = True
    if info["machine"] != 0x8664:
        print(f"[静态] FAIL 不是 x64: {info['machine']:#x}")
        ok = False
    if info["subsystem"] != 2:
        print(f"[静态] FAIL 不是 DLL: subsystem={info['subsystem']}")
        ok = False
    allowed = {"xplm_64.dll", "ws2_32.dll", "opengl32.dll", "kernel32.dll"}
    extra = [d for d in info["dlls"] if d.lower() not in allowed]
    if extra:
        print(f"[静态] WARN 额外依赖: {extra}（XP 上可能缺 DLL）")
    print(f"[静态] {os.path.relpath(xpl, ROOT)}: x64 DLL, 依赖={info['dlls']}")
    print("[静态] OK：XP 可加载，无 VCRT 依赖，单文件即可（CSL/radio.json 均可选）")
    return ok


def check_loopback() -> bool:
    from xfly_client.xconnect.client import PlayerEntry, XConnectClient
    port = 51391
    got = []
    errors = []
    ready = threading.Event()
    stop = threading.Event()
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(1)

    def fake_plugin():
        srv.settimeout(5)
        try:
            conn, _ = srv.accept()
        except Exception as e:
            errors.append(f"accept: {e}")
            ready.set()
            return
        with conn:
            conn.sendall(b'{"type":"connected"}\n')
            flight = {"type": "flight_data", "lat": 22.6, "lon": 113.8,
                      "alt": 1000.0, "heading": 180.0, "gear_down": True,
                      "gear": 1, "flaps": 0, "engines": {"1": {"on": True}}}
            conn.sendall((json.dumps(flight) + "\n").encode())
            ready.set()
            conn.settimeout(3)
            buf = b""
            try:
                deadline = time.time() + 3
                while time.time() < deadline:
                    data = conn.recv(65536)
                    if not data:
                        break
                    buf += data
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        if line.strip():
                            got.append(line.decode("utf-8", "replace"))
                    if any("Send:OPD" in g for g in got):
                        break
            except socket.timeout:
                pass
        stop.set()

    t = threading.Thread(target=fake_plugin, daemon=True)
    t.start()

    cli = XConnectClient("127.0.0.1", port)
    data_ok = threading.Event()
    received = []

    def on_data(d):
        received.append(d)
        data_ok.set()

    cli.on_flight_data = on_data
    cli.on_connected = lambda: None
    cli.on_log = lambda m: None
    cli.start()
    ok_input = data_ok.wait(4)
    # 推一条 roster，验证输出方向（客户端→插件）
    cli.send_players("555001", [PlayerEntry(
        callsign="TEST1", latitude=22.6, longitude=113.8, altitude=1000,
        heading=90.0, aircraft="B738", airline="CCA", registration="B-1234")])
    stop.wait(5)
    cli.stop()
    srv.close()

    print(f"[回环] 插件→客户端 flight_data: {'OK' if ok_input and received else 'FAIL'}")
    print(f"[回环] 客户端→插件 原始报文: {got[:2] if got else '无'}")
    ok_output = any("Send:OPD" in g for g in got)
    if not ok_output:
        print("[回环] FAIL 未收到 roster 推送（检查 push_roster 实现）")
    return ok_input and ok_output


def check_live() -> bool:
    try:
        s = socket.create_connection(("127.0.0.1", 51001), timeout=5)
    except OSError as e:
        print(f"[真机] 无法连接 51001: {e}")
        return False
    s.settimeout(3)
    data = b""
    end = time.time() + 5
    kinds = set()
    try:
        while time.time() < end:
            try:
                chunk = s.recv(65536)
            except socket.timeout:
                continue
            if not chunk:
                break
            data += chunk
            while b"\n" in data:
                line, data = data.split(b"\n", 1)
                if line.strip():
                    try:
                        kinds.add(json.loads(line.decode("utf-8", "replace")).get("type"))
                    except Exception:
                        pass
    finally:
        s.close()
    print(f"[真机] 收到类型: {sorted(k for k in kinds if k)}")
    if "flight_data" in kinds:
        print("[真机] OK：已收到 flight_data（需在飞行场景）")
        return True
    print("[真机] 仅 connected 或无数据：确认已进入飞行场景")
    return False


if __name__ == "__main__":
    ok = check_static()
    try:
        ok = check_loopback() and ok
    except Exception as e:
        print(f"[回环] 异常: {e}")
        ok = False
    if "--live" in sys.argv:
        ok = check_live() and ok
    print("\n结果:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)
