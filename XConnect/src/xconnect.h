/* XConnect — 公共定义：常量、锁、单调时钟、诊断日志。
 *
 * 各模块：
 *   xconnect.c  插件入口、飞行循环、网络线程、客户端协议解析、CSL 状态汇总
 *   datarefs.c  本机飞行数据 dataref 采集与 flight_data 构建
 *   players.c   远程玩家表与位置时间插值平滑（CSL 渲染驱动）
 *   csl.c       CSL 实例加载与位置写入
 *   net.c       TCP 收发
 *   json.c      JSON 解析
 */
#ifndef XCONNECT_H
#define XCONNECT_H

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#endif

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "XPLMDataAccess.h"
#include "XPLMInstance.h"
#include "XPLMPlanes.h"
#include "XPLMProcessing.h"
#include "XPLMPlugin.h"
#include "XPLMUtilities.h"

#include "csl.h"
#include "json.h"
#include "net.h"

#define XCONNECT_NAME "XConnect"
#define XCONNECT_SIG  "xflysim.xconnect"
#define XCONNECT_DESC "XFlySim X-Plane bridge: flight data + CSL rendering"
#define LISTEN_PORT   51001
#define PLAYER_MAX    256
#define PLAYER_TIMEOUT 30.0   /* 秒：玩家超时清理 */
#define DATA_INTERVAL 0.2f   /* 发送间隔：0.2 秒 = 每秒 5 条（5Hz） */
#define LOOP_INTERVAL 0.05f  /* 主循环节拍（有客户端时） */
#define LOOP_IDLE_INTERVAL 0.5f  /* 空闲降频节拍（无客户端时） */

/* ---------------- 锁与时钟 ---------------- */

extern void* g_players_lock;   /* 玩家表锁（网络线程 / 主循环共用） */
extern void* g_sock_lock;      /* 客户端 socket 锁 */

void lock_init(void);
void lock_done(void);

#ifdef _WIN32
#include <windows.h>
#define LOCK_TYPE CRITICAL_SECTION
#define LOCK_INIT(l) InitializeCriticalSection((CRITICAL_SECTION*)(l))
#define LOCK_DONE(l) DeleteCriticalSection((CRITICAL_SECTION*)(l))
#define LOCK_ENTER(l) EnterCriticalSection((CRITICAL_SECTION*)(l))
#define LOCK_LEAVE(l) LeaveCriticalSection((CRITICAL_SECTION*)(l))

/* 单调时钟（秒）。网络线程与主循环共用，避免在非主线程调用 XPLM API。 */
static double xc_now_sec(void) {
    return (double)GetTickCount64() / 1000.0;
}

#define THREAD_RET DWORD WINAPI
#define THREAD_ARG LPVOID

#else
#include <pthread.h>
#include <time.h>
#define LOCK_TYPE pthread_mutex_t
#define LOCK_INIT(l) pthread_mutex_init((pthread_mutex_t*)(l), NULL)
#define LOCK_DONE(l) pthread_mutex_destroy((pthread_mutex_t*)(l))
#define LOCK_ENTER(l) pthread_mutex_lock((pthread_mutex_t*)(l))
#define LOCK_LEAVE(l) pthread_mutex_unlock((pthread_mutex_t*)(l))

static double xc_now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

#define THREAD_RET void*
#define THREAD_ARG void*

#endif

/* ---------------- 诊断日志 ---------------- */

/* 写诊断日志（插件目录 xconnect.log）。路径由 XPluginStart 初始化。 */
void xconnect_logf(const char* fmt, ...);

#endif /* XCONNECT_H */