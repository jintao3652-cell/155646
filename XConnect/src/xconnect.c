/* XConnect — 插件入口、飞行循环、网络线程、客户端协议解析、CSL 状态汇总。
 *
 * 模块拆分：
 *   xconnect.c  本文件（入口 / 飞行循环 / 网络线程 / 协议解析）
 *   datarefs.c  本机飞行数据 dataref 采集与 flight_data 构建
 *   players.c   远程玩家表与位置时间插值平滑（CSL 渲染驱动）
 *   csl.c / net.c / json.c  基础设施
 *
 * 编译需 X-Plane SDK（XPLM.h / XPLMInstance.h 等）。
 */
#include "xconnect.h"
#include "datarefs.h"
#include "players.h"
#include "labels.h"

static char g_log_path[2048] = "";

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

void* g_players_lock = NULL;   /* pthread_mutex_t / CRITICAL_SECTION */
void* g_sock_lock = NULL;

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

/* 写诊断日志（插件目录 xconnect.log）。飞行的每一步关键入口都记录，
 * 崩溃后可从最后一行定位阶段。 */
void xconnect_logf(const char* fmt, ...) {
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

/* ---------------- 主飞行循环 ---------------- */

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
    xconnect_logf("XConnect v8 (built %s %s): entry", __DATE__, __TIME__);
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

    load_all_datarefs();
    xconnect_logf("  step: datarefs done");

    csl_init();
    XPLMDebugString(XCONNECT_NAME
        ": CSL root: Resources/plugins/XConnect/CSL\n");
    xconnect_logf("  step: csl done");

    /* 呼叫号标签（2D 顶层绘制回调） */
    xconnect_logf(labels_init() ? "  step: labels ok"
                                : "  step: labels register FAILED");

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

    labels_shutdown();
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