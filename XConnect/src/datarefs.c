/* XConnect — dataref 采集：本机飞行数据 / 机模部件 / flight_data 构建。
 *
 * 频率字段统一输出 MHz*1000（不同机型/版本下 com*_frequency_hz_833 单位
 * 不一致：Hz / kHz / MHz，按量级自动识别并四舍五入）。
 * 机模部件（parts）键名与 swift CAircraftParts JSON 一致，供 ACC 配置上报。
 */
#include "datarefs.h"
#include "json.h"
#include "xstr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    /* ⚠️ 必须是 flightmodel2 下的 gear/deploy_ratio（float[10] 数组）。
     * 原来写成 sim/flightmodel/gear/deploy_ratio —— 那个路径根本不存在
     * （flightmodel 下没有 gear 命名空间），XPLMFindDataRef 返回 NULL，
     * dref_float(NULL) 恒为 0 → gearDown 永远上报 false，即使飞机停在地上。
     * 表现：站点 JSON 里 onGround=true 但 gearDown=false（自相矛盾）。 */
    "sim/flightmodel2/gear/deploy_ratio",
    "sim/flightmodel/failures/onground_any",
    /* 襟翼：**实际展开比例**（0.0=全收，1.0=全放），非手柄目标位置。
     *
     * 三个候选的语义差异（DataRefs.txt 原文）：
     *   sim/cockpit2/controls/flap_ratio                 → 已废弃
     *   sim/cockpit2/controls/flap_handle_request_ratio   → 手柄"目标"位置
     *   sim/cockpit2/controls/flap_system_deploy_ratio    → **实际展开**，
     *       原文："The ACTUAL FLAP deployment... takes into account that
     *       flaps deploy slowly, not instantaneously as the handle is dragged"
     *
     * swift 读的是 sim/flightmodel2/controls/flap_handle_deploy_ratio，
     * 但那个也已废弃、官方指向 flap_system_deploy_ratio。两者语义相同
     * （都是"实际展开"而非手柄位置），所以直接用官方推荐的替代品。
     *
     * 必须用"实际展开"而非"手柄目标"：手柄一拨到 FULL，目标立刻变成 1.0，
     * 但襟翼要几秒才真正放到 1.0。若报目标值，对端 CSL 会在你还没放完时
     * 就把襟翼画到全开，与模拟器画面不符。 */
    "sim/cockpit2/controls/flap_system_deploy_ratio",
    /* actuators 而不是 indicators 下 —— indicators 里没有 throttle_ratio，
     * 旧路径返回 NULL 会让油门比例恒为 0。该 dataref 是 float[16] 数组，
     * 下面按数组读首元素（1 号发）代表整体油门。 */
    "sim/cockpit2/engine/actuators/throttle_ratio",
    "sim/cockpit2/switches/landing_lights_on",
    "sim/cockpit2/switches/taxi_light_on",
    "sim/cockpit2/switches/navigation_lights_on",
    "sim/cockpit2/switches/beacon_on",
    "sim/cockpit2/switches/strobe_lights_on",
    "sim/cockpit2/engine/indicators/N1_percent",
    "sim/flightmodel/engine/ENGN_running",
    "sim/cockpit2/controls/speedbrake_ratio",
    /* 音频面板（与 datarefs.h 的 DR_AVIONICS_ON..DR_AUD_COM_SEL 同序） */
    "sim/cockpit/electrical/avionics_on",
    "sim/cockpit2/radios/actuators/com1_power",
    "sim/cockpit2/radios/actuators/com2_power",
    "sim/cockpit2/radios/actuators/com3_power",
    "sim/cockpit2/radios/actuators/audio_selection_com1",
    "sim/cockpit2/radios/actuators/audio_selection_com2",
    "sim/cockpit2/radios/actuators/audio_selection_com3",
    "sim/cockpit2/radios/actuators/audio_volume_com1",
    "sim/cockpit2/radios/actuators/audio_volume_com2",
    "sim/cockpit2/radios/actuators/audio_selection_com1_copilot",
    "sim/cockpit2/radios/actuators/audio_selection_com2_copilot",
    "sim/cockpit2/radios/actuators/audio_volume_com1_copilot",
    "sim/cockpit2/radios/actuators/audio_volume_com2_copilot",
    "sim/cockpit2/radios/actuators/audio_com_selection",
    "sim/aircraft/view/acf_ICAO",
    "sim/aircraft/view/acf_descrip",
    "sim/aircraft/view/acf_ui_name",
    "sim/cockpit2/radios/actuators/flight_id",
    /* X-Plane 的 dataref 叫 acf_tailnum（曾误写成 acf_tailnumber）。
     * 写错名字时 XPLMFindDataRef 返回 NULL，尾号永远读到空串 —— 曾经因此
     * PI:GEN 一直不发 LIVERY，服务器侧 livery 只能是 null。 */
    "sim/aircraft/view/acf_tailnum",
    "sim/aircraft/view/acf_livery_path",
    /* 顺序必须与 datarefs.h 的 DrIndex 一一对应：NUM_ENGINES 在 ALT_IND 之前。
     * 之前把 acf_num_engines 追加在末尾，导致两个索引互换——
     * DR_NUM_ENGINES 读到高度表、DR_ALT_IND 读到引擎数（引擎数被当成气压高度
     * 写进 @ 包高度修正字段，尾号/引擎也都是错的）。 */
    "sim/aircraft/engine/acf_num_engines",
    "sim/cockpit2/gauges/indicators/altitude_ft_pilot",
};

static XPLMDataRef g_drefs[DR_COUNT];

/* 编译期护栏：kDrefNames 必须与 DrIndex 严格同序同长。
 * 这类错位不会报错、只会静默读错 dataref（曾经把引擎数当气压高度用），
 * 靠人工核对极易漏掉，所以让编译器替我们守住。 */
typedef char dref_names_must_match_enum[
    (sizeof(kDrefNames) / sizeof(kDrefNames[0]) == DR_COUNT) ? 1 : -1];

/* 机模部件（ACC 配置上报）当前值，build_flight_data 每帧刷新 */
static int g_parts_nav = 0, g_parts_strobe = 0, g_parts_landing = 0;
static int g_parts_taxi = 0, g_parts_beacon = 0;
static int g_parts_gear = 1, g_parts_flaps = 0, g_parts_onground = 1;
/* 起落架放下比例 0..1（连续值，供 gear_deploy 字段用；数组里的最大值） */
static float g_parts_gear_ratio = 0.0f;
/* 襟翼展开比例 0..1（由整数百分比换算回来，供 flight_data.flaps_ratio 用）*/
static float g_parts_flaps_ratio = 0.0f;
static int g_parts_spoilers = 0;
static int g_parts_eng_on[4] = { 0, 0, 0, 0 };
static float g_parts_eng_n1[4] = { 0, 0, 0, 0 };
static int g_num_engines = 0;          /* 本机真实发动机数量（0 = 未知） */

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
/* 频率归一化：任意单位 → MHz*1000（121.800 MHz → 121800）。
 *
 * X-Plane 里 COM 频率至少有四种单位，实测值（A330，座舱 121.800）：
 *   sim/cockpit2/radios/actuators/com1_frequency_hz_833         = 121800  (kHz)  ← 我们默认读这个
 *   sim/cockpit2/radios/actuators/com1_frequency_hz             = 12180   (10kHz)
 *   sim/cockpit2/radios/actuators/com1_frequency_Mhz            = 121     (MHz 整数部分)
 *   （第三方机模还可能给 121.800 这种 MHz 浮点）
 *
 * **不能只靠量级判断**：kHz 的 121800 和 10kHz 的 12180 都落在 [1e4,1e7)，
 * 光看大小分不开，会把 10kHz 值少乘 10（121.800 显示成 12.180）。
 * 所以按"哪个单位能落进 COM 波段 118.000~136.975 MHz"来判断，
 * 优先选最合理的解释；都不合法才按量级兜底（保持旧行为）。
 *
 * 另注意 com1_frequency_Mhz 只有 MHz 整数部分（121），单独用会丢小数，
 * 所以它必须在 _hz_833 之后才被考虑 —— radio.json 里别把 com 填成它。 */
static long freq_to_mhz1000(double raw) {
    /* 0 或负数：视为"该 COM 未安装/无数据"（VHF3 常见），返回 0 由上层显示占位 */
    if (raw <= 0.0) return 0;

    /* 依次尝试各单位的解释，取第一个落进合法 COM 波段的结果。
     * 换算统一为 raw / div（div 即"该单位相对 mhz1000 的比值"）：
     *   kHz   div=1      : 121800 → 121800 ✓
     *   10kHz div=0.1    : 12180  → 121800 ✓
     *   Hz    div=1000   : 121800000 → 121800 ✓
     *   MHz   div=0.001  : 121.8  → 121800 ✓
     * 顺序按 X-Plane 实际使用频率排列（8.33kHz 的 kHz 最常见）。 */
    {
        static const double kDiv[] = { 1.0, 0.1, 1000.0, 0.001 };
        unsigned i;
        for (i = 0; i < sizeof(kDiv) / sizeof(kDiv[0]); i++) {
            long r = (long)(raw / kDiv[i] + 0.5);
            if (r >= 118000 && r <= 137000) return r;
        }
    }

    /* 都不在 COM 波段内（NAV/ADF/HF 之类被误传进来，或异常值）：
     * 退回旧量级逻辑，保证行为可预期，不返回 0 以免上层误判"未安装"。 */
    if (raw >= 1e7) return (long)(raw / 1000.0 + 0.5);   /* Hz */
    if (raw >= 1e4) return (long)(raw + 0.5);            /* kHz */
    return (long)(raw * 1000.0 + 0.5);                   /* MHz */
}

/* 机型 ICAO 识别：首选 sim/aircraft/view/acf_ICAO，空则按机型名兜底 */
static void read_aircraft_icao(char* out, int cap) {
    char tmp[64];
    int n, i;
    out[0] = '\0';
    if (cap < 1) return;

    if (g_drefs[DR_AC_ICAO]) {
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

    /* 注意：acf_ICAO 为空时**这里不做任何猜测**，原样返回空串。
     *
     * 机型推断交给客户端（Python 侧 `aircraft_db.guess_designator`），因为
     * 那里有完整机型库（1 万条，来自 swift 的 aircrafticao.json）可以做
     * 级联打分匹配。插件里只塞一张小映射表既不完整、又容易误配
     * （"B-1" 会误吞 "B-17"），而且改一次就得重编插件。
     *
     * 客户端同时会带上 `aircraft_name`（acf_ui_name）供推断使用。 */
}

/* 机型显示名：sim/aircraft/view/acf_ui_name。
 *
 * 为什么需要它：`acf_ICAO` 只给机型码，同一个 `A320` 可能是 Laminar 默认、
 * ToLiss、Fenix 三份完全不同的机模，无线电 dataref 也各不相同。光靠 ICAO
 * 无法区分，必须看 UI 名（如 "ToLiss A321" / "Airbus A320 Neo"）。
 *
 * 注意：MobiFlight 与 XVoice 都提到这个字符串 dataref 的**变更通知不可靠**
 * （换机模时不一定会触发订阅回调）。我们不依赖订阅回调 —— 每帧主动读并与
 * 上次值比对，所以不受该缺陷影响。 */
static void read_aircraft_ui_name(char* out, int cap) {
    char tmp[256];
    int n, i;
    out[0] = '\0';
    if (cap < 1) return;
    if (!g_drefs[DR_AC_UI_NAME]) return;
    n = XPLMGetDatab(g_drefs[DR_AC_UI_NAME], tmp, 0, (int)sizeof(tmp) - 1);
    if (n < 0) n = 0;
    if (n > (int)sizeof(tmp) - 1) n = (int)sizeof(tmp) - 1;
    tmp[n] = '\0';
    /* UI 名可能含空格/中文，JSON 里要转义；这里只保留可打印 ASCII，
     * 非 ASCII 一律丢弃（机型名足以区分机模，没必要冒编码风险）。
     *
     * ⚠️ 必须用独立的写出下标 j：若沿用源下标 i，被跳过的字符位置会留下
     * **未初始化字节**，拼进 JSON 后可能是引号/控制符，导致客户端整条
     * flight_data 解析失败。 */
    {
        int j = 0;
        for (i = 0; tmp[i] && j < cap - 1; i++) {
            unsigned char c = (unsigned char)tmp[i];
            if (c < 0x20 || c > 0x7e) continue;
            if (c == '"' || c == '\\') continue;
            out[j++] = (char)c;
        }
        out[j] = '\0';
    }
}

/* Mode-S 航班标识：sim/cockpit2/radios/actuators/flight_id（byte[8]）。
 *
 * 语义易被误解：它是**应答机 Mode-S 播发的 Flight ID**（最多 7 字符，如
 * N844X/KLM511），不是 .acf 文件名，也不是 Mode-S hexcode。
 * 用途：CSL/对端匹配的第二参考，以及排查"对端看到我是谁"。
 * 长度按 dataref 规格 8 字节读满，不做 40 字节的宽松声明。 */
static void read_flight_id(char* out, int cap) {
    char tmp[8];
    int n, i;
    out[0] = '\0';
    if (cap < 2 || !g_drefs[DR_FLIGHT_ID]) return;
    n = XPLMGetDatab(g_drefs[DR_FLIGHT_ID], tmp, 0, (int)sizeof(tmp) - 1);
    if (n < 0) n = 0;
    if (n > (int)sizeof(tmp) - 1) n = (int)sizeof(tmp) - 1;
    tmp[n] = '\0';
    for (i = 0; tmp[i] && i < cap - 1; i++) {
        unsigned char c = (unsigned char)tmp[i];
        if (c < 0x20 || c > 0x7e) break;
        if (c == '"' || c == '\\') break;
        out[i] = (char)c;
    }
    out[i] = '\0';
}

/* 注册号（尾号）：byte 数组型 dataref，只保留字母数字与 '-'，大写。
 * 用于 CSL 匹配的第一级规则（同一架飞机的专属涂装），并随 flight_data 上报。
 *
 * 两级来源，**涂装优先**（客户端按「手动填写 > 模拟器 > 上次记录 > 空」选最终值）：
 *   1) sim/aircraft/view/acf_livery_path —— 涂装目录名里的尾号，例如
 *      ".../liveries/Lufthansa D-AIKI" → D-AIKI。**这才是玩家实际在飞的那架**，
 *      因为选涂装就是在选具体飞机；
 *   2) sim/aircraft/view/acf_tailnum —— 机模自带的默认尾号。它是 `.acf` 里的
 *      出厂值，**不随涂装变化**：X-Plane 自带 A330 的 `P acf/_tailnum` 恒为
 *      `D-AIKN`，但选 Lufthansa 涂装时真实尾号是 `D-AIKI`（实测目录名
 *      `liveries/Lufthansa D-AIKI`，目录内无配置文件）。
 *      所以它只能当兜底，放在涂装之后。
 * 两者都没有则留空（不伪造），对端 CSL 匹配自动退到"同航司同机型"一级。
 * `src` 回填 "livery" / "tailnum" / ""（空串），供客户端区分并写日志。 */

/* 尾号形状判定。两类合法形态：
 *   a) 含数字（`B-1234` / `N829PL` / `VH-OEI`）—— 中美澳等主流格式；
 *   b) 含 '-' 且两侧都是字母（`D-AIKN` / `G-EUUU` / `F-WWBB`）—— 欧陆格式，
 *      注册号里常常一个数字都没有（X-Plane 自带 A330 的尾号就是 `D-AIKN`）。
 * 排除：航司码 `DLH`/`CCA`（纯字母无 '-'）、机型码 `A320`/`B738`（4 位数字+字母
 * 但没有 '-'，靠长度 5 位下限 + 下面的机型码黑名单挡掉）。
 * 之前要求"必须含数字"，导致 D-AIKN 被丢弃、PI 一直不发 LIVERY。 */
static int tail_like(const char* s) {
    int n = 0, digit = 0, alpha = 0, dash = 0;
    const char* p = s;
    for (; *p; p++) {
        if (isalnum((unsigned char)*p)) { n++; if (isdigit((unsigned char)*p)) digit++; else alpha++; }
        else if (*p == '-') { n++; dash++; }
        else return 0;               /* 含其他字符：不是尾号 */
    }
    if (n < 5 || n > 10) return 0;
    if (digit) return alpha || dash;          /* a) 含数字 */
    /* b) 纯字母：必须带 '-' 且两侧各有字母（D-AIKN），排除 DLH / CCA */
    return dash > 0 && alpha >= 4;
}

/* 按空格/下划线切 token，返回第一个像尾号的（原地写入 out，已归一化）。 */
static int tail_from_text(const char* text, char* out, int cap) {
    char tok[32];
    int ti = 0;
    const char* p = text;
    if (cap < 2) return 0;
    for (;;) {
        const char c = *p;
        const int sep = (c == '\0' || c == ' ' || c == '_' || c == '\t' ||
                         c == '\r' || c == '\n');
        if (sep) {
            int i;
            tok[ti] = '\0';
            if (ti >= 5 && tail_like(tok)) {
                for (i = 0; i < ti && i < cap - 1; i++)
                    out[i] = (char)toupper((unsigned char)tok[i]);
                out[i] = '\0';
                return 1;
            }
            ti = 0;
            if (c == '\0') break;
        } else if (ti < (int)sizeof(tok) - 1) {
            tok[ti++] = c;
        }
        p++;
    }
    out[0] = '\0';
    return 0;
}

static void read_aircraft_registration(char* out, int cap, const char** src) {
    char tmp[192];
    int n, i;
    out[0] = '\0';
    if (src) *src = "";
    if (cap < 2) return;

    /* 1) 涂装目录名（优先）：选涂装就是在选具体飞机，目录名里的尾号最贴近实际。
     *    取最后一段，去掉结尾分隔符。
     *    注意 acf_livery_path 在 DataRefs.txt 里标注 "slow dataref, don't read
     *    a lot!"，而本函数被 5Hz 调用。所以按机型+路径做缓存：只有值变化时
     *    才真正 XPLMGetDatab，其余帧复用上次结果。 */
    if (g_drefs[DR_LIVERY_PATH]) {
        static char cached_tail[16] = "";
        static int cached_ok = 0;
        static int cached = 0;
        static double last_read = -1.0;
        double now = xc_now_sec();

        /* 节流：acf_livery_path 是 slow dataref（官方标注），不按 5Hz 读。
         * 每 2 秒读一次足够 —— 涂装只可能在加载/切换机模时变化。 */
        if (!cached || now - last_read >= 2.0) {
            char path[192];
            const char* leaf;
            last_read = now;
            n = XPLMGetDatab(g_drefs[DR_LIVERY_PATH], tmp, 0, (int)sizeof(tmp) - 1);
            if (n < 0) n = 0;
            if (n > (int)sizeof(tmp) - 1) n = (int)sizeof(tmp) - 1;
            tmp[n] = '\0';

            memcpy(path, tmp, sizeof(path));
            path[sizeof(path) - 1] = '\0';
            /* 去掉结尾的目录分隔符与空白（官方说明路径以分隔符结尾） */
            for (i = (int)strlen(path) - 1; i >= 0; i--) {
                if (path[i] == '/' || path[i] == '\\' || path[i] == ' ' ||
                    path[i] == '\t' || path[i] == '\r' || path[i] == '\n')
                    path[i] = '\0';
                else break;
            }
            leaf = path;
            for (i = 0; path[i]; i++)
                if (path[i] == '/' || path[i] == '\\') leaf = path + i + 1;

            {
                char new_tail[16];
                int ok = tail_from_text(leaf, new_tail, sizeof(new_tail));
                /* 只在结果变化时打日志，避免刷屏 */
                if (ok != cached_ok || strcmp(new_tail, cached_tail) != 0) {
                    strncpy(cached_tail, new_tail, sizeof(cached_tail) - 1);
                    cached_tail[sizeof(cached_tail) - 1] = '\0';
                    cached_ok = ok;
                    xconnect_logf("livery_path: \"%s\" -> leaf=\"%s\" tail=%s",
                                  path, leaf, ok ? cached_tail : "(none)");
                }
            }
            cached = 1;
        }
        if (cached_ok) {
            strncpy(out, cached_tail, cap - 1);
            out[cap - 1] = '\0';
            if (src) *src = "livery";
            return;
        }
        out[0] = '\0';
    }

    /* 2) 机模尾号（兜底）：`.acf` 里的出厂值，不随涂装变化。
     *    X-Plane 自带 A330 恒为 D-AIKN，选 Lufthansa 涂装时真实是 D-AIKI —— 
     *    所以只有涂装目录名里拿不到尾号时才用它。 */
    if (g_drefs[DR_TAILNUM]) {
        n = XPLMGetDatab(g_drefs[DR_TAILNUM], tmp, 0, (int)sizeof(tmp) - 1);
        if (n < 0) n = 0;
        if (n > (int)sizeof(tmp) - 1) n = (int)sizeof(tmp) - 1;
        tmp[n] = '\0';
        /* 独立写出下标 j：跳过空白时不能复用源下标（否则留下未初始化字节） */
        {
            int j = 0;
            for (i = 0; tmp[i] && j < cap - 1; i++) {
                const unsigned char c = (unsigned char)tmp[i];
                if (c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
                if (!isalnum(c) && c != '-') break;
                out[j++] = (char)toupper(c);
            }
            out[j] = '\0';
        }
        if (tail_like(out)) {
            if (src) *src = "tailnum";
            return;
        }
        out[0] = '\0';
    }
}

/* ---------------- 机型适配表（radio.json） ----------------
 *
 * 照 XVoice `SimAircraftTable` 的思路，但用外部 JSON 而不是 Luban 二进制 ——
 * 用户可以直接改，不必重编译插件。格式：
 *
 * {
 *   "aircraft": [
 *     { "icao": "A321", "reg": "", "name": "ToLiss A321",
 *       "power":   "AirbusFBW/BatteryVolt",   // 可选，覆盖 avionics_on
 *       "com1":    "AirbusFBW/COM1Freq",      // 可选，覆盖 com1_frequency
 *       "com1_stby": "...", "com2": "...", "com2_stby": "...", "com3": "...",
 *       "audio_com_sel": "..." }
 *   ]
 * }
 *
 * 匹配规则：`icao` 必须相等（大小写不敏感）；条目写了 `reg` 时还要求尾号相等。
 * 未命中任何条目 → 全部沿用通用 dataref，行为与旧版完全一致。
 *
 * 这里只覆盖「读」用的 dataref；「写」仍走通用路径（XPLMSetData 对
 * 自定义 dataref 未必可写，贸然写会污染机模状态）。 */

#define RADIO_TABLE_MAX 32

typedef struct {
    char icao[16];
    char reg[16];
    char name[48];
    char ui_match[64];       /* 可选：acf_ui_name 子串匹配（区分同 ICAO 不同机模） */
    /* 每个字段存放 dataref 名；空串 = 用通用 */
    char d_power[64];
    char d_com[3][64];       /* [0]=com1 [1]=com2 [2]=com3 主频 */
    char d_com_stby[3][64];  /* 备频 */
    char d_audio_sel[2][64]; /* 收听选择 com1/com2 */
    char d_audio_vol[2][64]; /* 音量 com1/com2 */
    char d_audio_comsel[64]; /* 发言选择 */
} RadioOverride;

static RadioOverride g_radio_ov[RADIO_TABLE_MAX];
static int g_radio_ov_n = 0;
static const RadioOverride* g_radio_cur = NULL;   /* 当前生效覆盖项 */

/* 生效的 dataref：有覆盖且解析成功则用覆盖，否则回通用。
 * 解析结果缓存到 g_radio_res（按覆盖项下标），避免 5Hz 每帧 XPLMFindDataRef。 */
static XPLMDataRef g_radio_res[RADIO_TABLE_MAX + 1][12];
static int g_radio_res_done[RADIO_TABLE_MAX + 1];

enum {
    RR_POWER = 0,
    RR_COM1, RR_COM1S, RR_COM2, RR_COM2S, RR_COM3, RR_COM3S,
    RR_SEL1, RR_SEL2, RR_VOL1, RR_VOL2, RR_COMSEL
};

/* 通用回退表，与 RR_* 下标一一对应 */
static XPLMDataRef radio_fallback(int slot) {
    switch (slot) {
    case RR_POWER:  return g_drefs[DR_AVIONICS_ON];
    case RR_COM1:   return g_drefs[DR_COM1];
    case RR_COM1S:  return g_drefs[DR_COM1_STBY];
    case RR_COM2:   return g_drefs[DR_COM2];
    case RR_COM2S:  return g_drefs[DR_COM2_STBY];
    case RR_COM3:   return g_drefs[DR_COM3];
    case RR_COM3S:  return g_drefs[DR_COM3_STBY];
    case RR_SEL1:   return g_drefs[DR_AUD_SEL_COM1];
    case RR_SEL2:   return g_drefs[DR_AUD_SEL_COM2];
    case RR_VOL1:   return g_drefs[DR_AUD_VOL_COM1];
    case RR_VOL2:   return g_drefs[DR_AUD_VOL_COM2];
    case RR_COMSEL: return g_drefs[DR_AUD_COM_SEL];
    default:        return NULL;
    }
}

static const char* radio_override_name(const RadioOverride* o, int slot) {
    if (!o) return NULL;
    switch (slot) {
    case RR_POWER:  return o->d_power;
    case RR_COM1:   return o->d_com[0];
    case RR_COM1S:  return o->d_com_stby[0];
    case RR_COM2:   return o->d_com[1];
    case RR_COM2S:  return o->d_com_stby[1];
    case RR_COM3:   return o->d_com[2];
    case RR_COM3S:  return o->d_com_stby[2];
    case RR_SEL1:   return o->d_audio_sel[0];
    case RR_SEL2:   return o->d_audio_sel[1];
    case RR_VOL1:   return o->d_audio_vol[0];
    case RR_VOL2:   return o->d_audio_vol[1];
    case RR_COMSEL: return o->d_audio_comsel;
    default:        return NULL;
    }
}

static XPLMDataRef pick(const char* override_name, XPLMDataRef fallback) {
    XPLMDataRef r;
    if (!override_name || !override_name[0]) return fallback;
    r = XPLMFindDataRef(override_name);
    if (r) return r;
    xconnect_logf("radio.json: dataref not found: %s (fallback)", override_name);
    return fallback;
}

static void copy_str(char* dst, size_t cap, const Json* obj, const char* key) {
    const Json* v = obj ? json_get(obj, key) : NULL;
    if (v && v->type == JSON_STR && v->sval) {
        strncpy(dst, v->sval, cap - 1);
        dst[cap - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

void radio_table_load(const char* path) {
    FILE* f;
    long sz;
    char* buf;
    Json* root;
    const Json* arr;
    int i, n;

    g_radio_ov_n = 0;
    g_radio_cur = NULL;
    if (!path || !path[0]) return;

    f = fopen(path, "rb");
    if (!f) return;                     /* 没有配置文件是正常情况，静默跳过 */
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 256 * 1024) { fclose(f); return; }
    buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return;
    }
    buf[sz] = '\0';
    fclose(f);

    root = json_parse(buf);
    free(buf);
    if (!root) {
        xconnect_logf("radio.json: parse failed (%s)", path);
        return;
    }

    arr = json_get(root, "aircraft");
    n = arr ? json_len(arr) : 0;
    for (i = 0; i < n && g_radio_ov_n < RADIO_TABLE_MAX; i++) {
        const Json* e = json_at(arr, i);
        RadioOverride* o = &g_radio_ov[g_radio_ov_n];
        const Json* c1;
        if (!e || e->type != JSON_OBJ) continue;
        memset(o, 0, sizeof(*o));
        copy_str(o->icao, sizeof(o->icao), e, "icao");
        copy_str(o->reg, sizeof(o->reg), e, "reg");
        copy_str(o->name, sizeof(o->name), e, "name");
        copy_str(o->ui_match, sizeof(o->ui_match), e, "ui");
        if (!o->icao[0]) continue;              /* 没 icao 的条目无意义 */
        copy_str(o->d_power, sizeof(o->d_power), e, "power");
        copy_str(o->d_audio_comsel, sizeof(o->d_audio_comsel), e, "audio_com_sel");

        c1 = json_get(e, "com");
        if (c1 && c1->type == JSON_ARR) {
            int k;
            for (k = 0; k < 3 && k < json_len(c1); k++) {
                const Json* it = json_at(c1, k);
                if (it && it->type == JSON_OBJ) {
                    copy_str(o->d_com[k], sizeof(o->d_com[k]), it, "active");
                    copy_str(o->d_com_stby[k], sizeof(o->d_com_stby[k]), it, "standby");
                }
            }
        }
        c1 = json_get(e, "audio");
        if (c1 && c1->type == JSON_ARR) {
            int k;
            for (k = 0; k < 2 && k < json_len(c1); k++) {
                const Json* it = json_at(c1, k);
                if (it && it->type == JSON_OBJ) {
                    copy_str(o->d_audio_sel[k], sizeof(o->d_audio_sel[k]), it, "select");
                    copy_str(o->d_audio_vol[k], sizeof(o->d_audio_vol[k]), it, "volume");
                }
            }
        }
        g_radio_ov_n++;
    }
    json_free(root);
    xconnect_logf("radio.json: loaded %d aircraft override(s)", g_radio_ov_n);
}

static int ieq(const char* a, const char* b) {
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 0;
    return *a == '\0' && *b == '\0';
}

/* 不区分大小写的子串包含：needle 在 hay 中出现即返回 1。
 * 用于 radio.json 的 `ui` 键（如 "ToLiss" 匹配 "ToLiss A321"）。 */
static int istr(const char* needle, const char* hay) {
    size_t nl, i;
    if (!needle || !needle[0] || !hay) return 0;
    nl = strlen(needle);
    for (; *hay; hay++) {
        for (i = 0; i < nl; i++) {
            if (!hay[i]) return 0;
            if (toupper((unsigned char)hay[i]) !=
                toupper((unsigned char)needle[i])) break;
        }
        if (i == nl) return 1;
    }
    return 0;
}

void radio_table_select(const char* icao, const char* reg, const char* ui_name) {
    int i, slot;
    const RadioOverride* found = NULL;
    const RadioOverride* best = NULL;   /* 仅 icao 命中（最低优先级） */

    /* 机型/尾号/UI名没变就复用上次结果，避免 5Hz 反复查找 */
    {
        static char last[120] = "";
        char key[120];
        snprintf(key, sizeof(key), "%s|%s|%s", icao ? icao : "",
                 reg ? reg : "", ui_name ? ui_name : "");
        if (strcmp(key, last) == 0) return;
        memcpy(last, key, sizeof(last));
    }

    g_radio_cur = NULL;
    if (icao && icao[0] && g_radio_ov_n > 0) {
        /* 优先级：icao+reg+ui > icao+ui > icao+reg > 仅 icao。
         * 同一 ICAO 下可能有多份机模（Laminar A320 / ToLiss A321 / Fenix），
         * 只有 ui 子串能区分它们。 */
        int score_icao = -1, score_ui = -1, score_reg = -1;
        for (i = 0; i < g_radio_ov_n; i++) {
            const RadioOverride* o = &g_radio_ov[i];
            if (!ieq(o->icao, icao)) continue;

            /* reg 项要求尾号一致；没写 reg 则不限 */
            if (o->reg[0] && (!reg || !ieq(o->reg, reg))) continue;
            /* ui 项要求 UI 名包含该子串（不区分大小写） */
            if (o->ui_match[0]) {
                if (!ui_name || !ui_name[0]) continue;
                if (!istr(o->ui_match, ui_name)) continue;
            }

            {
                int s_ui = o->ui_match[0] ? 1 : 0;
                int s_reg = o->reg[0] ? 1 : 0;
                if (s_ui > score_ui || (s_ui == score_ui && s_reg > score_reg)) {
                    score_ui = s_ui;
                    score_reg = s_reg;
                    best = o;
                }
            }
        }
        found = best;
        (void)score_icao;
    }
    g_radio_cur = found;

    /* 解析并缓存该覆盖项的全部 dataref（未命中则全部回退通用） */
    for (slot = 0; slot < 12; slot++)
        g_radio_res[0][slot] =
            pick(radio_override_name(found, slot), radio_fallback(slot));
    g_radio_res_done[0] = 1;

    if (found)
        xconnect_logf("radio.json: matched %s / %s / %s (%s)", icao,
                      reg && reg[0] ? reg : "-",
                      ui_name && ui_name[0] ? ui_name : "-",
                      found->name[0] ? found->name : "unnamed");
    else if (g_radio_ov_n > 0)
        xconnect_logf("radio.json: no match for %s / %s / %s (generic datarefs)",
                      icao ? icao : "-", reg && reg[0] ? reg : "-",
                      ui_name && ui_name[0] ? ui_name : "-");
}

/* 取第 slot 项生效 dataref（已缓存） */
static XPLMDataRef radio_slot(int slot) {
    if (!g_radio_res_done[0]) {
        /* 还没 select 过（理论上不会），退回通用 */
        return radio_fallback(slot);
    }
    return g_radio_res[0][slot];
}

XPLMDataRef radio_com_dref(int com_index, int standby) {
    int k = com_index - 1;
    if (k < 0) k = 0;
    if (k > 2) k = 2;
    return radio_slot(k == 0 ? (standby ? RR_COM1S : RR_COM1)
                          : k == 1 ? (standby ? RR_COM2S : RR_COM2)
                                   : (standby ? RR_COM3S : RR_COM3));
}

void datarefs_shutdown(void) {
    g_radio_cur = NULL;
    memset(g_radio_res_done, 0, sizeof(g_radio_res_done));
}

void load_all_datarefs(void) {
    int i;
    for (i = 0; i < DR_COUNT; i++)
        g_drefs[i] = XPLMFindDataRef(kDrefNames[i]);

    /* 关键 dataref 解析结果打一行日志：名字写错时 XPLMFindDataRef 返回 NULL，
     * 表现为"尾号/引擎数永远是空"，但运行期没有任何报错——只能靠这行排查。
     * 正常应为 tailnum=1 livery=1 engines=1，出现 0 就是名字不匹配。 */
    xconnect_logf("datarefs: tailnum=%d livery=%d engines=%d (1=ok)",
                  g_drefs[DR_TAILNUM] != NULL,
                  g_drefs[DR_LIVERY_PATH] != NULL,
                  g_drefs[DR_NUM_ENGINES] != NULL);

    /* 索引对位自检：直接比对已解析指针是否指向预期名称。
     * 数组与 DrIndex 错位时不会报错，只会静默读错值，所以显式验证一次。 */
    {
        static const struct { int idx; const char* want; } kMust[] = {
            { DR_TAILNUM,     "sim/aircraft/view/acf_tailnum" },
            { DR_NUM_ENGINES, "sim/aircraft/engine/acf_num_engines" },
            { DR_ALT_IND,     "sim/cockpit2/gauges/indicators/altitude_ft_pilot" },
            { DR_COM1,        "sim/cockpit2/radios/actuators/com1_frequency_hz_833" },
        };
        int k;
        for (k = 0; k < (int)(sizeof(kMust) / sizeof(kMust[0])); k++) {
            XPLMDataRef r = g_drefs[kMust[k].idx];
            if (!r) {
                xconnect_logf("datarefs: MISSING idx=%d want=%s",
                              kMust[k].idx, kMust[k].want);
                continue;
            }
#if kXPLM_Version >= 400
            {
                XPLMDataRefInfo_t info;
                memset(&info, 0, sizeof(info));
                info.structSize = (int)sizeof(info);
                XPLMGetDataRefInfo(r, &info);
                if (!info.name || strcmp(info.name, kMust[k].want) != 0)
                    xconnect_logf("datarefs: MISMATCH idx=%d want=%s got=%s",
                                  kMust[k].idx, kMust[k].want,
                                  info.name ? info.name : "(null)");
            }
#endif
        }
    }
}

void build_flight_data(char* out, int size) {
    /* 先把机型/尾号读出来，供机型覆盖表选择（它决定频率用哪个 dataref） */
    char icao[16], reg[16], ui_name[128], flight_id[16];
    const char* reg_src = "";
    read_aircraft_icao(icao, sizeof(icao));
    read_aircraft_registration(reg, sizeof(reg), &reg_src);
    read_aircraft_ui_name(ui_name, sizeof(ui_name));
    read_flight_id(flight_id, sizeof(flight_id));
    radio_table_select(icao, reg, ui_name);

    /* 频率：优先机型覆盖 dataref（ToLiss/Fenix 等），否则通用 hz_833 */
    long com1 = freq_to_mhz1000(dref_float(radio_com_dref(1, 0)));
    long com2 = freq_to_mhz1000(dref_float(radio_com_dref(2, 0)));
    long com3 = freq_to_mhz1000(dref_float(radio_com_dref(3, 0)));
    long com1s = freq_to_mhz1000(dref_float(radio_com_dref(1, 1)));
    long com2s = freq_to_mhz1000(dref_float(radio_com_dref(2, 1)));
    long com3s = freq_to_mhz1000(dref_float(radio_com_dref(3, 1)));

    /* 音频面板（对齐 XVoice）：电源、收听选择、音量、发言选择。
     * 这些是 COM 是否可用/是否在听的前提，前端据此决定 RMP 显示与告警。 */
    int avionics_on = dref_int(radio_slot(RR_POWER)) > 0;
    /* 单个 COM 的电源开关（实测 A330 上 com1_power=1）。dataref 缺失时按
     * avionics_on 兜底 —— 老机模没有独立 COM 开关，通电即可用。 */
    int com1_pwr = g_drefs[DR_COM1_POWER] ? (dref_int(g_drefs[DR_COM1_POWER]) > 0)
                                          : avionics_on;
    int com2_pwr = g_drefs[DR_COM2_POWER] ? (dref_int(g_drefs[DR_COM2_POWER]) > 0)
                                          : avionics_on;
    int com3_pwr = g_drefs[DR_COM3_POWER] ? (dref_int(g_drefs[DR_COM3_POWER]) > 0)
                                          : avionics_on;
    int aud_sel1 = dref_int(radio_slot(RR_SEL1)) > 0;
    int aud_sel2 = dref_int(radio_slot(RR_SEL2)) > 0;
    /* COM3 收听：部分双台机型没有该 dataref（NULL → 恒 0），属于正常降级 */
    int aud_sel3 = dref_int(g_drefs[DR_AUD_SEL_COM3]) > 0;
    /* COM3 收听 dataref 是否存在：部分双台机型没有它，前端据此决定
     * 是"未选中收听"还是"该机型无此概念"，避免把 COM3 永远画成灰色 */
    int has_aud_sel3 = g_drefs[DR_AUD_SEL_COM3] != NULL;
    float aud_vol1 = dref_float(radio_slot(RR_VOL1));
    float aud_vol2 = dref_float(radio_slot(RR_VOL2));
    /* 副驾驶音频面板：通用 dataref，无覆盖（机模适配表暂不覆盖副驾） */
    int aud_sel1_cp = dref_int(g_drefs[DR_AUD_SEL_COM1_CP]) > 0;
    int aud_sel2_cp = dref_int(g_drefs[DR_AUD_SEL_COM2_CP]) > 0;
    float aud_vol1_cp = dref_float(g_drefs[DR_AUD_VOL_COM1_CP]);
    float aud_vol2_cp = dref_float(g_drefs[DR_AUD_VOL_COM2_CP]);
    /* 发言选择：X-Plane 枚举 6=com1 7=com2 2=com3（0/1 在旧版本也见过，
     * 因此同时接受 0=com1 1=com2 2=com3。无法判定时 tx_com 报 0 = 未选中） */
    int aud_comsel = dref_int(radio_slot(RR_COMSEL));
    int tx_com = (aud_comsel == 6 || aud_comsel == 0) ? 1
               : (aud_comsel == 7 || aud_comsel == 1) ? 2
               : (aud_comsel == 2) ? 3
               : 0;

    int xpdr_mode = dref_int(g_drefs[DR_XPDR_MODE]);
    int tcas_filter = dref_int(g_drefs[DR_TCAS_FILTER]);
    int tcas_alert = dref_int(g_drefs[DR_TCAS_ALERT]);
    int tcas_advisory = dref_int(g_drefs[DR_TCAS_ADVISORY]);
    /* 机型自定义字段：dataref 不存在（其他机型）时上报 -1，前端据此降级到通用字段 */
    int xpdr_sys = g_drefs[DR_XPDR_SYS] ? dref_int(g_drefs[DR_XPDR_SYS]) : -1;
    int xpdr_pwr = g_drefs[DR_XPDR_PWR] ? dref_int(g_drefs[DR_XPDR_PWR]) : -1;
    int xpdr_tcas_mode = g_drefs[DR_XPDR_TCAS_MODE] ? dref_int(g_drefs[DR_XPDR_TCAS_MODE]) : -1;
    int xpdr_tcas_alt = g_drefs[DR_XPDR_TCAS_ALT] ? dref_int(g_drefs[DR_XPDR_TCAS_ALT]) : -1;
    /* 应答机模式 → FSD `@` 报文的模式字母。**必须是 S/N/Y，方向别写反**。
     *
     * 依据 swift 两处代码：
     *   ① `simulatorxplane.cpp::xpdrMode()` 把 XP 枚举折算成 swift 模式：
     *        ident        → StateIdent
     *        mode 0 或 1  → StateStandby     ← Off / Stby 都算待机
     *        其余(2..7)   → ModeC           ← On/Alt/Test/GND/TA/TA-RA 都在发射
     *   ② `fsd/serializer.cpp::toQString(TransponderMode)` 再出字母：
     *        StateStandby → "S"
     *        ModeA/C/S、Mil1..5 → "N"        ← 正常发射（"N" = normal）
     *        StateIdent   → "Y"
     *
     * 所以正确映射是 **mode <= 1 → "S"，mode >= 2 → "N"**。
     * 之前写成 `xpdr_mode >= 2 ? "S" : "N"` —— 方向完全反了，8 个模式全部
     * 报错：待机被报成"在发射"，正常发射被报成"待机"，对端与管制员看到
     * 的应答机状态永远是错的。（注意 `modeAsShortString()` 返回的是
     * "S"/"C"/"I"，那是 UI 显示用，不是 FSD 报文，别拿错了。）
     *
     * IDENT 是瞬时脉冲，`transponder_id` 只读、报文也不单独发，这里不处理。 */
    const char* squark = xpdr_mode <= 1 ? "S" : "N";

    /* 机模部件（机模配置 ACC 上报，与 swift CAircraftParts JSON 键一致） */
    {
        float n1[4] = { 0, 0, 0, 0 };
        int i;
        /* 真实发动机数量（A333=2 / B744=4 / C172=1）。之前固定发 4 组，
         * 双发机多出两台永远关车的"幽灵引擎"，服务器/客户端据此判断机型
         * 会误判，也让"ENG 数量/状态"完全不可信。 */
        g_num_engines = g_drefs[DR_NUM_ENGINES]
                        ? dref_int(g_drefs[DR_NUM_ENGINES]) : 0;
        if (g_num_engines < 0) g_num_engines = 0;
        if (g_num_engines > 4) g_num_engines = 4;   /* 本插件最多支持 4 发 */
        if (g_drefs[DR_N1]) XPLMGetDatavf(g_drefs[DR_N1], n1, 0, 4);
        g_parts_nav = dref_int(g_drefs[DR_NAV_LT]) > 0;
        g_parts_strobe = dref_int(g_drefs[DR_STROBE_LT]) > 0;
        g_parts_landing = dref_int(g_drefs[DR_LANDING_LT]) > 0;
        g_parts_taxi = dref_int(g_drefs[DR_TAXI_LT]) > 0;
        g_parts_beacon = dref_int(g_drefs[DR_BEACON_LT]) > 0;
        /* 起落架：deploy_ratio 是 float[10] 数组（0=收起，1=放下），
         * 不能用 dref_float 当标量读。
         * 判定"放下"取所有条目里的最大值（前几项是主轮、后面可能是尾轮；
         * 有些机型只有部分条目非零，只看 [0] 会漏判）。
         * 另存连续值 g_parts_gear_ratio 供 gear_deploy 字段上报。 */
        {
            float gr[10];
            int k;
            float gmax = 0.0f;
            memset(gr, 0, sizeof(gr));
            if (g_drefs[DR_GEAR]) XPLMGetDatavf(g_drefs[DR_GEAR], gr, 0, 10);
            for (k = 0; k < 10; k++) {
                float v = gr[k];
                if (!(v >= 0.0f && v <= 1.0f)) continue;   /* NaN / 越界 → 跳过 */
                if (v > gmax) gmax = v;
            }
            g_parts_gear_ratio = gmax;
            g_parts_gear = gmax > 0.5f;
        }
        /* 襟翼百分比：**0..100 的整数**（对齐 swift
         * `qRound(flapsDeployRatio * 100.0)`，见 simulatorxplane.cpp
         * slowTimerTimeout 里构造 CAircraftParts 的第 3 个参数）。
         *
         * `+0.5` 对正数等价于 qRound 的四舍五入。但**必须先挡 NaN**：
         * 极少数机模在襟翼未初始化的瞬间给出 NaN，`(int)(NaN)` 是未定义
         * 行为（MSVC 给 INT_MIN），会让 JSON 里出现 -2147483648，服务器
         * 解析失败后**丢弃整个 ACC 包**（之前真踩过：NaN 导致后续配置更新
         * 全部失效）。所以这里用区间判断把 NaN 一并滤掉，
         * 最后再夹到 0..100。
         *
         * 单位必须是百分比而不是 0..1 的比例 —— 服务器站点 JSON 的
         * `flapsPct` 就是整数百分比（swift 的 CAircraftParts::getFlapsPercent
         * 也是 int 百分比），发比例会让对端把 0.5 当成"0%"画。 */
        {
            float fr = dref_float(g_drefs[DR_FLAPS]);
            int fp;
            if (!(fr > 0.0f && fr <= 1.0f)) fr = 0.0f;   /* NaN / 越界 → 0 */
            fp = (int)(fr * 100.0f + 0.5f);
            if (fp < 0) fp = 0;
            if (fp > 100) fp = 100;
            g_parts_flaps = fp;
        }
        g_parts_flaps_ratio = g_parts_flaps / 100.0f;
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
        }
        /* "on" 的判定：**优先用 ENGN_running**（X-Plane 官方的
         * "Engine on and using fuel"，最贴近"发动机在运转"这个语义）。
         * 没有该 dataref 的极老版本才退回 N1 > 5% 的阈值判断。
         *
         * 之前只用 N1 阈值有两个问题：
         *   ① 起动过程中 N1 会上来但引擎尚未稳定，会被误报成 on；
         *   ② 风车状态下 N1 也可能略高于 0。
         * 用户报的用例：1 发关、2 发运转 → 期望 {"1":{"on":false},
         * "2":{"on":true}}，用 ENGN_running 才是权威答案。 */
        if (g_drefs[DR_ENG_RUNNING]) {
            int run[16];
            memset(run, 0, sizeof(run));
            XPLMGetDatavi(g_drefs[DR_ENG_RUNNING], run, 0, 16);
            for (i = 0; i < 4; i++) g_parts_eng_on[i] = run[i] ? 1 : 0;
        } else {
            for (i = 0; i < 4; i++)
                g_parts_eng_on[i] = g_parts_eng_n1[i] > 5.0f;
        }
        /* 引擎状态变化时记一行（不是每帧）。用于核对
         * "编号 ↔ N1 数组下标" 的映射，以及 on 判定阈值是否合适。
         * N1_percent 是 float[16]，下标 0 = 1 号引擎。 */
        {
            static int last_on[4] = { -1, -1, -1, -1 };
            int changed = 0;
            for (i = 0; i < 4; i++)
                if (last_on[i] != g_parts_eng_on[i]) changed = 1;
            if (changed) {
                for (i = 0; i < 4; i++) last_on[i] = g_parts_eng_on[i];
                xconnect_logf("engines: count=%d  N1=[%.1f/%.1f/%.1f/%.1f]  "
                              "on=[%d/%d/%d/%d]  src=%s",
                              g_num_engines, n1[0], n1[1], n1[2], n1[3],
                              g_parts_eng_on[0], g_parts_eng_on[1],
                              g_parts_eng_on[2], g_parts_eng_on[3],
                              g_drefs[DR_ENG_RUNNING] ? "ENGN_running" : "N1>5%");
            }
        }
    }

    /* 尾号/机型/引擎数变化时记一行（不是每帧，避免刷爆日志）。
     * 用于确认 acf_tailnum 真的读到了值，而不是静默为空。
     * 带上 ui_name：同 ICAO 不同机模（Laminar/ToLiss/Fenix）靠它区分。 */
    {
        static char last_key[192] = "";
        char key[192];
        snprintf(key, sizeof(key), "%s|%s|%d|%s", icao, reg, g_num_engines,
                 ui_name);
        if (strcmp(key, last_key) != 0) {
            memcpy(last_key, key, sizeof(last_key));
            xconnect_logf("aircraft: icao=%s name=\"%s\" reg=%s src=%s engines=%d",
                          icao[0] ? icao : "?", ui_name[0] ? ui_name : "?",
                          reg[0] ? reg : "(none)",
                          reg_src[0] ? reg_src : "(none)", g_num_engines);
        }
    }

    /* 引擎数组按真实数量拼。数量未知（dataref 缺失，如极老机模）时退回 2 发，
     * 避免上报空数组让对端完全无法判断机型。 */
    {
        char eng[512];
        int ei;
        int el = 0;
        int en = g_num_engines > 0 ? g_num_engines : 2;
        /* **JSON 对象**，不是数组！键是发动机编号（1 基），值是该台状态。
         *
         * swift 的 CAircraftEngineList::toJson() 实现：
         *     for (const auto &e : *this)
         *         map.insert(QString::number(e.getNumber()), e.toJson());
         *     return map;        // QJsonObject，不是 QJsonArray
         * 所以线上格式是 {"1":{"on":false},"2":{"on":false}}。
         *
         * 之前我们发的是数组 [{"on":..,"rpm_pct":..},...]，类型不对 →
         * 服务器解析失败，站点 JSON 里 engines 恒为 null（实测三次才定位）。
         * 注意 rpm_pct 仍保留（swift CAircraftEngine 有此成员），只是
         * 用户实测的 swift 报文里省略了它（开关量默认不上报）。 */
        /* ⚠️ 不能用 `el += snprintf(...)` —— 截断时 snprintf 返回"本应写入的
         * 长度"，该值大于实际剩余空间 → el 越过 sizeof(eng) → 下一轮
         * `sizeof(eng) - el` 溢出成巨大 size_t → 越界写。
         * XSTR_APPEND 自动算剩余空间并把位置夹回 [0, sizeof-1]。
         * （4 台引擎约 100 字节、缓冲 512，正常不触发，但必须堵死。） */
        XSTR_APPEND(eng, el, "\"engines\":{");
        for (ei = 0; ei < en && ei < 4; ei++) {
            /* **只发 on，不发 rpm_pct**。
             *
             * swift 源码里 CAircraftEngine 的 rpmPct 没有 DisabledForJson，
             * 理论上会序列化；但 swift 客户端实际上报、以及服务器回显的
             * 格式都只有 {"on":false}，没有 rpm_pct。实测发现我们带上
             * rpm_pct 后，服务器返回的 engines 是**空对象** {}（key 全丢），
             * 去掉后才有内容 —— 说明服务器只认它认识的键，多余键会导致
             * 该台引擎解析失败。
             *
             * 宁可少发一个可选字段，也要让服务器能解析出引擎数量和开关态。 */
            XSTR_APPEND(eng, el, "%s\"%d\":{\"on\":%s}",
                        ei ? "," : "", ei + 1,
                        g_parts_eng_on[ei] ? "true" : "false");
        }
        XSTR_APPEND(eng, el, "},");
        /* eng 只含合法 JSON 片段，作为 %s 嵌入主报文 */
        snprintf(out, size,
            "{\"type\":\"flight_data\","
            "\"aircraft_icao\":\"%s\",\"aircraft_name\":\"%s\",\"flight_id\":\"%s\","
            "\"registration\":\"%s\",\"registration_src\":\"%s\","
            "\"engine_count\":%d,"
            "\"latitude\":%.6f,\"longitude\":%.6f,"
            "\"altitude_msl\":%.1f,\"altitude_agl\":%.1f,\"elevation\":%.1f,"
            "\"altitude_indicated\":%.1f,"
            "\"pitch\":%.2f,\"bank\":%.2f,\"heading\":%.2f,\"true_heading\":%.2f,"
            "\"groundspeed\":%.1f,\"indicated_airspeed\":%.1f,\"vertical_speed\":%.1f,"
            "\"com1_freq\":%ld,\"com2_freq\":%ld,\"com3_freq\":%ld,"
            "\"com1_standby\":%ld,\"com2_standby\":%ld,\"com3_standby\":%ld,"
            /* 音频面板：COM 通电/收听/音量/当前发话通道（对齐 XVoice） */
            "\"avionics_on\":%d,"
            "\"com1_power\":%d,\"com2_power\":%d,\"com3_power\":%d,"
            "\"audio_sel_com1\":%d,\"audio_sel_com2\":%d,"
            "\"audio_sel_com3\":%d,\"has_audio_sel_com3\":%d,"
            "\"audio_vol_com1\":%.2f,\"audio_vol_com2\":%.2f,"
            /* 副驾驶音频面板（XVoice 也采集；双人机组/前后座用） */
            "\"audio_sel_com1_copilot\":%d,\"audio_sel_com2_copilot\":%d,"
            "\"audio_vol_com1_copilot\":%.2f,\"audio_vol_com2_copilot\":%.2f,"
            /* tx_com：1/2/3 = 当前选定发射台，0 = 未选中。前端 RMP 用它高亮 */
            "\"tx_com\":%d,"
            /* pilot_xmit：与 MSFS 侧对齐（0=COM1 1=COM2 2=COM3），
             * 便于客户端用同一套字段处理两个平台 */
            "\"pilot_xmit\":%d,"
            "\"transponder\":%d,\"xpdr_mode\":%d,\"squark\":\"%s\","
            "\"tcas_filter\":%d,\"tcas_alert\":%d,\"tcas_advisory\":%d,"
            "\"xpdr_sys\":%d,\"xpdr_pwr\":%d,\"xpdr_tcas_mode\":%d,\"xpdr_tcas_alt\":%d,"
            "\"gear_deploy\":%.2f,"
            "\"flaps_ratio\":%.2f,\"throttle_ratio\":%.2f,"
            "\"parts\":{\"lights\":{\"strobe_on\":%d,\"landing_on\":%d,\"taxi_on\":%d,"
            "\"beacon_on\":%d,\"nav_on\":%d},\"gear_down\":%d,\"flaps_pct\":%d,"
            "\"spoilers_out\":%d,"
            "%s"
            "\"on_ground\":%d}}",
            icao, ui_name, flight_id, reg, reg_src, g_num_engines,
            dref_float(g_drefs[DR_LAT]), dref_float(g_drefs[DR_LON]),
            dref_float(g_drefs[DR_ELEV]) * 3.28084,
            dref_float(g_drefs[DR_AGL]) * 3.28084,
            dref_float(g_drefs[DR_ELEV]),
            (double)dref_float(g_drefs[DR_ALT_IND]),
            dref_float(g_drefs[DR_PITCH]), dref_float(g_drefs[DR_BANK]),
            dref_float(g_drefs[DR_MAG_HEAD]), dref_float(g_drefs[DR_TRUE_HEAD]),
            dref_float(g_drefs[DR_GS]) * 1.943844,
            dref_float(g_drefs[DR_IAS]), dref_float(g_drefs[DR_VSI]),
            com1, com2, com3, com1s, com2s, com3s,
            avionics_on, com1_pwr, com2_pwr, com3_pwr,
            aud_sel1, aud_sel2, aud_sel3, has_aud_sel3,
            (double)aud_vol1, (double)aud_vol2,
            aud_sel1_cp, aud_sel2_cp,
            (double)aud_vol1_cp, (double)aud_vol2_cp,
            tx_com,
            /* pilot_xmit 用 0-based（0=COM1）与 MSFS 侧对齐；tx_com=0 时
             * 表示"未选中"，这里映射成 -1 让客户端能区分 */
            (tx_com > 0) ? (tx_com - 1) : -1,
            dref_int(g_drefs[DR_XPDR_CODE]), xpdr_mode, squark,
            tcas_filter, tcas_alert, tcas_advisory,
            xpdr_sys, xpdr_pwr, xpdr_tcas_mode, xpdr_tcas_alt,
            /* gear_deploy 用前面算好的连续比例（deploy_ratio 数组的最大值）。
             * 不能再写 dref_float(g_drefs[DR_GEAR]) —— 那是数组，dref_float
             * 只会读到一个元素且旧路径本来就不存在。 */
            (double)g_parts_gear_ratio,
            (double)g_parts_flaps_ratio, dref_float(g_drefs[DR_THROTTLE]),
            g_parts_strobe, g_parts_landing, g_parts_taxi, g_parts_beacon,
            g_parts_nav,
            g_parts_gear, g_parts_flaps, g_parts_spoilers,
            eng,                                  /* %s 引擎数组 */
            g_parts_onground);
    }
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
/* 读一个 dataref 的当前值并格式化成可读文本（扫描诊断用）。
 * 只处理标量/可取首元素的情况；读不到就返回 "-"。 */
static void dref_value_str(XPLMDataRef ref, char* out, int cap) {
    int t;
    out[0] = '\0';
    if (!ref) { snprintf(out, cap, "-"); return; }
    t = XPLMGetDataRefTypes(ref);
    if (t & xplmType_Int) {
        snprintf(out, cap, "%d", XPLMGetDatai(ref));
        return;
    }
    if (t & xplmType_Float) {
        snprintf(out, cap, "%.3f", (double)XPLMGetDataf(ref));
        return;
    }
    if (t & xplmType_Double) {
        snprintf(out, cap, "%.3f", XPLMGetDatad(ref));
        return;
    }
    if (t & xplmType_IntArray) {
        int v = 0;
        if (XPLMGetDatavi(ref, &v, 0, 1) > 0) {
            snprintf(out, cap, "[0]=%d", v);
            return;
        }
    }
    if (t & xplmType_FloatArray) {
        float v = 0.0f;
        if (XPLMGetDatavf(ref, &v, 0, 1) > 0) {
            snprintf(out, cap, "[0]=%.3f", (double)v);
            return;
        }
    }
    if (t & xplmType_Data) {
        char b[48];
        int n = XPLMGetDatab(ref, b, 0, (int)sizeof(b) - 1);
        int i;
        if (n > 0) {
            if (n > (int)sizeof(b) - 1) n = (int)sizeof(b) - 1;
            b[n] = '\0';
            for (i = 0; i < n; i++)
                if ((unsigned char)b[i] < 0x20 || (unsigned char)b[i] > 0x7e) b[i] = '.';
            snprintf(out, cap, "\"%s\"", b);
            return;
        }
    }
    snprintf(out, cap, "-");
}

void discover_matching_datarefs(void) {
#if kXPLM_Version >= 400
    /* 关键词分组：应答机组始终输出；无线电组按需（radio.json 要填的就是这类）。
     * 注意第三方机模的私有 dataref 只有在本机加载该机模后才注册到 XPLM，
     * 所以必须"进游戏、加载目标机模"再扫，静态的 DataRefs.txt 里查不到。 */
    static const char* kws[] = {
        /* 应答机 */
        "tcas", "TCAS", "xpdr", "XPDR", "transponder", "Transponder",
        /* 无线电频率（radio.json 的 com/standby 要填这些） */
        "com1", "com2", "com3", "COM1", "COM2", "COM3",
        "freq", "Freq", "FREQ",
        /* 音频面板（radio.json 的 audio/select/volume/tx 要填这些） */
        "audio", "Audio", "AUDIO", "vhf", "VHF", "Vhf",
        "volume", "Volume", "VOLUME", "listen", "Listen",
        /* 常见第三方机模前缀，便于整块定位 */
        "AirbusFBW", "laminar/B738", "Fenix", "Toliss", "ToLiss",
        "ckpt/"
    };
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
                /* 连值一起打：光有名字无法判断哪个 dataref 真的在生效，
                 * 比如同型机可能同时存在 sim/cockpit2/... 和机模私有两套，
                 * 只有值非 0 的那个才是机模真正在用的。 */
                char val[64];
                dref_value_str(ref, val, sizeof(val));
                if (info.writable)
                    xconnect_logf("dref: %s = %s  [writable]", info.name, val);
                else
                    xconnect_logf("dref: %s = %s", info.name, val);
                found++;
                break;
            }
        }
    }
    xconnect_logf("dref: scanned=%d matched=%d", total, found);
#endif
}

/* ================= 反向写入（客户端 → 模拟器） =================
 *
 * X-Plane 的频率/应答机 dataref 都是可写的，直接 XPLMSetData* 即可。
 * 与 MConnect（MSFS）走 SimConnect 事件形成对照：那边频率是只读 SimVar，
 * 只能触发事件；这边是"改数据"。协议对客户端保持一致。
 *
 * 线程约束：XPLM 非线程安全，网络线程只入队，主线程（flight loop）执行。
 */

#define XCMD_QUEUE_MAX 64

static XCommand g_cmd_q[XCMD_QUEUE_MAX];
static int g_cmd_head = 0;
static int g_cmd_count = 0;

long xconnect_mhz1000_to_hz(int mhz1000) {
    /* COM 波段 118.000~136.975 MHz，协议单位 MHz*1000。
     * 上界是 136975 而不是 137000：137.000 是波段预留边界，不是可调频率
     * （8.33kHz 间隔下最高合法值就是 136.975）。 */
    if (mhz1000 < 118000 || mhz1000 > 136975) return 0;
    return (long)mhz1000 * 1000L;
}

int xconnect_parse_squawk(const char* s) {
    int i, v = 0;
    if (!s) return -1;
    for (i = 0; i < 4; i++) {
        if (s[i] < '0' || s[i] > '7') return -1;
        v = v * 10 + (s[i] - '0');
    }
    if (s[4] != '\0') return -1;
    return v;
}

int xconnect_control_post(const XCommand* cmd) {
    int dropped = 0;
    if (!cmd || cmd->kind == XCMD_NONE) return -1;

    LOCK_ENTER(g_players_lock);   /* 复用玩家表锁：调用频率低，不必单独一把 */
    if (g_cmd_count >= XCMD_QUEUE_MAX) {
        g_cmd_head = (g_cmd_head + 1) % XCMD_QUEUE_MAX;   /* 丢最旧 */
        g_cmd_count--;
        dropped = 1;
    }
    g_cmd_q[g_cmd_head] = *cmd;
    g_cmd_head = (g_cmd_head + 1) % XCMD_QUEUE_MAX;
    g_cmd_count++;
    LOCK_LEAVE(g_players_lock);

    if (dropped) xconnect_logf("control: queue full, dropped oldest");
    return 0;
}

/* ---------------- 命令缓存（XPLMCommandRef） ----------------
 *
 * 与 dataref 写不同，命令是"触发一次动作"。机模内部监听的是命令而非 dataref，
 * 所以凡是「机模面板要同步响应」的动作都该走命令。命令按名称懒解析并缓存。 */

#define XCMD_CACHE_MAX 16

typedef struct {
    char name[64];
    XPLMCommandRef ref;
} CmdCacheEnt;

static CmdCacheEnt g_cmd_cache[XCMD_CACHE_MAX];
static int g_cmd_cache_n = 0;

static XPLMCommandRef cmd_get(const char* name) {
    int i;
    XPLMCommandRef r;
    if (!name || !name[0]) return NULL;
    for (i = 0; i < g_cmd_cache_n; i++)
        if (strcmp(g_cmd_cache[i].name, name) == 0) return g_cmd_cache[i].ref;
    r = XPLMFindCommand(name);
    if (r && g_cmd_cache_n < XCMD_CACHE_MAX) {
        strncpy(g_cmd_cache[g_cmd_cache_n].name, name,
                sizeof(g_cmd_cache[0].name) - 1);
        g_cmd_cache[g_cmd_cache_n].name[sizeof(g_cmd_cache[0].name) - 1] = '\0';
        g_cmd_cache[g_cmd_cache_n].ref = r;
        g_cmd_cache_n++;
    }
    return r;
}

/* 触发一次命令（begin+end 一次完整按压）。返回 1 成功。 */
static int cmd_once(const char* name) {
    XPLMCommandRef r = cmd_get(name);
    if (!r) {
        xconnect_logf("control: command not found: %s", name);
        return 0;
    }
    XPLMCommandOnce(r);
    return 1;
}

/* 写一个 int dataref（应答机码/模式都是 int） */
static int write_int(XPLMDataRef r, int v) {
    if (!r) return 0;
    if (!(XPLMGetDataRefTypes(r) & xplmType_Int)) return 0;
    XPLMSetDatai(r, v);
    return 1;
}

/* 写一个 float dataref（频率是 hz_833，整数形式但类型可能是 int 或 float） */
static int write_num(XPLMDataRef r, double v) {
    int t;
    if (!r) return 0;
    t = XPLMGetDataRefTypes(r);
    if (t & xplmType_Int) { XPLMSetDatai(r, (int)(v + 0.5)); return 1; }
    if (t & xplmType_Float) { XPLMSetDataf(r, (float)v); return 1; }
    return 0;
}

void xconnect_control_drain(void) {
    XCommand batch[XCMD_QUEUE_MAX];
    int n = 0, i;

    LOCK_ENTER(g_players_lock);
    while (g_cmd_count > 0 && n < XCMD_QUEUE_MAX) {
        int tail = (g_cmd_head - g_cmd_count + XCMD_QUEUE_MAX) % XCMD_QUEUE_MAX;
        batch[n++] = g_cmd_q[tail];
        g_cmd_count--;
    }
    LOCK_LEAVE(g_players_lock);

    for (i = 0; i < n; i++) {
        const XCommand* c = &batch[i];
        switch (c->kind) {
        case XCMD_COM_SET: {
            XPLMDataRef r =
                (c->com_index == 2) ? g_drefs[DR_COM2]
              : (c->com_index == 3) ? g_drefs[DR_COM3]
                                    : g_drefs[DR_COM1];
            XPLMDataRef rs =
                (c->com_index == 2) ? g_drefs[DR_COM2_STBY]
              : (c->com_index == 3) ? g_drefs[DR_COM3_STBY]
                                    : g_drefs[DR_COM1_STBY];
            if (!c->active) r = rs;
            if (write_num(r, (double)c->hz))
                xconnect_logf("control: COM%d %s = %ld Hz", c->com_index,
                              c->active ? "active" : "standby", c->hz);
            else
                xconnect_logf("control: COM%d %s write failed (no dref)",
                              c->com_index, c->active ? "active" : "standby");
            break;
        }

        case XCMD_COM_SWAP: {
            XPLMDataRef ra =
                (c->com_index == 2) ? g_drefs[DR_COM2]
              : (c->com_index == 3) ? g_drefs[DR_COM3]
                                    : g_drefs[DR_COM1];
            XPLMDataRef rs =
                (c->com_index == 2) ? g_drefs[DR_COM2_STBY]
              : (c->com_index == 3) ? g_drefs[DR_COM3_STBY]
                                    : g_drefs[DR_COM1_STBY];
            double a = (double)dref_float(ra);
            double s = (double)dref_float(rs);
            if (write_num(ra, s) && write_num(rs, a))
                xconnect_logf("control: COM%d swap %.0f <-> %.0f",
                              c->com_index, a, s);
            else
                xconnect_logf("control: COM%d swap write failed", c->com_index);
            break;
        }

        case XCMD_XPNDR_CODE:
            if (write_int(g_drefs[DR_XPDR_CODE], c->code))
                xconnect_logf("control: XPDR code = %04d", c->code);
            else
                xconnect_logf("control: XPDR code write failed");
            break;

        case XCMD_XPNDR_MODE: {
            /* X-Plane 枚举：0=Off 1=Stby 2=On(Mode A) 3=Alt(Mode C) 4=Test
             * 5=GND 6=TA only 7=TA/RA —— 与本项目协议顺序一致，无需重映射。
             *
             * 优先走**命令**（sim/transponder/transponder_off/standby/on/alt/
             * test/ground/ta_only/ta_ra）：机模监听命令并同步面板，
             * 直接写 dataref 会让面板与内部状态不一致（同 COM 互换的道理）。
             * 命令不存在时才退回写 dataref（老机模可能没有这些命令）。 */
            static const char* kModeCmd[] = {
                "sim/transponder/transponder_off",     /* 0 */
                "sim/transponder/transponder_standby", /* 1 */
                "sim/transponder/transponder_on",      /* 2 */
                "sim/transponder/transponder_alt",     /* 3 */
                "sim/transponder/transponder_test",    /* 4 */
                "sim/transponder/transponder_ground",  /* 5 */
                "sim/transponder/transponder_ta_only", /* 6 */
                "sim/transponder/transponder_ta_ra"    /* 7 */
            };
            int m = c->mode;
            if (m < 0) m = 0;
            if (m > 7) m = 7;
            if (!cmd_once(kModeCmd[m])) {
                if (write_int(g_drefs[DR_XPDR_MODE], m))
                    xconnect_logf("control: XPDR mode = %d (dataref fallback)", m);
                else
                    xconnect_logf("control: XPDR mode write failed");
            } else {
                xconnect_logf("control: XPDR mode = %d (command)", m);
            }
            break;
        }

        case XCMD_XPNDR_IDENT:
            /* transponder_id 是只读 dataref，只能触发命令。 */
            if (cmd_once("sim/transponder/transponder_ident"))
                xconnect_logf("control: XPDR ident");
            break;

        case XCMD_COM_FLIP: {
            /* 主备互换：命令路径。机模（ToLiss 等）自己监听该命令并同步面板，
             * 直接写 dataref 会导致面板显示与实际值不一致。
             * 注意 X-Plane 官方把 standby 拼成了 standy，别改。 */
            char nm[48];
            snprintf(nm, sizeof(nm), "sim/radios/com%d_standy_flip", c->com_index);
            if (cmd_once(nm))
                xconnect_logf("control: COM%d flip via command", c->com_index);
            break;
        }

        case XCMD_AUDIO_TX: {
            char nm[56];
            snprintf(nm, sizeof(nm), "sim/audio_panel/transmit_audio_com%d",
                     c->com_index);
            if (cmd_once(nm))
                xconnect_logf("control: audio TX -> COM%d", c->com_index);
            break;
        }

        case XCMD_AUDIO_MON: {
            char nm[56];
            snprintf(nm, sizeof(nm), "sim/audio_panel/monitor_audio_com%d",
                     c->com_index);
            if (cmd_once(nm))
                xconnect_logf("control: audio MON toggle COM%d", c->com_index);
            break;
        }

        case XCMD_COM_POWER: {
            char nm[48];
            snprintf(nm, sizeof(nm), "sim/radios/power_com%d_%s",
                     c->com_index, c->active ? "on" : "off");
            if (cmd_once(nm))
                xconnect_logf("control: COM%d power %s", c->com_index,
                              c->active ? "on" : "off");
            break;
        }

        default:
            break;
        }
    }
}
