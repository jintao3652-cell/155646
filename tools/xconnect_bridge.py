#!/usr/bin/env python3
"""XConnect 插件配合示例。

连接插件（127.0.0.1:51001），接收 flight_data 并展示；
周期推送一组模拟在线玩家（Send:OPD:FSD9），插件将渲染为 CSL 飞机并跟随移动。

真实使用：将 XFlySim-Client 的在线 roster（玩家位置/机型）推送给插件即可。
运行: python tools/xconnect_bridge.py
"""

import json
import math
import socket
import threading
import time

HOST, PORT = "127.0.0.1", 51001
PUSH_INTERVAL = 5.0

# 模拟玩家：绕本机坐标旋转飞行（演示机型映射）
DEMO_PLANES = [
    {"callsign": "CCA999", "aircraft": "B738", "family": "CSL_B738", "index": 0},
    {"callsign": "CSN301", "aircraft": "A320", "family": "CSL_A320", "index": 1},
    {"callsign": "CES888", "aircraft": "A333", "family": "CSL_A333", "index": 2},
]


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
            if msg.get("type") == "flight_data" and time.time() - last_print > 1.0:
                last_print = time.time()
                fd = msg
                print(f"飞行数据: {fd['latitude']:.5f},{fd['longitude']:.5f} "
                      f"ALT {fd['altitude_msl']:.0f}ft GS {fd['groundspeed']:.0f}kt "
                      f"HDG {fd['true_heading']:.0f} XPDR {fd['transponder']:04d}")


def send_players(sock: socket.socket, t: float, base_lat: float, base_lon: float) -> None:
    data = []
    for p in DEMO_PLANES:
        r = 0.02
        theta = t * 0.15 + p["index"] * math.pi * 2 / 3
        data.append({
            "callsign": p["callsign"],
            "latitude": base_lat + r * math.cos(theta),
            "longitude": base_lon + r * math.sin(theta),
            "altitude": 3000 + p["index"] * 500,
            "pitch": 1.5,
            "bank": -10.0 * math.sin(theta),
            "heading": (theta * 180 / math.pi) % 360,
            "ground_speed": 260 - p["index"] * 20,
            "aircraft": p["aircraft"],
            "aircraft_family": p["family"],
        })
    sock.sendall((json.dumps(
        {"type": "Send:OPD:FSD9", "mycid": "1001", "data": data}) + "\n").encode())


def main() -> None:
    print(f"连接 XConnect 插件 {HOST}:{PORT} ...")
    sock = socket.create_connection((HOST, PORT), timeout=5)
    print("已连接，等待插件就绪…")
    buf = b""
    deadline = time.time() + 5
    while time.time() < deadline and b'"connected"' not in buf:
        try:
            buf += sock.recv(4096)
        except socket.timeout:
            pass
        time.sleep(0.1)

    stop = threading.Event()
    threading.Thread(target=rx_loop, args=(sock, stop), daemon=True).start()

    t0 = time.time()
    base_lat, base_lon = 23.13, 113.20  # 以本机位置为中心（实际应使用 flight_data）
    print(f"每 {PUSH_INTERVAL:.0f}s 推送模拟玩家（演示 CSL 渲染），Ctrl+C 退出")
    try:
        while True:
            time.sleep(PUSH_INTERVAL)
            send_players(sock, time.time() - t0, base_lat, base_lon)
            print(f"[推] 已发送 {len(DEMO_PLANES)} 架演示飞机")
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        sock.close()
        print("已断开")


if __name__ == "__main__":
    main()