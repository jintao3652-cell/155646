/* XConnect — dataref 采集模块：本机飞行数据、机模部件、flight_data 构建。 */
#ifndef XCONNECT_DATAREFS_H
#define XCONNECT_DATAREFS_H

#include "xconnect.h"

/* dataref 索引与名称表（见 datarefs.c） */
typedef enum {
    DR_LAT, DR_LON, DR_ELEV, DR_AGL, DR_PITCH, DR_BANK, DR_TRUE_HEAD,
    DR_MAG_HEAD, DR_GS, DR_IAS, DR_VSI,
    DR_COM1, DR_COM2, DR_COM3,
    DR_COM1_STBY, DR_COM2_STBY, DR_COM3_STBY,
    DR_XPDR_CODE, DR_XPDR_MODE,
    DR_TCAS_FILTER, DR_TCAS_ALERT, DR_TCAS_ADVISORY,
    /* 机型自定义（ToLiss A321 / AirbusFBW）：应答机 1/2 号、电源、TCAS 模式、
     * TCAS 高度选择；ckpt/.../anim 为面板旋钮动画值（仅校准日志用） */
    DR_XPDR_SYS, DR_XPDR_PWR, DR_XPDR_TCAS_MODE, DR_XPDR_TCAS_ALT,
    DR_XPDR_1, DR_XPDR_2, DR_XPDR_3, DR_XPDR_4,
    DR_XPDR_ALTITUDE, DR_TCAS_STATUS,
    DR_CKPT_XPDR_SYS, DR_CKPT_XPDR_PWR, DR_CKPT_XPDR_ALT, DR_CKPT_XPDR_MODE,
    DR_GEAR, DR_ONGROUND, DR_FLAPS, DR_THROTTLE,
    /* 机模部件（机模配置 ACC 上报用：灯光用开关布尔，可靠性高于亮度比） */
    DR_LANDING_LT, DR_TAXI_LT, DR_NAV_LT, DR_BEACON_LT, DR_STROBE_LT, DR_N1,
    /* 引擎是否真正在转（X-Plane 官方判定："Engine on and using fuel"）。
     * int[16]，下标 0 = 1 号引擎。比"N1 > 阈值"可靠：起动过程中 N1 已经
     * 上来但引擎尚未稳定运转，靠阈值会把"正在起动"误报成 on。 */
    DR_ENG_RUNNING,
    DR_SPLR,
    /* 音频面板（对齐 XVoice：COM 电源、收听选择、音量、发言选择）。
     * 都是通用 dataref，Laminar 全系机模可用；ToLiss/Fenix 这类自定义机模
     * 靠 radio.json 覆盖。 */
    DR_AVIONICS_ON,  /* sim/cockpit/electrical/avionics_on（COM 是否通电） */
    /* 单个 COM 的电源开关。比 avionics_on 更精确：实测 A330 上
     * avionics_on 可能为 1 而某个 COM 仍处于关闭态，
     * 且部分机模允许单独关某个 COM。 */
    DR_COM1_POWER,   /* sim/cockpit2/radios/actuators/com1_power */
    DR_COM2_POWER,   /* sim/cockpit2/radios/actuators/com2_power */
    DR_COM3_POWER,   /* sim/cockpit2/radios/actuators/com3_power（多数机型无） */
    DR_AUD_SEL_COM1, /* 收听：is com1 selected for listening */
    DR_AUD_SEL_COM2, /* 收听：is com2 selected for listening */
    DR_AUD_SEL_COM3, /* 收听：is com3 selected for listening（部分机型无 → 恒 0） */
    DR_AUD_VOL_COM1, /* com1 音量 [0..1] */
    DR_AUD_VOL_COM2, /* com2 音量 [0..1] */
    /* 副驾驶音频面板（XVoice 也采集这 4 项，双人机组/前后座都需要）。
     * 注意没有 audio_com_selection_copilot 这个 dataref，副驾发话选择不单独暴露。 */
    DR_AUD_SEL_COM1_CP, /* audio_selection_com1_copilot */
    DR_AUD_SEL_COM2_CP, /* audio_selection_com2_copilot */
    DR_AUD_VOL_COM1_CP, /* audio_volume_com1_copilot */
    DR_AUD_VOL_COM2_CP, /* audio_volume_com2_copilot */
    DR_AUD_COM_SEL,  /* 发言选择 enum：0=com1, 1=com2, 2=com3, 6=com1, 7=com2 */
    DR_AC_ICAO,      /* 机型 ICAO：sim/aircraft/view/acf_ICAO
                      * 注意：部分老机模（如 Laminar F-14D）的 .acf 里
                      * **根本没有这个字段**，读出来是空 → 需按机型名兜底 */
    DR_AC_DESCRIP,   /* 机型描述：sim/aircraft/view/acf_descrip（byte[260]）
                      * 与 ui_name 一样只在无 ICAO 时用来兜底推断机型码 */
    DR_AC_UI_NAME,   /* 机型显示名：sim/aircraft/view/acf_ui_name
                      * 同 ICAO 不同机模（如 A320 有 Laminar/ToLiss/Fenix 三份）
                      * 只有靠它才能分辨，radio.json 匹配也更准 */
    DR_FLIGHT_ID,    /* Mode-S 航班标识：sim/cockpit2/radios/actuators/flight_id
                      * byte[8]，最多 7 字符（N844X / KLM511）。
                      * 注意这是应答机播发的 Flight ID，不是 .acf 文件名。 */
    DR_TAILNUM,      /* 飞机注册号（尾号）：sim/aircraft/view/acf_tailnum */
    DR_LIVERY_PATH,  /* 当前涂装目录：sim/aircraft/view/acf_livery_path
                      * （尾号为空时从目录名提取，如 ".../liveries/B-1234 CS"） */
    DR_NUM_ENGINES,  /* 发动机数量：sim/aircraft/engine/acf_num_engines
                      * （A333=2、B744=4；只上报真实数量，不补零到 4） */
    DR_ALT_IND,      /* 座舱高度表指示高度：用于 @ 包 pressure−true 修正字段 */
    /* XP12 灯光**实际亮度**（权威来源；部分机模不再镜像旧开关标量）。
     * 泛化数组的槽位语义由机模定义，737-800 实证：
     *   generic[0]=wing  [1]=logo  [2]/[3]=rwy turnoff L/R  [5]=wheel well */
    DR_LT_LAND_BRI,  /* landing_lights_brightness_ratio float[16] */
    DR_LT_TAXI_BRI,  /* taxi_lights_brightness_ratio float[1] */
    DR_LT_NAV_BRI,   /* nav_lights_brightness_ratio float[1] */
    DR_LT_BEACON_BRI,/* beacon_brightness_ratio float[4] */
    DR_LT_STROBE_BRI,/* strobe_brightness_ratio float[4] */
    DR_LT_GEN_BRI,   /* generic_lights_brightness_ratio float[128] */
    DR_COUNT
} DrIndex;

/* 解析 kDrefNames 并填充内部 g_drefs（XPluginStart 调用） */
void load_all_datarefs(void);

/* ---------------- 机型适配（radio.json） ----------------
 *
 * 通用 dataref 对 Laminar 全系机模够用，但 ToLiss/FlightFactor/Fenix 这类把
 * 无线电存进私有 dataref（`AirbusFBW/COM1Freq` 等）的机模读不到。照 XVoice
 * `SimAircraftTable` 的思路做一张轻量覆盖表，命中就用机型专属 dataref。
 *
 * 匹配键：`icao`（acf_ICAO，区分大小写不敏感）+ 可选 `reg`（尾号）。
 * 未命中 → 全部走通用 dataref，行为与旧版完全一致。 */
void radio_table_load(const char* path);

/* 按当前机型/尾号/UI名挑选覆盖项（内部在 build_flight_data 开头调用）。
 * `ui_name` 可为空；radio.json 条目写了 `ui` 时才参与匹配。 */
void radio_table_select(const char* icao, const char* reg, const char* ui_name);

/* 当前生效的 COM 频率 dataref（已含机型覆盖；空则回通用）。供反写用。 */
XPLMDataRef radio_com_dref(int com_index, int standby);

/* 撤销飞行数据采集（插件卸载/断开时清理）。 */
void datarefs_shutdown(void);

/* 构建本机飞行数据 JSON 行（flight_data，含机模部件 parts） */
void build_flight_data(char* out, int size);

/* dataref 扫描：枚举含 tcas/xpdr/transponder 关键词的已注册 dataref，
 * 写入 xconnect.log（前缀 "dref:"），用于发现机型自定义 dataref */
void discover_matching_datarefs(void);

/* ToLiss/机型自定义应答机旋钮值周期转储（前缀 "xpdrv:"，每秒一次） */
void dump_xpdr_values(void);

/* ---------------- 反向写入（客户端 → 模拟器） ----------------
 *
 * 与 MSFS 侧（MConnect）不同，X-Plane 的频率/应答机 dataref 是**可写**的，
 * 直接 XPLMSetData* 即可，不需要事件。但必须在主线程（flight loop）执行：
 * XPLM 的非线程安全 API 不能从网络线程调用。
 *
 * 命令由网络线程入队（xconnect_control_post），主线程每帧 drain
 * （xconnect_control_drain）。timeout 由 dataref 自身生效，写失败会记日志。 */

typedef enum {
    XCMD_NONE = 0,
    XCMD_COM_SET,      /* 设置 COM 主频/备频（active 区分） */
    XCMD_COM_SWAP,     /* 主备互换（读备频写主频、读主频写备频） */
    XCMD_XPNDR_CODE,   /* 设置应答机 4 位码 */
    XCMD_XPNDR_MODE,   /* 设置应答机模式（优先走命令，见下） */
    /* 应答机 IDENT：transponder_id 是**只读** dataref（DataRefs.txt 标 n），
     * 必须触发 sim/transponder/transponder_ident 命令。 */
    XCMD_XPNDR_IDENT,
    /* 命令型操作（抄 MobiFlight 的双通道设计：DataRef 写 vs Command 触发）。
     * 有些动作改 dataref 不会被机模感知 —— 机模自己监听的是 X-Plane 命令，
     * 典型就是主备互换。走命令才能让 ToLiss/Fenix 这类机模同步面板显示。 */
    XCMD_COM_FLIP,     /* 主备互换走 sim/radios/comN_standy_flip 命令 */
    XCMD_AUDIO_TX,     /* 发话通道：sim/audio_panel/transmit_audio_comN */
    XCMD_AUDIO_MON,    /* 收听开关：sim/audio_panel/monitor_audio_comN（toggle） */
    /* 全收开关（MConnect 的 recv_all 字段互通）：XP 没有"全收"命令，用
     * audio_selection_com1..3 的当前读数决定对哪几台触发 monitor toggle——
     * on = 把没在收的台打开，off = 把在收的台关掉。active 字段复用为 on/off。 */
    XCMD_AUDIO_RECV_ALL,   /* active = 目标开关值（1=全收 0=全关） */
    XCMD_COM_POWER     /* COM 电源：sim/radios/power_comN_on/off */
} XCmdKind;

typedef struct {
    XCmdKind kind;
    int  com_index;   /* 1..3 */
    int  active;      /* 1=主频 0=备频 */
    long hz;          /* 频率 Hz（8.33kHz 间隔下含第三位小数，如 122805） */
    int  code;        /* 应答机码（十进制，逐位八进制） */
    int  mode;        /* 规范模式枚举 0..7（0=Off..7=TA/RA） */
} XCommand;

/* 入队（网络线程，线程安全）。返回 0 成功。 */
int  xconnect_control_post(const XCommand* cmd);

/* 执行队列里的全部命令（主线程每帧调用）。 */
void xconnect_control_drain(void);

/* 频率 MHz*1000 → Hz；越界返回 0。 */
long xconnect_mhz1000_to_hz(int mhz1000);

/* "1234" → 1234；非法返回 -1。 */
int  xconnect_parse_squawk(const char* s);

#endif /* XCONNECT_DATAREFS_H */