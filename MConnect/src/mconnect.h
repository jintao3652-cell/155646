/* MConnect — 公共定义：常量、锁、单调时钟、诊断日志。
 *
 * MConnect 是 XConnect 的 Microsoft Flight Simulator 版本：同样在模拟器侧
 * 起 TCP 服务端（默认 51002），向 XFlySim-Client 推送 flight_data，并接收
 * 在线玩家列表注入为 AI 飞机。
 *
 * 模块：
 *   mconnect.c  入口、SimConnect 派发线程、客户端协议解析、系统状态
 *   sc.c        SimConnect.dll 动态装载与 API 表（2020/2024 运行时兼容）
 *   datadefs.c  本机飞行数据数据定义、读取与 flight_data 构建
 *   catalog.c   机型/涂装目录扫描与其四级匹配
 *   players.c   玩家表、位置时间插值、AI 对象注入与位置驱动
 *   net.c       TCP 收发
 *   json.c      JSON 解析
 *   ini.c       aircraft.cfg / livery.cfg 轻量 INI 解析（catalog 用）
 */
#ifndef MCONNECT_H
#define MCONNECT_H

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#define MCONNECT_NAME   "MConnect"
#define MCONNECT_SIG    "xflysim.mconnect"
#define MCONNECT_DESC   "XFlySim MSFS bridge: flight data + AI traffic rendering"
#define MCONNECT_VER    "1.0"

/* 协议端口。
 *
 * 51001 是 X-Plane 的 XConnect 插件默认端口 —— MSFS 侧的 MConnect 必须避开，
 * 否则同时开 X-Plane 与 MSFS 时端口冲突（实测 X-Plane 12 会占住 51001）。
 * 因此 MSFS 侧固定用 51002；被占用时再退到 51003/51004。
 *
 * 客户端（XFlySim-Client）按 sim 类型选端口：
 *   X-Plane → 51001（XConnect）
 *   MSFS    → 51002（MConnect）
 * 详见 xfly_client/config.py 的 xconnect_port / mconnect_port。 */
#define LISTEN_PORT           51002
#define LISTEN_PORT_ALT1      51003
#define LISTEN_PORT_ALT2      51004

#define PLAYER_MAX            256
#define PLAYER_TIMEOUT        30.0   /* 秒：玩家超时清理 */
#define DATA_INTERVAL         0.2    /* 秒：flight_data 发送间隔（5Hz） */
#define LOOP_INTERVAL         0.02   /* 秒：派发/渲染节拍（50Hz 平滑） */
#define CPU_LOAD_EVERY        1.0    /* 秒：SimConnect CPU 负载探测间隔 */

/* 内部角度/距离单位：协议为 ft / kt / 度，与 MSFS 原生一致，故内部直接用 ft。 */

/* ---------------- 锁与时钟 ---------------- */

#ifdef _WIN32
#include <windows.h>

#define LOCK_TYPE  CRITICAL_SECTION
#define LOCK_INIT(l)  InitializeCriticalSection((CRITICAL_SECTION*)(l))
#define LOCK_DONE(l)  DeleteCriticalSection((CRITICAL_SECTION*)(l))
#define LOCK_ENTER(l) EnterCriticalSection((CRITICAL_SECTION*)(l))
#define LOCK_LEAVE(l) LeaveCriticalSection((CRITICAL_SECTION*)(l))

/* 单调时钟（秒）。插值依赖高精度时间轴，GetTickCount64 只有 ~15ms 粒度，
 * 会让 50Hz 渲染出现抖动，故用 QPC。 */
double xc_now_sec(void);

#define THREAD_RET DWORD WINAPI
#define THREAD_ARG LPVOID
#else
#error "MConnect 仅支持 Windows（Microsoft Flight Simulator 2020/2024）"
#endif

/* ---------------- 诊断日志 ---------------- */

/* 写诊断日志（exe 目录 mconnect.log）。路径在 main 里初始化。 */
void xc_logf(const char* fmt, ...);

/* 同时写日志与 stdout（用户可见的关键事件：连上模拟器、监听端口等）。 */
void xc_infof(const char* fmt, ...);

/* exe 所在目录（不含末尾分隔符）。 */
const char* xc_exe_dir(void);

/* 自绘呼号标签开关在 control.c（见 control.h 的
 * control_labels_overlay_on / control_toggle_labels_overlay /
 * control_set_labels_overlay）：--labels / Send:LABELS 控制 HTTP roster
 * 快照（本进程同端口 51002-4）里的 labels_on，供 sim 内 xflysim-labels
 * overlay 轮询显隐 —— MSFS 对注入 AI 不渲染原生铭牌（硬限制）。 */

#endif /* MCONNECT_H */