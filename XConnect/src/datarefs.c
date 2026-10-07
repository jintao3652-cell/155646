/* XConnect — dataref 采集：本机飞行数据 / 机模部件 / flight_data 构建。
 *
 * 频率字段统一输出 MHz*1000（不同机型/版本下 com*_frequency_hz_833 单位
 * 不一致：Hz / kHz / MHz，按量级自动识别并四舍五入）。
 * 机模部件（parts）键名与 swift CAircraftParts JSON 一致，供 ACC 配置上报。
 */
#include "datarefs.h"

static const char* kDrefNames[DR_COUNT] = {
    "sim/flightmodel/position/latitude",
    "sim/flightmodel/position/longitude",
    "sim/flightmodel/position/elevation",
    "sim/flightmodel/position/y_agl",
    "sim/flightmodel/position/true_theta",
    "sim/flightmodel/position/true_phi",
    "sim/flightmodel/position/true_psi",
    "sim/flightmodel/position/mag_psi",
    "sim/flightmodel/position/groundspeed",
    "sim/flightmodel/position/indicated_airspeed",
    "sim/flightmodel/position/vh_ind_fpm",
    "sim/cockpit2/radios/actuators/com1_frequency_hz_833",
    "sim/cockpit2/radios/actuators/com2_frequency_hz_833",
    "sim/cockpit2/radios/actuators/com3_frequency_hz_833",
    "sim/cockpit2/radios/actuators/com1_standby_frequency_hz_833",
    "sim/cockpit2/radios/actuators/com2_standby_frequency_hz_833",
    "sim/cockpit2/radios/actuators/com3_standby_frequency_hz_833",
    "sim/cockpit2/radios/actuators/transponder_code",
    "sim/cockpit2/radios/actuators/transponder_mode",
    "sim/cockpit2/radios/actuators/tcas_filter",
    "sim/cockpit2/tcas/indicators/tcas_alert",
    "sim/cockpit2/tcas/indicators/tcas_active_advisory",
    "AirbusFBW/XPDRSystem",
    "AirbusFBW/XPDRPower",
    "AirbusFBW/XPDRTCASMode",
    "AirbusFBW/XPDRTCASAltSelect",
    "AirbusFBW/XPDR1",
    "AirbusFBW/XPDR2",
    "AirbusFBW/XPDR3",
    "AirbusFBW/XPDR4",
    "AirbusFBW/XPDRAltitude",
    "AirbusFBW/TCASStatus",
    "ckpt/transponder/sys/anim",
    "ckpt/transponder/pwrx/anim",
    "ckpt/transponder/alt/anim",
    "ckpt/transponder/mode/anim",
    "sim/flightmodel/gear/deploy_ratio",
    "sim/flightmodel/failures/onground_any",
    "sim/cockpit2/controls/flap_ratio",
    "sim/cockpit2/engine/indicators/throttle_ratio",
    "sim/cockpit2/switches/landing_lights_on",
    "sim/cockpit2/switches/taxi_light_on",
    "sim/cockpit2/switches/navigation_lights_on",
    "sim/cockpit2/switches/beacon_on",
    "sim/cockpit2/switches/strobe_lights_on",
    "sim/cockpit2/engine/indicators/N1_percent",
    "sim/cockpit2/controls/speedbrake_ratio",
    "sim/aircraft/view/acf_ICAO",
};

static XPLMDataRef g_drefs[DR_COUNT];

/* 机模部件（ACC 配置上报）当前值，build_flight_data 每帧刷新 */
static int g_parts_nav = 0, g_parts_strobe = 0, g_parts_landing = 0;
static int g_parts_taxi = 0, g_parts_beacon = 0;
static int g_parts_gear = 1, g_parts_flaps = 0, g_parts_onground = 1;
static int g_parts_spoilers = 0;
static int g_parts_eng_on[4] = { 0, 0, 0, 0 };
static float g_parts_eng_n1[4] = { 0, 0, 0, 0 };

/* ---------------- dataref 安全读取 ---------------- */

static float dref_float(XPLMDataRef r) {
    int t;
    if (!r) return 0.0f;
    t = XPLMGetDataRefTypes(r);
#if kXPLM_Version >= 430
    if (t & xplmType_Float) return XPLMGetDataf(r);
    if (t & xplmType_Int) return (float)XPLMGetDatai(r);
#else
    if (t & xplmType_Float) {
        float v = 0.0f;
        XPLMGetDataf(r, &v);
        return v;
    }
    if (t & xplmType_Int) {
        int v = 0;
        XPLMGetDatai(r, &v);
        return (float)v;
    }
#endif
    if (t & xplmType_FloatArray) {
        float v = 0.0f;
        XPLMGetDatavf(r, &v, 0, 1);
        return v;
    }
    if (t & xplmType_IntArray) {
        int v = 0;
        XPLMGetDatavi(r, &v, 0, 1);
        return (float)v;
    }
    return 0.0f;
}

static int dref_int(XPLMDataRef r) {
    int t;
    if (!r) return 0;
    t = XPLMGetDataRefTypes(r);
#if kXPLM_Version >= 430
    if (t & xplmType_Int) return XPLMGetDatai(r);
#else
    if (t & xplmType_Int) {
        int v = 0;
        XPLMGetDatai(r, &v);
        return v;
    }
#endif
    if (t & xplmType_IntArray) {
        int v = 0;
        XPLMGetDatavi(r, &v, 0, 1);
        return v;
    }
    return (int)dref_float(r);
}

/* ---------------- 主循环 ---------------- */

/* 频率 → MHz*1000。不同机型/版本下 com*_frequency_hz_833 返回的单位不一致：
 * 有的给 Hz（133115000）、有的给 kHz（133115）、个别给 MHz（133.115）。
 * 按量级自动识别并四舍五入，保证报文统一为 MHz*1000（133115 = 133.115）。 */
static long freq_to_mhz1000(double raw) {
    if (raw >= 1e7) return (long)(raw / 1000.0 + 0.5);   /* Hz */
    if (raw >= 1e4) return (long)(raw + 0.5);            /* kHz */
    return (long)(raw * 1000.0 + 0.5);                   /* MHz */
}

/* 机型 ICAO 识别：唯一来源 sim/aircraft/view/acf_ICAO（注意 ICAO 大写） */
static void read_aircraft_icao(char* out, int cap) {
    char tmp[64];
    int n, i;
    out[0] = '\0';
    if (cap < 1) return;

    if (!g_drefs[DR_AC_ICAO]) return;
    n = XPLMGetDatab(g_drefs[DR_AC_ICAO], tmp, 0, (int)sizeof(tmp) - 1);
    if (n < 0) n = 0;
    if (n > (int)sizeof(tmp) - 1) n = (int)sizeof(tmp) - 1;
    tmp[n] = '\0';
    /* 仅保留字母数字与连字符，其余截断（JSON 安全） */
    for (i = 0; tmp[i] && i < cap - 1; i++) {
        const unsigned char c = (unsigned char)tmp[i];
        if (isalnum(c) || c == '-') out[i] = (char)toupper(c);
        else break;
    }
    out[i] = '\0';
}

void load_all_datarefs(void) {
    int i;
    for (i = 0; i < DR_COUNT; i++)
        g_drefs[i] = XPLMFindDataRef(kDrefNames[i]);
}

void build_flight_data(char* out, int size) {
    long com1 = freq_to_mhz1000(dref_float(g_drefs[DR_COM1]));
    long com2 = freq_to_mhz1000(dref_float(g_drefs[DR_COM2]));
    long com3 = freq_to_mhz1000(dref_float(g_drefs[DR_COM3]));
    long com1s = freq_to_mhz1000(dref_float(g_drefs[DR_COM1_STBY]));
    long com2s = freq_to_mhz1000(dref_float(g_drefs[DR_COM2_STBY]));
    long com3s = freq_to_mhz1000(dref_float(g_drefs[DR_COM3_STBY]));
    int xpdr_mode = dref_int(g_drefs[DR_XPDR_MODE]);
    int tcas_filter = dref_int(g_drefs[DR_TCAS_FILTER]);
    int tcas_alert = dref_int(g_drefs[DR_TCAS_ALERT]);
    int tcas_advisory = dref_int(g_drefs[DR_TCAS_ADVISORY]);
    /* 机型自定义字段：dataref 不存在（其他机型）时上报 -1，前端据此降级到通用字段 */
    int xpdr_sys = g_drefs[DR_XPDR_SYS] ? dref_int(g_drefs[DR_XPDR_SYS]) : -1;
    int xpdr_pwr = g_drefs[DR_XPDR_PWR] ? dref_int(g_drefs[DR_XPDR_PWR]) : -1;
    int xpdr_tcas_mode = g_drefs[DR_XPDR_TCAS_MODE] ? dref_int(g_drefs[DR_XPDR_TCAS_MODE]) : -1;
    int xpdr_tcas_alt = g_drefs[DR_XPDR_TCAS_ALT] ? dref_int(g_drefs[DR_XPDR_TCAS_ALT]) : -1;
    const char* squark = xpdr_mode >= 2 ? "S" : "N";
    char icao[16];

    /* 机模部件（机模配置 ACC 上报，与 swift CAircraftParts JSON 键一致） */
    {
        float n1[4] = { 0, 0, 0, 0 };
        int i;
        if (g_drefs[DR_N1]) XPLMGetDatavf(g_drefs[DR_N1], n1, 0, 4);
        g_parts_nav = dref_int(g_drefs[DR_NAV_LT]) > 0;
        g_parts_strobe = dref_int(g_drefs[DR_STROBE_LT]) > 0;
        g_parts_landing = dref_int(g_drefs[DR_LANDING_LT]) > 0;
        g_parts_taxi = dref_int(g_drefs[DR_TAXI_LT]) > 0;
        g_parts_beacon = dref_int(g_drefs[DR_BEACON_LT]) > 0;
        g_parts_gear = dref_float(g_drefs[DR_GEAR]) > 0.5;
        g_parts_flaps = (int)(dref_float(g_drefs[DR_FLAPS]) * 100.0 + 0.5);
        if (g_parts_flaps < 0) g_parts_flaps = 0;
        if (g_parts_flaps > 100) g_parts_flaps = 100;
        g_parts_onground = dref_int(g_drefs[DR_ONGROUND]) > 0;
        /* 扰流板：speedbrake_ratio（0=收，0.5=半开，1=全开，-0.5=预位），>0.1 视为伸出 */
        g_parts_spoilers = dref_float(g_drefs[DR_SPLR]) > 0.1;
        /* N1 可能为 NaN（引擎未启动等）：消毒为 0，否则 JSON 输出 "NaN" 非法，
         * 服务器解析失败导致后续配置更新全部被丢弃 */
        for (i = 0; i < 4; i++) {
            float v = n1[i];
            if (!(v > 0.0f && v < 100.0f)) v = 0.0f;   /* NaN / 负数 / 越界 → 0 */
            g_parts_eng_n1[i] = v;
            g_parts_eng_on[i] = v > 5.0f;
        }
    }

    read_aircraft_icao(icao, sizeof(icao));

    snprintf(out, size,
        "{\"type\":\"flight_data\","
        "\"aircraft_icao\":\"%s\","
        "\"latitude\":%.6f,\"longitude\":%.6f,"
        "\"altitude_msl\":%.1f,\"altitude_agl\":%.1f,\"elevation\":%.1f,"
        "\"pitch\":%.2f,\"bank\":%.2f,\"heading\":%.2f,\"true_heading\":%.2f,"
        "\"groundspeed\":%.1f,\"indicated_airspeed\":%.1f,\"vertical_speed\":%.1f,"
        "\"com1_freq\":%ld,\"com2_freq\":%ld,\"com3_freq\":%ld,"
        "\"com1_standby\":%ld,\"com2_standby\":%ld,\"com3_standby\":%ld,"
        "\"transponder\":%d,\"xpdr_mode\":%d,\"squark\":\"%s\","
        "\"tcas_filter\":%d,\"tcas_alert\":%d,\"tcas_advisory\":%d,"
        "\"xpdr_sys\":%d,\"xpdr_pwr\":%d,\"xpdr_tcas_mode\":%d,\"xpdr_tcas_alt\":%d,"
        "\"gear_deploy\":%.2f,"
        "\"flaps_ratio\":%.2f,\"throttle_ratio\":%.2f,"
        "\"parts\":{\"lights\":{\"strobe_on\":%d,\"landing_on\":%d,\"taxi_on\":%d,"
        "\"beacon_on\":%d,\"nav_on\":%d},\"gear_down\":%d,\"flaps_pct\":%d,"
        "\"spoilers_out\":%d,"
        "\"engines\":[{\"on\":%d,\"rpm_pct\":%.1f},{\"on\":%d,\"rpm_pct\":%.1f},"
        "{\"on\":%d,\"rpm_pct\":%.1f},{\"on\":%d,\"rpm_pct\":%.1f}],"
        "\"on_ground\":%d}}",
        icao,
        dref_float(g_drefs[DR_LAT]), dref_float(g_drefs[DR_LON]),
        dref_float(g_drefs[DR_ELEV]) * 3.28084,
        dref_float(g_drefs[DR_AGL]) * 3.28084,
        dref_float(g_drefs[DR_ELEV]),
        dref_float(g_drefs[DR_PITCH]), dref_float(g_drefs[DR_BANK]),
        dref_float(g_drefs[DR_MAG_HEAD]), dref_float(g_drefs[DR_TRUE_HEAD]),
        dref_float(g_drefs[DR_GS]) * 1.943844,
        dref_float(g_drefs[DR_IAS]), dref_float(g_drefs[DR_VSI]),
        com1, com2, com3, com1s, com2s, com3s,
        dref_int(g_drefs[DR_XPDR_CODE]), xpdr_mode, squark,
        tcas_filter, tcas_alert, tcas_advisory,
        xpdr_sys, xpdr_pwr, xpdr_tcas_mode, xpdr_tcas_alt,
        dref_float(g_drefs[DR_GEAR]),
        dref_float(g_drefs[DR_FLAPS]), dref_float(g_drefs[DR_THROTTLE]),
        g_parts_strobe, g_parts_landing, g_parts_taxi, g_parts_beacon, g_parts_nav,
        g_parts_gear, g_parts_flaps, g_parts_spoilers,
        g_parts_eng_on[0], g_parts_eng_n1[0],
        g_parts_eng_on[1], g_parts_eng_n1[1],
        g_parts_eng_on[2], g_parts_eng_n1[2],
        g_parts_eng_on[3], g_parts_eng_n1[3],
        g_parts_onground);
}

/* ---------------- 诊断转储 / dataref 扫描 ---------------- */

/* ToLiss/机型自定义应答机校准转储（前缀 "xpdrv:"）。
 * 每秒输出全部候选 dataref 的原始值（浮点，%.3g），不依赖"变化检测"——
 * 无论值是整数枚举还是连续浮点，旋钮拨动都会体现在转储中。
 * 用于标定数值→标签映射（如 XPDRPower 0/1/2/3 对应 OFF/STBY/AUTO/ON）。 */
void dump_xpdr_values(void) {
    static double last_dump = -1.0;
    const double now = xc_now_sec();
    if (now - last_dump < 1.0) return;
    last_dump = now;
    xconnect_logf("xpdrv: sys=%.3g pwr=%.3g tcm=%.3g tca=%.3g alt=%.3g "
                  "x1=%.3g x2=%.3g x3=%.3g x4=%.3g tcstat=%.3g "
                  "anim(sys=%.3g pwr=%.3g alt=%.3g mode=%.3g)",
                  dref_float(g_drefs[DR_XPDR_SYS]),
                  dref_float(g_drefs[DR_XPDR_PWR]),
                  dref_float(g_drefs[DR_XPDR_TCAS_MODE]),
                  dref_float(g_drefs[DR_XPDR_TCAS_ALT]),
                  dref_float(g_drefs[DR_XPDR_ALTITUDE]),
                  dref_float(g_drefs[DR_XPDR_1]),
                  dref_float(g_drefs[DR_XPDR_2]),
                  dref_float(g_drefs[DR_XPDR_3]),
                  dref_float(g_drefs[DR_XPDR_4]),
                  dref_float(g_drefs[DR_TCAS_STATUS]),
                  dref_float(g_drefs[DR_CKPT_XPDR_SYS]),
                  dref_float(g_drefs[DR_CKPT_XPDR_PWR]),
                  dref_float(g_drefs[DR_CKPT_XPDR_ALT]),
                  dref_float(g_drefs[DR_CKPT_XPDR_MODE]));
}

/* dataref 扫描：枚举全部已注册 dataref，把名称含应答机/TCAS 关键词的路径
 * 写入 xconnect.log（前缀 "dref:"）。用于发现机型自定义 dataref（如 ToLiss
 * 的应答机 1/2 号选择、STBY/AUTO/ON 模式），在无法从打包文件中读取时，
 * 以运行中的真实 dataref 树为准。SDK 400+（XPLMCountDataRefs）。 */
void discover_matching_datarefs(void) {
#if kXPLM_Version >= 400
    static const char* kws[] = { "tcas", "TCAS", "xpdr", "XPDR",
                                 "transponder", "Transponder" };
    const int total = XPLMCountDataRefs();
    int found = 0;
    int i;
    for (i = 0; i < total; i++) {
        XPLMDataRef ref = NULL;
        XPLMDataRefInfo_t info;
        unsigned k;
        XPLMGetDataRefsByIndex(i, 1, &ref);
        if (!ref) continue;
        memset(&info, 0, sizeof(info));
        info.structSize = (int)sizeof(info);
        XPLMGetDataRefInfo(ref, &info);
        if (!info.name) continue;
        for (k = 0; k < sizeof(kws) / sizeof(kws[0]); k++) {
            if (strstr(info.name, kws[k])) {
                xconnect_logf("dref: %s", info.name);
                found++;
                break;
            }
        }
    }
    xconnect_logf("dref: scanned=%d matched=%d", total, found);
#endif
}