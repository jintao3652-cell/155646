#!/usr/bin/env python3
"""XConnect 插件配合示例。

连接插件（127.0.0.1:51001），接收 flight_data 并展示；
周期推送一组模拟在线玩家（Send:OPD:FSD9），插件将渲染为 CSL 飞机并跟随移动。

真实使用：将 XFlySim-Client 的在线 roster（玩家位置/机型）推送给插件即可。

`--ground`：在停机位推 4 架"地面目标"，故意制造 0 / +250ft / −120ft 的高度
偏差并置 on_ground=true，用于验证落地吸附（不再悬空/陷地）、涂装回退
（AAA 无此涂装 → 用包内基础白机而非别家航司）、未知机型的 ZZZZ 回退，以及
机型别名（FA8X → F900、B38M → B738）。
运行: python tools/xconnect_bridge.py [--ground]
"""

import json
import math
import socket
import sys
import threading
import time

HOST, PORT = "127.0.0.1", 51001
PUSH_INTERVAL = 5.0
GROUND_TEST = "--ground" in sys.argv

# 演示编队参数：绕本机飞行
DEMO_RADIUS_DEG = 0.00005    # ≈550m 编队半径（在视野正旁）
DEMO_ALT_OFFSETS = [0, 300, -300]   # 各机相对本机高度差（ft）

# 模拟玩家：绕本机坐标旋转飞行（演示机型/涂装映射）
DEMO_PLANES = [
    {"callsign": "CCA999", "aircraft": "B738", "airline": "CCA", "index": 0},
    {"callsign": "CSN301", "aircraft": "A320", "airline": "CSN", "index": 1},
    {"callsign": "CES888", "aircraft": "A333", "airline": "CES", "index": 2},
    {"callsign": "BAW751", "aircraft": "A359", "airline": "BAW", "index": 3},
    {"callsign": "ZZZ1", "aircraft": "XXXX", "airline": "", "index": 4},
    # 别名表验证：库里没有 FA8X/B38M，应分别落到 F900（白机）/ B738（CCA 涂装）
    {"callsign": "N829PL", "aircraft": "FA8X", "airline": "DLH", "index": 5},
    {"callsign": "CCA38M", "aircraft": "B38M", "airline": "CCA", "index": 6},
]

# 地面验证用：高度偏差（ft，相对本机海拔）+ 涂装码
GROUND_PLANES = [
    {"callsign": "TST001", "aircraft": "B738", "airline": "CCA", "alt_off": 0.0},
    {"callsign": "TST002", "aircraft": "B738", "airline": "CCA", "alt_off": 250.0},
    {"callsign": "TST003", "aircraft": "A359", "airline": "BAW", "alt_off": -120.0},
    {"callsign": "TST004", "aircraft": "XXXX", "airline": "", "alt_off": 0.0},
    # 别名回退：FA8X → F900 白机（DLH 在 F900 包里没有，落 ZZZ 变体）
    {"callsign": "TST005", "aircraft": "FA8X", "airline": "DLH", "alt_off": 0.0},
    # 尾号匹配（第一级）：包内若有 B738_CCA_B-1234.obj 这类"绑定尾号"的变体，
    # 插件会优先选它；没有则退回同航司同机型
    {"callsign": "TST006", "aircraft": "B738", "airline": "CCA",
     "alt_off": 0.0, "registration": "B-1234"},
]

# 圆点坐标（本机位置与高度，ft），由 flight_data 实时更新（GIL 下写读安全）
_base = [23.13, 113.20, 3000.0]


def rx_loop(sock: socket.socket, stop: threading.Event) -> None:
    buf = b""
    last_print = 0.0
    while not stop.is_set():
        try:
            data = sock.recv(16384)
        except OSError:
            break
        if not data:
            print("[!] 插件连接关闭")
            break
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                continue
            if msg.get("type") == "flight_data":
                _base[0] = msg["latitude"]   # 以本机为圆心，保证飞机始终可见
                _base[1] = msg["longitude"]
                _base[2] = msg["altitude_msl"]
                if time.time() - last_print > 1.0:
                    last_print = time.time()
                    fd = msg
                    print(f"飞行数据: {fd['latitude']:.5f},{fd['longitude']:.5f} "
                          f"ALT {fd['altitude_msl']:.0f}ft GS {fd['groundspeed']:.0f}kt "
                          f"HDG {fd['true_heading']:.0f} XPDR {fd['transponder']:04d}")


def send_players(sock: socket.socket, t: float) -> None:
    data = []
    base_lat, base_lon, base_alt = _base
    if GROUND_TEST:
        # 地面验证：并排停在机头前方，on_ground=1、AGL=0，高度带人为偏差
        for i, p in enumerate(GROUND_PLANES):
            data.append({
                "callsign": p["callsign"],
                "latitude": base_lat + 0.00003 * (i - 1),
                "longitude": base_lon + 0.00018,
                "altitude": base_alt + p["alt_off"],   # 故意偏离地形高度
                "pitch": 0.0,
                "bank": 0.0,
                "heading": 90.0,
                "ground_speed": 0,
                "aircraft": p["aircraft"],
                "airline": p["airline"],
                "registration": p.get("registration", ""),
                "on_ground": True,
                "altitude_agl": 0.0,
            })
    else:
        for p in DEMO_PLANES:
            r = DEMO_RADIUS_DEG
            theta = t * 0.15 + p["index"] * math.pi * 2 / 3
            alt_off = DEMO_ALT_OFFSETS[p["index"] % len(DEMO_ALT_OFFSETS)]
            data.append({
                "callsign": p["callsign"],
                "latitude": base_lat + r * math.cos(theta),
                "longitude": base_lon + r * math.sin(theta),
                "altitude": max(0.0, base_alt + alt_off),   # 跟随本机高度
                "pitch": 1.5,
                "bank": -10.0 * math.sin(theta),
                "heading": (theta * 180 / math.pi) % 360,
                "ground_speed": 260 - p["index"] * 20,
                "aircraft": p["aircraft"],
                "airline": p["airline"],
                "registration": p.get("registration", ""),
                "on_ground": False,
            })
    sock.sendall((json.dumps(
        {"type": "Send:OPD:FSD9", "mycid": "1001", "data": data}) + "\n").encode())


def main() -> None:
    print(f"连接 XConnect 插件 {HOST}:{PORT} ...")
    sock = socket.create_connection((HOST, PORT), timeout=5)
    print("已连接，等待插件就绪…")
    buf = b""
    deadline = time.time() + 5
    accepted = False
    while time.time() < deadline and not accepted:
        try:
            sock.settimeout(0.2)
            buf += sock.recv(4096)
        except socket.timeout:
            continue
        if b'"connected"' in buf:
            accepted = True
    if not accepted:
        print("[!] 插件未接受本连接（插件为单客户端，webapp 正占用 51001？）")
        print("    请先关闭 webapp（或插件管理禁用后仅运行本工具），再重试。")
        sock.close()
        return

    stop = threading.Event()
    threading.Thread(target=rx_loop, args=(sock, stop), daemon=True).start()

    t0 = time.time()
    print(f"每 {PUSH_INTERVAL:.0f}s 推送模拟玩家（绕本机飞行，演示 CSL 渲染），Ctrl+C 退出")
    try:
        while True:
            time.sleep(PUSH_INTERVAL)
            send_players(sock, time.time() - t0)
            print(f"[推] 已发送 {len(DEMO_PLANES)} 架演示飞机")
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        sock.close()
        print("已断开")


if __name__ == "__main__":
    main()