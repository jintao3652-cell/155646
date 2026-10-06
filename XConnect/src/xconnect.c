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
#define PLAYER_TIMEOUT 30.0   /* 秒 */
#define DATA_INTERVAL 0.2f   /* 发送间隔：0.2 秒 = 每秒 5 条（5Hz） */
#define LOOP_INTERVAL 0.05f  /* 主循环节拍（有客户端时） */
#define LOOP_IDLE_INTERVAL 0.5f  /* 空闲降频节拍（无客户端时） */

/* ---------------- 本机飞行数据 dataref ---------------- */

typedef enum {
    DR_LAT, DR_LON, DR_ELEV, DR_AGL, DR_PITCH, DR_BANK, DR_TRUE_HEAD,
    DR_MAG_HEAD, DR_GS, DR_IAS, DR_VSI,
    DR_COM1, DR_COM2, DR_COM3,
    DR_COM1_STBY, DR_COM2_STBY, DR_COM3_STBY,
    DR_XPDR_CODE, DR_XPDR_MODE,
    DR_TCAS_FILTER, DR_TCAS_ALERT, DR_TCAS_ADVISORY,
    /* 机型自定义（ToLiss A321 / AirbusFBW）：应答机 1/2 号、电源、TCAS 模式、
     * TCAS 高度选择；ckpt/.../anim 为面板旋钮动画值（仅校准日志用） */
    DR_XPDR_SYS, DR_XPDR_PWR, DR_XPDR_TCAS_MODE, DR_XPDR_TCAS_ALT,
    DR_XPDR_1, DR_XPDR_2, DR_XPDR_3, DR_XPDR_4,
    DR_XPDR_ALTITUDE, DR_TCAS_STATUS,
    DR_CKPT_XPDR_SYS, DR_CKPT_XPDR_PWR, DR_CKPT_XPDR_ALT, DR_CKPT_XPDR_MODE,
    DR_GEAR, DR_ONGROUND, DR_FLAPS, DR_THROTTLE,
    /* 机模部件（机模配置 ACC 上报用：灯光用开关布尔，可靠性高于亮度比） */
    DR_LANDING_LT, DR_TAXI_LT, DR_NAV_LT, DR_BEACON_LT, DR_STROBE_LT, DR_N1,
    DR_SPLR,
    DR_AC_ICAO,      /* 机型 ICAO 首选：sim/aircraft/view/acf_icao */
    DR_AC_ICAO2,     /* 回退：sim/cockpit2/aircraft/view/acf_icao */
    DR_AC_DESCRIP,   /* 兜底：sim/aircraft/view/acf_descrip（提取 ICAO 词元） */
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
    "sim/cockpit2/radios/actuators/com3_frequency_hz_833",
    "sim/cockpit2/radios/actuators/com1_standby_frequency_hz_833",
    "sim/cockpit2/radios/actuators/com2_standby_frequency_hz_833",
    "sim/cockpit2/radios/actuators/com3_standby_frequency_hz_833",
    "sim/cockpit2/radios/actuators/transponder_code",
    "sim/cockpit2/radios/actuators/transponder_mode",
    "sim/cockpit2/radios/actuators/tcas_filter",
    "sim/cockpit2/tcas/indicators/tcas_alert",
    "sim/cockpit2/tcas/indicators/tcas_active_advisory",
    "AirbusFBW/XPDRSystem",
    "AirbusFBW/XPDRPower",
    "AirbusFBW/XPDRTCASMode",
    "AirbusFBW/XPDRTCASAltSelect",
    "AirbusFBW/XPDR1",
    "AirbusFBW/XPDR2",
    "AirbusFBW/XPDR3",
    "AirbusFBW/XPDR4",
    "AirbusFBW/XPDRAltitude",
    "AirbusFBW/TCASStatus",
    "ckpt/transponder/sys/anim",
    "ckpt/transponder/pwrx/anim",
    "ckpt/transponder/alt/anim",
    "ckpt/transponder/mode/anim",
    "sim/flightmodel/gear/deploy_ratio",
    "sim/flightmodel/failures/onground_any",
    "sim/cockpit2/flight/indicators/flaps_ratio",
    "sim/cockpit2/engine/indicators/throttle_ratio",
    "sim/cockpit/electrical/landing_lights_on",
    "sim/flightmodel2/lights/taxi_lights_brightness_ratio",
    "sim/cockpit/electrical/nav_lights_on",
    "sim/cockpit/electrical/beacon_lights_on",
    "sim/cockpit/electrical/strobe_lights_on",
    "sim/cockpit2/engine/indicators/N1_percent",
    "sim/flightmodel/controls/splr_def",
    "sim/aircraft/view/acf_icao",
    "sim/cockpit2/aircraft/view/acf_icao",
    "sim/aircraft/view/acf_descrip",
};

static XPLMDataRef g_drefs[DR_COUNT];

/* 机模部件（ACC 配置上报）当前值，build_flight_data 每帧刷新 */
static int g_parts_nav = 0, g_parts_strobe = 0, g_parts_landing = 0;
static int g_parts_taxi = 0, g_parts_beacon = 0;
static int g_parts_gear = 1, g_parts_flaps = 0, g_parts_onground = 1;
static int g_parts_spoilers = 0;
static int g_parts_eng_on[4] = { 0, 0, 0, 0 };
static float g_parts_eng_n1[4] = { 0, 0, 0, 0 };

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
 * 参考 swift-project/pilotclient（Swift）的插件层设计：
 * 网络位置按时间戳进入历史缓冲，渲染时用「渲染时刻 = now - 延迟」在历史中
 * 找包夹该时刻的相邻两帧做线性插值（航向取最短弧）。
 *
 * 关键：渲染延迟必须 ≥ 位置更新间隔（+ 余量），否则外推会越过最新帧、
 * 下帧到达时产生"回跳 → 动一下停一下"。Swift 的做法即为
 * c_positionTimeOffsetMsec = 6000ms（对应 5s 位置包）/ 1500ms（对应 1s interim）。
 *
 * - RENDER_DELAY：常规更新的渲染滞后 = 更新间隔 + 余量（本机 3s roster → 6s）。
 * - RENDER_DELAY_FAST：间隔 <1.5s 的快速更新（Swift interim 风格）用 1.5s。
 * - RENDER_DELAY_MAX：自适应上限（更长间隔也尽量纯插值，>8s 只能部分外推）。
 * - EXTRAP_MAX_SEC：渲染时刻越过最新帧后允许外推的秒数（数据迟到缓冲），
 *   超过即停在最新帧（Swift 断流后停住的行为）。
 * - SNAP_DIST_M：新旧两帧距离超过此值判定为网络跳变/重生，直接吸附。
 */
#define RENDER_DELAY        6.0   /* 秒：常规更新渲染滞后（= 间隔 + 余量） */
#define RENDER_DELAY_FAST   1.5   /* 秒：快速更新（间隔 <1.5s）渲染滞后 */
#define RENDER_DELAY_MAX    8.0   /* 秒：自适应上限 */
#define RENDER_DELAY_RESERVE 1.0  /* 秒：期望间隔之外的余量 */
#define EXTRAP_MAX_SEC      3.0   /* 秒：越过最新帧后继续外推的秒数 */
#define SNAP_DIST_M         60000.0 /* 米：跳变判定阈值 */

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
    double avg_interval;   /* 帧到达平均间隔（EWMA，秒），用于自适应渲染滞后 */
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
 * 同时间戳的帧直接覆盖（Swift push_frontKeepLatestAdjustedFirst 的行为），
 * 同时以 EWMA 更新帧到达平均间隔（供自适应渲染滞后使用）。 */
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
        if (newest) {
            const double dt = now - newest->ts;
            if (dt > 1e-3) {
                if (p->avg_interval <= 0.0) p->avg_interval = dt;
                else p->avg_interval = 0.9 * p->avg_interval + 0.1 * dt;
            }
        }
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

/* 自适应渲染滞后：间隔 + 余量（≥ RENDER_DELAY），快速更新用 1.5s。
 * 等价 Swift 的 c_positionTimeOffsetMsec（6000ms）/ c_interimPositionTimeOffsetMsec（1500ms）。 */
static double player_delay(const Player* p) {
    const double iv = p->avg_interval;
    if (iv > 0.0 && iv < 1.5) return RENDER_DELAY_FAST;
    if (iv > 0.0) {
        double d = iv + RENDER_DELAY_RESERVE;
        if (d < RENDER_DELAY) d = RENDER_DELAY;
        if (d > RENDER_DELAY_MAX) d = RENDER_DELAY_MAX;
        return d;
    }
    return RENDER_DELAY;
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
 * 3) 渲染时刻 t = now - delay 落在历史窗口内 → 找包夹 t 的相邻两帧做
 *    线性插值（Swift 的 Spline/Linear 段内插值思想；本插件为 C，采用
 *    线性，航向走最短弧）。delay 自适应 ≥ 帧到达间隔（详见 player_delay），
 *    保证 t 总是在最后两帧之间，从而**永不外推、永不回跳**；
 * 4) 例外一：t 早于历史最旧帧（刚出现/启动期）→ 停在最旧帧，等 t 进入
 *    窗口再动，防止瞬移回跳；
 * 5) 例外二：t 越过最新帧（下一帧迟到）→ 沿最近两帧匀速外推，最多
 *    EXTRAP_MAX_SEC；数据停滞则停在最新帧（Swift 失联后的行为）。
 *
 * 全程基于绝对时间戳，与主循环帧率无关。
 */
static void interpolate_player(Player* p, double now) {
    const PositionFix* newest;
    const PositionFix* older;
    const PositionFix* oldest;
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

    /* 渲染时刻（滞后于真实时间：滞后 ≥ 帧到达间隔 ⇒ 总落在已知两帧之间） */
    t_render = now - player_delay(p);

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

    /* 4) t_render 早于最旧帧：停在最旧帧等待窗口推进（不向过去外推，避免倒跳） */
    oldest = fix_at(p, p->fix_count - 1);
    if (oldest && t_render < oldest->ts) {
        snap_to(oldest, p);
        return;
    }

    /* 5) t_render 越过最新帧（下一帧迟到）：沿最近两帧短时外推，
     *    超过 EXTRAP_MAX_SEC 视为数据停滞 → 停在最新帧 */
    seg_dt = newest->ts - older->ts;
    if (seg_dt > 1e-6 && t_render > newest->ts &&
        t_render - newest->ts <= EXTRAP_MAX_SEC) {
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

/* 频率 → MHz*1000。不同机型/版本下 com*_frequency_hz_833 返回的单位不一致：
 * 有的给 Hz（133115000）、有的给 kHz（133115）、个别给 MHz（133.115）。
 * 按量级自动识别并四舍五入，保证报文统一为 MHz*1000（133115 = 133.115）。
 * 修复：直接 /1000 截断在 kHz 输入下得到 133 → 前端显示 100.133。 */
static long freq_to_mhz1000(double raw) {
    if (raw >= 1e7) return (long)(raw / 1000.0 + 0.5);   /* Hz */
    if (raw >= 1e4) return (long)(raw + 0.5);            /* kHz */
    return (long)(raw * 1000.0 + 0.5);                   /* MHz */
}

/* 机型 ICAO 识别（实测两个 acf_icao dataref 在此环境恒为空）。
 * 优先级：XPLMGetNthAircraftModel 模型文件名 → 两个 acf_icao dataref →
 * acf_descrip 提取 ICAO 词元。 */
static void read_aircraft_icao(char* out, int cap) {
    char tmp[128];
    int n, i;
    out[0] = '\0';
    if (cap < 1) return;

#define READ_RAW(dr) do { \
    if (g_drefs[(dr)]) { \
        n = XPLMGetDatab(g_drefs[(dr)], tmp, 0, (int)sizeof(tmp) - 1); \
        if (n < 0) n = 0; \
        if (n > (int)sizeof(tmp) - 1) n = (int)sizeof(tmp) - 1; \
        tmp[n] = '\0'; \
    } else { tmp[0] = '\0'; } \
} while (0)

#if kXPLM_Version >= 210
    /* 1) 模型文件名：B738.acf → B738；A321_CFM.acf → A321；C172SP.acf → C172。
     *    词元规则：3~4 字符直接用；更长则取前 4，但要求含数字（防 "CESSNA" 误取）。 */
    {
        char fn[256], pth[2048];
        int len;
        XPLMGetNthAircraftModel(0, fn, pth);
        for (i = 0; fn[i]; i++) {
            if (fn[i] == '.') { fn[i] = '\0'; break; }
        }
        len = (int)strlen(fn);
        for (i = 0; i < len && i < (int)sizeof(fn) - 1; i++) {
            if (isalnum((unsigned char)fn[i])) fn[i] = (char)toupper((unsigned char)fn[i]);
            else fn[i] = ' ';
        }
        for (i = 0; fn[i];) {
            char tok[16];
            int j = 0, k, has_digit = 0;
            while (fn[i] == ' ') i++;
            while (fn[i] && fn[i] != ' ' && j < (int)sizeof(tok) - 1) {
                tok[j++] = fn[i++];
            }
            tok[j] = '\0';
            if (j < 3 || !isalpha((unsigned char)tok[0])) continue;
            if (j <= 4) {
                memcpy(out, tok, (size_t)j);
                out[j] = '\0';
                return;
            }
            /* 长词元：取前 4，需含数字（ICAO 常在第二位起带数字） */
            for (k = 0; k < 4 && tok[k]; k++)
                if (isdigit((unsigned char)tok[k])) has_digit = 1;
            if (has_digit) {
                memcpy(out, tok, 4);
                out[4] = '\0';
                return;
            }
            /* 否则继续看下一个词元（如 "Nolinor B737" 之类） */
        }
    }
#endif

    /* 2) / 3) 两个 acf_icao dataref */
    READ_RAW(DR_AC_ICAO);
    if (!tmp[0]) READ_RAW(DR_AC_ICAO2);
    for (i = 0; tmp[i] && i < cap - 1; i++) {
        const unsigned char c = (unsigned char)tmp[i];
        if (isalnum(c) || c == '-') out[i] = (char)toupper(c);
        else break;
    }
    out[i] = '\0';
    if (out[0]) return;

    /* 4) acf_descrip 兜底：取首个 4 字符、首字符为字母的词元（如 "A321"） */
    READ_RAW(DR_AC_DESCRIP);
    for (i = 0; tmp[i];) {
        char tok[16];
        int j = 0;
        while (tmp[i] && !isalnum((unsigned char)tmp[i])) i++;
        while (tmp[i] && isalnum((unsigned char)tmp[i]) && j < (int)sizeof(tok) - 1) {
            tok[j++] = (char)toupper((unsigned char)tmp[i++]);
        }
        tok[j] = '\0';
        if (j == 4 && isalpha((unsigned char)tok[0])) {
            memcpy(out, tok, 4);
            out[4] = '\0';
            return;
        }
    }
#undef READ_RAW
}

static void build_flight_data(char* out, int size) {
    long com1 = freq_to_mhz1000(dref_float(g_drefs[DR_COM1]));
    long com2 = freq_to_mhz1000(dref_float(g_drefs[DR_COM2]));
    long com3 = freq_to_mhz1000(dref_float(g_drefs[DR_COM3]));
    long com1s = freq_to_mhz1000(dref_float(g_drefs[DR_COM1_STBY]));
    long com2s = freq_to_mhz1000(dref_float(g_drefs[DR_COM2_STBY]));
    long com3s = freq_to_mhz1000(dref_float(g_drefs[DR_COM3_STBY]));
    int xpdr_mode = dref_int(g_drefs[DR_XPDR_MODE]);
    int tcas_filter = dref_int(g_drefs[DR_TCAS_FILTER]);
    int tcas_alert = dref_int(g_drefs[DR_TCAS_ALERT]);
    int tcas_advisory = dref_int(g_drefs[DR_TCAS_ADVISORY]);
    /* 机型自定义字段：dataref 不存在（其他机型）时上报 -1，前端据此降级到通用字段 */
    int xpdr_sys = g_drefs[DR_XPDR_SYS] ? dref_int(g_drefs[DR_XPDR_SYS]) : -1;
    int xpdr_pwr = g_drefs[DR_XPDR_PWR] ? dref_int(g_drefs[DR_XPDR_PWR]) : -1;
    int xpdr_tcas_mode = g_drefs[DR_XPDR_TCAS_MODE] ? dref_int(g_drefs[DR_XPDR_TCAS_MODE]) : -1;
    int xpdr_tcas_alt = g_drefs[DR_XPDR_TCAS_ALT] ? dref_int(g_drefs[DR_XPDR_TCAS_ALT]) : -1;
    const char* squark = xpdr_mode >= 2 ? "S" : "N";
    char icao[16];

    /* 机模部件（机模配置 ACC 上报，与 swift CAircraftParts JSON 键一致） */
    {
        float n1[4] = { 0, 0, 0, 0 };
        int i;
        if (g_drefs[DR_N1]) XPLMGetDatavf(g_drefs[DR_N1], n1, 0, 4);
        g_parts_nav = dref_int(g_drefs[DR_NAV_LT]) > 0;
        g_parts_strobe = dref_int(g_drefs[DR_STROBE_LT]) > 0;
        g_parts_landing = dref_int(g_drefs[DR_LANDING_LT]) > 0;
        g_parts_taxi = dref_float(g_drefs[DR_TAXI_LT]) > 0.0;
        g_parts_beacon = dref_int(g_drefs[DR_BEACON_LT]) > 0;
        g_parts_gear = dref_float(g_drefs[DR_GEAR]) > 0.5;
        g_parts_flaps = (int)(dref_float(g_drefs[DR_FLAPS]) * 100.0 + 0.5);
        if (g_parts_flaps < 0) g_parts_flaps = 0;
        if (g_parts_flaps > 100) g_parts_flaps = 100;
        g_parts_onground = dref_int(g_drefs[DR_ONGROUND]) > 0;
        /* 扰流板：splr_def 偏转度（deg），任一 >2° 视为伸出 */
        g_parts_spoilers = 0;
        if (g_drefs[DR_SPLR]) {
            float def[8];
            XPLMGetDatavf(g_drefs[DR_SPLR], def, 0, 8);
            for (i = 0; i < 8; i++) {
                if (def[i] > 2.0f) { g_parts_spoilers = 1; break; }
            }
        }
        /* N1 可能为 NaN（引擎未启动等）：消毒为 0，否则 JSON 输出 "NaN" 非法，
         * 服务器解析失败导致后续配置更新全部被丢弃 */
        for (i = 0; i < 4; i++) {
            float v = n1[i];
            if (!(v > 0.0f && v < 100.0f)) v = 0.0f;   /* NaN / 负数 / 越界 → 0 */
            g_parts_eng_n1[i] = v;
            g_parts_eng_on[i] = v > 5.0f;
        }
    }

    read_aircraft_icao(icao, sizeof(icao));

    snprintf(out, size,
        "{\"type\":\"flight_data\","
        "\"aircraft_icao\":\"%s\","
        "\"latitude\":%.6f,\"longitude\":%.6f,"
        "\"altitude_msl\":%.1f,\"altitude_agl\":%.1f,\"elevation\":%.1f,"
        "\"pitch\":%.2f,\"bank\":%.2f,\"heading\":%.2f,\"true_heading\":%.2f,"
        "\"groundspeed\":%.1f,\"indicated_airspeed\":%.1f,\"vertical_speed\":%.1f,"
        "\"com1_freq\":%ld,\"com2_freq\":%ld,\"com3_freq\":%ld,"
        "\"com1_standby\":%ld,\"com2_standby\":%ld,\"com3_standby\":%ld,"
        "\"transponder\":%d,\"xpdr_mode\":%d,\"squark\":\"%s\","
        "\"tcas_filter\":%d,\"tcas_alert\":%d,\"tcas_advisory\":%d,"
        "\"xpdr_sys\":%d,\"xpdr_pwr\":%d,\"xpdr_tcas_mode\":%d,\"xpdr_tcas_alt\":%d,"
        "\"gear_deploy\":%.2f,"
        "\"flaps_ratio\":%.2f,\"throttle_ratio\":%.2f,"
        "\"parts\":{\"lights\":{\"strobe_on\":%d,\"landing_on\":%d,\"taxi_on\":%d,"
        "\"beacon_on\":%d,\"nav_on\":%d},\"gear_down\":%d,\"flaps_pct\":%d,"
        "\"spoilers_out\":%d,"
        "\"engines\":[{\"on\":%d,\"rpm_pct\":%.1f},{\"on\":%d,\"rpm_pct\":%.1f},"
        "{\"on\":%d,\"rpm_pct\":%.1f},{\"on\":%d,\"rpm_pct\":%.1f}],"
        "\"on_ground\":%d}}",
        icao,
        dref_float(g_drefs[DR_LAT]), dref_float(g_drefs[DR_LON]),
        dref_float(g_drefs[DR_ELEV]) * 3.28084,
        dref_float(g_drefs[DR_AGL]) * 3.28084,
        dref_float(g_drefs[DR_ELEV]),
        dref_float(g_drefs[DR_PITCH]), dref_float(g_drefs[DR_BANK]),
        dref_float(g_drefs[DR_MAG_HEAD]), dref_float(g_drefs[DR_TRUE_HEAD]),
        dref_float(g_drefs[DR_GS]) * 1.943844,
        dref_float(g_drefs[DR_IAS]), dref_float(g_drefs[DR_VSI]),
        com1, com2, com3, com1s, com2s, com3s,
        dref_int(g_drefs[DR_XPDR_CODE]), xpdr_mode, squark,
        tcas_filter, tcas_alert, tcas_advisory,
        xpdr_sys, xpdr_pwr, xpdr_tcas_mode, xpdr_tcas_alt,
        dref_float(g_drefs[DR_GEAR]),
        dref_float(g_drefs[DR_FLAPS]), dref_float(g_drefs[DR_THROTTLE]),
        g_parts_strobe, g_parts_landing, g_parts_taxi, g_parts_beacon, g_parts_nav,
        g_parts_gear, g_parts_flaps, g_parts_spoilers,
        g_parts_eng_on[0], g_parts_eng_n1[0],
        g_parts_eng_on[1], g_parts_eng_n1[1],
        g_parts_eng_on[2], g_parts_eng_n1[2],
        g_parts_eng_on[3], g_parts_eng_n1[3],
        g_parts_onground);
}

/* ---------------- CSL 加载状态上报 ---------------- */

/* 机型 ICAO 诊断转储（前缀 "acf:"）。每秒输出各识别源的原始字符串，
 * 用于确诊机型识别失败的具体环节（dataref 空 / 描述串 / 模型文件名）。 */
static void dump_acf_values(void) {
    static double last_acf = -1.0;
    const double now = xc_now_sec();
    char a[32] = "", b[32] = "", d[80] = "", fn[256] = "", pth[1024] = "";
    int n;
    if (now - last_acf < 1.0) return;
    last_acf = now;
    if (g_drefs[DR_AC_ICAO]) {
        n = XPLMGetDatab(g_drefs[DR_AC_ICAO], a, 0, (int)sizeof(a) - 1);
        if (n < 0) n = 0;
        a[n] = '\0';
    }
    if (g_drefs[DR_AC_ICAO2]) {
        n = XPLMGetDatab(g_drefs[DR_AC_ICAO2], b, 0, (int)sizeof(b) - 1);
        if (n < 0) n = 0;
        b[n] = '\0';
    }
    if (g_drefs[DR_AC_DESCRIP]) {
        n = XPLMGetDatab(g_drefs[DR_AC_DESCRIP], d, 0, (int)sizeof(d) - 1);
        if (n < 0) n = 0;
        d[n] = '\0';
    }
#if kXPLM_Version >= 210
    XPLMGetNthAircraftModel(0, fn, pth);
#endif
    xconnect_logf("acf: view='%s' cockpit2='%s' descrip='%s' model='%s'", a, b, d, fn);
}

/* ToLiss/机型自定义应答机校准转储（前缀 "xpdrv:"）。
 * 每秒输出全部候选 dataref 的原始值（浮点，%.3g），不依赖"变化检测"——
 * 无论值是整数枚举还是连续浮点，旋钮拨动都会体现在转储中。
 * 用于标定数值→标签映射（如 XPDRPower 0/1/2/3 对应 OFF/STBY/AUTO/ON）。 */
static void dump_xpdr_values(void) {
    static double last_dump = -1.0;
    const double now = xc_now_sec();
    if (now - last_dump < 1.0) return;
    last_dump = now;
    xconnect_logf("xpdrv: sys=%.3g pwr=%.3g tcm=%.3g tca=%.3g alt=%.3g "
                  "x1=%.3g x2=%.3g x3=%.3g x4=%.3g tcstat=%.3g "
                  "anim(sys=%.3g pwr=%.3g alt=%.3g mode=%.3g)",
                  dref_float(g_drefs[DR_XPDR_SYS]),
                  dref_float(g_drefs[DR_XPDR_PWR]),
                  dref_float(g_drefs[DR_XPDR_TCAS_MODE]),
                  dref_float(g_drefs[DR_XPDR_TCAS_ALT]),
                  dref_float(g_drefs[DR_XPDR_ALTITUDE]),
                  dref_float(g_drefs[DR_XPDR_1]),
                  dref_float(g_drefs[DR_XPDR_2]),
                  dref_float(g_drefs[DR_XPDR_3]),
                  dref_float(g_drefs[DR_XPDR_4]),
                  dref_float(g_drefs[DR_TCAS_STATUS]),
                  dref_float(g_drefs[DR_CKPT_XPDR_SYS]),
                  dref_float(g_drefs[DR_CKPT_XPDR_PWR]),
                  dref_float(g_drefs[DR_CKPT_XPDR_ALT]),
                  dref_float(g_drefs[DR_CKPT_XPDR_MODE]));
}

/* dataref 扫描：枚举全部已注册 dataref，把名称含应答机/TCAS 关键词的路径
 * 写入 xconnect.log（前缀 "dref:"）。用于发现机型自定义 dataref（如 ToLiss
 * 的应答机 1/2 号选择、STBY/AUTO/ON 模式），在无法从打包文件中读取时，
 * 以运行中的真实 dataref 树为准。SDK 400+（XPLMCountDataRefs）。 */
static void discover_matching_datarefs(void) {
#if kXPLM_Version >= 400
    static const char* kws[] = { "tcas", "TCAS", "xpdr", "XPDR",
                                 "transponder", "Transponder" };
    const int total = XPLMCountDataRefs();
    int found = 0;
    int i;
    for (i = 0; i < total; i++) {
        XPLMDataRef ref = NULL;
        XPLMDataRefInfo_t info;
        unsigned k;
        XPLMGetDataRefsByIndex(i, 1, &ref);
        if (!ref) continue;
        memset(&info, 0, sizeof(info));
        info.structSize = (int)sizeof(info);
        XPLMGetDataRefInfo(ref, &info);
        if (!info.name) continue;
        for (k = 0; k < sizeof(kws) / sizeof(kws[0]); k++) {
            if (strstr(info.name, kws[k])) {
                xconnect_logf("dref: %s", info.name);
                found++;
                break;
            }
        }
    }
    xconnect_logf("dref: scanned=%d matched=%d", total, found);
#endif
}

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
    static int dref_scanned = 0;

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

        /* 启动约 1s 后扫描一次 dataref（此时机型已加载，自定义 dataref 已注册） */
        if (!dref_scanned && counter >= 10) {
            dref_scanned = 1;
            discover_matching_datarefs();
        }

        /* 应答机自定义旋钮值周期转储（每秒一次，校准用） */
        dump_xpdr_values();
        /* 机型 ICAO 识别源周期转储（每秒一次，诊断用） */
        dump_acf_values();

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