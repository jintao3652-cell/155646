/* XConnect — X-Plane 数据桥接 + CSL 渲染插件。
 *
 * - TCP 监听 51001，向客户端发送本机飞行数据（JSON 行，5Hz）
 * - 接收 Send:OPD:FSD9 玩家列表，用 XPLMInstance 渲染 CSL 模型
 * - 接收 Send:ATC 管制列表（预留；当前仅记录数量）
 *
 * 编译需 X-Plane SDK（XPLM.h / XPLMInstance.h 等）。
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#define DATA_INTERVAL 0.2f   /* 秒（5Hz） */
#define LOOP_INTERVAL 0.05f  /* 主循环节拍 */

/* ---------------- 本机飞行数据 dataref ---------------- */

typedef enum {
    DR_LAT, DR_LON, DR_ELEV, DR_AGL, DR_PITCH, DR_BANK, DR_TRUE_HEAD,
    DR_MAG_HEAD, DR_GS, DR_IAS, DR_VSI,
    DR_COM1, DR_COM2, DR_XPDR_CODE, DR_XPDR_MODE,
    DR_GEAR, DR_ONGROUND, DR_FLAPS, DR_THROTTLE,
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
};

static XPLMDataRef g_drefs[DR_COUNT];

/* 安全读取：对空引用/数组类 dataref 也不会崩溃 */
static float dref_float(XPLMDataRef r) {
    int t;
    if (!r) return 0.0f;
    t = XPLMGetDataRefTypes(r);
    if (t & xplmType_Float) {
        float v = 0.0f;
        XPLMGetDataf(r, &v);
        return v;
    }
    if (t & xplmType_FloatArray) {
        float v = 0.0f;
        XPLMGetDatavf(r, &v, 0, 1);
        return v;
    }
    if (t & xplmType_Int) {
        int v = 0;
        XPLMGetDatai(r, &v);
        return (float)v;
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
    if (t & xplmType_Int) {
        int v = 0;
        XPLMGetDatai(r, &v);
        return v;
    }
    if (t & xplmType_IntArray) {
        int v = 0;
        XPLMGetDatavi(r, &v, 0, 1);
        return v;
    }
    return (int)dref_float(r);
}

/* ---------------- 玩家表 ---------------- */

typedef struct {
    char callsign[24];
    char icao[8];
    char family[64];
    double lat, lon;
    double alt_m;        /* MSL 米 */
    double heading, pitch, roll;
    double gs_kt;
    double last_seen;    /* XPLMGetElapsedTime() */
    int active;
    CslNode model;
} Player;

static Player g_players[PLAYER_MAX];
static net_sock_t g_client_sock = NET_INVALID;
static net_sock_t g_listen_sock = NET_INVALID;
static int g_running = 0;
static double g_last_send = 0.0;
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

/* ---------------- 网络线程 ---------------- */

static void send_line_nolock(const char* line) {
    if (g_client_sock != NET_INVALID)
        net_send_line(g_client_sock, line);
}

static void handle_client_line(const char* line) {
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

                    GETSTR("aircraft", p->icao);
                    GETSTR("aircraft_family", p->family);
                    GETNUM("latitude", p->lat);
                    GETNUM("longitude", p->lon);
                    GETNUM("altitude", p->alt_m);
                    p->alt_m *= 0.3048;          /* ft -> m */
                    GETNUM("pitch", p->pitch);
                    GETNUM("bank", p->roll);
                    GETNUM("heading", p->heading);
                    GETNUM("ground_speed", p->gs_kt);
                    p->last_seen = xc_now_sec();   /* 非主线程：不使用 XPLM API */
                }
                /* 不在列表中的玩家由超时机制（PLAYER_TIMEOUT）清理 */
                LOCK_LEAVE(g_players_lock);
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
}

#ifdef _WIN32
static DWORD WINAPI net_thread(LPVOID arg) {
#else
static void* net_thread(void* arg) {
#endif
    (void)arg;
    g_listen_sock = net_listen(LISTEN_PORT);
    if (g_listen_sock == NET_INVALID) return 0;

    while (g_running) {
        net_sock_t sock = net_accept(g_listen_sock);
        if (sock == NET_INVALID) break;   /* 监听被关闭 */

        LOCK_ENTER(g_sock_lock);
        if (g_client_sock != NET_INVALID) net_close(g_client_sock);
        g_client_sock = sock;
        LOCK_LEAVE(g_sock_lock);

        net_send_line(sock, "{\"type\":\"connected\",\"version\":1}");

        for (;;) {
            char line[16384];
            int n = net_read_line(sock, line, sizeof(line));
            if (n <= 0) break;
            handle_client_line(line);
        }

        LOCK_ENTER(g_sock_lock);
        if (g_client_sock == sock) g_client_sock = NET_INVALID;
        LOCK_LEAVE(g_sock_lock);
        net_close(sock);
    }
    return 0;
}

/* ---------------- 主循环 ---------------- */

static void build_flight_data(char* out, int size) {
    long com1 = (long)(dref_float(g_drefs[DR_COM1]) / 1000.0);
    long com2 = (long)(dref_float(g_drefs[DR_COM2]) / 1000.0);
    const char* squark = dref_int(g_drefs[DR_XPDR_MODE]) >= 2 ? "S" : "N";

    snprintf(out, size,
        "{\"type\":\"flight_data\","
        "\"latitude\":%.6f,\"longitude\":%.6f,"
        "\"altitude_msl\":%.1f,\"altitude_agl\":%.1f,\"elevation\":%.1f,"
        "\"pitch\":%.2f,\"bank\":%.2f,\"heading\":%.2f,\"true_heading\":%.2f,"
        "\"groundspeed\":%.1f,\"indicated_airspeed\":%.1f,\"vertical_speed\":%.1f,"
        "\"com1_freq\":%ld,\"com2_freq\":%ld,"
        "\"transponder\":%d,\"squark\":\"%s\",\"gear_deploy\":%.2f,"
        "\"flaps_ratio\":%.2f,\"throttle_ratio\":%.2f}",
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

static void update_players(double now) {
    float anim[CSL_MAX_DATAREFS];
    int i;
    LOCK_ENTER(g_players_lock);
    for (i = 0; i < PLAYER_MAX; i++) {
        Player* p = &g_players[i];
        if (!p->active) continue;

        /* 超时清理 */
        if (now - p->last_seen > PLAYER_TIMEOUT) {
            csl_release_node(&p->model);
            p->active = 0;
            continue;
        }
        if (!p->model.inst)
            csl_load(&p->model, p->icao, p->family);
        if (p->model.inst) {
            anim[0] = 0.0f;                 /* flap */
            anim[1] = 0.0f;                 /* gear */
            anim[2] = 0.0f;                 /* throttle */
            anim[3] = 1.0f;                 /* battery on */
            anim[4] = 1.0f;                 /* beacon */
            anim[5] = 1.0f;                 /* nav light */
            anim[6] = 0.0f;                 /* left brake */
            anim[7] = 0.0f;                 /* right brake */
            anim[8] = 1.0f;                 /* xpdr available */
            anim[9] = 0.0f;                 /* xpdr code */
            anim[10] = 3.0f;                /* xpdr mode ALT */
            csl_set_position(p->model.inst, p->lat, p->lon, p->alt_m,
                             p->heading, p->pitch, p->roll,
                             anim, csl_dataref_count());
        }
    }
    LOCK_LEAVE(g_players_lock);
}

static float xconnect_flight_loop(float elapsedMe, float elapsedSim,
                                  int counter, void* refcon) {
    (void)refcon;
    char line[4096];
    int hasClient = 0;

    LOCK_ENTER(g_sock_lock);
    hasClient = (g_client_sock != NET_INVALID);
    if (hasClient && (elapsedMe - g_last_send) >= DATA_INTERVAL) {
        build_flight_data(line, sizeof(line));
        send_line_nolock(line);
        g_last_send = elapsedMe;
    }
    LOCK_LEAVE(g_sock_lock);

    update_players(xc_now_sec());
    return LOOP_INTERVAL;
}

/* ---------------- 插件入口 ---------------- */

PLUGIN_API int XPluginStart(const char* host, const char* sdk,
                            char* outName, char* outSig, char* outDesc) {
    int i;

    (void)host;
    (void)sdk;

    strcpy(outName, XCONNECT_NAME);
    strcpy(outSig, XCONNECT_SIG);
    strcpy(outDesc, XCONNECT_DESC);

    lock_init();
    g_running = 1;
    if (net_init() != 0) {
        XPLMDebugString(XCONNECT_NAME ": network init failed\n");
        return 1;
    }

    for (i = 0; i < DR_COUNT; i++)
        g_drefs[i] = XPLMFindDataRef(kDrefNames[i]);

    csl_init();
    XPLMDebugString(XCONNECT_NAME
        ": CSL root: Resources/plugins/XConnect/CSL\n");

    XPLMRegisterFlightLoopCallback(xconnect_flight_loop, 1.0, NULL);

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

    return 1;
}

PLUGIN_API void XPluginStop(void) {
    int i;
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

    XPLMUnregisterFlightLoopCallback(xconnect_flight_loop, NULL);

    for (i = 0; i < PLAYER_MAX; i++)
        csl_release_node(&g_players[i].model);

    csl_shutdown();
    lock_done();
}

PLUGIN_API void XPluginEnable(void) {
}

PLUGIN_API int XPluginDisable(void) {
    return 1;
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID inFrom, int inMsg,
                                      void* inParam) {
    (void)inFrom;
    (void)inMsg;
    (void)inParam;
}