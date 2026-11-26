/* MConnect — 玩家表、位置插值、AI 对象注入与位置驱动。 */
#include "players.h"
#include "catalog.h"
#include "sc.h"

#include <stdio.h>
#include <string.h>

Player g_players[PLAYER_MAX];

/* AI 创建请求 ID 池：从 1000 起，与固定请求 ID（1/3/4）错开，
 * 且每个玩家独占一段，回包时靠 request_id → 玩家槽位反查。 */
#define AI_REQ_BASE 1000
#define AI_REQ_SPAN 4          /* 每玩家预留 4 个（重建时轮转，避免同一 ID 复用过快） */
static DWORD g_ai_req_seq = 0;

DWORD players_next_request_id(void) {
    g_ai_req_seq++;
    return AI_REQ_BASE + g_ai_req_seq;
}

/* 探针模式：跳过"等待机型目录"门（--parts-probe 只有 12s 窗口，
 * 而全量扫描要 40s+；探针用内置 C172 即可）。 */
static int g_skip_catalog_wait = 0;
void players_set_probe_mode(int on) { g_skip_catalog_wait = on; }

/* ---------------- 平滑插值（与 XConnect 同构） ---------------- */

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

const PositionFix* fix_at(const Player* p, int i) {
    if (i < 0 || i >= p->fix_count) return NULL;
    return &p->fixes[(p->fix_head - i + FIX_MAX * 2) % FIX_MAX];
}

void push_fix(Player* p, double lat, double lon, double alt_m,
              double heading, double pitch, double roll, double gs,
              double agl_m, int on_ground, double now) {
    const PositionFix* newest = fix_at(p, 0);
    PositionFix* slot;

    if (newest && fabs(now - newest->ts) < 1e-3) {
        slot = &p->fixes[p->fix_head];
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
    slot->agl = agl_m;
    slot->on_ground = on_ground;
    slot->ts = now;
}

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

static void snap_to(const PositionFix* f, Player* p) {
    p->cur_lat = f->lat;
    p->cur_lon = f->lon;
    p->cur_alt = f->alt;
    p->cur_heading = f->heading;
    p->cur_pitch = f->pitch;
    p->cur_roll = f->roll;
}

static void lerp_fix_to(const PositionFix* a, const PositionFix* b,
                        double t, Player* p) {
    p->cur_lat = lerp(a->lat, b->lat, t);
    p->cur_lon = lerp(a->lon, b->lon, t);
    p->cur_alt = lerp(a->alt, b->alt, t);
    p->cur_heading = lerp_angle(a->heading, b->heading, t);
    p->cur_pitch = lerp(a->pitch, b->pitch, t);
    p->cur_roll = lerp(a->roll, b->roll, t);
}

void interpolate_player(Player* p, double now) {
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
        snap_to(newest, p);
        return;
    }
    if (!older) {
        snap_to(newest, p);
        return;
    }

    t_render = now - player_delay(p);

    for (i = 0; i + 1 < p->fix_count; i++) {
        seg_b = fix_at(p, i);
        seg_a = fix_at(p, i + 1);
        seg_dt = seg_b->ts - seg_a->ts;
        if (seg_dt <= 1e-6) continue;
        if (t_render >= seg_a->ts && t_render <= seg_b->ts) {
            t = (t_render - seg_a->ts) / seg_dt;
            lerp_fix_to(seg_a, seg_b, t, p);
            return;
        }
    }

    oldest = fix_at(p, p->fix_count - 1);
    if (oldest && t_render < oldest->ts) {
        snap_to(oldest, p);
        return;
    }

    seg_dt = newest->ts - older->ts;
    if (seg_dt > 1e-6 && t_render > newest->ts &&
        t_render - newest->ts <= EXTRAP_MAX_SEC) {
        lerp_fix_to(older, newest, (t_render - older->ts) / seg_dt, p);
        return;
    }

    snap_to(newest, p);
}

/* ---------------- AI 对象管理 ---------------- */

/* 释放一个 AI 对象（发 AIRemoveObject）。模拟器在下一帧真正删除，
 * 因此这里只清本地状态，不等回包。 */
static void ai_remove(Player* p) {
    if (p->state == AI_STATE_ACTIVE && p->object_id && g_sc.AIRemoveObject) {
        g_sc.AIRemoveObject(g_sc_handle, p->object_id, players_next_request_id());
        xc_logf("ai: %s remove object=%lu", p->callsign,
                (unsigned long)p->object_id);
    }
    p->state = AI_STATE_IDLE;
    p->object_id = 0;
    p->create_request_id = 0;
    p->write_fail_logged = 0;
}

/* 创建 AI 飞机。level 仅用于日志。
 *
 * 2024 与 2020 的差异在这里落地：
 *   - 有 _EX1（2024 SU1+）：先试 _EX1 + livery。模块化机型（MSFS 2024 自带的
 *     全部官方机）只有 _EX1 才能带涂装；legacy 机型 _EX1 也支持（livery 传空）。
 *   - 无 _EX1（2020）：只能用基础版，且 livery 概念不存在。
 *   - _EX1 失败（机型不支持/涂装名错）时退回基础版再试一次，避免因为
 *     涂装名不匹配而整架飞机消失。 */
static void ai_create(Player* p) {
    CatalogMatch m;
    SC_DATA_INITPOSITION pos;
    HRESULT hr = E_FAIL;
    DWORD req;
    int use_ex1;
    const PositionFix* f = fix_at(p, 0);

    if (!g_sc_handle) return;

    /* 机型目录扫描未完成时不创建。FSLTL/AIG 全量扫描要 40s+，若在此
     * 窗口内用内置兜底表建出来，扫完后虽会自动换模型（update 循环的
     * model-change 检测），但用户会先看到一架 Asobo 通用白机再"跳"成
     * AIG/FSLTL —— 观感上就是"映射没生效"。给 90s 上限：目录彻底失败
     * 时也能开始渲染（届时退回内置表，比永远不显示好）。 */
    if (!catalog_ready() && !g_skip_catalog_wait) {
        static double s_scan_deadline = 0.0;
        double now_c = xc_now_sec();
        if (!s_scan_deadline) s_scan_deadline = now_c + 90.0;
        if (now_c < s_scan_deadline) {
            if (!p->catalog_wait_logged) {
                xc_logf("ai: %s 等待机型目录扫描完成（已扫 %d 条，含 FSLTL/AIG）",
                        p->callsign, catalog_count());
                p->catalog_wait_logged = 1;
            }
            return;   /* 保持 IDLE，下一轮再试 */
        }
        /* 超时：放行，走内置兜底 */
    }

    catalog_match(p->icao, p->airline, p->reg, &m);
    if (!m.found) {
        xc_logf("ai: %s no catalog match icao=%s airline=%s reg=%s",
                p->callsign, p->icao, p->airline, p->reg);
        p->state = AI_STATE_FAILED;
        p->retry_at = xc_now_sec() + 30.0;   /* 机型目录可能还没扫完，晚点再试 */
        return;
    }

    /* 匹配结果行：明确记录用了哪个来源的模型（AIGAIM_/FSLTL_/Asobo），
     * 排查"映射没生效"时与 --scan-only 抽样对照 */
    xc_logf("ai: %s match lvl=%d icao=%s airline=%s reg=%s -> \"%s\"/\"%s\"",
            p->callsign, m.level, p->icao, p->airline, p->reg,
            m.title, m.livery);

    /* 记住已选的模型（换型/换漆判断用） */
    snprintf(p->model_title, sizeof(p->model_title), "%s", m.title);
    snprintf(p->model_livery, sizeof(p->model_livery), "%s", m.livery);
    snprintf(p->model_icao, sizeof(p->model_icao), "%s", p->icao);
    snprintf(p->model_airline, sizeof(p->model_airline), "%s", p->airline);

    memset(&pos, 0, sizeof(pos));
    if (f) {
        pos.Latitude = f->lat;
        pos.Longitude = f->lon;
        pos.Altitude = f->alt * 3.280839895;   /* m → ft */
        pos.Pitch = f->pitch;
        pos.Bank = f->roll;
        pos.Heading = f->heading;
        pos.OnGround = f->on_ground ? 1 : 0;
        pos.Airspeed = (DWORD)(f->gs > 0.0 ? f->gs : 0.0);
    }

    req = players_next_request_id();
    p->create_request_id = req;
    p->state = AI_STATE_PENDING;
    p->create_attempts++;

    /* 涂装传递策略（2024 SDK 实测语义：szLivery **先查涂装文件夹名**，
     * 再查 livery.cfg 的 name，都找不到才用默认涂装）：
     *   - 模块化机型：必须走 _EX1（legacy 基础版建不出来），有涂装名就传；
     *   - legacy 机型（FSLTL/AIG 等 2020 包）：有 texture 文件夹名也传 ——
     *     这正是 AIG/FSLTL 的航司涂装选择方式，传对了就显示对应航司涂装，
     *     传错了 SDK 会自动退默认涂装，不会导致创建失败。 */
    use_ex1 = (g_sc.AICreateNonATCAircraft_EX1 != NULL) &&
              (m.modular || m.livery[0]);

    if (use_ex1) {
        hr = g_sc.AICreateNonATCAircraft_EX1(g_sc_handle, m.title,
                                             m.livery[0] ? m.livery : "",
                                             p->reg[0] ? p->reg : "",
                                             pos, req);
        xc_logf("ai: %s create EX1 title=\"%s\" livery=\"%s\" req=%lu -> 0x%08lx",
                p->callsign, m.title, m.livery, (unsigned long)req,
                (unsigned long)hr);
        if (FAILED(hr) && (m.livery[0] || m.modular)) {
            /* 涂装名不被接受（枚举与实际包不一致）→ 退回不带涂装 */
            req = players_next_request_id();
            p->create_request_id = req;
            hr = g_sc.AICreateNonATCAircraft_EX1(g_sc_handle, m.title, "",
                                                 p->reg[0] ? p->reg : "",
                                                 pos, req);
            xc_logf("ai: %s create EX1 (no livery) req=%lu -> 0x%08lx",
                    p->callsign, (unsigned long)req, (unsigned long)hr);
        }
    }

    if (FAILED(hr)) {
        /* 基础版最后兜底：2020 全走这里；2024 上 legacy 机型也能成功 */
        req = players_next_request_id();
        p->create_request_id = req;
        hr = g_sc.AICreateNonATCAircraft(g_sc_handle, m.title,
                                         p->reg[0] ? p->reg : "",
                                         pos, req);
        xc_logf("ai: %s create base title=\"%s\" req=%lu -> 0x%08lx",
                p->callsign, m.title, (unsigned long)req, (unsigned long)hr);
    }

    if (FAILED(hr)) {
        xc_logf("ai: %s CREATE FAILED level=%d icao=%s title=\"%s\"",
                p->callsign, m.level, p->icao, m.title);
        p->state = AI_STATE_FAILED;
        /* 指数退避，最多 60s：机型目录扫完后重试能成功 */
        {
            double back = 5.0 * (double)(p->create_attempts < 5 ? p->create_attempts : 5);
            if (back > 60.0) back = 60.0;
            p->retry_at = xc_now_sec() + back;
        }
    }
    /* 成功时保持 PENDING，等 ASSIGNED_OBJECT_ID 回包 */
}

void players_on_assigned_object(DWORD request_id, DWORD object_id) {
    int i;
    if (request_id < AI_REQ_BASE) return;   /* 非 AI 创建请求 */
    for (i = 0; i < PLAYER_MAX; i++) {
        Player* p = &g_players[i];
        if (!p->active) continue;
        if (p->create_request_id != request_id) continue;
        p->object_id = object_id;
        p->state = AI_STATE_ACTIVE;
        p->write_fail_logged = 0;
        xc_logf("ai: %s object assigned id=%lu (req=%lu) title=\"%s\"",
                p->callsign, (unsigned long)object_id,
                (unsigned long)request_id, p->model_title);
        return;
    }
    xc_logf("ai: ASSIGNED_OBJECT_ID req=%lu id=%lu 无匹配玩家（可能已超时清理）",
            (unsigned long)request_id, (unsigned long)object_id);
}

/* 写位置。返回 1 成功。 */
static int ai_write_pose(Player* p, double now) {
    AiPoseRaw pose;
    const PositionFix* f = fix_at(p, 0);
    HRESULT hr;

    if (!g_sc_handle || !p->object_id) return 0;
    if (now - p->last_write < AI_WRITE_INTERVAL) return 1;
    p->last_write = now;

    /* 地面吸附：AI 系统会让飞机"贴地"，我们给的高度是重心高度（MSL），
     * 有落差时看起来悬空/陷地。地面状态直接用 AGL 把高度压到地形上：
     * 遥测 onGround 为真时，用 高度 - AGL 得到地面高度，再加 0（模型原点
     * 在轮胎接地点，与 XConnect 同样的处理思路）。 */
    {
        double alt_m = p->cur_alt;
        int ground = f ? f->on_ground : 0;

        if (ground && f && f->agl >= 0.0 && p->cur_alt > -1000.0) {
            double terrain = p->cur_alt - f->agl;
            if (fabs(f->agl) < 400.0 / 3.280839895) {
                /* AGL 在 ±400ft 内才 trusting：超出说明遥测基准不对 */
                alt_m = terrain;
            }
        }

        datadefs_fill_ai_pose(&pose, p->cur_lat, p->cur_lon,
                              alt_m * 3.280839895,
                              p->cur_heading, p->cur_pitch, p->cur_roll,
                              f ? f->gs : 0.0, ground);
    }

    hr = g_sc.SetDataOnSimObject(g_sc_handle, SC_DATA_SET_FLAG_DEFAULT,
                                 p->object_id, DEF_AI_POS, 1,
                                 (DWORD)sizeof(AiPoseRaw), &pose);
    if (FAILED(hr)) {
        /* SIZE_MISMATCH 最常见（定义与结构体不一致），只记一次以免刷日志 */
        if (!p->write_fail_logged) {
            p->write_fail_logged = 1;
            xc_logf("ai: %s SetDataOnSimObject failed hr=0x%08lx "
                    "(struct=%d bytes)", p->callsign, (unsigned long)hr,
                    (int)sizeof(AiPoseRaw));
        }
        /* 对象可能已被模拟器删除（撞地/超距）→ 重建 */
        if (hr == (HRESULT)0x80004005L) {
            p->state = AI_STATE_FAILED;
            p->retry_at = now + 5.0;
        }
        return 0;
    }
    return 1;
}

/* ---------------- AI 部件驱动 ---------------- */
/*
 * MSFS 的 AI 对象与 X-Plane 的 CSL 实例能力不同：
 *   - 起落架/襟翼/扰流板/油门：SimVar 写入（DEF_AI_PARTS）
 *   - 灯：LIGHT xxx SimVar 只读，只能按 ObjectID 发 SimConnect 事件
 * 两者都做能力探测：第一次失败就全局禁用并打一条日志，避免每帧刷屏。
 */
#define CEV_AI_LIGHT_ON_BASE 220
static int g_ai_parts_writable = 1;
static int g_ai_parts_write_tried = 0;
static int g_ai_parts_write_ok = 0;
static int g_ai_light_events_ok = 0;
static int g_ai_light_events_tried = 0;

/* `--parts-probe` 用：把能力探测结果交给 main 打印结论。 */
void players_parts_status(int* parts_writable, int* parts_tried,
                          int* lights_ok, int* lights_tried) {
    if (parts_writable) *parts_writable = g_ai_parts_writable;
    if (parts_tried)    *parts_tried = g_ai_parts_write_tried;
    if (lights_ok)      *lights_ok = g_ai_light_events_ok;
    if (lights_tried)   *lights_tried = g_ai_light_events_tried;
}

static const DWORD kAiLightOn[6] = {
    CEV_AI_LIGHT_ON_BASE,      /* beacon */
    CEV_AI_LIGHT_ON_BASE + 2,  /* landing */
    CEV_AI_LIGHT_ON_BASE + 4,  /* taxi */
    CEV_AI_LIGHT_ON_BASE + 6,  /* nav */
    CEV_AI_LIGHT_ON_BASE + 8,  /* strobe */
    CEV_AI_LIGHT_ON_BASE + 10  /* logo */
};
static const char* kAiLightNames[6] = {
    "BEACON_LIGHTS_ON", "LANDING_LIGHTS_ON", "TAXI_LIGHTS_ON",
    "NAV_LIGHTS_ON", "STROBE_LIGHTS_ON", "LOGO_LIGHTS_ON"
};

static void ai_light_events_init(void) {
    int i;
    HRESULT hr = S_OK;
    if (g_ai_light_events_tried) return;
    g_ai_light_events_tried = 1;
    if (!g_sc_handle || !g_sc.MapClientEventToSimEvent) return;
    for (i = 0; i < 6; ++i) {
        char off_name[64];
        snprintf(off_name, sizeof(off_name), "%s", kAiLightNames[i]);
        /* _ON → _OFF：事件名只在末尾替换，映射必须成对 */
        {
            char* p = strstr(off_name, "_ON");
            if (p) memcpy(p, "_OFF", 4);
        }
        hr |= g_sc.MapClientEventToSimEvent(g_sc_handle, kAiLightOn[i],
                                            kAiLightNames[i]);
        hr |= g_sc.MapClientEventToSimEvent(g_sc_handle, kAiLightOn[i] + 1,
                                            off_name);
    }
    if (FAILED(hr)) {
        xc_logf("ai: light event map failed hr=0x%08lx (lights disabled)",
                (unsigned long)hr);
        return;
    }
    g_ai_light_events_ok = 1;
}

static void ai_write_lights(Player* p) {
    int i;
    if (!p->has_parts || !g_sc_handle || !g_sc.TransmitClientEvent) return;
    ai_light_events_init();
    if (!g_ai_light_events_ok || p->lights == p->lights_sent) return;
    for (i = 0; i < 6; ++i) {
        unsigned bit = 1u << i;
        if ((p->lights & bit) == (p->lights_sent & bit)) continue;
        HRESULT hr = g_sc.TransmitClientEvent(
            g_sc_handle, p->object_id,
            kAiLightOn[i] + ((p->lights & bit) ? 0u : 1u), 0,
            SC_GROUP_PRIORITY_HIGHEST,
            SC_EVENT_FLAG_GROUPID_IS_PRIORITY);
        if (FAILED(hr)) {
            g_ai_light_events_ok = 0;
            xc_logf("ai: light event failed hr=0x%08lx on %s (lights disabled)",
                    (unsigned long)hr, p->callsign);
            return;
        }
    }
    p->lights_sent = p->lights;
}

static void ai_write_parts(Player* p, const PositionFix* f) {
    AiPartsRaw raw;
    HRESULT hr;
    int gear;

    if (!p->has_parts || !g_ai_parts_writable ||
        !g_sc_handle || !g_sc.SetDataOnSimObject || !p->object_id) return;

    gear = p->has_gear ? p->gear_down : ((f && f->on_ground) ? 1 : 0);
    if (!p->has_gear && p->gear_type[0] == 'f') gear = 1;
    datadefs_fill_ai_parts(&raw, gear, p->flaps_pct, p->spoilers_out,
                           p->engines_on, p->engine_count,
                           p->has_throttle ? p->throttle : -1.0);

    hr = g_sc.SetDataOnSimObject(g_sc_handle, SC_DATA_SET_FLAG_DEFAULT,
                                 p->object_id, DEF_AI_PARTS, 1,
                                 (DWORD)sizeof(AiPartsRaw), &raw);
    g_ai_parts_write_tried = 1;
    if (SUCCEEDED(hr)) g_ai_parts_write_ok = 1;
    if (FAILED(hr)) {
        g_ai_parts_writable = 0;
        xc_logf("ai: parts SetDataOnSimObject failed hr=0x%08lx on %s "
                "(parts disabled, struct=%d)",
                (unsigned long)hr, p->callsign, (int)sizeof(AiPartsRaw));
    }
}

/* ---------------- 主更新循环 ---------------- */

void update_players(double now) {
    int i;
    LOCK_ENTER(g_players_lock);
    for (i = 0; i < PLAYER_MAX; i++) {
        Player* p = &g_players[i];
        const PositionFix* f;

        if (!p->active) continue;

        /* 超时清理 */
        if (now - p->last_seen > PLAYER_TIMEOUT) {
            ai_remove(p);
            p->active = 0;
            continue;
        }

        /* 机型/涂装变化 → 换模型（先删后建） */
        if (p->state == AI_STATE_ACTIVE || p->state == AI_STATE_PENDING) {
            int changed = 0;
            CatalogMatch m;
            catalog_match(p->icao, p->airline, p->reg, &m);
            if (m.found &&
                (_stricmp(m.title, p->model_title) != 0 ||
                 _stricmp(m.livery, p->model_livery) != 0)) {
                changed = 1;
            }
            if (changed) {
                xc_logf("ai: %s model change \"%s\"/\"%s\" -> \"%s\"/\"%s\"",
                        p->callsign, p->model_title, p->model_livery,
                        m.title, m.livery);
                ai_remove(p);
                p->create_attempts = 0;
            }
        }

        /* 需要建对象 */
        if (p->state == AI_STATE_IDLE || p->state == AI_STATE_FAILED) {
            if (p->state == AI_STATE_FAILED && now < p->retry_at) {
                /* 退避期内不重试 */
            } else if (!fix_at(p, 0)) {
                /* 还没有位置数据，等一帧再建（否则会先出现在 0,0） */
            } else {
                ai_create(p);
            }
        }

        if (p->state != AI_STATE_ACTIVE) continue;

        interpolate_player(p, now);
        f = fix_at(p, 0);
        if (f && p->cur_alt < 0.0) p->cur_alt = 0.0;   /* 防御：负高度异常 */

        ai_write_pose(p, now);
        ai_write_parts(p, f);
        ai_write_lights(p);

        /* 呼号标签：把 ATC ID 写成呼号。MSFS 的内置"机模标签"显示的就是
         * ATC ID —— 用户开启标签（或 MConnect --labels）后即可看到呼号。
         * 只写一次；换模型（ai_remove 清零）后自动重写。 */
        if (!p->atcid_written && p->callsign[0]) {
            if (datadefs_write_ai_atcid(p->object_id, p->callsign) == 0)
                p->atcid_written = 1;
        }

        /* 部件写入诊断（节流 30s，卡断层用：raw → JSON → 动画三层对照） */
        if (p->has_parts && now - p->last_parts_log > 30.0) {
            p->last_parts_log = now;
            xc_logf("anim: %s src=%s gear=%d flaps=%d spoiler=%d "
                    "eng=%d/%d(%s) lights=%u parts_writable=%d lights_ok=%d",
                    p->callsign, p->parts_source[0] ? p->parts_source : "-",
                    p->gear_down, p->flaps_pct, p->spoilers_out,
                    (int)p->engines_on, p->engine_count,
                    p->engine_type[0] ? p->engine_type : "?",
                    p->lights, g_ai_parts_writable, g_ai_light_events_ok);
        }

        /* 地面吸附诊断（节流 10s） */
        if (f && f->on_ground && f->agl > 0.0 && f->agl < 400.0 / 3.280839895 &&
            now - p->last_snap_log > 10.0) {
            p->last_snap_log = now;
            xc_logf("ground-snap %s agl=%.1fm gs=%.0fkt onGround=1",
                    p->callsign, f->agl, f->gs);
        }
    }
    LOCK_LEAVE(g_players_lock);
}

void players_remove_all(void) {
    int i;
    LOCK_ENTER(g_players_lock);
    for (i = 0; i < PLAYER_MAX; i++) {
        if (g_players[i].active || g_players[i].object_id) ai_remove(&g_players[i]);
        g_players[i].active = 0;
    }
    LOCK_LEAVE(g_players_lock);
}

void players_stats(int* active, int* pending, int* loaded_icao_n,
                   char loaded[][8], int cap) {
    int i, a = 0, pd = 0, ln = 0;
    LOCK_ENTER(g_players_lock);
    for (i = 0; i < PLAYER_MAX; i++) {
        Player* p = &g_players[i];
        if (!p->active) continue;
        if (p->state == AI_STATE_ACTIVE) {
            a++;
            if (ln < cap && p->icao[0]) {
                int k, dup = 0;
                for (k = 0; k < ln; k++)
                    if (_stricmp(loaded[k], p->icao) == 0) { dup = 1; break; }
                if (!dup) snprintf(loaded[ln++], 8, "%s", p->icao);
            }
        } else if (p->state == AI_STATE_PENDING) {
            pd++;
        }
    }
    LOCK_LEAVE(g_players_lock);
    if (active) *active = a;
    if (pending) *pending = pd;
    if (loaded_icao_n) *loaded_icao_n = ln;
}