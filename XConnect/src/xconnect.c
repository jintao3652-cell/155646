/* XConnect — X-Plane 数据桥接 + CSL 渲染插件。
 *
 * - TCP 监听 51001，向客户端发送本机飞行数据（JSON 行，5Hz）
 * - 接收 Send:OPD:FSD9 玩家列表，用 XPLMInstance 渲染 CSL 模型
 * - 接收 Send:ATC 管制列表（预留；当前仅记录数量）
 *
 * 远程玩家位置平滑（v6，参考 Swift pilotclient 的插件层设计）：
 *   网络位置帧（含接收时间戳）入环形缓冲 → 渲染时刻 now-RENDER_DELAY
 *   在历史中插值；越过最新帧后按最近两帧短时外推，数据停滞则停住。
 *   完全基于时间轴，与帧率无关；首帧/遥传跳变直接吸附。
 *
 * 编译需 X-Plane SDK（XPLM.h / XPLMInstance.h 等）。
 */
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
#define PLAYER_TIMEOUT 30.0   /* 秒 */
#define DATA_INTERVAL 0.2f   /* 发送间隔：0.2 秒 = 每秒 5 条（5Hz） */
#define LOOP_INTERVAL 0.05f  /* 主循环节拍（有客户端时） */
#define LOOP_IDLE_INTERVAL 0.5f  /* 空闲降频节拍（无客户端时） */

/* ---------------- 本机飞行数据 dataref ---------------- */

typedef enum {
    DR_LAT, DR_LON, DR_ELEV, DR_AGL, DR_PITCH, DR_BANK, DR_TRUE_HEAD,
    DR_MAG_HEAD, DR_GS, DR_IAS, DR_VSI,
    DR_COM1, DR_COM2, DR_XPDR_CODE, DR_XPDR_MODE,
    DR_GEAR, DR_ONGROUND, DR_FLAPS, DR_THROTTLE,
    DR_AC_ICAO,   /* 当前航空器机型 ICAO（字符串） */
    DR_COUNT
} DrIndex;

static const char* kDrefNames[DR_COUNT] = {
    "sim/flightmodel/position/latitude",
    "sim/flightmodel/position/longitude",
    "sim/flightmodel/position/elevation",
    "sim/flightmodel/position/y_agl",
    "sim/flightmodel/position/true_theta",
    "sim/flightmodel/position/true_phi",
    "sim/flightmodel/position/true_psi",
    "sim/flightmodel/position/mag_psi",
    "sim/flightmodel/position/groundspeed",
    "sim/flightmodel/position/indicated_airspeed",
    "sim/flightmodel/position/vh_ind_fpm",
    "sim/cockpit2/radios/actuators/com1_frequency_hz_833",
    "sim/cockpit2/radios/actuators/com2_frequency_hz_833",
    "sim/cockpit2/transponders/transponder_code",
    "sim/cockpit2/transponders/transponder_mode",
    "sim/flightmodel/gear/deploy_ratio",
    "sim/flightmodel/failures/onground_any",
    "sim/cockpit2/flight/indicators/flaps_ratio",
    "sim/cockpit2/engine/indicators/throttle_ratio",
    "sim/cockpit2/aircraft/view/acf_icao",
};

static XPLMDataRef g_drefs[DR_COUNT];

/* ---------------- 诊断日志 ---------------- */

static char g_log_path[2048] = "";

static double xc_now_sec(void);   /* 定义见锁宏块：优先于日志工具使用 */

/* 写诊断日志（插件目录 xconnect.log）。飞行的每一步关键入口都记录，
 * 崩溃后可从最后一行定位阶段。 */
static void xconnect_logf(const char* fmt, ...) {
    FILE* f;
    char ts[32];
    va_list ap;
    if (!g_log_path[0]) return;
    f = fopen(g_log_path, "a");
    if (!f) return;
    snprintf(ts, sizeof(ts), "%.0f", xc_now_sec());
    fputs(ts, f);
    fputs(" ", f);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputs("\n", f);
    fclose(f);
}

/* ---------------- dataref 安全读取 ---------------- */
static float dref_float(XPLMDataRef r) {
    int t;
    if (!r) return 0.0f;
    t = XPLMGetDataRefTypes(r);
#if kXPLM_Version >= 430
    if (t & xplmType_Float) return XPLMGetDataf(r);
    if (t & xplmType_Int) return (float)XPLMGetDatai(r);
#else
    if (t & xplmType_Float) {
        float v = 0.0f;
        XPLMGetDataf(r, &v);
        return v;
    }
    if (t & xplmType_Int) {
        int v = 0;
        XPLMGetDatai(r, &v);
        return (float)v;
    }
#endif
    if (t & xplmType_FloatArray) {
        float v = 0.0f;
        XPLMGetDatavf(r, &v, 0, 1);
        return v;
    }
    if (t & xplmType_IntArray) {
        int v = 0;
        XPLMGetDatavi(r, &v, 0, 1);
        return (float)v;
    }
    return 0.0f;
}

static int dref_int(XPLMDataRef r) {
    int t;
    if (!r) return 0;
    t = XPLMGetDataRefTypes(r);
#if kXPLM_Version >= 430
    if (t & xplmType_Int) return XPLMGetDatai(r);
#else
    if (t & xplmType_Int) {
        int v = 0;
        XPLMGetDatai(r, &v);
        return v;
    }
#endif
    if (t & xplmType_IntArray) {
        int v = 0;
        XPLMGetDatavi(r, &v, 0, 1);
        return v;
    }
    return (int)dref_float(r);
}

/* ---------------- 位置平滑插值参数 ----------------
 *
 * 参考 swift-project/pilotclient（Swift）的四层平滑链路中的插件层设计：
 * 网络位置按时间戳进入历史缓冲，渲染时用「渲染时刻 = now - RENDER_DELAY」
 * 在历史中找包夹该时刻的相邻两帧做线性插值（航向取最短弧）。
 *
 * - RENDER_DELAY：渲染滞后，等价于 Swift 的 position time offset。取 0.4~0.6
 *   倍位置更新间隔时，既有已知帧可插值，又不产生明显"追不上"感。
 * - EXTRAP_MAX_SEC：超过最新帧的墙钟时间上限，期间按最近两帧匀速外推；
 *   超过即停住（Swift 断流后停住的行为），避免长时间失联时飞机飞走。
 *   取「RENDER_DELAY + 期望更新间隔 + 余量」≈ 5.5s。
 * - SNAP_DIST_M：新旧两帧距离超过此值判定为网络跳变/重生，直接吸附。
 *   5s 间隔下 60km 已远超任何真实地速（≈ 23700 km/h），仅网络异常触发。
 */
#define RENDER_DELAY    2.0      /* 秒：渲染滞后（渲染时刻 = now - 该值） */
#define EXTRAP_MAX_SEC  3.5      /* 秒：最新帧之后允许多久继续外推 */
#define SNAP_DIST_M     60000.0  /* 米：跳变判定阈值 */

/* 位置历史环形缓冲：fixes[fix_head] 为最新帧 */
#define FIX_MAX 6

typedef struct {
    double lat, lon;   /* 度（WGS84） */
    double alt;        /* 米（MSL） */
    double heading, pitch, roll; /* 度 */
    double gs;         /* 地速（kt，与协议一致） */
    double ts;         /* 接收时刻（xc_now_sec） */
} PositionFix;

/* ---------------- 玩家表 ---------------- */

typedef struct {
    char callsign[24];
    char icao[8];
    char family[64];
    /* 网络位置历史（最新在前）与当前渲染位置（平滑插值输出） */
    PositionFix fixes[FIX_MAX];
    int fix_count;   /* 有效帧数（≤ FIX_MAX） */
    int fix_head;    /* 最新帧下标 */
    double cur_lat, cur_lon, cur_alt;
    double cur_heading, cur_pitch, cur_roll;
    int have_first_fix;
    double last_seen;    /* xc_now_sec */
    int active;
    CslNode model;
} Player;

static Player g_players[PLAYER_MAX];
static net_sock_t g_client_sock = NET_INVALID;
static net_sock_t g_listen_sock = NET_INVALID;
static XPLMFlightLoopID g_flight_loop = NULL;
static double g_last_loop_log = -1.0;
static char g_csl_loaded[PLAYER_MAX][8];
static char g_csl_missing[PLAYER_MAX][8];
static int g_csl_loaded_n = 0;
static int g_csl_missing_n = 0;
static double g_last_send_log = -1.0;
static int g_running = 0;
static double g_last_send = -1.0;   /* xc_now_sec 绝对时间 */
static int g_atc_count = 0;

static void* g_players_lock = NULL;   /* pthread_mutex_t / CRITICAL_SECTION */
static void* g_sock_lock = NULL;

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
#endif

static void lock_init(void) {
    static LOCK_TYPE a, b;
    g_players_lock = &a;
    g_sock_lock = &b;
    LOCK_INIT(g_players_lock);
    LOCK_INIT(g_sock_lock);
}

static void lock_done(void) {
    LOCK_DONE(g_players_lock);
    LOCK_DONE(g_sock_lock);
}

/* ---------------- 平滑插值助手 ---------------- */

/* 角度归一化到 (-180, 180]，用于航向取最短弧插值（等价 Swift 的
 * interpolatorlinearpbh.cpp 中 deltaDeg 的 ±360 修正）。 */
static double angle_delta(double a, double b) {
    double d = b - a;
    while (d > 180.0) d -= 360.0;
    while (d < -180.0) d += 360.0;
    return d;
}

static double lerp(double a, double b, double t) { return a + (b - a) * t; }

static double lerp_angle(double a, double b, double t) {
    return a + angle_delta(a, b) * t;
}

/* 大圆距离（米），Haversine。用于判定网络跳变。 */
static double great_circle_m(const PositionFix* a, const PositionFix* b) {
    const double r = 6371000.0;
    const double d2r = 3.14159265358979323846 / 180.0;
    const double lat1 = a->lat * d2r, lat2 = b->lat * d2r;
    const double dlat = lat2 - lat1;
    const double dlon = (b->lon - a->lon) * d2r;
    double s = sin(dlat * 0.5);
    double x = s * s;
    s = sin(dlon * 0.5);
    x += cos(lat1) * cos(lat2) * s * s;
    if (x > 1.0) x = 1.0;
    return 2.0 * r * asin(sqrt(x));
}

/* 取第 i 帧（0 = 最新）；返回 NULL 表示越界。 */
static const PositionFix* fix_at(const Player* p, int i) {
    if (i < 0 || i >= p->fix_count) return NULL;
    return &p->fixes[(p->fix_head - i + FIX_MAX * 2) % FIX_MAX];
}

/* 追加一帧到环形缓冲（网络线程，持有 g_players_lock）。
 * 同时间戳的帧直接覆盖（Swift push_frontKeepLatestAdjustedFirst 的行为）。 */
static void push_fix(Player* p, double lat, double lon, double alt_m,
                     double heading, double pitch, double roll, double gs,
                     double now) {
    const PositionFix* newest = fix_at(p, 0);
    PositionFix* slot;

    if (newest && fabs(now - newest->ts) < 1e-3) {
        slot = &p->fixes[p->fix_head];   /* 同刻：就地更新最新帧 */
    } else {
        p->fix_head = (p->fix_head + 1) % FIX_MAX;
        if (p->fix_count < FIX_MAX) p->fix_count++;
        slot = &p->fixes[p->fix_head];
    }
    slot->lat = lat;
    slot->lon = lon;
    slot->alt = alt_m;
    slot->heading = heading;
    slot->pitch = pitch;
    slot->roll = roll;
    slot->gs = gs;
    slot->ts = now;
}

/* 将渲染位置直接对齐到某帧（首次定位、网络跳变、失联超时停住时使用）。 */
static void snap_to(const PositionFix* f, Player* p) {
    p->cur_lat = f->lat;
    p->cur_lon = f->lon;
    p->cur_alt = f->alt;
    p->cur_heading = f->heading;
    p->cur_pitch = f->pitch;
    p->cur_roll = f->roll;
}

/* 由 a→b 按时间分数 t 插值渲染位置；t 允许 >1（外推）。 */
static void lerp_fix_to(const PositionFix* a, const PositionFix* b,
                        double t, Player* p) {
    p->cur_lat = lerp(a->lat, b->lat, t);
    p->cur_lon = lerp(a->lon, b->lon, t);
    p->cur_alt = lerp(a->alt, b->alt, t);
    p->cur_heading = lerp_angle(a->heading, b->heading, t);
    p->cur_pitch = lerp(a->pitch, b->pitch, t);
    p->cur_roll = lerp(a->roll, b->roll, t);
}

/* 核心：按时间轴计算当前渲染位置。
 *
 * 1) 首帧直接定位，避免从 (0,0) 平滑飞过来；
 * 2) 新旧帧距离超阈值判定为网络跳变/重生 → 直接吸附；
 * 3) 渲染时刻 t = now - RENDER_DELAY 落在历史窗口内 → 找包夹 t 的相邻
 *    两帧做线性插值（Swift 的 Spline/Linear 段内插值思想；本插件为 C，
 *    采用线性，航向走最短弧）；
 * 4) t 超出最新帧（下一帧尚未到达）→ 沿最近两帧匀速外推，但不超过
 *    RENDER_DELAY + EXTRAP_MAX_SEC；超过则停住（Swift 失联后的行为）。
 *
 * 全程基于绝对时间戳，与主循环帧率无关。
 */
static void interpolate_player(Player* p, double now) {
    const PositionFix* newest;
    const PositionFix* older;
    const PositionFix* seg_a;
    const PositionFix* seg_b;
    double t_render, seg_dt, t;
    int i;

    newest = fix_at(p, 0);
    if (!newest) return;

    if (!p->have_first_fix) {
        snap_to(newest, p);
        p->have_first_fix = 1;
        return;
    }

    older = fix_at(p, 1);
    if (older && great_circle_m(older, newest) > SNAP_DIST_M) {
        snap_to(newest, p);   /* 网络跳变/重生：不做长距离滑行 */
        return;
    }
    if (!older) {
        snap_to(newest, p);
        return;
    }

    /* 渲染时刻（滞后于真实时间，保证区间内已有可插值的历史） */
    t_render = now - RENDER_DELAY;

    /* 3) 在历史中找包夹 t_render 的相邻两帧（fixes 最新在前 → 时间递增） */
    for (i = 0; i + 1 < p->fix_count; i++) {
        seg_b = fix_at(p, i);       /* 较晚 */
        seg_a = fix_at(p, i + 1);   /* 较早 */
        seg_dt = seg_b->ts - seg_a->ts;
        if (seg_dt <= 1e-6) continue;
        if (t_render >= seg_a->ts && t_render <= seg_b->ts) {
            t = (t_render - seg_a->ts) / seg_dt;
            lerp_fix_to(seg_a, seg_b, t, p);
            return;
        }
    }

    /* 4) t_render 越过最新帧：沿最近两帧匀速外推，等待下一帧到达；
     *    距最新帧超过 RENDER_DELAY + EXTRAP_MAX_SEC（数据停滞）→ 停住 */
    seg_dt = newest->ts - older->ts;
    if (seg_dt > 1e-6 && t_render > newest->ts &&
        now - newest->ts <= RENDER_DELAY + EXTRAP_MAX_SEC) {
        lerp_fix_to(older, newest, (t_render - older->ts) / seg_dt, p);
        return;
    }

    /* 数据停滞：停在最新帧，不做无界外推 */
    snap_to(newest, p);
}

/* ---------------- 网络线程 ---------------- */

static void handle_client_line(const char* line) {
#ifdef _WIN32
    __try {
#endif
        Json* root = json_parse(line);
        const Json* jt;
        if (!root) return;

        jt = json_get(root, "type");
        if (jt && jt->type == JSON_STR) {
            if (strcmp(jt->sval, "Send:OPD:FSD9") == 0) {
                const Json* data = json_get(root, "data");
                if (data && data->type == JSON_ARR) {
                    int i, n = json_len(data);
                    LOCK_ENTER(g_players_lock);
                    g_atc_count = 0;
                    for (i = 0; i < n; i++) {
                        const Json* el = json_at(data, i);
                        const Json* f;
                        Player* p = NULL;
                        int slot;
                        if (!el) continue;

                        /* 查找或分配玩家槽位 */
                        f = json_get(el, "callsign");
                        if (!f || f->type != JSON_STR) continue;
                        for (slot = 0; slot < PLAYER_MAX; slot++) {
                            if (g_players[slot].active &&
                                strcmp(g_players[slot].callsign, f->sval) == 0) {
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
                                    p->model.inst = NULL;
                                    p->model.tried = 0;
                                    strncpy(p->callsign, f->sval,
                                            sizeof(p->callsign) - 1);
                                    p->callsign[sizeof(p->callsign) - 1] = '\0';
                                    break;
                                }
                            }
                        }
                        if (!p) continue;

#define GETSTR(key, dst) do { \
    const Json* _t = json_get(el, key); \
    if (_t && _t->type == JSON_STR) \
        strncpy(dst, _t->sval, sizeof(dst) - 1), \
        dst[sizeof(dst) - 1] = '\0'; \
} while (0)
#define GETNUM(key, dst) do { \
    const Json* _t = json_get(el, key); \
    if (_t && _t->type == JSON_NUM) dst = _t->num; \
} while (0)

                        {
                            /* 新一帧位置入历史缓冲（带接收时间戳），
                             * 平滑在 update_players / interpolate_player 完成 */
                            double lat = 0, lon = 0, alt_m = 0;
                            double pitch = 0, bank = 0, heading = 0, gs = 0;
                            GETSTR("aircraft", p->icao);
                            GETSTR("aircraft_family", p->family);
                            GETNUM("latitude", lat);
                            GETNUM("longitude", lon);
                            GETNUM("altitude", alt_m);
                            alt_m *= 0.3048;        /* ft -> m */
                            GETNUM("pitch", pitch);
                            GETNUM("bank", bank);
                            GETNUM("heading", heading);
                            GETNUM("ground_speed", gs);
                            p->last_seen = xc_now_sec();   /* 非主线程：不使用 XPLM API */
                            push_fix(p, lat, lon, alt_m, heading, pitch,
                                     bank, gs, p->last_seen);
                        }
                    }
                    /* 不在列表中的玩家由超时机制（PLAYER_TIMEOUT）清理 */
                    LOCK_LEAVE(g_players_lock);
                    xconnect_logf("players updated (n=%d)", n);
                }
            } else if (strcmp(jt->sval, "Send:ATC") == 0) {
                const Json* data = json_get(root, "data");
                if (data && data->type == JSON_ARR) {
                    LOCK_ENTER(g_players_lock);
                    g_atc_count = json_len(data);
                    LOCK_LEAVE(g_players_lock);
                }
            }
        }
        json_free(root);
#ifdef _WIN32
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        xconnect_logf("net: handle_client_line crash code=%08x",
                      (unsigned)GetExceptionCode());
    }
#endif
}

#ifdef _WIN32
#define THREAD_RET DWORD WINAPI
#define THREAD_ARG LPVOID
#else
#define THREAD_RET void*
#define THREAD_ARG void*
#endif

static THREAD_RET net_thread(THREAD_ARG arg) {
    (void)arg;
    g_listen_sock = net_listen(LISTEN_PORT);
    if (g_listen_sock == NET_INVALID) {
        xconnect_logf("net: listen %d failed", LISTEN_PORT);
        return 0;
    }
    xconnect_logf("net: listening on %d", LISTEN_PORT);

    while (g_running) {
        net_sock_t sock = net_accept(g_listen_sock);
        if (sock == NET_INVALID) break;   /* 监听被关闭 */

        LOCK_ENTER(g_sock_lock);
        if (g_client_sock != NET_INVALID) net_close(g_client_sock);
        g_client_sock = sock;
        LOCK_LEAVE(g_sock_lock);
        xconnect_logf("net: client connected");

        net_send_line(sock, "{\"type\":\"connected\",\"version\":1}");

        for (;;) {
            /* 玩家列表单行 JSON 可能很大（数百玩家），放宽到 256KB */
            static char line[256 * 1024];
            int n = net_read_line(sock, line, sizeof(line));
            if (n == -2) continue;          /* 超长行已丢弃，继续读下一条 */
            if (n <= 0) break;
            handle_client_line(line);
        }

        LOCK_ENTER(g_sock_lock);
        if (g_client_sock == sock) g_client_sock = NET_INVALID;
        LOCK_LEAVE(g_sock_lock);
        g_csl_loaded_n = 0;
        g_csl_missing_n = 0;
        net_close(sock);
        xconnect_logf("net: client disconnected");
    }
    return 0;
}

/* ---------------- 主循环 ---------------- */

static void build_flight_data(char* out, int size) {
    long com1 = (long)(dref_float(g_drefs[DR_COM1]) / 1000.0);
    long com2 = (long)(dref_float(g_drefs[DR_COM2]) / 1000.0);
    const char* squark = dref_int(g_drefs[DR_XPDR_MODE]) >= 2 ? "S" : "N";
    char icao[16];
    int n, i;

    icao[0] = '\0';
    if (g_drefs[DR_AC_ICAO]) {
        n = XPLMGetDatab(g_drefs[DR_AC_ICAO], icao, 0, (int)sizeof(icao) - 1);
        if (n < 0) n = 0;
        if (n > (int)sizeof(icao) - 1) n = (int)sizeof(icao) - 1;
        icao[n] = '\0';
        /* 仅保留字母数字（JSON 安全） */
        for (i = 0; icao[i]; i++) {
            if (!isalnum((unsigned char)icao[i]) && icao[i] != '-') {
                icao[i] = '\0';
                break;
            }
        }
    }

    snprintf(out, size,
        "{\"type\":\"flight_data\","
        "\"aircraft_icao\":\"%s\","
        "\"latitude\":%.6f,\"longitude\":%.6f,"
        "\"altitude_msl\":%.1f,\"altitude_agl\":%.1f,\"elevation\":%.1f,"
        "\"pitch\":%.2f,\"bank\":%.2f,\"heading\":%.2f,\"true_heading\":%.2f,"
        "\"groundspeed\":%.1f,\"indicated_airspeed\":%.1f,\"vertical_speed\":%.1f,"
        "\"com1_freq\":%ld,\"com2_freq\":%ld,"
        "\"transponder\":%d,\"squark\":\"%s\",\"gear_deploy\":%.2f,"
        "\"flaps_ratio\":%.2f,\"throttle_ratio\":%.2f}",
        icao,
        dref_float(g_drefs[DR_LAT]), dref_float(g_drefs[DR_LON]),
        dref_float(g_drefs[DR_ELEV]) * 3.28084,
        dref_float(g_drefs[DR_AGL]) * 3.28084,
        dref_float(g_drefs[DR_ELEV]),
        dref_float(g_drefs[DR_PITCH]), dref_float(g_drefs[DR_BANK]),
        dref_float(g_drefs[DR_MAG_HEAD]), dref_float(g_drefs[DR_TRUE_HEAD]),
        dref_float(g_drefs[DR_GS]) * 1.943844,
        dref_float(g_drefs[DR_IAS]), dref_float(g_drefs[DR_VSI]),
        com1, com2,
        dref_int(g_drefs[DR_XPDR_CODE]), squark,
        dref_float(g_drefs[DR_GEAR]),
        dref_float(g_drefs[DR_FLAPS]), dref_float(g_drefs[DR_THROTTLE]));
}

/* ---------------- CSL 加载状态上报 ---------------- */

/* 汇总当前 CSL 加载状态；有变化时向客户端推送 csl_status。 */
static void refresh_csl_status(void) {
    char loaded[PLAYER_MAX][8];
    char missing[PLAYER_MAX][8];
    int ln = 0, mn = 0;
    int i;
    int changed = 0;

    LOCK_ENTER(g_players_lock);
    for (i = 0; i < PLAYER_MAX; i++) {
        Player* p = &g_players[i];
        if (!p->active || !p->icao[0]) continue;
        if (p->model.inst) {
            if (ln < PLAYER_MAX)
                snprintf(loaded[ln++], 8, "%s", p->icao);
        } else if (p->model.tried) {
            if (mn < PLAYER_MAX)
                snprintf(missing[mn++], 8, "%s", p->icao);
        }
    }
    LOCK_LEAVE(g_players_lock);

    if (ln != g_csl_loaded_n || mn != g_csl_missing_n)
        changed = 1;
    else
        for (i = 0; i < ln && !changed; i++)
            if (strcmp(loaded[i], g_csl_loaded[i]) != 0) changed = 1;
    for (i = 0; i < mn && !changed; i++)
        if (strcmp(missing[i], g_csl_missing[i]) != 0) changed = 1;

    if (!changed) return;

    memcpy(g_csl_loaded, loaded, sizeof(loaded[0]) * ln);
    memcpy(g_csl_missing, missing, sizeof(missing[0]) * mn);
    g_csl_loaded_n = ln;
    g_csl_missing_n = mn;

    {
        char line[4096];
        int n = snprintf(line, sizeof(line),
                         "{\"type\":\"csl_status\",\"loaded\":[");
        for (i = 0; i < ln; i++)
            n += snprintf(line + n, sizeof(line) - (size_t)n,
                          "%s\"%s\"", i ? "," : "", loaded[i]);
        n += snprintf(line + n, sizeof(line) - (size_t)n, "],\"missing\":[");
        for (i = 0; i < mn; i++)
            n += snprintf(line + n, sizeof(line) - (size_t)n,
                          "%s\"%s\"", i ? "," : "", missing[i]);
        n += snprintf(line + n, sizeof(line) - (size_t)n, "]}");
        LOCK_ENTER(g_sock_lock);
        if (g_client_sock != NET_INVALID)
            net_send_line(g_client_sock, line);
        LOCK_LEAVE(g_sock_lock);
    }
}

static void update_players(double now) {
    float anim[CSL_MAX_DATAREFS];
    int i;
    LOCK_ENTER(g_players_lock);
    for (i = 0; i < PLAYER_MAX; i++) {
        Player* p = &g_players[i];
        const PositionFix* f;
        if (!p->active) continue;

        /* 超时清理：无活跃玩家时循环体为空，遍历开销可忽略 */
        if (now - p->last_seen > PLAYER_TIMEOUT) {
            csl_release_node(&p->model);
            p->active = 0;
            continue;
        }
        if (!p->model.inst)
            csl_load(&p->model, p->icao, p->family);
        if (!p->model.inst) continue;

        /* 时间插值：按历史帧与当前时间计算渲染位置（详见 interpolate_player） */
        interpolate_player(p, now);
        f = fix_at(p, 0);   /* 最新网络帧，用于动画判断 */
        if (!f) continue;

        /* 起落架：低高度（<60m MSL）或低地速（<40kt）视为地面/进近，放下 */
        anim[0] = 0.0f;                 /* flap */
        anim[1] = (f->alt < 60.0 || f->gs < 40.0) ? 1.0f : 0.0f;   /* <40kt 视为地面 */
        anim[2] = 0.0f;                 /* throttle */
        anim[3] = 1.0f;                 /* battery on */
        anim[4] = 1.0f;                 /* beacon */
        anim[5] = 1.0f;                 /* nav light */
        anim[6] = 0.0f;                 /* left brake */
        anim[7] = 0.0f;                 /* right brake */
        anim[8] = 1.0f;                 /* xpdr available */
        anim[9] = 0.0f;                 /* xpdr code */
        anim[10] = 3.0f;                /* xpdr mode ALT */
        csl_set_position(p->model.inst, p->cur_lat, p->cur_lon,
                         p->cur_alt, p->cur_heading, p->cur_pitch,
                         p->cur_roll, anim, csl_dataref_count());
    }
    LOCK_LEAVE(g_players_lock);
}

static float xconnect_flight_loop(float elapsedMe, float elapsedSim,
                                  int counter, void* refcon) {
    (void)refcon;
    (void)elapsedSim;
    char line[4096];

#ifdef _WIN32
    __try {
#endif
        /* 节拍日志：确认飞行循环实际被调用的频率（仅首个循环与客户端在场时） */
        if (counter == 0) {
            xconnect_logf("loop first frame");
        } else if (xc_now_sec() - g_last_loop_log >= 5.0) {
            g_last_loop_log = xc_now_sec();
            xconnect_logf("loop tick elapsedMe=%.1f counter=%d",
                          elapsedMe, counter);
        }

        /* 注意：elapsedMe 是"距上次调用间隔"（每次 ~0.1s），不是绝对时间，
           不可用于节流；用单调时钟 xc_now_sec() 做发送节流。
           无客户端时不构建飞行数据：XP12 加载界面同样在跑飞行循环，
           无条件构建 + 写日志会显著拖慢启动。 */
        {
            double now = xc_now_sec();
            int has_client;
            LOCK_ENTER(g_sock_lock);
            has_client = (g_client_sock != NET_INVALID);
            LOCK_LEAVE(g_sock_lock);
            if (has_client && now - g_last_send >= DATA_INTERVAL) {
                int rc;
                build_flight_data(line, sizeof(line));
                LOCK_ENTER(g_sock_lock);
                rc = (g_client_sock != NET_INVALID)
                     ? net_send_line(g_client_sock, line) : 1;
                LOCK_LEAVE(g_sock_lock);
                if (now - g_last_send_log >= 0.25) {
                    g_last_send_log = now;
                    xconnect_logf("send flight_data rc=%d hasClient=%d",
                                  rc, g_client_sock != NET_INVALID);
                }
                g_last_send = now;
            }
        }

        update_players(xc_now_sec());
        /* 无客户端时不推送 CSL 状态（省去 JSON 构建与遍历） */
        {
            int has_client;
            LOCK_ENTER(g_sock_lock);
            has_client = (g_client_sock != NET_INVALID);
            LOCK_LEAVE(g_sock_lock);
            if (has_client) refresh_csl_status();
        }
#ifdef _WIN32
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        xconnect_logf("loop crash code=%08x", (unsigned)GetExceptionCode());
    }
#endif
    /* 无客户端时降频到 0.5s，降低加载/空闲时 CPU 与日志开销；
       客户端在场时保持 0.05s（20Hz 平滑 CSL）。 */
    {
        int has_client;
        LOCK_ENTER(g_sock_lock);
        has_client = (g_client_sock != NET_INVALID);
        LOCK_LEAVE(g_sock_lock);
        return has_client ? LOOP_INTERVAL : LOOP_IDLE_INTERVAL;
    }
}

/* ---------------- 插件入口 ---------------- */

/* 标准 XPLM 插件入口签名：XPluginStart(char* outName, char* outSig, char* outDesc)
 * —— 只有 3 个参数（无 host/sdk）。此前误用 5 参数签名导致 x64 寄存器错位：
 * X-Plane 设置 RCX/RDX/R8 三个输出缓冲，R9 为残留值；
 * 结果 outSig 从未被写入 → X-Plane 读到残留签名 → "plugin already loaded" 冲突、
 * 插件名空、无法启用。 */
PLUGIN_API int XPluginStart(char* outName, char* outSig, char* outDesc) {
    int i;

    /* 第一件事：初始化日志路径并记录入口，任何后续崩溃都有现场 */
    {
        char sys[1024];
        XPLMGetSystemPath(sys);
        snprintf(g_log_path, sizeof(g_log_path),
                 "%sResources%splugins%sXConnect%sxconnect.log",
                 sys, XPLMGetDirectorySeparator(),
                 XPLMGetDirectorySeparator(), XPLMGetDirectorySeparator());
    }
    xconnect_logf("XConnect v6 (built %s %s): entry", __DATE__, __TIME__);
    xconnect_logf("  params: outName=%p outSig=%p outDesc=%p",
                  (void*)outName, (void*)outSig, (void*)outDesc);

    if (outName) snprintf(outName, 256, "%s", XCONNECT_NAME);
    if (outSig)  snprintf(outSig, 256, "%s", XCONNECT_SIG);
    if (outDesc) snprintf(outDesc, 256, "%s", XCONNECT_DESC);
    xconnect_logf("  step: names done");

    lock_init();
    g_running = 1;
    xconnect_logf("  step: locks done");

    if (net_init() != 0) {
        XPLMDebugString(XCONNECT_NAME ": network init failed\n");
        return 1;
    }
    xconnect_logf("  step: net init done");

    for (i = 0; i < DR_COUNT; i++)
        g_drefs[i] = XPLMFindDataRef(kDrefNames[i]);
    xconnect_logf("  step: datarefs done");

    csl_init();
    XPLMDebugString(XCONNECT_NAME
        ": CSL root: Resources/plugins/XConnect/CSL\n");
    xconnect_logf("  step: csl done");

    /* X-Plane 12.4 起 legacy XPLMRegisterFlightLoopCallback 安装的
     * pre-flight-loop 回调不再被调用；改用新 API XPLMCreateFlightLoop。 */
    {
        XPLMCreateFlightLoop_t params;
        memset(&params, 0, sizeof(params));
        params.structSize = (int)sizeof(params);
        params.phase = xplm_FlightLoop_Phase_BeforeFlightModel;
        params.callbackFunc = xconnect_flight_loop;
        params.refcon = NULL;
        g_flight_loop = XPLMCreateFlightLoop(&params);
        if (g_flight_loop) {
            /* -1 + inRelativeToNow=1：下一个循环即调用 */
            XPLMScheduleFlightLoop(g_flight_loop, -1.0f, 1);
            xconnect_logf("  step: flight loop scheduled");
        } else {
            xconnect_logf("  step: flight loop create FAILED");
        }
    }

    /* 网络线程（回调签名与 LPTHREAD_START_ROUTINE 完全一致） */
#ifdef _WIN32
    {
        HANDLE h = CreateThread(NULL, 0, net_thread, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }
#else
    {
        pthread_t tid;
        pthread_create(&tid, NULL, net_thread, NULL);
        pthread_detach(tid);
    }
#endif
    xconnect_logf("  step: net thread started");

    xconnect_logf("XPluginStart end ok");
    return 1;
}

PLUGIN_API void XPluginStop(void) {
    int i;
    xconnect_logf("XPluginStop begin");
    g_running = 0;
    if (g_listen_sock != NET_INVALID) {
        net_close(g_listen_sock);
        g_listen_sock = NET_INVALID;
    }
    LOCK_ENTER(g_sock_lock);
    if (g_client_sock != NET_INVALID) {
        net_close(g_client_sock);
        g_client_sock = NET_INVALID;
    }
    LOCK_LEAVE(g_sock_lock);

    if (g_flight_loop) {
        XPLMDestroyFlightLoop(g_flight_loop);
        g_flight_loop = NULL;
    }

    for (i = 0; i < PLAYER_MAX; i++)
        csl_release_node(&g_players[i].model);

    csl_shutdown();
    lock_done();
    xconnect_logf("XPluginStop done");
}

/* SDK 430：XPluginEnable 返回 int，1 = 启用成功；返回 0 会导致
 * XPLMEnablePlugin 返回 0，插件管理器状态不切换（Enable 点击无效果）。 */
PLUGIN_API int XPluginEnable(void) {
    xconnect_logf("XPluginEnable ok");
    return 1;
}

PLUGIN_API int XPluginDisable(void) {
    xconnect_logf("XPluginDisable");
    return 1;   /* 允许被禁用 */
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID inFrom, int inMsg,
                                      void* inParam) {
    (void)inFrom;
    (void)inMsg;
    (void)inParam;
}