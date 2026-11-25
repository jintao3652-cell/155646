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
#define GROUND_CLAMP_AGL_M  8.0   /* 米：AGL 低于此值视为地面，模型贴地（消除悬空） */

/* 遥测 onGround 为真时的落地吸附：
 * FSD @ 包的 alt 是**发送方的气压/指示高度基准**，QNH 偏离 1013 时与真高相差
 * 可达数百英尺；同时 CSL 模型原点在轮胎接地点，而遥测高度是飞机参考点（重心），
 * 大型机在地面就有 2~7m 的差。两者叠加就是"机体悬空"。因此只要对方报 onGround
 * 且速度不高，就直接用地形探针高度落地，容错窗口覆盖上述误差。 */
#define GROUND_SNAP_WINDOW_M 400.0 /* 米：onGround 吸附允许的高度偏差窗口（±） */
#define GROUND_SNAP_MAX_GS   100.0 /* kt：超过此速度不按 onGround 吸附（防误标志） */
#define GROUND_CLAMP_SINK_M  60.0  /* 米：AGL 低于 -此值视为穿地，不再抬高（避免误判） */

/* 位置历史环形缓冲：fixes[fix_head] 为最新帧 */
#define FIX_MAX 6

typedef struct {
    double lat, lon;   /* 度（WGS84） */
    double alt;        /* 米（MSL） */
    double heading, pitch, roll; /* 度 */
    double gs;         /* 地速（kt，与协议一致） */
    double agl;        /* 米：遥测离地高度（^/#SL/#ST 携带），<0 表示未知 */
    int    on_ground;  /* 遥测 onGround 标志（PBH bit1 / #SB VI） */
    double ts;         /* 接收时刻（xc_now_sec） */
} PositionFix;

typedef struct {
    char callsign[24];
    char icao[8];
    char airline[8];       /* 航司/涂装码（PI AIRLINE/LIVERY 或呼号前三位） */
    char reg[16];          /* 飞机注册号（尾号）：对端 PI REG= 或本机 acf_tailnum */
    char family[64];
    char model_icao[8];    /* 当前已加载 CSL 模型的机型（icao 变化时换型重载） */
    char model_airline[8]; /* 当前已加载 CSL 涂装码（airline 变化时换漆重载） */
    char model_reg[16];    /* 当前已加载 CSL 注册号（reg 变化时换漆重载） */
    /* 网络位置历史（最新在前）与当前渲染位置（平滑插值输出） */
    PositionFix fixes[FIX_MAX];
    int fix_count;   /* 有效帧数（≤ FIX_MAX） */
    int fix_head;    /* 最新帧下标 */
    double avg_interval;   /* 帧到达平均间隔（EWMA，秒），用于自适应渲染滞后 */
    double cur_lat, cur_lon, cur_alt;
    double cur_heading, cur_pitch, cur_roll;
    int have_first_fix;
    double last_seen;    /* xc_now_sec */
    double last_snap_log; /* 上次落地吸附诊断日志时间（节流） */
    double tire_angle;   /* 轮胎累计转角（由地速积分） */
    double last_anim_t;  /* 上次动画更新时间（积分 dt） */
    /* 远端 ACC 机模部件（客户端合并后随 roster 下发） */
    int has_parts;       /* 是否收到过远端 ACC */
    int has_gear;        /* 是否收到过 gear_down（没收到则用地面启发值） */
    int flaps_pct;       /* 0..100 */
    int gear_down;       /* 0/1 */
    int spoilers_out;    /* 0/1 */
    unsigned lights;     /* bit0 beacon, bit1 landing, bit2 taxi, bit3 nav, bit4 strobe */
    int engines_on;      /* bit0..bit3 = 1..4 号引擎在运转 */
    int logged_parts;    /* 是否已记录首条 ACC 部件诊断日志 */
    int logged_anim;     /* 是否已记录首帧动画值诊断日志 */
    int active;
    CslNode model;
} Player;

extern Player g_players[PLAYER_MAX];

/* 追加一帧到环形缓冲（网络线程，持有玩家表锁）。
 * 同时间戳的帧直接覆盖，并以 EWMA 更新帧到达平均间隔。
 * agl_m 为遥测 AGL（米），未知填 <0；on_ground 为遥测 onGround 标志。 */
void push_fix(Player* p, double lat, double lon, double alt_m,
              double heading, double pitch, double roll, double gs,
              double agl_m, int on_ground, double now);

/* 核心：按时间轴计算当前渲染位置（插值/外推/停住/吸附） */
void interpolate_player(Player* p, double now);

/* 取第 i 帧（0 = 最新）；返回 NULL 表示越界 */
const PositionFix* fix_at(const Player* p, int i);

/* 每帧：超时清理 → CSL 加载 → 插值 → 写入 CSL 实例。持有玩家表锁。 */
void update_players(double now);

#endif /* XCONNECT_PLAYERS_H */