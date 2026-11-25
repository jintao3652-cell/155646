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
#include "xstr.h"

/* 桥协议版本（客户端握手/诊断用） */
#define XCONNECT_PROTOCOL 1
#define XCONNECT_VERSION  "1.0.0"
#define XCONNECT_CAPABILITIES \
    "\"csl\",\"parts\",\"controls\",\"diagnostics\"," \
    "\"position_vi\",\"position_visual\",\"acc\""

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
static volatile int g_running = 0;
/* 网络线程句柄：卸载时需等待其真正退出，再销毁锁/CSL */
#ifdef _WIN32
static HANDLE g_net_thread = NULL;
#else
static pthread_t g_net_thread;
static int g_net_thread_started = 0;
#endif
static double g_last_send = -1.0;   /* xc_now_sec 绝对时间 */
static int g_atc_count = 0;
/* 手动重扫 dataref 的请求标志：网络线程置位，主线程消费（XPLM 非线程安全） */
static volatile int g_scan_request = 0;

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

/* 日志滚动：>8MB 时 xconnect.log -> .1 -> .2 -> .3。
 * 每秒最多检查一次，避免频繁 stat；并发轮转的窗口极小。 */
static void maybe_rotate_log(void) {
    static double last_check = 0.0;
    double now = xc_now_sec();
    FILE* f;
    long sz;
    char a[2100], b[2100];
    int i;
    if (now - last_check < 1.0) return;
    last_check = now;
    f = fopen(g_log_path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fclose(f);
    if (sz < 8L * 1024L * 1024L) return;
    snprintf(a, sizeof(a), "%s.%d", g_log_path, 3);
    remove(a);
    for (i = 2; i >= 1; i--) {
        snprintf(a, sizeof(a), "%s.%d", g_log_path, i);
        snprintf(b, sizeof(b), "%s.%d", g_log_path, i + 1);
        remove(b);
        rename(a, b);
    }
    snprintf(b, sizeof(b), "%s.1", g_log_path);
    remove(b);
    rename(g_log_path, b);
}

/* 写诊断日志（插件目录 xconnect.log）。飞行的每一步关键入口都记录，
 * 崩溃后可从最后一行定位阶段。 */
void xconnect_logf(const char* fmt, ...) {
    FILE* f;
    char ts[32];
    va_list ap;
    if (!g_log_path[0]) return;
    maybe_rotate_log();
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

/* ---------------- 网络线程 ---------------- */

/* 反向控制报文处理器（定义在下方；handle_client_line 先用到，故先声明） */
static void handle_radio_command(const Json* root);
static void handle_xpdr_command(const Json* root);
/* 定义在下方；handle_client_line 解析远端 ACC 部件时先用到 */
static int jnum_field(const Json* obj, const char* key, double* out);

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
                                    memset(p, 0, sizeof(*p));   /* 含 model（inst_count=0/tried=0） */
                                    p->active = 1;
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
                            double agl_m = -1.0;   /* 未知：客户端未提供 */
                            int on_ground = 0;
                            const Json* jg;
                            GETSTR("aircraft", p->icao);
                            GETSTR("airline", p->airline);
                            GETSTR("registration", p->reg);
                            GETSTR("aircraft_family", p->family);
                            GETNUM("latitude", lat);
                            GETNUM("longitude", lon);
                            GETNUM("altitude", alt_m);
                            alt_m *= 0.3048;        /* ft -> m */
                            GETNUM("pitch", pitch);
                            GETNUM("bank", bank);
                            GETNUM("heading", heading);
                            GETNUM("ground_speed", gs);
                            /* altitude_agl（ft）：^/#SL/#ST 包携带的离地高度，
                             * 缺失/负值表示未知 */
                            jg = json_get(el, "altitude_agl");
                            if (jg && jg->type == JSON_NUM && jg->num >= 0.0)
                                agl_m = jg->num * 0.3048;
                            /* on_ground：遥测 onGround 标志（bool 或 0/1） */
                            jg = json_get(el, "on_ground");
                            if (jg) {
                                if (jg->type == JSON_BOOL) on_ground = jg->bval ? 1 : 0;
                                else if (jg->type == JSON_NUM) on_ground = jg->num != 0.0;
                            }
                            /* 远端 ACC 部件：客户端把对方的
                             * $CQ:@94836:ACC 增量合并后随 roster 下发。
                             * 之前插件把远端灯光硬编码为全亮，这里改用真实值。 */
                            {
                                const Json* parts = json_get(el, "parts");
                                if (parts && parts->type == JSON_OBJ && parts->child) {
                                    const Json* lights;
                                    double pv;
                                    p->has_parts = 1;
                                    lights = json_get(parts, "lights");
                                    if (lights && lights->type == JSON_OBJ) {
                                        unsigned mask = 0;
                                        if (jnum_field(lights, "beacon_on", &pv) && pv != 0.0)
                                            mask |= 1u;
                                        if (jnum_field(lights, "landing_on", &pv) && pv != 0.0)
                                            mask |= 2u;
                                        if (jnum_field(lights, "taxi_on", &pv) && pv != 0.0)
                                            mask |= 4u;
                                        if (jnum_field(lights, "nav_on", &pv) && pv != 0.0)
                                            mask |= 8u;
                                        if (jnum_field(lights, "strobe_on", &pv) && pv != 0.0)
                                            mask |= 16u;
                                        p->lights = mask;
                                    }
                                    if (jnum_field(parts, "flaps_pct", &pv)) {
                                        int fp = (int)(pv + 0.5);
                                        if (fp < 0) fp = 0;
                                        if (fp > 100) fp = 100;
                                        p->flaps_pct = fp;
                                    }
                                    if (jnum_field(parts, "gear_down", &pv)) {
                                        p->gear_down = pv != 0.0;
                                        p->has_gear = 1;
                                    }
                                    if (jnum_field(parts, "spoilers_out", &pv))
                                        p->spoilers_out = pv != 0.0;
                                    /* 引擎运转状态：{"1":{"on":true},"2":{"on":true}} */
                                    {
                                        const Json* eng = json_get(parts, "engines");
                                        if (eng && eng->type == JSON_OBJ) {
                                            unsigned emask = 0;
                                            const Json* e;
                                            for (e = eng->child; e; e = e->next) {
                                                int idx;
                                                double onv;
                                                if (!e->key || e->type != JSON_OBJ)
                                                    continue;
                                                idx = atoi(e->key);
                                                if (idx >= 1 && idx <= 4 &&
                                                    jnum_field(e, "on", &onv) &&
                                                    onv != 0.0)
                                                    emask |= 1u << (idx - 1);
                                            }
                                            p->engines_on = (int)emask;
                                        }
                                    }
                                    if (!p->logged_parts ||
                                        p->parts_log_gear != p->gear_down ||
                                        p->parts_log_flaps != p->flaps_pct ||
                                        p->parts_log_engines != p->engines_on ||
                                        p->parts_log_lights != p->lights) {
                                        p->logged_parts = 1;
                                        p->parts_log_gear = p->gear_down;
                                        p->parts_log_flaps = p->flaps_pct;
                                        p->parts_log_engines = p->engines_on;
                                        p->parts_log_lights = p->lights;
                                        xconnect_logf("parts: %s gear=%d(has=%d) "
                                                      "flaps=%d eng=%d lights=%u",
                                                      p->callsign, p->gear_down,
                                                      p->has_gear, p->flaps_pct,
                                                      p->engines_on, p->lights);
                                    }
                                }
                            }
                            p->last_seen = xc_now_sec();   /* 非主线程：不使用 XPLM API */
                            push_fix(p, lat, lon, alt_m, heading, pitch,
                                     bank, gs, agl_m, on_ground, p->last_seen);
                        }
                    }
                    /* 不在列表中的玩家由超时机制（PLAYER_TIMEOUT）清理 */
                    LOCK_LEAVE(g_players_lock);
                    {
                        static int last_n = -1;
                        if (n != last_n) {
                            last_n = n;
                            xconnect_logf("players updated (n=%d)", n);
                        }
                    }
                }
            } else if (strcmp(jt->sval, "Send:ATC") == 0) {
                const Json* data = json_get(root, "data");
                if (data && data->type == JSON_ARR) {
                    LOCK_ENTER(g_players_lock);
                    g_atc_count = json_len(data);
                    LOCK_LEAVE(g_players_lock);
                }
            } else if (strcmp(jt->sval, "Send:RADIO") == 0) {
                handle_radio_command(root);
            } else if (strcmp(jt->sval, "Send:XPDR") == 0) {
                handle_xpdr_command(root);
            } else if (strcmp(jt->sval, "Send:SCAN") == 0) {
                /* 手动重扫 dataref（诊断用）：换了机模之后想重新枚举，
                 * 不必重启 X-Plane。结果写进 xconnect.log 的 dref: 行。
                 * 注意必须在主线程执行（XPLM 非线程安全），这里只置标志。 */
                g_scan_request = 1;
            } else if (strcmp(jt->sval, "hello") == 0 ||
                       strcmp(jt->sval, "Hello") == 0) {
                /* 桥协议 v1 握手：客户端主动询问；同时 connected 已带同样信息 */
                LOCK_ENTER(g_sock_lock);
                if (g_client_sock != NET_INVALID) {
                    net_send_line(g_client_sock,
                        "{\"type\":\"hello_ack\",\"protocol\":1,"
                        "\"plugin\":\"XConnect\","
                        "\"version\":\"" XCONNECT_VERSION "\","
                        "\"capabilities\":[" XCONNECT_CAPABILITIES "]}");
                }
                LOCK_LEAVE(g_sock_lock);
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

/* ---------------- 反向控制报文解析（网络线程） ----------------
 *
 * 只入队，实际写 dataref 由主线程的 xconnect_control_drain() 完成
 * （XPLM 非线程安全）。报文格式与 MConnect（MSFS）完全一致。 */

/* 取数值字段（数字或 bool 都接受） */
static int jnum_field(const Json* obj, const char* key, double* out) {
    const Json* t = json_get(obj, key);
    if (!t) return 0;
    if (t->type == JSON_NUM) { *out = t->num; return 1; }
    if (t->type == JSON_BOOL) { *out = t->bval ? 1.0 : 0.0; return 1; }
    return 0;
}

/* Send:RADIO
 *   {"type":"Send:RADIO","com":1,"active":true,"freq":122800}
 *   {"type":"Send:RADIO","com":1,"swap":true}         主备互换（走命令）
 *   {"type":"Send:RADIO","com":1,"flip":true}         同上（别名）
 *   {"type":"Send:RADIO","com":1,"tx":true}           切发话通道（走命令）
 *   {"type":"Send:RADIO","com":1,"monitor":true}      收听开关 toggle（走命令）
 *   {"type":"Send:RADIO","com":1,"power":0}           COM 电源 on/off（走命令）
 *
 * 为什么互换/发话/收听/电源走命令而不是写 dataref：
 * 机模内部监听的是 X-Plane 命令，直接改 dataref 会让机模面板与实际值脱节
 * （ToLiss/Fenix 尤其明显）。MobiFlight 也是这么分的：DataRef 写 vs Command 触发。 */
static void handle_radio_command(const Json* root) {
    XCommand c;
    double v;
    int com = 1, active = 1, swap = 0;
    const Json* t;

    memset(&c, 0, sizeof(c));
    if (jnum_field(root, "com", &v)) com = (int)v;
    if (com < 1 || com > 3) {
        xconnect_logf("control: RADIO bad com=%d", com);
        return;
    }
    t = json_get(root, "active");
    if (t) active = (t->type == JSON_BOOL) ? (t->bval ? 1 : 0)
                                          : (int)(t->num != 0.0);
    if (jnum_field(root, "swap", &v) && v != 0.0) swap = 1;

    /* 命令型操作（互斥，按顺序判定） */
    if (swap) {
        c.kind = XCMD_COM_FLIP;      /* 命令路径，机模会同步面板 */
        c.com_index = com;
        xconnect_control_post(&c);
        return;
    }
    if (jnum_field(root, "flip", &v) && v != 0.0) {
        /* swap 的别名，语义相同 */
        c.kind = XCMD_COM_FLIP;
        c.com_index = com;
        xconnect_control_post(&c);
        return;
    }
    /* 只在值为真时触发。此前只看键是否存在，"tx": false / "monitor": false
     * 也会执行一次 toggle，语义错误。 */
    if (jnum_field(root, "tx", &v) && v != 0.0) {
        c.kind = XCMD_AUDIO_TX;
        c.com_index = com;
        xconnect_control_post(&c);
        return;
    }
    if (jnum_field(root, "monitor", &v) && v != 0.0) {
        c.kind = XCMD_AUDIO_MON;
        c.com_index = com;
        xconnect_control_post(&c);
        return;
    }
    t = json_get(root, "power");
    if (t) {
        int on = (t->type == JSON_BOOL) ? (t->bval ? 1 : 0)
                                        : (int)(t->num != 0.0);
        c.kind = XCMD_COM_POWER;
        c.com_index = com;
        c.active = on;
        xconnect_control_post(&c);
        return;
    }

    if (!jnum_field(root, "freq", &v) || v <= 0.0) {
        xconnect_logf("control: RADIO missing freq");
        return;
    }
    {
        long hz = xconnect_mhz1000_to_hz((int)(v + 0.5));
        if (hz <= 0) {
            xconnect_logf("control: RADIO freq out of band: %.0f", v);
            return;
        }
        c.kind = XCMD_COM_SET;
        c.com_index = com;
        c.active = active;
        c.hz = hz;
        xconnect_control_post(&c);
    }
}

/* Send:XPDR
 *   {"type":"Send:XPDR","code":"1234"} / {"code":1234}
 *   {"type":"Send:XPDR","state":3}    */
static void handle_xpdr_command(const Json* root) {
    XCommand c;
    double v;
    const Json* t;

    memset(&c, 0, sizeof(c));

    t = json_get(root, "code");
    if (t) {
        int code = -1;
        if (t->type == JSON_STR) {
            code = xconnect_parse_squawk(t->sval);
        } else if (t->type == JSON_NUM) {
            int n = (int)(t->num + 0.5);
            if (n >= 0 && n <= 7777) {
                char tmp[8];
                snprintf(tmp, sizeof(tmp), "%04d", n);
                code = xconnect_parse_squawk(tmp);
            }
        }
        if (code < 0) {
            xconnect_logf("control: XPDR bad code");
            return;
        }
        c.kind = XCMD_XPNDR_CODE;
        c.code = code;
        xconnect_control_post(&c);
        return;
    }

    if (jnum_field(root, "state", &v)) {
        int st = (int)(v + 0.5);
        if (st < 0) st = 0;
        if (st > 7) st = 7;      /* 0=Off..7=TA/RA（含 TA only / TA-RA） */
        c.kind = XCMD_XPNDR_MODE;
        c.mode = st;
        xconnect_control_post(&c);
        return;
    }

    /* IDENT：transponder_id 是只读 dataref，只能触发命令 */
    t = json_get(root, "ident");
    if (t) {
        int on = (t->type == JSON_BOOL) ? (t->bval ? 1 : 0)
                                        : (int)(t->num != 0.0);
        if (on) {
            c.kind = XCMD_XPNDR_IDENT;
            xconnect_control_post(&c);
            return;
        }
        /* ident=false 无法"取消"——命令是一次性脉冲，忽略并记日志 */
        xconnect_logf("control: XPDR ident=false ignored (momentary)");
        return;
    }

    xconnect_logf("control: XPDR no actionable field");
}

static THREAD_RET net_thread(THREAD_ARG arg) {
    (void)arg;
    g_listen_sock = net_listen(LISTEN_PORT);
    if (g_listen_sock == NET_INVALID) {
        xconnect_logf("net: listen %d failed", LISTEN_PORT);
        return 0;
    }
    if (!g_running) {   /* 创建线程与插件卸载竞态：立刻关闭新监听并退出 */
        net_close(g_listen_sock);
        g_listen_sock = NET_INVALID;
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

        net_send_line(sock,
            "{\"type\":\"connected\",\"protocol\":1,"
            "\"plugin\":\"XConnect\",\"version\":\"" XCONNECT_VERSION "\","
            "\"sim\":\"xplane\",\"capabilities\":[" XCONNECT_CAPABILITIES "]}");

        for (;;) {
            /* 玩家列表单行 JSON 可能很大（数百玩家），放宽到 256KB */
            static char line[256 * 1024];
            int n;
            if (!g_running) break;   /* 卸载中：尽快退出连接循环 */
            n = net_read_line(sock, line, sizeof(line));
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
        if (p->model.inst_count > 0) {
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
        int n;
        /* ⚠️ 用 XSTR_APPEND 而不是 `n += snprintf(...)`。
         *
         * `n += snprintf` 是错的：截断时 snprintf 返回"本应写入的长度"，
         * 该值大于实际剩余空间 → n 越过 sizeof(line) → 下一轮
         * `sizeof(line) - n` 溢出成巨大 size_t → 越界写破坏堆栈。
         *
         * 这里**真能触发**：PLAYER_MAX=256，loaded 和 missing 各最多 256
         * 个 ICAO，每个 `"XXXX",` 约 8 字节，两组加起来 4096 字节刚好卡在
         * 边界上，满员 + 长机型码就溢出。XSTR_APPEND 会自己夹住位置。 */
        n = snprintf(line, sizeof(line),
                     "{\"type\":\"csl_status\",\"loaded\":[");
        if (n < 0) n = 0;
        if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
        for (i = 0; i < ln; i++)
            XSTR_APPEND(line, n, "%s\"%s\"", i ? "," : "", loaded[i]);
        XSTR_APPEND(line, n, "],\"missing\":[");
        for (i = 0; i < mn; i++)
            XSTR_APPEND(line, n, "%s\"%s\"", i ? "," : "", missing[i]);
        XSTR_APPEND(line, n, "]}");
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
        /* 客户端请求重扫（Send:SCAN）：换机模后无需重启 X-Plane */
        if (g_scan_request) {
            g_scan_request = 0;
            xconnect_logf("dref: rescan requested by client");
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
                if (now - g_last_send_log >= 30.0) {
                    g_last_send_log = now;
                    xconnect_logf("send flight_data rc=%d hasClient=%d",
                                  rc, g_client_sock != NET_INVALID);
                }
                g_last_send = now;
            }
        }

        update_players(xc_now_sec());

        /* 反向控制：执行客户端排队的应答机/频率写入（XPLM 只能主线程调） */
        xconnect_control_drain();
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
    /* 第一件事：初始化日志路径并记录入口，任何后续崩溃都有现场 */
    {
        char sys[1024];
        XPLMGetSystemPath(sys);
        snprintf(g_log_path, sizeof(g_log_path),
                 "%sResources%splugins%sXConnect%sxconnect.log",
                 sys, XPLMGetDirectorySeparator(),
                 XPLMGetDirectorySeparator(), XPLMGetDirectorySeparator());
    }
    xconnect_logf("XConnect v29 (built %s %s): entry", __DATE__, __TIME__);
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
        xconnect_logf("  net init FAILED -> abort start");
        g_running = 0;
        lock_done();
        return 0;   /* 启动失败必须返回 0，否则 X-Plane 认为插件已加载但网络不可用 */
    }
    xconnect_logf("  step: net init done");

    load_all_datarefs();
    xconnect_logf("  step: datarefs done");

    /* 机型适配表（可选）：放在插件目录下，缺失/损坏都只是退回通用 dataref。
     * 用于 ToLiss/Fenix 这类把无线电存进私有 dataref 的机模。 */
    {
        char sys[1024], rp[1200];
        XPLMGetSystemPath(sys);
        snprintf(rp, sizeof(rp), "%sResources%splugins%sXConnect%sradio.json",
                 sys, XPLMGetDirectorySeparator(),
                 XPLMGetDirectorySeparator(), XPLMGetDirectorySeparator());
        radio_table_load(rp);
    }
    xconnect_logf("  step: radio table done");

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
    g_net_thread = CreateThread(NULL, 0, net_thread, NULL, 0, NULL);
    if (!g_net_thread) xconnect_logf("  net thread create FAILED");
#else
    if (pthread_create(&g_net_thread, NULL, net_thread, NULL) == 0)
        g_net_thread_started = 1;
    else
        xconnect_logf("  net thread create FAILED");
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
        net_shutdown(g_listen_sock);   /* 唤醒阻塞中的 accept（Linux/macOS） */
        net_close(g_listen_sock);
        g_listen_sock = NET_INVALID;
    }
    /* 只 shutdown 不 close 客户端 socket：让网络线程的 recv 返回，
     * 由它自己关闭，避免两个线程对同一 fd 的 double-close。 */
    LOCK_ENTER(g_sock_lock);
    if (g_client_sock != NET_INVALID) net_shutdown(g_client_sock);
    LOCK_LEAVE(g_sock_lock);

    /* 等网络线程真正退出后，再销毁锁与 CSL 资源 */
#ifdef _WIN32
    if (g_net_thread) {
        WaitForSingleObject(g_net_thread, 3000);
        CloseHandle(g_net_thread);
        g_net_thread = NULL;
    }
#else
    if (g_net_thread_started) {
        pthread_join(g_net_thread, NULL);
        g_net_thread_started = 0;
    }
#endif
    g_client_sock = NET_INVALID;

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