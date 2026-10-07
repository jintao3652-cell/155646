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
    DR_SPLR,
    DR_AC_ICAO,      /* 机型 ICAO：sim/aircraft/view/acf_ICAO（唯一来源） */
    DR_COUNT
} DrIndex;

/* 解析 kDrefNames 并填充内部 g_drefs（XPluginStart 调用） */
void load_all_datarefs(void);

/* 构建本机飞行数据 JSON 行（flight_data，含机模部件 parts） */
void build_flight_data(char* out, int size);

/* dataref 扫描：枚举含 tcas/xpdr/transponder 关键词的已注册 dataref，
 * 写入 xconnect.log（前缀 "dref:"），用于发现机型自定义 dataref */
void discover_matching_datarefs(void);

/* ToLiss/机型自定义应答机旋钮值周期转储（前缀 "xpdrv:"，每秒一次） */
void dump_xpdr_values(void);

#endif /* XCONNECT_DATAREFS_H */