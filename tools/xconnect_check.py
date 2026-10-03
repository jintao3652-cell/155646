#!/usr/bin/env python3
"""XConnect 插件数据链路简易验证。

连接插件（127.0.0.1:51001），原始打印 10 秒内收到的所有报文。
用法: python tools/xconnect_check.py [地址] [端口]

判定：
- 收到 connected + 多条 flight_data  → 插件数据链路正常（客户端 UI 无显示则排查客户端）
- 仅收到 connected，无 flight_data   → 插件 flight loop 未运行：
    请确认已进入飞行场景（X-Plane 主菜单/设置界面不会发送飞行数据）
- 连接失败                          → 插件未启动/端口不通
"""
import socket
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 51001
DURATION = 10.0

print(f"连接 XConnect 插件 {HOST}:{PORT} ...")
try:
    s = socket.create_connection((HOST, PORT), timeout=5)
except OSError as exc:
    print(f"[失败] 无法连接插件: {exc}")
    print("请确认 X-Plane 已运行且 XConnect 插件已启用。")
    sys.exit(1)

print("已连接。等待插件报文（10 秒）...")
s.settimeout(2.0)
buf = b""
counts = {}
deadline = time.time() + DURATION
try:
    while time.time() < deadline:
        try:
            data = s.recv(65536)
        except socket.timeout:
            print("  （2 秒无数据）")
            continue
        if not data:
            print("[失败] 插件关闭了连接")
            break
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.strip()
            if not line:
                continue
            text = line.decode("utf-8", "replace")
            kind = text[:80]
            counts[kind] = counts.get(kind, 0) + 1
            print(f"  收到: {text[:160]}")
finally:
    s.close()

print("\n=== 统计 ===")
for kind, n in counts.items():
    print(f"  {kind}  x{n}")
if counts.get('{"type":"flight_data"'):
    print("\n结果: 数据链路正常（flight_data 已到达）。")
    print("若客户端界面仍无数据显示，请检查客户端调试输出（设置→调试模式）。")
elif any(k.startswith('{"type"') for k in counts):
    print("\n结果: 仅收到 connected，未收到 flight_data。")
    print("请进入飞行场景（主菜单/设置界面不触发飞行循环，不会发送飞行数据）。")
else:
    print("\n结果: 未收到任何报文。请检查插件是否启用与 Log.txt。")