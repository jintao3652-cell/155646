/* XConnect — 玩家表与位置时间插值平滑。 */
#include "players.h"

Player g_players[PLAYER_MAX];

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
const PositionFix* fix_at(const Player* p, int i) {
    if (i < 0 || i >= p->fix_count) return NULL;
    return &p->fixes[(p->fix_head - i + FIX_MAX * 2) % FIX_MAX];
}

/* 追加一帧到环形缓冲（网络线程，持有 g_players_lock）。
 * 同时间戳的帧直接覆盖（Swift push_frontKeepLatestAdjustedFirst 的行为），
 * 同时以 EWMA 更新帧到达平均间隔（供自适应渲染滞后使用）。 */
void push_fix(Player* p, double lat, double lon, double alt_m,
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

/* ---------------- CSL 渲染驱动 ---------------- */

void update_players(double now) {
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
        if (p->model.inst_count <= 0) {
            if (!p->model.tried) {
                csl_load(&p->model, p->icao, p->family, p->airline);
                if (p->model.inst_count > 0) {
                    strncpy(p->model_icao, p->icao, sizeof(p->model_icao) - 1);
                    p->model_icao[sizeof(p->model_icao) - 1] = '\0';
                    strncpy(p->model_airline, p->airline,
                            sizeof(p->model_airline) - 1);
                    p->model_airline[sizeof(p->model_airline) - 1] = '\0';
                    xconnect_logf("csl: %s loaded icao=%s airline=%s variant=%s objs=%d",
                                  p->callsign, p->icao, p->airline,
                                  p->model.variant, p->model.inst_count);
                } else {
                    xconnect_logf("csl: %s LOAD FAILED icao=%s airline=%s",
                                  p->callsign, p->icao, p->airline);
                }
            }
        } else if (strncmp(p->model_icao, p->icao,
                           sizeof(p->model_icao)) != 0 ||
                   (p->airline[0] &&
                    strncmp(p->model_airline, p->airline,
                            sizeof(p->model_airline)) != 0)) {
            /* 机型或涂装变化：释放旧模型并重载 */
            xconnect_logf("csl: %s icao %s->%s airline %s->%s (reload)",
                          p->callsign, p->model_icao, p->icao,
                          p->model_airline, p->airline);
            csl_release_node(&p->model);
            p->model_icao[0] = '\0';
            p->model_airline[0] = '\0';
            csl_load(&p->model, p->icao, p->family, p->airline);
            if (p->model.inst_count > 0) {
                strncpy(p->model_icao, p->icao, sizeof(p->model_icao) - 1);
                p->model_icao[sizeof(p->model_icao) - 1] = '\0';
                strncpy(p->model_airline, p->airline,
                        sizeof(p->model_airline) - 1);
                p->model_airline[sizeof(p->model_airline) - 1] = '\0';
                xconnect_logf("csl: %s loaded icao=%s airline=%s variant=%s (reloaded)",
                              p->callsign, p->icao, p->airline, p->model.variant);
            } else {
                xconnect_logf("csl: %s RELOAD FAILED icao=%s",
                              p->callsign, p->icao);
            }
        }
        if (p->model.inst_count <= 0) continue;

        /* 时间插值：按历史帧与当前时间计算渲染位置（详见 interpolate_player） */
        interpolate_player(p, now);
        f = fix_at(p, 0);   /* 最新网络帧，用于动画判断 */
        if (!f) continue;

        /* 动画值按 csl.c::kCslDataRefs 索引顺序填写（与模型 OBJ 键一一对应）。
         * 远程玩家无襟翼/油门/灯光遥传，可取部分用启发值，其余置 0。 */
        memset(anim, 0, sizeof(anim));
        {
            const float gear = (f->alt < 60.0 || f->gs < 40.0) ? 1.0f : 0.0f;
            anim[0] = gear;   /* gear_ratio      */
            anim[1] = gear;   /* gear_ratio[0]   */
            anim[2] = gear;   /* gear_ratio[1]   */
            anim[3] = gear;   /* gear_ratio[2]   */
            anim[14] = 1.0f;  /* taxi_lites_on   */
            anim[15] = 1.0f;  /* landing_lites_on*/
            anim[16] = 1.0f;  /* beacon_lites_on */
            anim[17] = 1.0f;  /* strobe_lites_on */
            anim[18] = 1.0f;  /* nav_lites_on    */
        }
        /* 4-13 操纵面/推力/反推、19-33 轮胎/风扇/触地：保持 0 */

        /* 地面钳制：远程上报的高度是飞机参考点（重心）海拔，而 X-CSL 模型原点
         * 在轮胎接地点。近地时把模型落到该点地形高度，避免整机悬空、轮子离地
         * （XPMP2 的 clampToGround 语义；地形探针仅主线程调用）。 */
        {
            double alt = p->cur_alt;
            /* 仅对近地/低速目标探测地形（巡航目标跳过，避免每帧探针开销） */
            if (f->gs < 60.0 || p->cur_alt < 200.0) {
                int hit = 0;
                double terrain = csl_terrain_msl(p->cur_lat, p->cur_lon, alt, &hit);
                if (hit) {
                    double agl = alt - terrain;
                    if (agl <= GROUND_CLAMP_AGL_M && agl > -20.0) alt = terrain;
                }
            }
            csl_set_position(&p->model, p->cur_lat, p->cur_lon, alt,
                             p->cur_heading, p->cur_pitch, p->cur_roll,
                             anim, csl_dataref_count());
        }
    }
    LOCK_LEAVE(g_players_lock);
}