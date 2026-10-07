/* XConnect — 玩家表：远程玩家位置历史、时间插值平滑与 CSL 渲染驱动。
 *
 * 参考 swift-project/pilotclient 的插件层设计：网络位置按时间戳进入历史缓冲，
 * 渲染时用「渲染时刻 = now - 延迟」在历史中找包夹该时刻的相邻两帧做线性插值
 * （航向取最短弧）。关键：渲染滞后必须 ≥ 位置更新间隔（+ 余量）。
 */
#ifndef XCONNECT_PLAYERS_H
#define XCONNECT_PLAYERS_H

#include "xconnect.h"

/* ---------------- 位置平滑插值参数 ---------------- */

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

typedef struct {
    char callsign[24];
    char icao[8];
    char family[64];
    char model_icao[8];    /* 当前已加载 CSL 模型的机型（icao 变化时换型重载） */
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

extern Player g_players[PLAYER_MAX];

/* 追加一帧到环形缓冲（网络线程，持有玩家表锁）。
 * 同时间戳的帧直接覆盖，并以 EWMA 更新帧到达平均间隔。 */
void push_fix(Player* p, double lat, double lon, double alt_m,
              double heading, double pitch, double roll, double gs,
              double now);

/* 核心：按时间轴计算当前渲染位置（插值/外推/停住/吸附） */
void interpolate_player(Player* p, double now);

/* 取第 i 帧（0 = 最新）；返回 NULL 表示越界 */
const PositionFix* fix_at(const Player* p, int i);

/* 每帧：超时清理 → CSL 加载 → 插值 → 写入 CSL 实例。持有玩家表锁。 */
void update_players(double now);

#endif /* XCONNECT_PLAYERS_H */