/* MConnect — 本机飞行数据：SimConnect 数据定义、读取与 flight_data 构建。 */
#include "datadefs.h"
#include "sc.h"
#include "catalog.h"
#include "aircraftdb.h"
#include "pmdgbridge.h"
#include "wasmbridge.h"
#include "xstr.h"

#include <stdio.h>
#include <string.h>

FlightRaw g_flight;
static int g_sim_running = 1;
static char g_icao[16] = "";
static char g_last_title[256] = "";   /* 上次 TITLE，用于检测换机模并重新识别 ICAO */

/* ---------------- BCD 解码 ---------------- */

/* Frequency BCD16 → MHz*1000。
 * SimConnect 返回 4 位 16 进制 BCD（0x1228 = 122.800 MHz 按 0.025 步进）。
 * 逐位取十进制数字，拼成 "1228" → 122800（MHz*1000）。
 * 注意：不能按整数直接除，BCD 每位是独立的十进制数字。 */
static int bcd16_to_mhz1000(DWORD raw) {
    unsigned d0 = (raw >> 12) & 0xF;
    unsigned d1 = (raw >> 8) & 0xF;
    unsigned d2 = (raw >> 4) & 0xF;
    unsigned d3 = raw & 0xF;
    unsigned v;
    if (d0 > 9 || d1 > 9 || d2 > 9 || d3 > 9) return 0;   /* 非 BCD（机模未提供）
                                                          * 或频率无效 */
    v = d0 * 1000u + d1 * 100u + d2 * 10u + d3;   /* 1228 */
    return (int)(v * 100u);                        /* 122800 = 122.800 MHz */
}

/* BCO16（八进制 BCD）→ 应答机 4 位码。TRANSPONDER CODE 是 BCO16，
 * 每位 3 bit 有效（0..7），0x1200|0x0234 这种字面值直接当十进制读会错。 */
static int bco16_to_squawk(DWORD raw) {
    unsigned d0 = (raw >> 12) & 0x7;
    unsigned d1 = (raw >> 8) & 0x7;
    unsigned d2 = (raw >> 4) & 0x7;
    unsigned d3 = raw & 0x7;
    return (int)(d0 * 1000 + d1 * 100 + d2 * 10 + d3);
}

/* ---------------- 数据定义 ---------------- */

/* 数据定义的字段顺序 = SIMOBJECT_DATA 里 dwData 之后的内存布局。
 * 结构体必须与此完全对应（见 FlightRaw 的读取函数）。
 * 全部使用固定宽度类型：float64（double）/ int32（DWORD）/ STRING256。 */

static HRESULT add(HANDLE h, SIMCONNECT_DATA_DEFINITION_ID def, const char* name,
                   const char* units, DWORD type) {
    return g_sc.AddToDataDefinition(h, def, name, units, type, 0.0f, SC_UNUSED);
}

/* 读字段的小工具：结构体布局与 add() 的调用顺序严格一一对应。
 * 用独立的读取结构体（而非直接 memcpy 到 FlightRaw）的原因是协议侧字段
 * 有换算（弧度→度、ft/s→fpm、BCD），并且顺序与 FlightRaw 字段顺序不同。 */
/* 通用浮点净化：NaN / Inf / 超范围一律钳回 lo..hi。
 *
 * 为什么必须做：SimConnect 在机模未加载完、变量未实现、或刚切换机型时，
 * 会给出 NaN/Inf。用 `%f` 打印出来是 `nan` / `inf` / `-nan(ind)`，
 * **都不是合法 JSON 字面量** → 客户端 `json.loads` 抛异常 → 整条
 * flight_data 被丢弃（连累所有字段）。XConnect 侧因为 NaN 丢过整个 ACC 包。
 *
 * 注意用 `!(v >= lo && v <= hi)` 而不是 `v < lo || v > hi`：
 * 前者能同时捕获 NaN（NaN 参与任何比较都是 false）。 */
static double sane(double v, double lo, double hi, double fallback) {
    if (!(v >= lo && v <= hi)) return fallback;
    return v;
}

#pragma pack(push, 1)
typedef struct {
    double lat_rad;
    double lon_rad;
    double alt_ft;
    double agl_ft;
    double alt_ind_ft;
    double pitch_rad;
    double bank_rad;
    double hdg_true_rad;
    double hdg_mag_rad;
    double gs_kt;
    double ias_kt;
    double vs_fps;
    double com1_bcd;
    double com2_bcd;
    double com3_bcd;
    double com1s_bcd;
    double com2s_bcd;
    double com3s_bcd;
    DWORD  xpdr_code;
    DWORD  xpdr_state;
    DWORD  com_xmit_1;
    DWORD  com_xmit_2;
    DWORD  com_xmit_3;
    DWORD  pilot_xmit_type;
    DWORD  copilot_xmit_type;
    DWORD  com_receive_all;
    DWORD  master_battery;
    double gear_pct;
    /* 襟翼**实际**展开（TRAILING EDGE FLAPS LEFT/RIGHT PERCENT，0..100）。
     * 不用 FLAPS HANDLE PERCENT —— 那是手柄位置，会让对端提前把襟翼画满。 */
    double flaps_left_pct;
    double flaps_right_pct;
    double throttle_pct;
    double spoilers_pct;
    DWORD  lt_landing;
    DWORD  lt_taxi;
    DWORD  lt_nav;
    DWORD  lt_beacon;
    DWORD  lt_strobe;
    DWORD  on_ground;
    DWORD  num_engines;
    double n1_1;
    double n1_2;
    double n1_3;
    double n1_4;
    char   title[256];
    char   atc_id[32];
    char   atc_airline[32];
    char   atc_model[32];
    DWORD  parking_brake;
} FlightPkt;
#pragma pack(pop)

int datadefs_init(void) {
    HANDLE h = g_sc_handle;
    HRESULT hr = S_OK;

    if (!h) return -1;

    /* 位置与姿态：SimVar 原生是 radians，直接要 degrees 由 SimConnect 换算，
     * 少一次每帧乘法也更不容易写错。单位名 "degrees" 在所有版本都可用。 */
    hr |= add(h, DEF_FLIGHT, "PLANE LATITUDE",  "degrees", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_FLIGHT, "PLANE LONGITUDE", "degrees", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_FLIGHT, "PLANE ALTITUDE",  "feet",    SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_FLIGHT, "PLANE ALT ABOVE GROUND", "feet", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_FLIGHT, "INDICATED ALTITUDE", "feet", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_FLIGHT, "PLANE PITCH DEGREES", "degrees", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_FLIGHT, "PLANE BANK DEGREES",  "degrees", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_FLIGHT, "PLANE HEADING DEGREES TRUE",     "degrees", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_FLIGHT, "PLANE HEADING DEGREES MAGNETIC", "degrees", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_FLIGHT, "GROUND VELOCITY",    "knots",   SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_FLIGHT, "AIRSPEED INDICATED", "knots",   SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_FLIGHT, "VERTICAL SPEED",     "feet per minute", SC_DATATYPE_FLOAT64);

    /* 通信/应答机：Frequency 直接要 "MHz"，SimConnect 会做 BCD→十进制换算，
     * 省掉手写 BCD 解码（实测 2020/2024 都支持）。应答机码仍要 BCO16 原值。 */
    hr |= add(h, DEF_FLIGHT, "COM ACTIVE FREQUENCY:1",  "MHz", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_FLIGHT, "COM ACTIVE FREQUENCY:2",  "MHz", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_FLIGHT, "COM ACTIVE FREQUENCY:3",  "MHz", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_FLIGHT, "COM STANDBY FREQUENCY:1", "MHz", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_FLIGHT, "COM STANDBY FREQUENCY:2", "MHz", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_FLIGHT, "COM STANDBY FREQUENCY:3", "MHz", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_FLIGHT, "TRANSPONDER CODE:1", "BCO16", SC_DATATYPE_INT32);
    hr |= add(h, DEF_FLIGHT, "TRANSPONDER STATE:1", "Enum", SC_DATATYPE_INT32);

    /* 发射/接收选择：语音通讯必需。XVoice 靠这三组判断"飞行员在哪个频率讲话"：
     *   COM TRANSMIT:n            各台是否处于发射状态（Bool，按 1/2/3 索引）
     *   PILOT TRANSMITTER TYPE    飞行员选定的发射台（0=COM1 1=COM2 2=COM3）
     *   COM RECEIVE ALL           是否全收（部分机型/配置下才有意义） */
    hr |= add(h, DEF_FLIGHT, "COM TRANSMIT:1", "Bool", SC_DATATYPE_INT32);
    hr |= add(h, DEF_FLIGHT, "COM TRANSMIT:2", "Bool", SC_DATATYPE_INT32);
    hr |= add(h, DEF_FLIGHT, "COM TRANSMIT:3", "Bool", SC_DATATYPE_INT32);
    hr |= add(h, DEF_FLIGHT, "PILOT TRANSMITTER TYPE", "Enum", SC_DATATYPE_INT32);
    hr |= add(h, DEF_FLIGHT, "COPILOT TRANSMITTER TYPE", "Enum", SC_DATATYPE_INT32);
    hr |= add(h, DEF_FLIGHT, "COM RECEIVE ALL", "Bool", SC_DATATYPE_INT32);
    /* 音频面板供电：断电时无线电实际不可用，客户端据此显示"离线"而不是
     * 把频率当成可用（XVoice 也是同样的判断逻辑）。 */
    hr |= add(h, DEF_FLIGHT, "ELECTRICAL MASTER BATTERY", "Bool", SC_DATATYPE_INT32);

    /* 构型 */
    /* 起落架：TOTAL PCT EXTENDED 是**实际**放下比例（0..100），与 XConnect
     * 用 flightmodel2/gear/deploy_ratio 的语义一致，可直接当 gear_ratio。 */
    hr |= add(h, DEF_FLIGHT, "GEAR TOTAL PCT EXTENDED", "Percent Over 100", SC_DATATYPE_FLOAT64);
    /* 襟翼：**必须用实际展开，不能用 FLAPS HANDLE PERCENT（手柄位置）**。
     *
     * 手柄一拨到 FULL，`FLAPS HANDLE PERCENT` 立刻变成 100，但襟翼要几秒
     * 才真正放到全展。若报手柄位置，对端 CSL 会在你还没放完时就把襟翼
     * 画到全开，与模拟器画面不符 —— 这与 XConnect 侧同一个问题
     * （那边用的是 X-Plane 的 flap_system_deploy_ratio 而非 flap_handle_*）。
     *
     * MSFS 的 `TRAILING EDGE FLAPS LEFT/RIGHT PERCENT` 才是实际偏转百分比。
     * 取左右平均（多数机型左右同步，某些故障/差动会不同）。 */
    hr |= add(h, DEF_FLIGHT, "TRAILING EDGE FLAPS LEFT PERCENT", "Percent Over 100", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_FLIGHT, "TRAILING EDGE FLAPS RIGHT PERCENT", "Percent Over 100", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_FLIGHT, "GENERAL ENG THROTTLE LEVER POSITION:1", "Percent Over 100", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_FLIGHT, "SPOILERS HANDLE POSITION", "Percent Over 100", SC_DATATYPE_FLOAT64);

    /* 灯光（Bool → int32） */
    hr |= add(h, DEF_FLIGHT, "LIGHT LANDING", "Bool", SC_DATATYPE_INT32);
    hr |= add(h, DEF_FLIGHT, "LIGHT TAXI",    "Bool", SC_DATATYPE_INT32);
    hr |= add(h, DEF_FLIGHT, "LIGHT NAV",     "Bool", SC_DATATYPE_INT32);
    hr |= add(h, DEF_FLIGHT, "LIGHT BEACON",  "Bool", SC_DATATYPE_INT32);
    hr |= add(h, DEF_FLIGHT, "LIGHT STROBE",  "Bool", SC_DATATYPE_INT32);

    hr |= add(h, DEF_FLIGHT, "SIM ON GROUND", "Bool", SC_DATATYPE_INT32);
    hr |= add(h, DEF_FLIGHT, "NUMBER OF ENGINES", "Number", SC_DATATYPE_INT32);
    hr |= add(h, DEF_FLIGHT, "TURB ENG N1:1", "Percent", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_FLIGHT, "TURB ENG N1:2", "Percent", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_FLIGHT, "TURB ENG N1:3", "Percent", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_FLIGHT, "TURB ENG N1:4", "Percent", SC_DATATYPE_FLOAT64);

    /* 机型/尾号/涂装：字符串必须显式给 STRING256（不同长度会错位） */
    hr |= add(h, DEF_FLIGHT, "TITLE",       "NULL", SC_DATATYPE_STRING256);
    hr |= add(h, DEF_FLIGHT, "ATC ID",      "NULL", SC_DATATYPE_STRING32);
    hr |= add(h, DEF_FLIGHT, "ATC AIRLINE", "NULL", SC_DATATYPE_STRING32);
    hr |= add(h, DEF_FLIGHT, "ATC MODEL",   "NULL", SC_DATATYPE_STRING32);

    hr |= add(h, DEF_FLIGHT, "BRAKE PARKING POSITION", "Bool", SC_DATATYPE_INT32);

    if (FAILED(hr)) {
        xc_logf("datadefs: AddToDataDefinition failed hr=0x%08lx",
                (unsigned long)hr);
        return -1;
    }

    /* SIM_FRAME 而非 VISUAL_FRAME：AI 飞机位置也由同一循环驱动，
     * 两者同频才不会出现"本机 60fps、他机 30fps"的错配。
     * 用 CHANGED 会漏掉"数值不变但状态需重发"的场景（例如悬停直升机），
     * 而 5Hz 的数据量（约 400 字节）在本地回环上可忽略，故用 DEFAULT。 */
    hr = g_sc.RequestDataOnSimObject(g_sc_handle, REQ_FLIGHT, DEF_FLIGHT,
                                     SC_OBJECT_ID_USER, SC_PERIOD_SIM_FRAME,
                                     SC_DATA_REQUEST_FLAG_DEFAULT, 0, 0, 0);
    if (FAILED(hr)) {
        xc_logf("datadefs: RequestDataOnSimObject failed hr=0x%08lx",
                (unsigned long)hr);
        return -1;
    }

    xc_logf("datadefs: flight definition ready (%d bytes)", (int)sizeof(FlightPkt));
    return 0;
}

/* ---------------- AI 位置写入定义 ---------------- */

int datadefs_register_ai(void) {
    HANDLE h = g_sc_handle;
    HRESULT hr = S_OK;

    if (!h) return -1;

    /* 直接映射到 SIMCONNECT_DATA_INITPOSITION 之外的运行时位置 SimVar。
     * 写这些变量即可让 AI 飞机移动（aircraft 由 AI 系统持有控制权）。 */
    hr |= add(h, DEF_AI_POS, "PLANE LATITUDE",  "degrees", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_AI_POS, "PLANE LONGITUDE", "degrees", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_AI_POS, "PLANE ALTITUDE",  "feet",    SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_AI_POS, "PLANE PITCH DEGREES", "degrees", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_AI_POS, "PLANE BANK DEGREES",  "degrees", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_AI_POS, "PLANE HEADING DEGREES TRUE", "degrees", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_AI_POS, "SIM ON GROUND", "Bool", SC_DATATYPE_FLOAT64);
    /* 速度必须一起写：只改位置不给速度，AI 系统会按"被瞬移"处理并抖动/回跳。
     * 同时给 WORLD 与 BODY 两套，2020/2024 对哪套生效不完全一致，两套都写
     * 能保证任一版本都有平滑运动。 */
    hr |= add(h, DEF_AI_POS, "VELOCITY WORLD X", "feet per second", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_AI_POS, "VELOCITY WORLD Y", "feet per second", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_AI_POS, "VELOCITY WORLD Z", "feet per second", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_AI_POS, "VELOCITY BODY X", "feet per second", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_AI_POS, "VELOCITY BODY Y", "feet per second", SC_DATATYPE_FLOAT64);
    hr |= add(h, DEF_AI_POS, "VELOCITY BODY Z", "feet per second", SC_DATATYPE_FLOAT64);

    if (FAILED(hr)) {
        xc_logf("datadefs: AI definition failed hr=0x%08lx", (unsigned long)hr);
        return -1;
    }
    xc_logf("datadefs: AI pose definition ready (%d bytes)", (int)sizeof(AiPoseRaw));
    return 0;
}

/* ---------------- 应答机模式写入定义 ---------------- */

/* TRANSPONDER STATE 是少数**可写**的无线电类 SimVar（其余频率/码都是只读，
 * 只能走事件）。反向控制设置模式时用 SetDataOnSimObject 写它。
 * 单独一个定义 ID，避免与 AI 位置定义混用（cbUnitSize 不同）。 */
int datadefs_register_xpdr(void) {
    HRESULT hr;

    if (!g_sc_handle) return -1;
    hr = add(g_sc_handle, DEF_XPNDR_STATE, "TRANSPONDER STATE:1", "Enum",
             SC_DATATYPE_FLOAT64);
    if (FAILED(hr)) {
        xc_logf("datadefs: xpdr state definition failed hr=0x%08lx",
                (unsigned long)hr);
        return -1;
    }
    xc_logf("datadefs: xpdr state definition ready");
    return 0;
}

/* ---------------- AI 部件写入定义 ---------------- */

/* 与 XConnect 的 CSL 动画不同，MSFS 的 AI 对象只能通过 SetDataOnSimObject
 * 写少数几个 SimVar。这里选的都是 SimConnect 明确可写的手柄/输入量：
 *   GEAR HANDLE POSITION / TRAILING EDGE FLAPS .. PERCENT /
 *   SPOILERS HANDLE POSITION / GENERAL ENG THROTTLE LEVER POSITION:n
 * 注册成功不等于对某个 AI 对象可写；真正写在 players.c 里检查 HRESULT，
 * 失败一次即全局禁用部件写入并打日志（避免每帧刷屏）。 */
int datadefs_register_ai_parts(void) {
    HRESULT hr = S_OK;

    if (!g_sc_handle) return -1;

    hr |= add(g_sc_handle, DEF_AI_PARTS, "GEAR HANDLE POSITION",
              "Bool", SC_DATATYPE_FLOAT64);
    hr |= add(g_sc_handle, DEF_AI_PARTS, "TRAILING EDGE FLAPS LEFT PERCENT",
              "Percent Over 100", SC_DATATYPE_FLOAT64);
    hr |= add(g_sc_handle, DEF_AI_PARTS, "TRAILING EDGE FLAPS RIGHT PERCENT",
              "Percent Over 100", SC_DATATYPE_FLOAT64);
    hr |= add(g_sc_handle, DEF_AI_PARTS, "SPOILERS HANDLE POSITION",
              "Percent Over 100", SC_DATATYPE_FLOAT64);
    hr |= add(g_sc_handle, DEF_AI_PARTS, "GENERAL ENG THROTTLE LEVER POSITION:1",
              "Percent Over 100", SC_DATATYPE_FLOAT64);
    hr |= add(g_sc_handle, DEF_AI_PARTS, "GENERAL ENG THROTTLE LEVER POSITION:2",
              "Percent Over 100", SC_DATATYPE_FLOAT64);
    hr |= add(g_sc_handle, DEF_AI_PARTS, "GENERAL ENG THROTTLE LEVER POSITION:3",
              "Percent Over 100", SC_DATATYPE_FLOAT64);
    hr |= add(g_sc_handle, DEF_AI_PARTS, "GENERAL ENG THROTTLE LEVER POSITION:4",
              "Percent Over 100", SC_DATATYPE_FLOAT64);

    if (FAILED(hr)) {
        xc_logf("datadefs: AI parts definition failed hr=0x%08lx",
                (unsigned long)hr);
        return -1;
    }
    xc_logf("datadefs: AI parts definition ready (%d bytes)",
            (int)sizeof(AiPartsRaw));
    return 0;
}

void datadefs_fill_ai_parts(AiPartsRaw* out, int gear_down,
                            int flaps_pct, int spoilers_out,
                            unsigned engines_mask, int engine_count,
                            double throttle_ratio) {
    int i;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->gear_handle = gear_down ? 1.0 : 0.0;
    if (flaps_pct < 0) flaps_pct = 0;
    if (flaps_pct > 100) flaps_pct = 100;
    out->flaps_left = out->flaps_right = (double)flaps_pct / 100.0;
    out->spoilers = spoilers_out ? 1.0 : 0.0;
    /* 远端油门：直接写 AI 的油门杆（对端推油门 → 本端 AI 引擎声音/推力变化）。
     * 未上报（<0）时维持 0.8 兜底（与 XConnect 的 CSL thrust_ratio 兜底一致）：
     * 运转中的引擎有一点推力观感，比恒 0 的"熄火样"合理。 */
    if (throttle_ratio < 0.0) throttle_ratio = 0.8;
    if (throttle_ratio > 1.0) throttle_ratio = 1.0;
    if (engine_count < 1) {
        /* 档案未知时按实测掩码最高位推断，不再默认 2 发 */
        for (i = 4; i >= 1; --i)
            if (engines_mask & (1u << (i - 1))) { engine_count = i; break; }
    }
    if (engine_count > 4) engine_count = 4;
    for (i = 0; i < engine_count; ++i)
        out->throttle[i] = (engines_mask & (1u << i)) ? throttle_ratio : 0.0;
}

/* ---------------- 辅助遥测（可选字段，失败不影响主遥测） ----------------
 *
 * flight_id / COM RECEIVE:n / 扩展灯光（LOGO/WING/RECOGNITION/CABIN）。
 * 与 XConnect flight_data 字段集对齐（功能对等修复）。
 * 全部 Bool/String 类型简单字段；任何一项注册失败则整体放弃 AUX
 * （这些字段在报文里填 0/空串，客户端按"未上报"处理）。 */
#pragma pack(push, 1)
typedef struct {
    char   flight_id[32];    /* ATC FLIGHT NUMBER（String32） */
    DWORD  com_recv_1;
    DWORD  com_recv_2;
    DWORD  com_recv_3;
    DWORD  lt_logo;
    DWORD  lt_wing;
    DWORD  lt_recog;
    DWORD  lt_cabin;
} FlightAuxPkt;
#pragma pack(pop)

static int g_aux_valid = 0;   /* AUX 注册成功且收到过数据 */

int datadefs_register_aux(void) {
    HANDLE h = g_sc_handle;
    HRESULT hr = S_OK;

    if (!h) return -1;
    hr |= add(h, DEF_FLIGHT_AUX, "ATC FLIGHT NUMBER", "NULL",
              SC_DATATYPE_STRING32);
    hr |= add(h, DEF_FLIGHT_AUX, "COM RECEIVE:1", "Bool", SC_DATATYPE_INT32);
    hr |= add(h, DEF_FLIGHT_AUX, "COM RECEIVE:2", "Bool", SC_DATATYPE_INT32);
    hr |= add(h, DEF_FLIGHT_AUX, "COM RECEIVE:3", "Bool", SC_DATATYPE_INT32);
    hr |= add(h, DEF_FLIGHT_AUX, "LIGHT LOGO",        "Bool", SC_DATATYPE_INT32);
    hr |= add(h, DEF_FLIGHT_AUX, "LIGHT WING",        "Bool", SC_DATATYPE_INT32);
    hr |= add(h, DEF_FLIGHT_AUX, "LIGHT RECOGNITION", "Bool", SC_DATATYPE_INT32);
    hr |= add(h, DEF_FLIGHT_AUX, "LIGHT CABIN",       "Bool", SC_DATATYPE_INT32);
    if (FAILED(hr)) {
        xc_logf("datadefs: AUX definition failed hr=0x%08lx "
                "(flight_id/audio_sel/ext-lights disabled)",
                (unsigned long)hr);
        return -1;
    }
    hr = g_sc.RequestDataOnSimObject(g_sc_handle, REQ_FLIGHT_AUX,
                                     DEF_FLIGHT_AUX, SC_OBJECT_ID_USER,
                                     SC_PERIOD_SIM_FRAME,
                                     SC_DATA_REQUEST_FLAG_DEFAULT, 0, 0, 0);
    if (FAILED(hr)) {
        xc_logf("datadefs: AUX request failed hr=0x%08lx", (unsigned long)hr);
        return -1;
    }
    xc_logf("datadefs: aux definition ready (%d bytes)",
            (int)sizeof(FlightAuxPkt));
    return 0;
}

/* ---------------- AI ATC ID 写入（呼号标签） ---------------- */

int datadefs_register_ai_atcid(void) {
    HRESULT hr;
    if (!g_sc_handle) return -1;
    hr = add(g_sc_handle, DEF_AI_ATCID, "ATC ID", "NULL",
             SC_DATATYPE_STRING32);
    if (FAILED(hr)) {
        xc_logf("datadefs: ATC ID definition failed hr=0x%08lx",
                (unsigned long)hr);
        return -1;
    }
    xc_logf("datadefs: AI ATC ID definition ready");
    return 0;
}

int datadefs_write_ai_atcid(DWORD object_id, const char* callsign) {
    char buf[32];
    HRESULT hr;
    if (!g_sc_handle || !g_sc.SetDataOnSimObject || !object_id) return -1;
    memset(buf, 0, sizeof(buf));
    snprintf(buf, sizeof(buf), "%s", callsign ? callsign : "");
    hr = g_sc.SetDataOnSimObject(g_sc_handle, SC_DATA_SET_FLAG_DEFAULT,
                                 object_id, DEF_AI_ATCID, 1,
                                 (DWORD)sizeof(buf), buf);
    if (FAILED(hr)) {
        xc_logf("datadefs: AI ATC ID write failed hr=0x%08lx",
                (unsigned long)hr);
        return -1;
    }
    return 0;
}

/* ---------------- 接收解析 ---------------- */

/* 字符串字段可能不带结尾 0（SimConnect 只保证定长缓冲），统一收尾。 */
static void str_field(char* dst, int cap, const char* src, int src_len) {
    int n = 0;
    if (cap < 1) return;
    while (n < src_len && n < cap - 1 && src[n]) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
    /* 去掉尾部空格（aircraft.cfg 的 title 常带尾随空格） */
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\t')) dst[--n] = '\0';
}

/* 频率 MHz → MHz*1000（协议单位）。3 号台在双台机型上读回 0 或 NaN。 */
static int mhz_to_mhz1000(double mhz) {
    if (!(mhz > 0.0) || mhz > 2000.0) return 0;   /* NaN / 无效 → 0（前端显示 ---） */
    return (int)(mhz * 1000.0 + 0.5);
}

void datadefs_on_simobject_data(const void* pkt, DWORD cbData) {
    const SC_RECV_SIMOBJECT_DATA* d = (const SC_RECV_SIMOBJECT_DATA*)pkt;
    const FlightPkt* p;
    DWORD need = (DWORD)sizeof(FlightPkt);

    if (!d) return;

    /* 辅助遥测（flight_id / 收听 / 扩展灯光）单独解析 */
    if (d->dwRequestID == REQ_FLIGHT_AUX) {
        const FlightAuxPkt* a;
        if (cbData < sizeof(SC_RECV_SIMOBJECT_DATA) - sizeof(DWORD) +
                     sizeof(FlightAuxPkt))
            return;
        a = (const FlightAuxPkt*)&d->dwData;
        str_field(g_flight.flight_id, sizeof(g_flight.flight_id),
                  a->flight_id, (int)sizeof(a->flight_id));
        g_flight.com_recv[0] = a->com_recv_1 != 0;
        g_flight.com_recv[1] = a->com_recv_2 != 0;
        g_flight.com_recv[2] = a->com_recv_3 != 0;
        g_flight.light_logo  = a->lt_logo != 0;
        g_flight.light_wing  = a->lt_wing != 0;
        g_flight.light_recog = a->lt_recog != 0;
        g_flight.light_cabin = a->lt_cabin != 0;
        g_flight.aux_valid = 1;
        return;
    }

    if (d->dwRequestID != REQ_FLIGHT) return;
    if (d->dwDefineID != DEF_FLIGHT) return;
    if (cbData < sizeof(SC_RECV_SIMOBJECT_DATA) - sizeof(DWORD) + need) {
        /* 数据被截断（模拟器版本字段不匹配）→ 记一次日志后忽略，
         * 绝不越界读，否则会读到随机内存并渲染出乱飞的目标。 */
        static int warned = 0;
        if (!warned) {
            warned = 1;
            xc_logf("datadefs: SIMOBJECT_DATA too short (cb=%lu need=%lu)",
                    (unsigned long)cbData, (unsigned long)need);
        }
        return;
    }
    p = (const FlightPkt*)&d->dwData;

    /* 所有浮点字段先经 sane() 消毒。
     *
     * SimConnect 在机模加载中/变量未实现/刚切机型时会吐 NaN 或 Inf。
     * 若原样塞进 `%f`，输出 `nan`/`inf` —— **非法 JSON**，客户端整包丢弃。
     * 范围取物理上可能的宽区间，只为挡住 NaN/Inf 和明显离谱的值，
     * 不做精确校验（那属于上层业务）。 */
    g_flight.lat_deg        = sane(p->lat_rad,   -90.0,     90.0,   0.0);
    g_flight.lon_deg        = sane(p->lon_rad,  -180.0,    180.0,   0.0);
    g_flight.alt_msl_ft     = sane(p->alt_ft,  -2000.0, 100000.0,   0.0);
    g_flight.alt_agl_ft     = sane(p->agl_ft,  -2000.0, 100000.0,   0.0);
    g_flight.alt_indicated_ft = sane(p->alt_ind_ft, -2000.0, 100000.0, 0.0);
    g_flight.pitch_deg      = sane(p->pitch_rad,  -90.0,     90.0,   0.0);
    g_flight.bank_deg       = sane(p->bank_rad,  -180.0,    180.0,   0.0);
    g_flight.heading_true_deg = sane(p->hdg_true_rad, 0.0,  360.0,   0.0);
    g_flight.heading_mag_deg  = sane(p->hdg_mag_rad,  0.0,  360.0,   0.0);
    g_flight.gs_kt          = sane(p->gs_kt,        0.0,   1000.0,  0.0);
    g_flight.ias_kt         = sane(p->ias_kt,       0.0,   1000.0,  0.0);
    g_flight.vs_fpm         = sane(p->vs_fps,  -30000.0,  30000.0,  0.0);
    g_flight.com1_mhz1000 = mhz_to_mhz1000(p->com1_bcd);
    g_flight.com2_mhz1000 = mhz_to_mhz1000(p->com2_bcd);
    g_flight.com3_mhz1000 = mhz_to_mhz1000(p->com3_bcd);
    g_flight.com1_stby_mhz1000 = mhz_to_mhz1000(p->com1s_bcd);
    g_flight.com2_stby_mhz1000 = mhz_to_mhz1000(p->com2s_bcd);
    g_flight.com3_stby_mhz1000 = mhz_to_mhz1000(p->com3s_bcd);
    g_flight.transponder = bco16_to_squawk(p->xpdr_code);
    /* 应答机模式：标准 SimVar 只对默认机可靠。
     * 深度机模（FBW/Fenix/A220…）不更新 TRANSPONDER STATE，
     * 因此若机型档案给了读取表达式，用 WASM 桥读到的原生档位覆盖。
     * 详见 datadefs_apply_profile_xpdr。 */
    g_flight.xpdr_state = (int)p->xpdr_state;
    datadefs_apply_profile_xpdr();
    g_flight.com_transmit[0] = p->com_xmit_1 != 0;
    g_flight.com_transmit[1] = p->com_xmit_2 != 0;
    g_flight.com_transmit[2] = p->com_xmit_3 != 0;
    /* 发射台枚举：0=COM1 1=COM2 2=COM3，**-1 = 未选中/未知**（与 XConnect 统一）。
     * 交叉验证逻辑见 datadefs_derive_pilot_xmit。 */
    g_flight.pilot_xmit_type =
        datadefs_derive_pilot_xmit(g_flight.com_transmit,
                                   (int)(p->pilot_xmit_type & 0xFFFF));
    g_flight.copilot_xmit_type = -1;
    {
        /* copilot 没有 COM TRANSMIT 可交叉验证，直接收窄枚举 */
        int c = (int)(p->copilot_xmit_type & 0xFFFF);
        if (c >= 0 && c <= 2) g_flight.copilot_xmit_type = c;
    }
    g_flight.com_receive_all = p->com_receive_all != 0;
    g_flight.audio_panel_on = p->master_battery != 0;
    g_flight.on_ground = p->on_ground != 0;
    g_flight.gear_ratio = p->gear_pct;
    /* 左右平均；某侧缺数据（NaN 或未实现，读 0）时取另一侧。
     * MSFS 里未实现的 SimVar 通常返回 0，所以"两侧都 0"就是真的收起。 */
    {
        double l = p->flaps_left_pct;
        double r = p->flaps_right_pct;
        if (!(l >= 0.0 && l <= 100.0)) l = 0.0;
        if (!(r >= 0.0 && r <= 100.0)) r = 0.0;
        g_flight.flaps_ratio = (l + r) * 0.5;
    }
    g_flight.throttle_ratio = p->throttle_pct;
    g_flight.spoilers_ratio = p->spoilers_pct;
    g_flight.light_landing = p->lt_landing != 0;
    g_flight.light_taxi = p->lt_taxi != 0;
    g_flight.light_nav = p->lt_nav != 0;
    g_flight.light_beacon = p->lt_beacon != 0;
    g_flight.light_strobe = p->lt_strobe != 0;
    g_flight.num_engines = (int)p->num_engines;
    if (g_flight.num_engines < 0) g_flight.num_engines = 0;
    if (g_flight.num_engines > 4) g_flight.num_engines = 4;
    g_flight.n1_pct[0] = p->n1_1;
    g_flight.n1_pct[1] = p->n1_2;
    g_flight.n1_pct[2] = p->n1_3;
    g_flight.n1_pct[3] = p->n1_4;

    str_field(g_flight.title, sizeof(g_flight.title), p->title, sizeof(p->title));
    str_field(g_flight.atc_id, sizeof(g_flight.atc_id), p->atc_id, sizeof(p->atc_id));
    str_field(g_flight.atc_airline, sizeof(g_flight.atc_airline),
              p->atc_airline, sizeof(p->atc_airline));
    str_field(g_flight.atc_model, sizeof(g_flight.atc_model),
              p->atc_model, sizeof(p->atc_model));
    g_flight.parking_brake = p->parking_brake != 0;

    /* 检测换机模：TITLE 变化时作废上次识别结果并重新反查 ICAO。
     * 之前只在 g_icao 为空时识别，导致同一会话内换机型后 equipment 仍旧值。 */
    if (g_flight.title[0] && strcmp(g_flight.title, g_last_title) != 0) {
        snprintf(g_last_title, sizeof(g_last_title), "%s", g_flight.title);
        g_icao[0] = '\0';
        g_flight.icao_type[0] = '\0';
        xc_logf("datadefs: aircraft changed -> \"%s\", re-detect ICAO",
                g_flight.title);
    }

    /* ICAO 机型：TITLE → 机型目录反查（catalog 里 icao_type_designator）。
     * 反查失败时退化为 ATC MODEL（很多机型在这里直接给 "A20N"/"B738"）。 */
    if (!g_icao[0] || strncmp(g_icao, "?", 1) == 0) {
        const char* by_title = catalog_icao_for_title(g_flight.title);
        if (by_title && by_title[0]) {
            snprintf(g_icao, sizeof(g_icao), "%s", by_title);
        } else if (g_flight.atc_model[0]) {
            catalog_sanitize_icao(g_flight.atc_model, g_icao, sizeof(g_icao));
        }
        if (g_icao[0]) {
            snprintf(g_flight.icao_type, sizeof(g_flight.icao_type), "%s", g_icao);
            xc_logf("datadefs: local aircraft title=\"%s\" -> ICAO %s",
                    g_flight.title, g_icao);
        }
    }

    if (!g_flight.valid) {
        g_flight.valid = 1;
        xc_logf("datadefs: first flight frame lat=%.5f lon=%.5f alt=%.0fft "
                "title=\"%s\"",
                g_flight.lat_deg, g_flight.lon_deg, g_flight.alt_msl_ft,
                g_flight.title);
    }
}

/* 推导飞行员当前发射台。见 datadefs.h 的说明。 */
int datadefs_derive_pilot_xmit(const int com_transmit[3], int raw_enum) {
    int i, active = -1, active_n = 0;

    if (raw_enum < 0 || raw_enum > 2) raw_enum = -1;

    if (com_transmit) {
        for (i = 0; i < 3; i++) {
            if (com_transmit[i]) {
                if (active < 0) active = i;
                active_n++;
            }
        }
    }

    if (active_n == 1) return active;
    if (active_n > 1) {
        if (raw_enum >= 0 && com_transmit[raw_enum]) return raw_enum;
        return active;
    }
    return raw_enum;   /* 含 -1 */
}

/* ---------------- 应答机状态：按机型档案读取 ----------------
 *
 * 问题
 * ----
 * 标准 SimVar `TRANSPONDER STATE:1` 只有**默认机**会维护。深度机模把状态
 * 存在自己的 LVar 里（FBW 的 A32NX_TRANSPONDER_MODE、Fenix 的 S_XPDR_OPERATION、
 * A220 的 A22X Transponder Mode），完全不更新那个 SimVar。
 * 结果：客户端发"设 ON"其实生效了（面板灯亮），但读回来仍是 0=Off，
 * 界面显示与实际相反 —— 这是比"写不进去"更糟的一类 bug，因为用户会以为没成功
 * 而反复点。
 *
 * 解法
 * ----
 * 1. 机型档案里的 xpdr_mode_read 给出读该机模状态的 calculator 表达式
 * 2. 用 WASM 桥的 watch 机制周期读取（槽位固定为 SLOT_XPDR_MODE，
 *    与业务槽位分开，避免冲突）
 * 3. 读到原生档位值后，用 xpdr_mode_native 反解回规范枚举
 *
 * 未装 WASM 模块时无法读取 LVar → 退回标准 SimVar（对默认机正确，
 * 对深度机模会显示过期值，属已知限制，已在档案 note 里注明）。
 */
#define SLOT_XPDR_MODE  30   /* 固定槽位：应答机模式（业务槽位从 0 起） */
#define SLOT_XPDR_CODE  31   /* 固定槽位：应答机码 */

static char g_xpdr_read_expr[2][128];   /* [0]=mode [1]=code，避免重复注册 */

/* 换机型时重置：清掉旧的读取表达式，让下次读取重新注册。 */
void datadefs_reset_profile_reads(void) {
    g_xpdr_read_expr[0][0] = '\0';
    g_xpdr_read_expr[1][0] = '\0';
}

void datadefs_apply_profile_xpdr(void) {
    const AircraftProfile* p = aircraftdb_current();
    double v = 0.0;
    PmdgXpdr pm;

    /* --- PMDG 优先 ---
     * PMDG 没有可读 LVar，也没维护标准 SimVar，唯一来源是官方 SDK 的
     * 二进制 ClientData（见 pmdgbridge.h）。它读到的模式是 SDK 原生枚举，
     * 与规范枚举**顺序不同**，必须重映射：
     *
     *   737: 0=STBY 1=ALT RPTG OFF 2=ON      3=ALT     4=TA/RA
     *   777: 0=STBY 1=ALT RPTG OFF 2=XPNDR   3=TA ONLY 4=TA/RA
     *   规范: 0=OFF  1=STBY        2=ON      3=ALT     4=TEST  5=GND
     *
     * 直接赋值会把 ALT RPTG OFF 显示成 STBY。注意 2 号和 3 号档在 737/777
     * 语义不同，但映射到规范枚举时**恰好一致**（737 的 ON/ALT 与 777 的
     * XPNDR/TA ONLY 都对应规范的 ON/ALT），因此这里无需按机型分支。 */
    if (pmdgbridge_get_xpdr(&pm)) {
        switch (pm.mode) {
        case PMDG_XPDR_STBY:    g_flight.xpdr_state = 1; break;  /* STBY */
        case PMDG_XPDR_ALT_OFF: g_flight.xpdr_state = 5; break;  /* 规范无对应档 → GND */
        case PMDG_XPDR_ON:      g_flight.xpdr_state = 2; break;  /* ON / XPNDR */
        case PMDG_XPDR_ALT:     g_flight.xpdr_state = 3; break;  /* ALT / TA ONLY */
        case PMDG_XPDR_TARA:    g_flight.xpdr_state = 4; break;  /* TA/RA → 用 TEST 档位承载 */
        default: break;
        }
        return;   /* PMDG 有效时不看其他来源，避免被过期值覆盖 */
    }

    if (!p || wasmbridge_state() != WASM_STATE_READY) return;
    if (!aircraftdb_needs_custom_read(p)) return;

    /* --- 模式 --- */
    if (p->xpdr_mode_read && p->xpdr_mode_read[0]) {
        if (strcmp(g_xpdr_read_expr[0], p->xpdr_mode_read) != 0) {
            snprintf(g_xpdr_read_expr[0], sizeof(g_xpdr_read_expr[0]), "%s",
                     p->xpdr_mode_read);
            wasmbridge_watch(p->xpdr_mode_read, SLOT_XPDR_MODE);
            xc_logf("datadefs: 应答机模式改读 %s", p->xpdr_mode_read);
        }
        if (wasmbridge_get_value(SLOT_XPDR_MODE, &v)) {
            int canon = aircraftdb_native_to_canon(p, (int)(v + 0.5));
            if (canon >= 0) {
                g_flight.xpdr_state = canon;
            }
            /* 读到未映射的值（机型档位表没覆盖到）时保留标准 SimVar 的值，
             * 不覆盖 —— 宁可显示旧值也不显示错值。 */
        }
    }

    /* --- 码 --- */
    if (p->xpdr_code_read && p->xpdr_code_read[0]) {
        if (strcmp(g_xpdr_read_expr[1], p->xpdr_code_read) != 0) {
            snprintf(g_xpdr_read_expr[1], sizeof(g_xpdr_read_expr[1]), "%s",
                     p->xpdr_code_read);
            wasmbridge_watch(p->xpdr_code_read, SLOT_XPDR_CODE);
            xc_logf("datadefs: 应答机码改读 %s", p->xpdr_code_read);
        }
        if (wasmbridge_get_value(SLOT_XPDR_CODE, &v) && v > 0.0) {
            /* LVar 里存的是十进制码（如 7000），与 g_flight.transponder 同语义 */
            int code = (int)(v + 0.5);
            if (code >= 0 && code <= 7777) g_flight.transponder = code;
        }
    }
}

/* ---------------- flight_data 构建 ---------------- */

/* N1 消毒：模拟器在发动机关车时可能给 NaN/负数/超量程；
 * JSON 里出现 "NaN" 是非法字面量，会让客户端整条报文解析失败。 */
static double n1_sane(double v) {
    if (!(v > 0.0) || v > 150.0) return 0.0;
    return v;
}

void datadefs_build_flight_json(char* out, int size) {
    int en = g_flight.num_engines;
    if (en <= 0) {
        /* 未知时按 N1 推断，不再默认 2 发（单发机不会有幽灵引擎） */
        for (en = 4; en >= 1; --en)
            if (g_flight.n1_pct[en - 1] > 5.0) break;
        if (en < 1) en = 1;
    }
    char eng[512];
    int el = 0;
    int ei;
    int flaps_pct;
    int spoilers_out;
    const char* squark;

    if (en > 4) en = 4;
    /* **JSON 对象**，不是数组！键是发动机编号（1 基），值只含 `{on}`。
     *
     * swift 的 `CAircraftEngineList::toJson()` 返回 `QJsonObject`：
     *     map.insert(QString::number(e.getNumber()), e.toJson());
     * 即 `{"1":{"on":false},"2":{"on":false}}`。
     *
     * 曾发成数组 `[{"on":..,"rpm_pct":..}]`，服务器类型不匹配 → **整包解析
     * 失败**，站点 JSON 里 `aircraft.engines` 恒为 null。XConnect 那边同样
     * 踩过这个坑（三次才定位），这里保持一致。
     *
     * `rpm_pct` 也不要发：服务器只认它认识的键，多余键会导致该台引擎
     * 解析失败（实测返回空对象 `{}`）。 */
    XSTR_APPEND(eng, el, "\"engines\":{");
    for (ei = 0; ei < en; ei++) {
        double n1 = n1_sane(g_flight.n1_pct[ei]);
        XSTR_APPEND(eng, el, "%s\"%d\":{\"on\":%s}",
                    ei ? "," : "", ei + 1,
                    n1 > 5.0 ? "true" : "false");
    }
    XSTR_APPEND(eng, el, "},");

    flaps_pct = (int)(g_flight.flaps_ratio * 100.0 + 0.5);
    if (flaps_pct < 0) flaps_pct = 0;
    if (flaps_pct > 100) flaps_pct = 100;
    spoilers_out = g_flight.spoilers_ratio > 0.1 ? 1 : 0;

    /* 第 ① 级诊断：SimConnect 原始读数，值变化时打一行。
     * 与客户端 [parts] acc json / AI anim 日志三级对照。 */
    {
        static char last_key[192] = "";
        char key[192];
        snprintf(key, sizeof(key), "%d/%d/%d/%d/%d/%d/%.2f/%.2f/%d",
                 g_flight.light_nav, g_flight.light_strobe,
                 g_flight.light_landing, g_flight.light_taxi,
                 g_flight.light_beacon, flaps_pct,
                 g_flight.gear_ratio, g_flight.spoilers_ratio,
                 en);
        if (strcmp(key, last_key) != 0) {
            snprintf(last_key, sizeof(last_key), "%s", key);
            xc_logf("raw: nav=%d strobe=%d landing=%d taxi=%d beacon=%d "
                    "gear=%.2f flaps=%d spoiler=%d onGround=%d engN=%d",
                    g_flight.light_nav, g_flight.light_strobe,
                    g_flight.light_landing, g_flight.light_taxi,
                    g_flight.light_beacon, g_flight.gear_ratio,
                    flaps_pct, spoilers_out, g_flight.on_ground, en);
        }
    }

    /* squark：FSD `@` 报文的模式字母，**只认三个值**：
     *   S = Standby（待机，不发射）
     *   N = Normal（Mode A/C/S 正常发射）—— "N" 是 normal，不是 "no"
     *   Y = Ident（瞬时脉冲，本插件不发）
     *
     * swift 侧由两段决定（与 XConnect 完全一致）：
     *   ① `simulatorxplane.cpp::xpdrMode()`：mode 0/1 → StateStandby，其余 → ModeC
     *   ② `fsd/serializer.cpp::toQString()`：Standby → "S"，ModeA/C/S → "N"
     *
     * MSFS 的 TRANSPONDER STATE 枚举在 2020/2024 都是
     *   0=Off 1=Stby 2=Test 3=On 4=Alt 5=Ground
     * 所以 **state <= 1 才是 S**，state >= 2 全部是 N。
     *
     * ⚠️ 之前写反了：`state >= 2 ? "S" : "N"` —— 与 XConnect 犯的是同一个错，
     * 8 个类别的模式全部报反（在发射被说成待机、待机被说成在发射），
     * 对端与管制员看到的应答机状态永远是错的。
     *
     * 部分机型不实现该变量（恒为 0），此时用"码非零即视为已开启"兜底。 */
    {
        int on_air = (g_flight.xpdr_state >= 2) ||
                     (g_flight.xpdr_state == 0 && g_flight.transponder > 0);
        squark = on_air ? "N" : "S";
    }

    snprintf(out, (size_t)size,
        "{\"type\":\"flight_data\","
        "\"sim\":\"msfs%s\","
        "\"aircraft_icao\":\"%s\","
        /* aircraft_name：与 XConnect 对齐。客户端在 aircraft_icao 为空时
         * 会用机型名去 swift 机型库反查（aircraft_db.guess_designator），
         * 缺这个字段就只能上报空机型。MSFS 的 title 通常是
         * "PMDG 737-800 Lufthansa" 这种，含足够的型号信息可反查。 */
        "\"aircraft_name\":\"%s\","
        /* flight_id（Mode-S 航班标识）：XConnect 读
         * sim/cockpit2/radios/actuators/flight_id，MSFS 用 ATC FLIGHT NUMBER。
         * AUX 定义失败时为空串 —— 客户端按"未上报"处理。 */
        "\"flight_id\":\"%s\","
        "\"registration\":\"%s\",\"registration_src\":\"%s\","
        "\"engine_count\":%d,"
        "\"latitude\":%.6f,\"longitude\":%.6f,"
        "\"altitude_msl\":%.1f,\"altitude_agl\":%.1f,\"elevation\":%.1f,"
        "\"altitude_indicated\":%.1f,"
        "\"pitch\":%.2f,\"bank\":%.2f,\"heading\":%.2f,\"true_heading\":%.2f,"
        "\"groundspeed\":%.1f,\"indicated_airspeed\":%.1f,\"vertical_speed\":%.1f,"
        "\"com1_freq\":%d,\"com2_freq\":%d,\"com3_freq\":%d,"
        "\"com1_standby\":%d,\"com2_standby\":%d,\"com3_standby\":%d,"
        "\"com_transmit\":[%d,%d,%d],"
        "\"pilot_xmit\":%d,\"copilot_xmit\":%d,"
        /* 音频面板（与 XConnect 字段集互通）：
         *   com*_power   MSFS 无单台 COM 电源 SimVar，用总电门兜底（同源
         *                audio_panel_on，XConnect 缺 dataref 时也是这么兜底）
         *   audio_sel_*  COM RECEIVE:n（收听，AUX 遥测，失败时恒 0）
         *   audio_vol_*  MSFS 无音量 SimVar → 收听=1.0 否则 0.0
         *   copilot      MSFS 是单音频面板，副驾驶与机长同源
         *   tx_com       1-based 发射台（0=未选中），由 pilot_xmit 派生 */
        "\"com1_power\":%d,\"com2_power\":%d,\"com3_power\":%d,"
        "\"audio_sel_com1\":%d,\"audio_sel_com2\":%d,"
        "\"audio_sel_com3\":%d,\"has_audio_sel_com3\":1,"
        "\"audio_vol_com1\":%.2f,\"audio_vol_com2\":%.2f,"
        "\"audio_sel_com1_copilot\":%d,\"audio_sel_com2_copilot\":%d,"
        "\"audio_vol_com1_copilot\":%.2f,\"audio_vol_com2_copilot\":%.2f,"
        "\"tx_com\":%d,"
        /* 同时发 avionics_on 与 audio_panel_on：两者在 MSFS 侧同源
         * （ELECTRICAL MASTER BATTERY），但字段名不同。
         * XConnect 用 `avionics_on`，MConnect 原来只用 `audio_panel_on`，
         * 客户端要写两套判断。统一成两个都发，客户端任一有值即可。 */
        "\"com_receive_all\":%d,\"audio_panel_on\":%d,\"avionics_on\":%d,"
        "\"transponder\":%d,\"xpdr_mode\":%d,\"squark\":\"%s\","
        /* TCAS：MSFS 2020 没有标准 TCAS 输出 SimVar（XP 侧读真实 TCAS
         * dataref），2024 的 TCAS INTRUDER/MODE 枚举语义未核实 → 发 0。
         * xpdr_sys/pwr/tcas_* 是 X-Plane 机型自定义 dataref（ToLiss 等），
         * MSFS 无对应物 → 发 -1（客户端语义 = 该机型无此字段）。 */
        "\"tcas_filter\":0,\"tcas_alert\":0,\"tcas_advisory\":0,"
        "\"xpdr_sys\":-1,\"xpdr_pwr\":-1,\"xpdr_tcas_mode\":-1,\"xpdr_tcas_alt\":-1,"
        "\"gear_deploy\":%.2f,"
        "\"flaps_ratio\":%.2f,\"throttle_ratio\":%.2f,"
        "\"parts\":{\"lights\":{\"strobe_on\":%d,\"landing_on\":%d,\"taxi_on\":%d,"
        "\"beacon_on\":%d,\"nav_on\":%d,\"logo_on\":%d,"
        "\"wing_on\":%d,\"recognition_on\":%d,\"cabin_on\":%d},"
        "\"gear_down\":%d,\"flaps_pct\":%d,"
        "\"spoilers_out\":%d,%s"
        "\"on_ground\":%d}}",
        sc_sim_name(),
        g_flight.icao_type,
        /* aircraft_name 的实参：用 title（"PMDG 737-800 Lufthansa" 这种）。
         * 必须和上面格式串里新增的 %s 位置严格对应，否则后面全部错位。 */
        g_flight.title,
        g_flight.flight_id,
        g_flight.atc_id,
        g_flight.atc_id[0] ? "atc_id" : "",
        en,
        g_flight.lat_deg, g_flight.lon_deg,
        g_flight.alt_msl_ft, g_flight.alt_agl_ft,
        /* elevation：协议里是"地面海拔"，MSFS 没有直接等价 SimVar，
         * 用 高度-离地高 推（AGL 有效时准确，无效时退化为 0 而不是乱值）。 */
        (g_flight.alt_agl_ft > -1000.0 && g_flight.alt_agl_ft < 100000.0)
            ? g_flight.alt_msl_ft - g_flight.alt_agl_ft : 0.0,
        g_flight.alt_indicated_ft,
        g_flight.pitch_deg, g_flight.bank_deg,
        g_flight.heading_mag_deg, g_flight.heading_true_deg,
        g_flight.gs_kt, g_flight.ias_kt, g_flight.vs_fpm,
        g_flight.com1_mhz1000, g_flight.com2_mhz1000, g_flight.com3_mhz1000,
        g_flight.com1_stby_mhz1000, g_flight.com2_stby_mhz1000,
        g_flight.com3_stby_mhz1000,
        g_flight.com_transmit[0], g_flight.com_transmit[1],
        g_flight.com_transmit[2],
        g_flight.pilot_xmit_type, g_flight.copilot_xmit_type,
        g_flight.audio_panel_on, g_flight.audio_panel_on,
        g_flight.audio_panel_on,
        g_flight.com_recv[0], g_flight.com_recv[1], g_flight.com_recv[2],
        (double)g_flight.com_recv[0], (double)g_flight.com_recv[1],
        g_flight.com_recv[0], g_flight.com_recv[1],
        (double)g_flight.com_recv[0], (double)g_flight.com_recv[1],
        (g_flight.pilot_xmit_type >= 0) ? g_flight.pilot_xmit_type + 1 : 0,
        g_flight.com_receive_all, g_flight.audio_panel_on, g_flight.audio_panel_on,
        g_flight.transponder, g_flight.xpdr_state, squark,
        g_flight.gear_ratio > 0.5 ? 1.0 : 0.0,
        g_flight.flaps_ratio, g_flight.throttle_ratio,
        g_flight.light_strobe, g_flight.light_landing, g_flight.light_taxi,
        g_flight.light_beacon, g_flight.light_nav,
        g_flight.light_logo, g_flight.light_wing,
        g_flight.light_recog, g_flight.light_cabin,
        g_flight.gear_ratio > 0.5 ? 1 : 0, flaps_pct, spoilers_out,
        eng,
        g_flight.on_ground);
}

/* ---------------- AI 姿态换算 ---------------- */

/* 航向/俯仰/滚转 → WORLD 与 BODY 速度分量（ft/s）。
 * MSFS 的坐标约定：
 *   BODY: X 右、Y 上、Z 前
 *   WORLD: X 北、Y 上、Z 东（注意不是右手系 Y-up/Z-south，官方按 Z=East）
 * 这里只保证**模长与方位一致**，不追求与模拟器内部完全同号——AI 飞机的
 * 真实位移由我们每帧写的 PLANE LATITUDE/LONGITUDE/ALTITUDE 决定，速度分量
 * 只是给 AI 系统一个"我在动"的提示，避免它把每帧瞬移当异常并回弹。
 *
 * on_ground 时把垂直速度清零，否则地面滑行会因微小爬升率而"跳动"。 */
void datadefs_fill_ai_pose(AiPoseRaw* pose, double lat, double lon, double alt_ft,
                           double heading_deg, double pitch_deg, double bank_deg,
                           double gs_kt, int on_ground) {
    const double d2r = 3.14159265358979323846 / 180.0;
    double gs_fps = gs_kt * 1.6878099;      /* kt → ft/s */
    double hdg = heading_deg * d2r;
    double vz_body = gs_fps * cos(pitch_deg * d2r);

    if (!pose) return;
    memset(pose, 0, sizeof(*pose));
    pose->latitude = lat;
    pose->longitude = lon;
    pose->altitude = alt_ft;
    pose->pitch = pitch_deg;
    pose->bank = bank_deg;
    pose->heading = heading_deg;
    pose->on_ground = on_ground ? 1.0 : 0.0;

    /* WORLD：北向 = cos(hdg)，东向 = sin(hdg) */
    pose->velocity_x = vz_body * cos(hdg);
    pose->velocity_y = on_ground ? 0.0 : vz_body * sin(pitch_deg * d2r);
    pose->velocity_z = vz_body * sin(hdg);

    /* BODY：前向 Z，右向 X */
    pose->vel_body_x = 0.0;
    pose->vel_body_y = on_ground ? 0.0 : vz_body * sin(pitch_deg * d2r);
    pose->vel_body_z = vz_body;
}

/* ---------------- 系统状态 ---------------- */

int datadefs_sim_running(void) {
    return g_sim_running;
}

const char* datadefs_icao(void) {
    return g_flight.icao_type;
}

/* 处理 SIMCONNECT_RECV_SYSTEM_STATE（"Sim" 状态） */
void datadefs_on_system_state(const void* pkt) {
    const SC_RECV_SYSTEM_STATE* s = (const SC_RECV_SYSTEM_STATE*)pkt;
    if (!s) return;
    if (s->dwRequestID != REQ_SYSSTATE) return;
    g_sim_running = s->dwInteger != 0;
}