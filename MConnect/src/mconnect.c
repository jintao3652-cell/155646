/* MConnect — 入口、SimConnect 派发循环、网络线程、客户端协议解析。
 *
 * 线程模型（关键约束：SimConnect 客户端**不是线程安全的**，官方文档明确
 * 指出 "SimConnect clients are not currently thread-safe"）：
 *
 *   主线程（sim 线程）
 *     - 只在这里调用任何 SimConnect_* API（CallDispatch / AICreate / SetData）
 *     - 每循环：CallDispatch → update_players（建/驱动 AI）→ 5Hz 发 flight_data
 *
 *   网络线程（net 线程）
 *     - accept / recv 行、解析 JSON 写玩家表（持 g_players_lock）
 *     - **不碰 SimConnect**：AI 的创建与位置写入全部留给主线程做
 *
 * 这样分工和 XConnect 一致（XConnect 里网络线程也不碰 XPLM），
 * 只是把"渲染"换成了"驱动 AI 对象"。
 */
#include "mconnect.h"
#include "xstr.h"
#include "sc.h"
#include "datadefs.h"
#include "catalog.h"
#include "players.h"
#include "control.h"
#include "inputev.h"
#include "wasmbridge.h"
#include "aircraftcaps.h"
#include "aircraftdb.h"
#include "pmdgbridge.h"
#include "net.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------- 全局 ---------------- */

void* g_players_lock = NULL;
void* g_sock_lock = NULL;

static CRITICAL_SECTION g_lock_players;
static CRITICAL_SECTION g_lock_sock;

static char g_log_path[MAX_PATH] = "";
static char g_exe_dir[MAX_PATH] = "";
static net_sock_t g_client_sock = NET_INVALID;
static net_sock_t g_listen_sock = NET_INVALID;
static int  g_listen_port = 0;
static volatile LONG g_running = 1;
static HANDLE g_net_thread = NULL;             /* 退出时等待其结束再销毁锁 */
static volatile LONG g_clear_players_req = 0;  /* 网络线程请求主线程清空 AI */
int g_verbose_flag = 0;   /* 非 static：control.c 也要读 */
static double g_last_send = -1.0;
static double g_last_sysstate = -1.0;
static double g_last_loop_log = -1.0;
static double g_last_csl_status = -1.0;
static char g_title_seen[256] = "";     /* 上一帧的机模 TITLE（换机模时重枚举） */
static char g_csl_loaded[PLAYER_MAX][8];
static char g_csl_missing[PLAYER_MAX][8];
static int  g_csl_loaded_n = 0;
static int  g_csl_missing_n = 0;

/* ---------------- 基础工具 ---------------- */

double xc_now_sec(void) {
    static LARGE_INTEGER freq;
    static int init = 0;
    LARGE_INTEGER now;
    if (!init) {
        QueryPerformanceFrequency(&freq);
        init = 1;
    }
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

const char* xc_exe_dir(void) { return g_exe_dir; }

static void log_write(const char* fmt, va_list ap) {
    FILE* f;
    char ts[48];
    time_t t;
    struct tm tmv;

    if (!g_log_path[0]) return;
    f = fopen(g_log_path, "a");
    if (!f) return;
    t = time(NULL);
    localtime_s(&tmv, &t);
    strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
    fprintf(f, "%s ", ts);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    fclose(f);
}

void xc_logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_write(fmt, ap);
    va_end(ap);
}

void xc_infof(const char* fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    log_write(fmt, ap);
    va_end(ap);

    va_start(ap2, fmt);
    vprintf(fmt, ap2);
    va_end(ap2);
    putchar('\n');
    fflush(stdout);
}

/* ---------------- 客户端协议解析（net 线程） ---------------- */

/* 数：JSON_NUM 或 JSON_BOOL 都接受（不同客户端可能发 true/1） */
static int jnum(const Json* obj, const char* key, double* out) {
    const Json* t = json_get(obj, key);
    if (!t) return 0;
    if (t->type == JSON_NUM) { *out = t->num; return 1; }
    if (t->type == JSON_BOOL) { *out = t->bval ? 1.0 : 0.0; return 1; }
    return 0;
}

static void jstr(const Json* obj, const char* key, char* dst, int cap) {
    const Json* t = json_get(obj, key);
    if (dst && cap > 0) dst[0] = '\0';
    if (!dst || cap < 1) return;
    if (t && t->type == JSON_STR)
        snprintf(dst, (size_t)cap, "%s", t->sval);
}

/* 解析客户端下发的 canonical parts（与 XConnect 完全同一 schema）。
 * 只在网络线程调用，持 g_players_lock；不碰 SimConnect。 */
static void parse_player_parts(const Json* el, Player* p) {
    const Json* parts = json_get(el, "parts");
    const Json* lights;
    const Json* engines;
    const Json* e;
    double v;
    unsigned mask = 0;
    unsigned emask = 0;

    if (!parts || parts->type != JSON_OBJ) return;
    p->has_parts = 1;

    lights = json_get(parts, "lights");
    if (lights && lights->type == JSON_OBJ) {
        if (jnum(lights, "beacon_on", &v) && v != 0.0) mask |= 1u;
        if (jnum(lights, "landing_on", &v) && v != 0.0) mask |= 2u;
        if (jnum(lights, "taxi_on", &v) && v != 0.0) mask |= 4u;
        if (jnum(lights, "nav_on", &v) && v != 0.0) mask |= 8u;
        if (jnum(lights, "strobe_on", &v) && v != 0.0) mask |= 16u;
        if (jnum(lights, "logo_on", &v) && v != 0.0) mask |= 32u;
        p->lights = mask;
    }
    if (jnum(parts, "gear_down", &v)) {
        p->gear_down = v != 0.0;
        p->has_gear = 1;
    }
    if (jnum(parts, "flaps_pct", &v)) {
        int fp = (int)(v + 0.5);
        if (fp < 0) fp = 0;
        if (fp > 100) fp = 100;
        p->flaps_pct = fp;
    }
    if (jnum(parts, "spoilers_out", &v)) p->spoilers_out = v != 0.0;
    /* 远端油门（ACC throttle_ratio 0..1）：写 AI 引擎油门杆 */
    if (jnum(parts, "throttle_ratio", &v)) {
        if (v < 0.0) v = 0.0;
        if (v > 1.0) v = 1.0;
        p->throttle = v;
        p->has_throttle = 1;
    }

    engines = json_get(parts, "engines");
    if (engines && engines->type == JSON_OBJ) {
        for (e = engines->child; e; e = e->next) {
            int idx;
            if (!e->key || e->type != JSON_OBJ) continue;
            idx = atoi(e->key);
            if (idx >= 1 && idx <= 8 &&
                jnum(e, "on", &v) && v != 0.0)
                emask |= 1u << (idx - 1);
        }
        p->engines_on = emask;
    }
    if (jnum(parts, "engine_count", &v)) {
        int n = (int)(v + 0.5);
        p->engine_count = (n >= 0 && n <= 8) ? n : 0;
    }
    jstr(parts, "engine_type", p->engine_type, sizeof(p->engine_type));
    jstr(parts, "gear_type", p->gear_type, sizeof(p->gear_type));
    jstr(parts, "parts_source", p->parts_source, sizeof(p->parts_source));

    if (g_verbose_flag)
        xc_logf("parts: %s src=%s gear=%d(has=%d) flaps=%d eng=%d/%d(%s) lights=%u",
                p->callsign, p->parts_source[0] ? p->parts_source : "-",
                p->gear_down, p->has_gear, p->flaps_pct,
                (int)p->engines_on, p->engine_count,
                p->engine_type[0] ? p->engine_type : "?", p->lights);
}

static void handle_player_list(const Json* root) {
    const Json* data = json_get(root, "data");
    int i, n, updated = 0;
    double now = xc_now_sec();

    if (!data || data->type != JSON_ARR) return;
    n = json_len(data);

    LOCK_ENTER(g_players_lock);
    for (i = 0; i < n; i++) {
        const Json* el = json_at(data, i);
        char callsign[24];
        Player* p = NULL;
        int slot;
        double v;
        double lat = 0, lon = 0, alt_ft = 0, pitch = 0, bank = 0, hdg = 0, gs = 0;
        double agl_ft = -1.0;
        int on_ground = 0;

        if (!el) continue;
        jstr(el, "callsign", callsign, sizeof(callsign));
        if (!callsign[0]) continue;

        /* 自己也上报？跳过——否则会在自己飞机上再叠一架 AI 飞机。
         * 客户端已经过滤过（roster 里不含本机），这里再防一层。 */
        if (g_flight.atc_id[0] && _stricmp(callsign, g_flight.atc_id) == 0)
            continue;

        for (slot = 0; slot < PLAYER_MAX; slot++) {
            if (g_players[slot].active &&
                _stricmp(g_players[slot].callsign, callsign) == 0) {
                p = &g_players[slot];
                break;
            }
        }
        if (!p) {
            for (slot = 0; slot < PLAYER_MAX; slot++) {
                if (!g_players[slot].active) {
                    p = &g_players[slot];
                    memset(p, 0, sizeof(*p));
                    p->active = 1;
                    p->state = AI_STATE_IDLE;
                    snprintf(p->callsign, sizeof(p->callsign), "%s", callsign);
                    break;
                }
            }
        }
        if (!p) continue;

        jstr(el, "aircraft", p->icao, sizeof(p->icao));
        jstr(el, "airline", p->airline, sizeof(p->airline));
        jstr(el, "registration", p->reg, sizeof(p->reg));
        jstr(el, "aircraft_family", p->family, sizeof(p->family));

        jnum(el, "latitude", &lat);
        jnum(el, "longitude", &lon);
        jnum(el, "altitude", &alt_ft);
        jnum(el, "pitch", &pitch);
        jnum(el, "bank", &bank);
        jnum(el, "heading", &hdg);
        jnum(el, "ground_speed", &gs);
        if (jnum(el, "altitude_agl", &v) && v >= 0.0) agl_ft = v;
        if (jnum(el, "on_ground", &v)) on_ground = v != 0.0;
        parse_player_parts(el, p);

        /* 机型串清洗：他机可能发 "B738/W"、"b738 " 这类脏值 */
        if (p->icao[0]) {
            char clean[16];
            catalog_sanitize_icao(p->icao, clean, sizeof(clean));
            snprintf(p->icao, sizeof(p->icao), "%s", clean);
        }

        p->last_seen = now;
        push_fix(p, lat, lon, alt_ft * 0.3048, hdg, pitch, bank, gs,
                 agl_ft * 0.3048, on_ground, now);
        updated++;
    }
    LOCK_LEAVE(g_players_lock);

    if (updated && g_verbose_flag)
        xc_logf("net: players updated n=%d", updated);
}

static void handle_client_line(const char* line) {
    Json* root;
    const Json* jt;

    root = json_parse(line);
    if (!root) return;

    /* 反向控制（Send:RADIO / Send:XPDR）：只入队，实际写操作留给主线程
     * —— SimConnect 非线程安全，网络线程绝不能直接调 TransmitClientEvent。 */
    if (control_handle_message(root, NULL)) {
        json_free(root);
        return;
    }

    jt = json_get(root, "type");
    if (jt && jt->type == JSON_STR) {
        if (strcmp(jt->sval, "Send:OPD:FSD9") == 0) {
            handle_player_list(root);
        } else if (strcmp(jt->sval, "Send:ATC") == 0) {
            /* 管制列表：MSFS 侧没有可渲染的对应物（不能注入塔台模型），
             * 仅记录数量，协议保持兼容。 */
            const Json* data = json_get(root, "data");
            if (data && data->type == JSON_ARR && g_verbose_flag)
                xc_logf("net: ATC list n=%d (ignored on MSFS)", json_len(data));
        } else if (strcmp(jt->sval, "hello") == 0 ||
                   strcmp(jt->sval, "Hello") == 0) {
            /* 桥协议 v1 握手应答（connected 已带同样信息） */
            LOCK_ENTER(g_sock_lock);
            if (g_client_sock != NET_INVALID) {
                net_send_line(g_client_sock,
                    "{\"type\":\"hello_ack\",\"protocol\":1,"
                    "\"plugin\":\"MConnect\",\"version\":\"1.0.0\","
                    "\"capabilities\":[\"csl\",\"parts\",\"controls\","
                    "\"wasm\",\"diagnostics\"]}");
            }
            LOCK_LEAVE(g_sock_lock);
        }
    }
    json_free(root);
}

/* ---------------- 网络线程 ---------------- */

static THREAD_RET net_thread(THREAD_ARG arg) {
    (void)arg;
    while (g_running) {
        net_sock_t sock = net_accept(g_listen_sock);
        if (sock == NET_INVALID) break;

        LOCK_ENTER(g_sock_lock);
        if (g_client_sock != NET_INVALID) net_close(g_client_sock);
        g_client_sock = sock;
        LOCK_LEAVE(g_sock_lock);
        xc_infof("[mconnect] 客户端已连接");

        net_send_line(sock,
            "{\"type\":\"connected\",\"protocol\":1,"
            "\"plugin\":\"MConnect\",\"version\":\"1.0.0\","
            "\"sim\":\"msfs\",\"capabilities\":[\"csl\",\"parts\","
            "\"controls\",\"wasm\",\"diagnostics\"]}");

        for (;;) {
            static char line[256 * 1024];   /* 数百玩家的单行 JSON */
            int n = net_read_line(sock, line, sizeof(line));
            if (n == -2) continue;
            if (n <= 0) break;
            handle_client_line(line);
        }

        LOCK_ENTER(g_sock_lock);
        if (g_client_sock == sock) g_client_sock = NET_INVALID;
        LOCK_LEAVE(g_sock_lock);
        net_close(sock);
        xc_infof("[mconnect] 客户端已断开");

        /* 客户端走人 → 请求主线程清掉所有 AI 对象。
         * 不能在网络线程直接调用：ai_remove 会调 SimConnect，而 SimConnect
         * 客户端不是线程安全的（见文件头约束）。 */
        InterlockedExchange(&g_clear_players_req, 1);
        g_csl_loaded_n = 0;
        g_csl_missing_n = 0;
    }
    return 0;
}

/* ---------------- 状态上报 ---------------- */

/* csl_status：协议沿用 XConnect 的名字（客户端按 type 处理），
 * 语义改为"已成功注入的机型列表" / "机型目录里找不到的机型"。 */
static void refresh_status(void) {
    char loaded[PLAYER_MAX][8];
    char missing[PLAYER_MAX][8];
    int ln = 0, mn = 0, i, ac = 0, pd = 0, changed = 0;
    char line[4096];
    int n;

    memset(loaded, 0, sizeof(loaded));
    memset(missing, 0, sizeof(missing));

    /* 机型目录里没有的：遍历玩家，标出 fallback 到 ZZZZ 的机型 */
    {
        int total_players = 0;
        LOCK_ENTER(g_players_lock);
        for (i = 0; i < PLAYER_MAX; i++) {
            Player* p = &g_players[i];
            if (!p->active || !p->icao[0]) continue;
            total_players++;
        }
        LOCK_LEAVE(g_players_lock);
        (void)total_players;
    }

    players_stats(&ac, &pd, &ln, loaded, PLAYER_MAX);

    LOCK_ENTER(g_players_lock);
    for (i = 0; i < PLAYER_MAX; i++) {
        Player* p = &g_players[i];
        if (!p->active || !p->icao[0]) continue;
        if (p->state == AI_STATE_FAILED) {
            int k, dup = 0;
            for (k = 0; k < mn; k++)
                if (_stricmp(missing[k], p->icao) == 0) { dup = 1; break; }
            if (!dup && mn < PLAYER_MAX)
                snprintf(missing[mn++], 8, "%s", p->icao);
        }
    }
    LOCK_LEAVE(g_players_lock);

    if (ln != g_csl_loaded_n || mn != g_csl_missing_n) changed = 1;
    for (i = 0; i < ln && !changed; i++)
        if (strcmp(loaded[i], g_csl_loaded[i]) != 0) changed = 1;
    for (i = 0; i < mn && !changed; i++)
        if (strcmp(missing[i], g_csl_missing[i]) != 0) changed = 1;
    if (!changed) return;

    memcpy(g_csl_loaded, loaded, sizeof(loaded[0]) * (size_t)ln);
    memcpy(g_csl_missing, missing, sizeof(missing[0]) * (size_t)mn);
    g_csl_loaded_n = ln;
    g_csl_missing_n = mn;

    /* 用 XSTR_APPEND 而非 `n += snprintf(...)`：截断时 snprintf 返回
     * "本应写入的长度"，大于实际剩余空间 → n 越过 sizeof(line) →
     * 下一轮 `sizeof(line) - n` 溢出成巨大 size_t → 越界写。
     * 与 XConnect 的 refresh_csl_status 是同一份逻辑、同一个坑。 */
    n = snprintf(line, sizeof(line),
                 "{\"type\":\"csl_status\",\"sim\":\"msfs\","
                 "\"active\":%d,\"pending\":%d,\"loaded\":[", ac, pd);
    if (n < 0) n = 0;
    if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
    for (i = 0; i < ln; i++)
        XSTR_APPEND(line, n, "%s\"%s\"", i ? "," : "", loaded[i]);
    XSTR_APPEND(line, n, "],\"missing\":[");
    for (i = 0; i < mn; i++)
        XSTR_APPEND(line, n, "%s\"%s\"", i ? "," : "", missing[i]);
    XSTR_APPEND(line, n, "]}");

    LOCK_ENTER(g_sock_lock);
    if (g_client_sock != NET_INVALID) net_send_line(g_client_sock, line);
    LOCK_LEAVE(g_sock_lock);
}

/* ---------------- SimConnect 派发回调 ---------------- */

static void CALLBACK dispatch_proc(SC_RECV* pData, DWORD cbData, void* ctx) {
    (void)cbData;
    (void)ctx;
    if (!pData) return;

    switch (pData->dwID) {
    case SC_RECV_ID_OPEN:
        sc_on_open_info((const SC_RECV_OPEN*)pData);
        break;

    case SC_RECV_ID_SIMOBJECT_DATA:
        datadefs_on_simobject_data(pData, cbData);
        break;

    case SC_RECV_ID_ASSIGNED_OBJECT_ID: {
        const SC_RECV_ASSIGNED_OBJECT_ID* a =
            (const SC_RECV_ASSIGNED_OBJECT_ID*)pData;
        players_on_assigned_object(a->dwRequestID, a->dwObjectID);
        break;
    }

    case SC_RECV_ID_SYSTEM_STATE:
        datadefs_on_system_state(pData);
        break;

    case SC_RECV_ID_EXCEPTION: {
        const SC_RECV_EXCEPTION* e = (const SC_RECV_EXCEPTION*)pData;
        /* 常见有意义的几个：SIZE_MISMATCH(2) / UNRECOGNIZED_ID(3) /
         * CREATE_OBJECT_FAILED(0x16) / NAME_UNRECOGNIZED(7) */
        static const char* kExcNames[] = {
            "NONE", "ERROR", "SIZE_MISMATCH", "UNRECOGNIZED_ID", "UNOPENED",
            "VERSION_MISMATCH", "TOO_MANY_GROUPS", "NAME_UNRECOGNIZED",
            "INDEX_OUT_OF_BOUNDS", "STRING_MISMATCH", "GROUP_NEVER_DEFINED",
            "GROUP_NOT_CREATABLE", "SYSTEM_NEVER_DEFINED", "SYSTEM_NOT_SUBSCRIBED",
            "SYSTEM_ALREADY_SUBSCRIBED", "DATA_AREA_INVALID",
            "DATA_AREA_NOT_AVAILABLE", "DUPLICATE_ID", "DUPLICATE_REQUEST",
            "INTERNAL_ERROR", "OBJECT_TYPE_MISMATCH",
            "OPERATION_INVALID_FOR_OBJECT_TYPE", "TOO_MANY_REQUESTS",
            "EXTERNAL_EVENT_CREATION_FAILED", "EXTERNAL_EVENT_SIGNAL_FAILED",
            "ALREADY_SUBSCRIBED", "ALREADY_CREATED", "UNKNOWN_EVENT_ID",
            "UNKNOWN_CLIENT_EVENT_ID", "CLIENT_DATA_NOT_FOUND",
            "CLIENT_DATA_DUPLICATE_DEFINE_ID", "CLIENT_DATA_INVALID_SIZE",
            "CLIENT_DATA_INVALID_ACCESS"
        };
        if (e->dwException < sizeof(kExcNames) / sizeof(kExcNames[0]))
            xc_logf("sc: EXCEPTION 0x%02lx(%s) sendID=%lu index=%lu",
                    (unsigned long)e->dwException,
                    kExcNames[e->dwException],
                    (unsigned long)e->dwSendID, (unsigned long)e->dwIndex);
        else
            xc_logf("sc: EXCEPTION 0x%02lx sendID=%lu index=%lu",
                    (unsigned long)e->dwException,
                    (unsigned long)e->dwSendID, (unsigned long)e->dwIndex);
        break;
    }

    case SC_RECV_ID_QUIT:
        xc_infof("[mconnect] 模拟器已退出");
        InterlockedExchange(&g_running, 0);
        break;

    case SC_RECV_ID_ENUMERATE_INPUT_EVENTS:
        inputev_on_enumerate(pData, cbData);
        break;

    case SC_RECV_ID_CLIENT_DATA:
        /* WASM 桥：响应字符串与 LVar 数组都从这里来 */
        wasmbridge_on_client_data(pData, cbData);
        /* PMDG SDK 桥：二进制数据广播（同一消息类型，按 DefineID 区分） */
        pmdgbridge_on_client_data(pData, cbData);
        break;

    case SC_RECV_ID_ENUMERATE_SIMOBJECT_AND_LIVERY_LIST: {
        const SC_RECV_ENUM_SIMOBJECT_LIVERY_LIST* l =
            (const SC_RECV_ENUM_SIMOBJECT_LIVERY_LIST*)pData;
        DWORD need = (DWORD)(sizeof(SC_RECV_ENUM_SIMOBJECT_LIVERY_LIST) -
                             sizeof(SC_ENUM_SIMOBJECT_LIVERY));
        DWORD i;
        if (cbData < need) break;
        for (i = 0; i < l->dwArraySize; i++) {
            const SC_ENUM_SIMOBJECT_LIVERY* it = &l->rgData[i];
            if (cbData < need + (i + 1) * sizeof(SC_ENUM_SIMOBJECT_LIVERY)) break;
            catalog_add_enumerated(it->AircraftTitle, it->LiveryName);
        }
        if (g_verbose_flag)
            xc_logf("sc: enumerate livery page %lu/%lu n=%lu",
                    (unsigned long)l->dwEntryNumber, (unsigned long)l->dwOutOf,
                    (unsigned long)l->dwArraySize);
        break;
    }

    default:
        break;   /* 2024 新增的其它消息类型：忽略 */
    }
}

/* ---------------- 主循环 ---------------- */

static void sim_loop(void) {
    while (g_running) {
        double now;
        int has_client;

        /* 1) 派发：把这一帧模拟器发来的所有消息处理掉 */
        g_sc.CallDispatch(g_sc_handle, dispatch_proc, NULL);

        now = xc_now_sec();

        /* 1b) 外部机型档案热加载（每 2s 检查 profiles.json） */
        aircraftdb_tick();

        /* 2) 驱动 AI 对象（建/换/写位置）—— 必须在本线程 */
        update_players(now);
        /* 网络线程请求的"客户端断开 → 清空 AI"也放到本线程执行 */
        if (InterlockedExchange(&g_clear_players_req, 0))
            players_remove_all();

        /* 2b) 执行客户端的反向控制命令（应答机/频率）。
         * 网络线程只入队；真正 TransmitClientEvent 必须在本线程。 */
        control_drain();

        /* 2c) WASM 桥状态机 + 命令发送（ClientData 只能在主线程碰） */
        wasmbridge_poll(now);
        /* PMDG SDK 桥状态机（超时判定：没开 EnableDataBroadcast 会停在这里） */
        pmdgbridge_poll(now);

        /* 2d) 换机模后重新枚举 Input Events。
         * InputEvent 表是**每个机模独有**的，换机模（或首次进座舱）后
         * 旧表全部失效，不重枚举的话深度机模写入会一直失败。 */
        if (g_flight.title[0] && strcmp(g_flight.title, g_title_seen) != 0) {
            snprintf(g_title_seen, sizeof(g_title_seen), "%s", g_flight.title);
            xc_logf("inputev: aircraft changed -> \"%s\", re-enumerate",
                    g_title_seen);
            /* 换机模：旧机型的能力表全部作废，等新机模枚举回来再报 */
            aircaps_on_aircraft_change(g_title_seen);
            /* 解析该机型用哪套写法（FBW 写 LVar / PMDG 旋钮 / 默认机走标准事件） */
            control_set_aircraft(g_title_seen, datadefs_icao());
            /* PMDG 机型才初始化 SDK 桥（通道名是 PMDG 专属的，
             * 非 PMDG 机模上请求会失败，没必要制造日志噪音）。
             * 737 和 777 的通道名/ID/偏移/事件号**全都不同**，
             * 必须按档案 id 区分（见 pmdgbridge.h 的协议对照表）。 */
            if (aircraftdb_current() &&
                strncmp(aircraftdb_current()->id, "pmdg-", 5) == 0) {
                PmdgType pt = PMDG_TYPE_NONE;
                if (strncmp(aircraftdb_current()->id, "pmdg-777", 8) == 0)
                    pt = PMDG_TYPE_777;
                else if (strncmp(aircraftdb_current()->id, "pmdg-737", 8) == 0)
                    pt = PMDG_TYPE_737;
                pmdgbridge_shutdown();
                if (pt != PMDG_TYPE_NONE) pmdgbridge_init(pt);
            }
            inputev_request_enum();
        }

        /* 2e) 机型能力变化时主动推给客户端（客户端据此渲染可选项） */
        if (aircaps_tick()) {
            char caps[4096];
            LOCK_ENTER(g_sock_lock);
            if (g_client_sock != NET_INVALID &&
                aircaps_build_json(caps, sizeof(caps)) > 0) {
                net_send_line(g_client_sock, caps);
                if (g_verbose_flag)
                    xc_logf("net: aircraft_caps pushed (%d bytes)",
                            (int)strlen(caps));
            }
            LOCK_LEAVE(g_sock_lock);
        }

        /* 3) 5Hz 发本机飞行数据 */
        LOCK_ENTER(g_sock_lock);
        has_client = (g_client_sock != NET_INVALID);
        LOCK_LEAVE(g_sock_lock);

        if (has_client && now - g_last_send >= DATA_INTERVAL) {
            char line[4096];
            int rc;
            datadefs_build_flight_json(line, sizeof(line));
            LOCK_ENTER(g_sock_lock);
            rc = (g_client_sock != NET_INVALID)
                 ? net_send_line(g_client_sock, line) : 1;
            LOCK_LEAVE(g_sock_lock);
            if (rc != 0) {
                /* 发送失败通常是客户端断开，net 线程会回收 socket */
                xc_logf("send flight_data failed");
            }
            g_last_send = now;
        }

        /* 4) 状态上报（变化时） */
        if (has_client && now - g_last_csl_status >= 1.0) {
            g_last_csl_status = now;
            refresh_status();
        }

        /* 5) 系统状态轮询（1Hz）：模拟器是否在飞行中 */
        if (now - g_last_sysstate >= 1.0 && g_sc.RequestSystemState) {
            g_last_sysstate = now;
            g_sc.RequestSystemState(g_sc_handle, REQ_SYSSTATE, "Sim");
        }

        /* 6) 心跳日志（10s），便于确认线程活着 */
        if (now - g_last_loop_log >= 10.0) {
            int ac = 0, pd = 0;
            g_last_loop_log = now;
            players_stats(&ac, &pd, NULL, NULL, 0);
            xc_logf("loop alive: ai_active=%d pending=%d client=%d catalog=%d",
                    ac, pd, has_client, catalog_count());
        }

        Sleep(10);   /* 100Hz 上限；实际由 SimConnect 数据到达节奏决定 */
    }
}

/* ---------------- 入口 ---------------- */

static void init_paths(void) {
    char* p;
    GetModuleFileNameA(NULL, g_exe_dir, sizeof(g_exe_dir));
    p = strrchr(g_exe_dir, '\\');
    if (p) *p = '\0';
    snprintf(g_log_path, sizeof(g_log_path), "%s\\mconnect.log", g_exe_dir);
}

/* 选监听端口。默认 51002（避开 X-Plane XConnect 的 51001），被占用再退到
 * 51003/51004。客户端可用 mconnect_port 配置，所以换端口不会连不上——
 * 只是需要用户改配置，因此在 stdout 与日志里明确打印实际端口。 */
static int bind_listen(void) {
    static const int kPorts[] = { LISTEN_PORT, LISTEN_PORT_ALT1, LISTEN_PORT_ALT2 };
    size_t i;
    for (i = 0; i < sizeof(kPorts) / sizeof(kPorts[0]); i++) {
        g_listen_sock = net_listen(kPorts[i]);
        if (g_listen_sock != NET_INVALID) {
            g_listen_port = kPorts[i];
            return 1;
        }
        xc_logf("net: port %d busy", kPorts[i]);
    }
    return 0;
}

int main(int argc, char** argv) {
    char err[512];
    char scan_dir[MAX_PATH] = "";
    const char* arg;
    int i;
    int port_arg = 0;
    int scan_only = 0;
    int dump_input = 0;
    int dump_wasm = 0;
    int dump_profiles = 0;
    int parts_probe = 0;
    int toggle_labels = 0;

    init_paths();

#ifdef MCONNECT_GUI
    /* GUI 子系统（无控制台）：stdout 不可见。诊断类参数改从 MConnectConsole.exe
     * 运行才能看到输出；这里先写日志，避免用户以为命令没生效。 */
    {
        int k;
        int diagnostic = 0;
        for (k = 1; k < argc; k++) {
            if (_stricmp(argv[k], "--help") == 0 || _stricmp(argv[k], "-h") == 0 ||
                _stricmp(argv[k], "--dump-input") == 0 ||
                _stricmp(argv[k], "--dump-wasm") == 0 ||
                _stricmp(argv[k], "--dump-profiles") == 0 ||
                _stricmp(argv[k], "--parts-probe") == 0 ||
                _stricmp(argv[k], "--scan-only") == 0)
                diagnostic = 1;
        }
        if (diagnostic) {
            /* init_paths() 已完成，日志路径可用 */
            xc_logf("提示：本程序为无窗口模式，诊断输出请改用 "
                    "MConnectConsole.exe（同目录）运行相同参数。");
        }
    }
#else
    SetConsoleOutputCP(CP_UTF8);
#endif

    /* 参数：--port N / --scan <dir> / --simconnect <dll|dir> / --verbose */
    for (i = 1; i < argc; i++) {
        arg = argv[i];
        if (_stricmp(arg, "--port") == 0 && i + 1 < argc) {
            port_arg = atoi(argv[++i]);
        } else if (_stricmp(arg, "--scan") == 0 && i + 1 < argc) {
            snprintf(scan_dir, sizeof(scan_dir), "%s", argv[++i]);
        } else if (_stricmp(arg, "--simconnect") == 0 && i + 1 < argc) {
            SetEnvironmentVariableA("MCONNECT_SIMCONNECT", argv[++i]);
        } else if (_stricmp(arg, "--verbose") == 0 || _stricmp(arg, "-v") == 0) {
            g_verbose_flag = 1;
        } else if (_stricmp(arg, "--scan-only") == 0) {
            /* 只扫描机型目录并打印统计，不连模拟器、不监听端口。
             * 用于装机后确认"他机模型能不能被找到"，也可在 CI 里跑。 */
            scan_only = 1;
        } else if (_stricmp(arg, "--dump-input") == 0) {
            /* 连上模拟器后把当前机模的 Input Events 全部写进日志，
             * 用来查"我要按的按钮到底叫什么名字"（深度机模调试必备）。 */
            dump_input = 1;
        } else if (_stricmp(arg, "--dump-wasm") == 0) {
            /* 检查 WASM 桥是否可用（是否装了 MobiFlight WASM 模块）。 */
            dump_wasm = 1;
        } else if (_stricmp(arg, "--dump-profiles") == 0) {
            /* 打印机型档案表（不连模拟器）。用于确认某机型被哪个档案命中、
             * 该档案支持哪些操作，以及各机模的已知限制。 */
            dump_profiles = 1;
        } else if (_stricmp(arg, "--parts-probe") == 0) {
            /* 真机能力探针：注入一架带完整 canonical parts 的临时 AI 飞机，
             * 观察 SimVar 写入与灯光事件在当前 MSFS 版本是否被接受，打印结论后退出。 */
            parts_probe = 1;
        } else if (_stricmp(arg, "--labels") == 0) {
            /* 连上后翻转一次 MSFS 内置"机模标签"（显示的是 AI 的 ATC ID，
             * 本插件把它写成远端呼号）。toggle 语义：标签已开启时会被关闭。 */
            toggle_labels = 1;
        } else if (_stricmp(arg, "--help") == 0 || _stricmp(arg, "-h") == 0) {
            printf("MConnect %s — XFlySim MSFS 数据桥接 + AI 交通渲染\n\n"
                   "  MConnect.exe [选项]\n\n"
                   "  --port <n>         TCP 监听端口（默认 51002，占用时自动换 51003/51004）\n"
                   "  --scan <dir>       只用指定目录作为机型目录（默认自动探测 Community/Official）\n"
                   "  --scan-only        只扫描机型目录并打印统计，不连模拟器（排查机型匹配用）\n"
                   "  --dump-input       把当前机模的 Input Events 表写入日志（调试深度机模用）\n"
                   "  --dump-wasm        检查 WASM 桥（MobiFlight 模块）是否可用\n"
                   "  --dump-profiles    打印机型档案表（各机模的实测写法与限制）\n"
                   "  --parts-probe      真机探针：注入临时 AI 飞机，检测部件 SimVar/灯光事件是否可写\n"
                   "  --labels           翻转 MSFS 内置机模标签（标签显示 AI 呼号，toggle 语义）\n"
                   "  --simconnect <p>   指定 SimConnect.dll 路径（或所在目录）\n"
                   "  --verbose, -v      详细日志（打印每批玩家/枚举分页）\n"
                   "  --help, -h         显示本帮助\n",
                   MCONNECT_VER);
            return 0;
        }
    }
    (void)port_arg;   /* 端口目前固定序列，保留参数位以便将来支持 */

    xc_infof("MConnect %s (%s %s)", MCONNECT_VER, __DATE__, __TIME__);
    xc_infof("  日志: %s", g_log_path);
    xc_logf("==== MConnect start ====");
    xc_logf("exe dir: %s", g_exe_dir);
    /* 外部机型档案：MConnect.exe 同目录 profiles.json（热加载） */
    aircraftdb_init();

    /* 1. 锁 */
    InitializeCriticalSection(&g_lock_players);
    InitializeCriticalSection(&g_lock_sock);
    g_players_lock = &g_lock_players;
    g_sock_lock = &g_lock_sock;

    /* 2. 网络 */
    if (net_init() != 0) {
        xc_infof("[x] 网络初始化失败");
        return 1;
    }

    /* --dump-profiles：打印机型档案表，无需模拟器 */
    if (dump_profiles) {
        aircraftdb_dump();
        printf("机型档案: %d 个（详见日志 %s）\n", aircraftdb_count(), g_log_path);
        return 0;
    }

    /* --scan-only：只扫描机型目录，无需模拟器与端口 */
    if (scan_only) {
        CatalogMatch sample;
        int waited = 0;

        catalog_start_scan(scan_dir[0] ? scan_dir : NULL);
        while (!catalog_ready() && waited < 60000) {
            Sleep(100);
            waited += 100;
        }
        catalog_dump_stats();
        printf("机型目录条目数: %d（内置兜底表 %d 条）\n",
               catalog_count(), catalog_builtin_count());

        /* 抽样验证：用几个常见机型看能否匹配到可创建的标题 */
        {
            static const char* kSamples[] = {
                "B738", "A20N", "A320", "B77W", "A359", "E190", "C172", "XXXX"
            };
            size_t k;
            printf("\n抽样匹配（机型 / 航司 / 尾号 → 容器标题 + 涂装）：\n");
            for (k = 0; k < sizeof(kSamples) / sizeof(kSamples[0]); k++) {
                catalog_match(kSamples[k], "CCA", "", &sample);
                if (sample.found)
                    printf("  %-6s -> level=%d  \"%s\"  livery=\"%s\"%s\n",
                           kSamples[k], sample.level, sample.title,
                           sample.livery, sample.modular ? "  [modular]" : "");
                else
                    printf("  %-6s -> 未匹配\n", kSamples[k]);
            }
        }
        catalog_stop(2000);
        return 0;
    }

    if (!bind_listen()) {
        xc_infof("[x] 端口 %d/%d/%d 全部被占用，无法监听",
                 LISTEN_PORT, LISTEN_PORT_ALT1, LISTEN_PORT_ALT2);
        return 1;
    }
    xc_infof("  监听: 127.0.0.1:%d", g_listen_port);
    xc_logf("net: listening on %d", g_listen_port);

    /* 3. SimConnect 装载与连接 */
    if (sc_load(err, sizeof(err)) != 0) {
        xc_infof("[x] %s", err);
        xc_infof("    请确认已安装 Microsoft Flight Simulator 2020 或 2024。");
        return 1;
    }
    if (sc_open(dispatch_proc, NULL) != 0) {
        xc_infof("[x] 无法连接模拟器（SimConnect_Open 失败）");
        xc_infof("    请先启动 MSFS 并进入主菜单或飞行，再运行 MConnect。");
        return 1;
    }
    xc_infof("  模拟器: MSFS %s（模块化机型支持: %s）",
             sc_sim_name(), g_sc.supports_modular ? "是" : "否");

    /* 4. 数据定义 */
    if (datadefs_init() != 0) {
        xc_infof("[x] SimConnect 数据定义创建失败（详见日志）");
        return 1;
    }
    if (datadefs_register_ai() != 0) {
        xc_infof("[!] AI 位置定义创建失败，他机将无法显示");
    }
    if (datadefs_register_xpdr() != 0) {
        xc_infof("[!] 应答机模式写入不可用（频率/码仍可设置）");
    }
    if (datadefs_register_ai_parts() != 0) {
        xc_infof("[!] AI 部件写入不可用（位置/其余功能正常）");
    }
    if (datadefs_register_aux() != 0) {
        xc_infof("[!] 辅助遥测不可用（flight_id/收听状态/扩展灯光不上报）");
    }
    if (datadefs_register_ai_atcid() != 0) {
        xc_infof("[!] AI 呼号标签（ATC ID）不可用");
    }

    /* 4b. 反向控制事件映射（应答机/频率设置） */
    if (control_init() != 0) {
        xc_infof("[!] 反向控制事件注册不完整，部分写入可能无效（详见日志）");
    } else {
        xc_infof("  反向控制: 应答机 / COM1-3 频率 可写");
    }
    if (toggle_labels) {
        xc_infof("  --labels: 翻转内置机模标签（AI 标签显示远端呼号）");
        control_toggle_aircraft_labels();
    }

    /* 4c. 真机能力探针：用一架临时 AI 飞机把 canonical parts 完整走一遍。
     * CI 无法验证"SimVar 对 AI 对象是否真的可写"，只能在真机跑这个开关。
     * 不启动网络线程，12 秒后打印结论并退出。 */
    if (parts_probe) {
        char probe_json[1200];
        Json* probe_root;
        int waited = 0;
        int active = 0, pending = 0, loaded_n = 0;
        char loaded[8][8] = { { 0 } };
        int pw = 0, pt = 0, lo = 0, lt = 0;
        double lat = g_flight.valid ? g_flight.lat_deg : 0.0;
        double lon = g_flight.valid ? g_flight.lon_deg : 0.0;
        double alt = g_flight.valid ? g_flight.alt_msl_ft : 0.0;

        xc_infof("  parts 探针：注入临时 AI 飞机 XPROBE1，约 12 秒后输出结论");
        /* 探针提前返回，不会走到下面正常的后台扫描；这里先启动以便匹配 C172 容器标题 */
        catalog_start_scan(scan_dir[0] ? scan_dir : NULL);
        players_set_probe_mode(1);   /* 探针不等扫描：12s 窗口，用内置 C172 */
        snprintf(probe_json, sizeof(probe_json),
                 "{\"data\":[{\"callsign\":\"XPROBE1\",\"aircraft\":\"C172\","
                 "\"latitude\":%.7f,\"longitude\":%.7f,\"altitude\":%.1f,"
                 "\"heading\":0,\"ground_speed\":0,\"on_ground\":1,"
                 "\"parts\":{\"gear_down\":1,\"flaps_pct\":0,\"spoilers_out\":0,"
                 "\"on_ground\":1,\"engine_count\":1,\"engine_type\":\"P\","
                 "\"gear_type\":\"fixed\",\"parts_source\":\"fsd_parts\","
                 "\"engines\":{\"1\":{\"on\":0}},"
                 "\"lights\":{\"beacon_on\":1,\"landing_on\":1,\"taxi_on\":1,"
                 "\"nav_on\":1,\"strobe_on\":1,\"logo_on\":1}}}]}",
                 lat, lon, alt);
        probe_root = json_parse(probe_json);
        if (probe_root) {
            handle_player_list(probe_root);
            json_free(probe_root);
        } else {
            xc_infof("[x] 探针 JSON 构造失败");
        }
        while (waited < 12000) {
            g_sc.CallDispatch(g_sc_handle, dispatch_proc, NULL);
            update_players(xc_now_sec());
            Sleep(100);
            waited += 100;
        }
        players_stats(&active, &pending, &loaded_n, loaded, 8);
        players_parts_status(&pw, &pt, &lo, &lt);
        xc_infof("parts probe: ai_objects=%d pending=%d "
                 "parts_setdata=%s light_events=%s",
                 active, pending,
                 pt ? (pw ? "OK" : "FAILED") : "NOT_TRIED",
                 lt ? (lo ? "OK" : "FAILED") : "NOT_TRIED");
        if (active == 0)
            xc_infof("[!] 未创建出 AI 对象：确认已进入飞行（不是主菜单），并检查机型目录");
        players_remove_all();
        catalog_stop(500);
        sc_close();
        return 0;
    }

    /* 5. 机型目录（后台扫描，不阻塞） */
    catalog_start_scan(scan_dir[0] ? scan_dir : NULL);

    /* 6. 2024：请求机型+涂装枚举（能力探测到的才发） */
    if (g_sc.EnumerateSimObjectsAndLiveries && g_sc.supports_modular) {
        HRESULT hr = g_sc.EnumerateSimObjectsAndLiveries(
            g_sc_handle, players_next_request_id(), SC_SIMOBJECT_TYPE_AIRCRAFT);
        xc_logf("sc: EnumerateSimObjectsAndLiveries hr=0x%08lx",
                (unsigned long)hr);
    }

    /* 6b. Input Events 枚举：拿到当前机模的自定义输入事件表。
     * 这是深度机模（Fenix/PMDG）唯一可写的通道——标准 SimConnect 事件
     * 对它们无效。枚举异步分页返回，由派发回调填充。
     * 机模切换后需要重新枚举（这里在检测到 TITLE 变化时重发，见主循环）。 */
    if (inputev_request_enum() == 0) {
        xc_infof("  Input Events: 已请求枚举（深度机模写入通道）");
    } else {
        xc_infof("[!] 当前 SimConnect 不支持 Input Events（深度机模无法写入）");
    }
    if (dump_input) {
        int waited = 0;
        while (waited < 8000) {
            Sleep(500);
            waited += 500;
            g_sc.CallDispatch(g_sc_handle, dispatch_proc, NULL);
            if (inputev_count() > 0 && waited > 3000) break;
        }
        inputev_dump(400);
        xc_infof("  Input Events 已写入日志（共 %d 条）", inputev_count());
        if (inputev_count() == 0) {
            xc_infof("[!] 未收到任何 Input Event：确认已载入飞行（不是主菜单）");
        }
        sc_close();
        return 0;
    }

    /* 6c. WASM 桥（MobiFlight 等 WASM 模块）：执行任意 RPN 的通道。
     * 未装 WASM 模块时会停在 FAILED，不影响其他功能。 */
    if (wasmbridge_init() == 0) {
        xc_infof("  WASM 桥: 已请求注册（RPN 通道，需装 MobiFlight WASM 模块）");
    } else {
        xc_infof("  WASM 桥: 不可用（未装 WASM 模块，RPN 通道关闭）");
    }

    if (dump_wasm) {
        int waited = 0;
        while (waited < 8000) {
            Sleep(500);
            waited += 500;
            g_sc.CallDispatch(g_sc_handle, dispatch_proc, NULL);
            wasmbridge_poll(xc_now_sec());
            if (wasmbridge_state() == WASM_STATE_READY ||
                wasmbridge_state() == WASM_STATE_FAILED) break;
        }
        wasmbridge_dump();
        xc_infof("  WASM 桥状态: %s", wasmbridge_state_name(wasmbridge_state()));
        {
            char v[32];
            if (wasmbridge_version(v, sizeof(v)))
                xc_infof("  WASM 模块版本: %s", v);
        }
        if (wasmbridge_state() == WASM_STATE_FAILED) {
            xc_infof("[!] 未连上 WASM 模块。请安装 MobiFlight WASM Module 到 Community：");
            xc_infof("    https://github.com/MobiFlight/MobiFlight-WASM-Module");
        }
        wasmbridge_shutdown();
        sc_close();
        return 0;
    }

    /* 7. 网络线程（句柄保留到退出，清理时等待其结束） */
    g_net_thread = CreateThread(NULL, 0, net_thread, NULL, 0, NULL);
    if (!g_net_thread) {
        xc_infof("[x] 无法创建网络线程");
        return 1;
    }

    xc_infof("  就绪：等待 XFlySim-Client 连接（Ctrl+C 退出）");

    /* 8. 主循环：派发 + 驱动 AI + 发送（本线程独占 SimConnect） */
    sim_loop();

    /* 9. 清理 */
    xc_infof("[mconnect] 正在退出…");
    InterlockedExchange(&g_running, 0);
    net_shutdown_listen(g_listen_sock);
    g_listen_sock = NET_INVALID;
    /* 只 shutdown 客户端 socket 唤醒 recv，由网络线程自己关闭 */
    LOCK_ENTER(g_sock_lock);
    if (g_client_sock != NET_INVALID) net_shutdown(g_client_sock);
    LOCK_LEAVE(g_sock_lock);
    /* 等网络线程退出后再清 AI / 销毁锁，避免它访问已释放资源 */
    if (g_net_thread) {
        WaitForSingleObject(g_net_thread, 3000);
        CloseHandle(g_net_thread);
        g_net_thread = NULL;
    }
    g_client_sock = NET_INVALID;
    players_remove_all();
    catalog_stop(3000);
    sc_close();
    DeleteCriticalSection(&g_lock_players);
    DeleteCriticalSection(&g_lock_sock);
    xc_logf("==== MConnect end ====");
    return 0;
}

#ifdef MCONNECT_GUI
/* GUI 子系统（/SUBSYSTEM:WINDOWS）的 CRT 入口是 WinMain，不是 main。
 * 这里把命令行原样转给 main，业务逻辑只有一份。 */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
    (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;
    return main(__argc, __argv);
}
#endif