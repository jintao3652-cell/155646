/* MConnect — 玩家表、位置时间插值、AI 对象注入与位置驱动。
 *
 * 与 XConnect 的差异（同样是"显示别人"这件事，机制完全不同）：
 *
 *   XConnect: XPLMInstance 加载 CSL 模型 → 每个渲染帧写实例位置。
 *             位置由我们自己完全控制，想放哪就放哪。
 *
 *   MConnect: SimConnect 让模拟器创建**真实的 AI 飞机**（AICreateNonATCAircraft），
 *             然后每帧用 SetDataOnSimObject 改它的位置/姿态/速度。
 *             模拟器自己也在跑 AI 逻辑（会尝试执行飞行计划、撞地处理等），
 *             因此这里必须：
 *               - 每帧都写位置（AI 系统会漂移/回弹）；
 *               - 同时写速度分量（只写位置会被当成瞬移而抖动）；
 *               - 对象 ID 由模拟器异步分配（ASSIGNED_OBJECT_ID），创建请求与
 *                 对象 ID 必须靠 RequestID 配对，不能假设创建立即成功。
 *
 * 平滑沿用 XConnect 的时间插值（环形缓冲 + 自适应渲染滞后 + 最短弧航向），
 * 这部分逻辑与平台无关，直接照搬——它解决的是**网络帧到达不均匀**问题，
 * 与模拟器无关。
 */
#ifndef MCONNECT_PLAYERS_H
#define MCONNECT_PLAYERS_H

#include "mconnect.h"
#include "datadefs.h"

/* ---------------- 位置平滑插值参数（与 XConnect 一致） ---------------- */

#define RENDER_DELAY          6.0   /* 秒：常规更新渲染滞后 */
#define RENDER_DELAY_FAST     1.5   /* 秒：快速更新（间隔 <1.5s） */
#define RENDER_DELAY_MAX      8.0   /* 秒：自适应上限 */
#define RENDER_DELAY_RESERVE  1.0   /* 秒：期望间隔之外的余量 */
#define EXTRAP_MAX_SEC        3.0   /* 秒：越过最新帧后的短时外推 */
#define SNAP_DIST_M           60000.0 /* 米：跳变判定阈值（重连/重生） */
#define GROUND_CLAMP_AGL_M    8.0   /* 米：AGL 低于此值视为地面 */

/* MSFS 的 AI 飞机由模拟器持有控制权，长时间不写位置会被 AI 逻辑接管
 * （开始自己飞/滑行）。因此"心跳写入"间隔要远小于 XConnect 的 50ms 节拍
 * 能容忍的范围——这里固定 50Hz（与主循环同频）。 */
#define AI_WRITE_INTERVAL     0.02

#define FIX_MAX 6

typedef struct {
    double lat, lon;   /* 度 */
    double alt;        /* 米 MSL（内部统一米，写 SimVar 时换 ft） */
    double heading, pitch, roll;
    double gs;         /* 节 */
    double agl;        /* 米；<0 = 未知 */
    int    on_ground;
    double ts;
} PositionFix;

/* AI 对象的生命周期状态 */
typedef enum {
    AI_STATE_IDLE = 0,      /* 无对象（未请求或已移除） */
    AI_STATE_PENDING,       /* 已发创建请求，等 ASSIGNED_OBJECT_ID */
    AI_STATE_ACTIVE,        /* 有有效 ObjectID，正在每帧驱动 */
    AI_STATE_FAILED         /* 创建被拒（机型不存在等），按退避重试 */
} AiState;

typedef struct {
    char callsign[24];
    char icao[8];
    char airline[8];
    char reg[16];
    char family[64];

    /* 已生效的模型选择（用于判断是否需要重建 AI 对象） */
    char model_title[112];
    char model_livery[64];
    char model_icao[8];
    char model_airline[8];

    /* 网络位置历史 + 当前渲染位置 */
    PositionFix fixes[FIX_MAX];
    int fix_count;
    int fix_head;
    double avg_interval;
    double cur_lat, cur_lon, cur_alt;
    double cur_heading, cur_pitch, cur_roll;
    int have_first_fix;

    double last_seen;
    double last_write;
    double last_snap_log;
    double last_parts_log;

    /* AI 对象 */
    AiState state;
    DWORD   object_id;
    DWORD   create_request_id;   /* 创建时用的 RequestID，与回包配对 */
    int     create_attempts;
    double  retry_at;            /* 下次允许重试的时间 */
    int     write_fail_logged;

    /* 远端 canonical 部件（随 roster 下发，与 XConnect 同 schema） */
    int     has_parts;
    int     has_gear;
    int     gear_down;
    int     flaps_pct;
    int     spoilers_out;
    unsigned lights;          /* bit0 beacon ... bit5 logo */
    unsigned engines_on;      /* bit0..bit7 = 1..8 号发 */
    int     engine_count;
    char    engine_type[4];
    char    gear_type[12];
    char    parts_source[16];
    unsigned lights_sent;     /* 已发过的灯状态，只在变化时触发事件 */
    int     parts_write_failed; /* 该 SimVar 对此 AI 不可写 → 全局禁用 */

    int active;
} Player;

extern Player g_players[PLAYER_MAX];

/* 玩家表锁（CRITICAL_SECTION，由 mconnect.c 初始化）。网络线程与主循环共用。 */
extern void* g_players_lock;

/* 追加一帧位置（网络线程调用，持锁）。 */
void push_fix(Player* p, double lat, double lon, double alt_m,
              double heading, double pitch, double roll, double gs,
              double agl_m, int on_ground, double now);

void interpolate_player(Player* p, double now);
const PositionFix* fix_at(const Player* p, int i);

/* 主循环每帧：超时清理 → 建/换 AI 对象 → 插值 → 写位置。持锁。
 * 必须在 SimConnect 派发线程（主线程）调用：SimConnect 非线程安全。 */
void update_players(double now);

/* 处理 ASSIGNED_OBJECT_ID（创建成功回包）。 */
void players_on_assigned_object(DWORD request_id, DWORD object_id);

/* 移除全部 AI 对象（断开连接/退出时）。 */
void players_remove_all(void);

/* 统计：已激活对象数 / 待创建数（状态上报用）。 */
void players_stats(int* active, int* pending, int* loaded_icao_n,
                   char loaded[][8], int cap);

/* 分配一个新的创建 RequestID（全局单调，避免与回包错配）。 */
DWORD players_next_request_id(void);

/* AI 部件/灯光写入能力探测结果（--parts-probe 用）。 */
void players_parts_status(int* parts_writable, int* parts_tried,
                          int* lights_ok, int* lights_tried);

#endif /* MCONNECT_PLAYERS_H */