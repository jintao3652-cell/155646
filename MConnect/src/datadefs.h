/* MConnect — 本机飞行数据：SimConnect 数据定义、读取与 flight_data 构建。
 *
 * 输出报文与 XConnect 的 flight_data **字段、单位、语义完全一致**，
 * XFlySim-Client 无需改动即可识别。差异只在数据来源：
 *   XConnect 读 XPLM dataref；MConnect 读 SimConnect SimVar。
 *
 * 单位约定（与协议一致，避免每帧换算）：
 *   经纬度  → 度（SimVar 原生 Radians，读取时换算）
 *   高度    → 英尺
 *   速度    → 节
 *   垂直速度 → 英尺/分（SimVar 原生 ft/s，读取时 ×60）
 *   频率    → MHz*1000（SimVar Frequency BCD16 → 逐位解码后 ×1000）
 */
#ifndef MCONNECT_DATADEFS_H
#define MCONNECT_DATADEFS_H

#include "mconnect.h"

/* 数据定义 / 请求 ID（客户端自定义，须全局唯一） */
#define DEF_FLIGHT      1    /* 本机飞行数据（5Hz 轮询） */
#define REQ_FLIGHT      1
#define DEF_AI_POS      2    /* AI 飞机的写入用位置定义 */
#define DEF_XPNDR_STATE 3    /* 应答机模式写入（TRANSPONDER STATE，可写 SimVar） */
#define REQ_SYSSTATE    4
#define REQ_CPU_LOAD    5

/* AI 飞机位置写入结构。**字段顺序必须与 ai_define_position() 完全一致**，
 * 否则 SetDataOnSimObject 会报 SIZE_MISMATCH。
 * 全部用 FLOAT64 以便直接写 double，免去 float/double 转换误差。 */
typedef struct {
    double latitude;      /* degrees */
    double longitude;     /* degrees */
    double altitude;      /* feet */
    double pitch;         /* degrees */
    double bank;          /* degrees */
    double heading;       /* degrees, true */
    double on_ground;     /* 1 = 在地面（0/1） */
    double velocity_x;    /* ft/s，机身右向 */
    double velocity_y;    /* ft/s，机身垂直 */
    double velocity_z;    /* ft/s，机身前向 */
    double vel_body_x;    /* 兼容：同上（部分 SU 需要 VELOCITY BODY 而非 WORLD） */
    double vel_body_y;
    double vel_body_z;
} AiPoseRaw;

/* 本机数据快照（从 SIMOBJECT_DATA 解析出的原始值，未做协议换算） */
typedef struct {
    int    valid;                 /* 至少收到过一帧 */
    double lat_deg, lon_deg;
    double alt_msl_ft;            /* 真高（PLANE ALTITUDE） */
    double alt_agl_ft;            /* 离地高（PLANE ALT ABOVE GROUND） */
    double alt_indicated_ft;      /* 座舱高度表 */
    double pitch_deg, bank_deg;
    double heading_mag_deg, heading_true_deg;
    double gs_kt, ias_kt, vs_fpm;
    int    com1_mhz1000, com2_mhz1000, com3_mhz1000;
    int    com1_stby_mhz1000, com2_stby_mhz1000, com3_stby_mhz1000;
    /* 发射/接收选择（语音通讯关键状态）。XVoice 用 PILOT/COPILOT TRANSMITTER
     * TYPE + COM TRANSMIT:n 判"飞行员在哪个频率讲话"，MConnect 对齐这套。 */
    int    com_transmit[3];       /* COM TRANSMIT:1..3，各台是否在发（0/1） */
    int    pilot_xmit_type;       /* PILOT TRANSMITTER TYPE: 0=COM1 1=COM2 2=COM3 */
    int    copilot_xmit_type;     /* COPILOT TRANSMITTER TYPE，同上 */
    int    com_receive_all;       /* COM RECEIVE ALL：是否全收 */
    int    audio_panel_on;        /* ELECTRICAL MASTER BATTERY（音频面板供电） */
    int    transponder;           /* 4 位八进制码（如 1234） */
    int    xpdr_state;            /* 0=Off 1=Stby 2=Test 3=On 4=Alt 5=Ground */
    int    on_ground;
    double gear_ratio;            /* 0=收起 1=放下（GEAR HANDLE POSITION → 用总位置） */
    double flaps_ratio;           /* 0..1 */
    double throttle_ratio;        /* 0..1（1 号发动机） */
    double spoilers_ratio;        /* 0..1 */
    int    light_landing, light_taxi, light_nav, light_beacon, light_strobe;
    int    num_engines;
    double n1_pct[4];             /* 各发 N1（%），不足 4 发填 0 */
    char   title[256];            /* 机型容器标题（TITLE） */
    char   atc_id[32];            /* 尾号（ATC ID） */
    char   atc_airline[32];       /* 航司（ATC AIRLINE） */
    char   atc_model[32];         /* ATC MODEL（常为机型名而非 ICAO） */
    char   icao_type[16];         /* 由 catalog 反查得到的 ICAO 机型代码 */
    int    parking_brake;
} FlightRaw;

extern FlightRaw g_flight;

/* 注册本机飞行数据定义并发起周期请求（sc_open 之后调用）。返回 0 成功。 */
int  datadefs_init(void);

/* 注册 AI 位置写入定义。返回 0 成功。 */
int  datadefs_register_ai(void);

/* 注册应答机模式写入定义（反向控制用，可写 SimVar TRANSPONDER STATE）。 */
int  datadefs_register_xpdr(void);

/* 处理 SIMCONNECT_RECV_SIMOBJECT_DATA（由派发回调转交）。 */
void datadefs_on_simobject_data(const void* pkt, DWORD cbData);

/* 应答机状态：按当前机型档案用 LVar 覆盖标准 SimVar 的读取值。
 * 标准 SimVar TRANSPONDER STATE 只有默认机维护，深度机模（FBW/Fenix/A220）
 * 会一直报 0=Off，导致"写成功但界面显示没生效"。在 on_simobject_data 里调用。 */
void datadefs_apply_profile_xpdr(void);

/* 换机型时调用：清掉旧的 LVar 读取表达式，下次读取重新注册。 */
void datadefs_reset_profile_reads(void);

/* 构建 flight_data JSON 行（写入 out）。 */
void datadefs_build_flight_json(char* out, int size);

/* 供 players.c 使用：把 AiPoseRaw 填成当前要写入的值（角度/速度换算集中在此）。 */
void datadefs_fill_ai_pose(AiPoseRaw* pose, double lat, double lon, double alt_ft,
                           double heading_deg, double pitch_deg, double bank_deg,
                           double gs_kt, int on_ground);

/* 当前模拟是否已进入飞行（RequestSystemState "Sim"）。 */
int  datadefs_sim_running(void);

/* 本机机型 ICAO（catelog 反查后填入 g_flight.icao_type 的结果）。 */
const char* datadefs_icao(void);

/* 推导"飞行员当前发射台"，返回 0..2（COM1..3），-1 = 未选中/未知。
 *
 * 为什么需要交叉验证：`PILOT TRANSMITTER TYPE` 在部分机型上没实现（恒为 0），
 * 直接信它会把"实际在 COM3 讲话"误报成 COM1 —— 语音通讯里这是严重错误
 * （管制听到的频率和客户端显示的对不上）。因此同时看 `COM TRANSMIT:n`：
 *   只有一台在发       → 以它为准（最可信）
 *   多台同时为真       → 取枚举指定的那台（枚举可靠时）
 *   都没在发           → 报枚举值（面板选了台但没按 PTT）
 *   两者都不可用       → -1
 * 抽成纯函数是为了能脱离模拟器做单元测试。 */
int datadefs_derive_pilot_xmit(const int com_transmit[3], int raw_enum);

/* 处理 SIMCONNECT_RECV_SYSTEM_STATE（"Sim" 状态）。 */
void datadefs_on_system_state(const void* pkt);

#endif /* MCONNECT_DATADEFS_H */