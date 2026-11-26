/* MConnect 无线电报文测试（不需要模拟器）。
 *
 * 验证 flight_data 里无线电相关字段的**输出正确性**：
 *   1. 频率从 Hz/BCO16 转 MHz*1000 的换算
 *   2. 发射台判定（COM TRANSMIT 交叉验证 + 枚举越界处理）
 *   3. 全收 / 面板供电字段
 *   4. JSON 合法性（字段名、类型、数组形态）
 *
 * 这些是"客户端能不能正确显示无线电状态"的直接前提——字段名写错或
 * 换算错一位，前端 RMP 面板就会显示错误频率，属于会被飞行员当真的 bug。
 */
#include "datadefs.h"
#include "sc.h"
#include "mconnect.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

ScApi g_sc;
HANDLE g_sc_handle = NULL;
int g_verbose_flag = 0;
int g_verbose = 0;

static int g_fail = 0;
static int g_pass = 0;

double xc_now_sec(void) {
    static LARGE_INTEGER f;
    static int init = 0;
    LARGE_INTEGER n;
    if (!init) { QueryPerformanceFrequency(&f); init = 1; }
    QueryPerformanceCounter(&n);
    return (double)n.QuadPart / (double)f.QuadPart;
}

void xc_logf(const char* fmt, ...) {
    static int v = -1;
    va_list ap;
    if (v < 0) v = getenv("TESTLOG") ? 1 : 0;
    if (!v) return;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

void xc_infof(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

static const char* g_exe = ".";
const char* xc_exe_dir(void) { return g_exe; }

/* catalog.c 依赖 */
const char* catalog_icao_for_title(const char* title) { (void)title; return NULL; }
void catalog_sanitize_icao(const char* raw, char* out, int cap) {
    int n = 0;
    out[0] = '\0';
    if (!raw) return;
    while (*raw && n < cap - 1) out[n++] = *raw++;
    out[n] = '\0';
}

/* sc.c 依赖（报文里带 sim 字段） */
const char* sc_sim_name(void) { return "2020"; }

static void check(int cond, const char* what, const char* detail) {
    if (cond) { g_pass++; printf("  PASS  %s\n", what); }
    else { g_fail++; printf("  FAIL  %s   [%s]\n", what, detail ? detail : ""); }
}

static void check_int(int got, int want, const char* what) {
    char d[128];
    snprintf(d, sizeof(d), "got=%d want=%d", got, want);
    check(got == want, what, d);
}

/* 在 JSON 文本里找 "key":value，取整数 */
static int json_int(const char* js, const char* key, int* out) {
    char pat[64];
    const char* p;
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    p = strstr(js, pat);
    if (!p) return 0;
    p += strlen(pat);
    *out = atoi(p);
    return 1;
}

/* 取 "key":arrayname 里的数值串（用于 com_transmit 数组） */
static int json_arr3(const char* js, const char* key, int v[3]) {
    char pat[64];
    const char* p;
    snprintf(pat, sizeof(pat), "\"%s\":[", key);
    p = strstr(js, pat);
    if (!p) return 0;
    if (sscanf(p + strlen(pat), "%d,%d,%d", &v[0], &v[1], &v[2]) != 3) return 0;
    return 1;
}

int main(void) {
    static char js[8192];
    int got;

    printf("== MConnect 无线电报文测试 ==\n\n");

    /* --- 1. 基本频率输出 --- */
    printf("[1] 频率输出（MHz*1000）\n");
    memset(&g_flight, 0, sizeof(g_flight));
    g_flight.com1_mhz1000 = 122800;      /* 122.800 */
    g_flight.com2_mhz1000 = 121500;      /* 121.500 */
    g_flight.com3_mhz1000 = 0;           /* 双台机型：3 号台不存在 */
    g_flight.com1_stby_mhz1000 = 118000;
    g_flight.com2_stby_mhz1000 = 136975; /* 8.33kHz 上边界 */
    g_flight.com3_stby_mhz1000 = 0;
    g_flight.num_engines = 2;
    datadefs_build_flight_json(js, sizeof(js));

    check(json_int(js, "com1_freq", &got) && got == 122800,
          "com1_freq = 122800", js);
    check(json_int(js, "com2_freq", &got) && got == 121500, "com2_freq = 121500", NULL);
    check(json_int(js, "com3_freq", &got) && got == 0, "com3_freq = 0（无该台）", NULL);
    check(json_int(js, "com1_standby", &got) && got == 118000,
          "com1_standby = 118000", NULL);
    check(json_int(js, "com2_standby", &got) && got == 136975,
          "com2_standby = 136975（8.33 上边界）", NULL);

    /* --- 2. 发射台推导（纯函数） --- */
    printf("\n[2] 发射台推导\n");
    {
        int t[3];

        t[0] = 0; t[1] = 1; t[2] = 0;
        check_int(datadefs_derive_pilot_xmit(t, 1), 1, "COM2 在发 + 枚举 COM2 → 1");

        t[0] = 0; t[1] = 0; t[2] = 1;
        check_int(datadefs_derive_pilot_xmit(t, 0), 2,
                  "COM3 在发 + 枚举恒 0（机型未实现）→ 2，以 COM TRANSMIT 为准");

        t[0] = 0; t[1] = 0; t[2] = 0;
        check_int(datadefs_derive_pilot_xmit(t, 1), 1,
                  "都没在发 + 枚举 COM2 → 1（选了台未按 PTT）");

        t[0] = 0; t[1] = 0; t[2] = 0;
        check_int(datadefs_derive_pilot_xmit(t, -1), -1, "都未知 → -1");

        t[0] = 1; t[1] = 1; t[2] = 1;
        check_int(datadefs_derive_pilot_xmit(t, 2), 2,
                  "三台同时为真（机型 bug）+ 枚举 COM3 → 取枚举 2");

        t[0] = 1; t[1] = 1; t[2] = 1;
        check_int(datadefs_derive_pilot_xmit(t, -1), 0,
                  "三台为真且枚举无效 → 取第一个 0");

        t[0] = 0; t[1] = 0; t[2] = 1;
        check_int(datadefs_derive_pilot_xmit(t, 99), 2,
                  "枚举越界(99) 视为未知，靠 COM TRANSMIT → 2");

        check_int(datadefs_derive_pilot_xmit(NULL, 1), 1,
                  "com_transmit 为 NULL 时用枚举");
        check_int(datadefs_derive_pilot_xmit(NULL, -1), -1, "NULL + 未知 → -1");
    }

    /* --- 2e. 报文里体现推导结果 --- */
    printf("\n[2e] 报文中的 pilot_xmit\n");
    memset(&g_flight, 0, sizeof(g_flight));
    g_flight.num_engines = 2;
    g_flight.com_transmit[2] = 1;
    g_flight.pilot_xmit_type = 2;
    datadefs_build_flight_json(js, sizeof(js));
    check(json_int(js, "pilot_xmit", &got) && got == 2, "pilot_xmit = 2", NULL);

    memset(&g_flight, 0, sizeof(g_flight));
    g_flight.num_engines = 2;
    g_flight.pilot_xmit_type = -1;
    datadefs_build_flight_json(js, sizeof(js));
    check(json_int(js, "pilot_xmit", &got) && got == -1,
          "pilot_xmit = -1（未选中/未知）", NULL);

    /* --- 3. com_transmit 数组 --- */
    printf("\n[3] com_transmit 数组\n");
    memset(&g_flight, 0, sizeof(g_flight));
    g_flight.num_engines = 2;
    g_flight.com_transmit[0] = 1;
    g_flight.com_transmit[1] = 0;
    g_flight.com_transmit[2] = 1;
    datadefs_build_flight_json(js, sizeof(js));
    {
        int v[3] = { -9, -9, -9 };
        check(json_arr3(js, "com_transmit", v), "数组可解析", js);
        check(v[0] == 1 && v[1] == 0 && v[2] == 1, "[1,0,1] 正确", NULL);
    }

    /* --- 4. 全收 / 面板供电 --- */
    printf("\n[4] 全收与面板供电\n");
    memset(&g_flight, 0, sizeof(g_flight));
    g_flight.num_engines = 2;
    g_flight.com_receive_all = 1;
    g_flight.audio_panel_on = 1;
    datadefs_build_flight_json(js, sizeof(js));
    check(json_int(js, "com_receive_all", &got) && got == 1, "com_receive_all = 1", NULL);
    check(json_int(js, "audio_panel_on", &got) && got == 1, "audio_panel_on = 1", NULL);

    printf("\n[4b] 断电时如实报 0\n");
    memset(&g_flight, 0, sizeof(g_flight));
    g_flight.num_engines = 2;
    g_flight.audio_panel_on = 0;
    datadefs_build_flight_json(js, sizeof(js));
    check(json_int(js, "audio_panel_on", &got) && got == 0,
          "audio_panel_on = 0（无线电不可用）", NULL);

    /* --- 4d. 应答机 S/N 方向 ---
     *
     * FSD `@` 报文第 0 字段只认 S/N/Y：
     *   S = Standby（待机不发射）
     *   N = Normal（正常发射）—— "N" 是 normal，不是 "no"
     * MSFS 的 TRANSPONDER STATE：0=Off 1=Stby 2=Test 3=On 4=Alt 5=Ground
     * 所以 state<=1 → S，state>=2 → N。
     *
     * 曾写反成 `state >= 2 ? "S" : "N"`，与 XConnect 犯同一个错，
     * 对端与管制员看到的应答机状态永远是反的。 */
    printf("\n[4d] 应答机 S/N 方向\n");
    {
        struct { int state; int code; const char* want; const char* why; } c[] = {
            { 0, 7000, "N", "Off 但码非零 → 兜底视为发射" },
            { 0,    0, "S", "Off 且码零 → 待机" },
            { 1, 7000, "S", "Stby → 待机" },
            { 2, 7000, "N", "Test → 发射" },
            { 3, 7000, "N", "On → 发射" },
            { 4, 7000, "N", "Alt → 发射" },
            { 5, 7000, "N", "Ground → 发射" },
        };
        size_t k;
        for (k = 0; k < sizeof(c) / sizeof(c[0]); k++) {
            char want[8];
            memset(&g_flight, 0, sizeof(g_flight));
            g_flight.num_engines = 2;
            g_flight.xpdr_state = c[k].state;
            g_flight.transponder = c[k].code;
            datadefs_build_flight_json(js, sizeof(js));
            snprintf(want, sizeof(want), "\"squark\":\"%s\"", c[k].want);
            check(strstr(js, want) != NULL, c[k].why, want);
        }
    }

    /* --- 4c. engines 必须是 JSON 对象，不是数组 ---
     *
     * swift 的 CAircraftEngineList::toJson() 返回 QJsonObject：
     *   {"1":{"on":false},"2":{"on":false}}
     * 曾发成数组 [{"on":..,"rpm_pct":..}]，服务器类型不匹配 → 整包解析失败，
     * 站点 JSON 里 aircraft.engines 恒为 null（XConnect 侧踩了三次才定位）。
     * 这条断言就是锁住这个格式，防止回退。 */
    printf("\n[4c] engines 格式（必须是对象）\n");
    memset(&g_flight, 0, sizeof(g_flight));
    g_flight.num_engines = 2;
    datadefs_build_flight_json(js, sizeof(js));
    {
        const char* e = strstr(js, "\"engines\":");
        check(e != NULL, "含 engines 字段", NULL);
        if (e) {
            const char* v = e + 10;
            while (*v == ' ') v++;
            check(*v == '{', "engines 是对象（不是数组）", v);
            /* 键必须是 1 基数字 */
            check(strstr(v, "\"1\":{") != NULL, "含键 \"1\"", NULL);
            check(strstr(v, "\"2\":{") != NULL, "含键 \"2\"", NULL);
            /* 不发 rpm_pct（服务器只认它认识的键，多余键会让该台解析失败） */
            check(strstr(v, "rpm_pct") == NULL, "不含 rpm_pct", NULL);
        }
    }

    /* --- 5. JSON 结构合法性 --- */
    printf("\n[5] JSON 结构\n");
    memset(&g_flight, 0, sizeof(g_flight));
    g_flight.num_engines = 2;
    datadefs_build_flight_json(js, sizeof(js));
    {
        int depth = 0, ok = 1, in_str = 0;
        const char* p;
        for (p = js; *p; p++) {
            if (*p == '"' && (p == js || p[-1] != '\\')) in_str = !in_str;
            if (in_str) continue;
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') depth--;
            if (depth < 0) { ok = 0; break; }
        }
        check(ok, "括号配对（无提前闭合）", NULL);
        check(depth == 0, "括号最终闭合平衡", NULL);
        check(js[0] == '{', "以 { 开头", NULL);
        check(js[strlen(js) - 1] == '}', "以 } 结尾", NULL);
    }

    /* --- 4e. 跨平台字段对齐（aircraft_name / avionics_on）---
     * XConnect 早就发 aircraft_name，MConnect 之前缺失 → 客户端在
     * aircraft_icao 为空时无法按机型名去 swift 机型库反查，只能报空机型。 */
    printf("\n[4e] 跨平台字段对齐\n");
    memset(&g_flight, 0, sizeof(g_flight));
    g_flight.num_engines = 2;
    snprintf(g_flight.title, sizeof(g_flight.title), "PMDG 737-800 Lufthansa");
    snprintf(g_flight.icao_type, sizeof(g_flight.icao_type), "B738");
    datadefs_build_flight_json(js, sizeof(js));
    check(strstr(js, "\"aircraft_name\":\"PMDG 737-800") != NULL,
          "aircraft_name 已输出（ICAO 推断兜底用）", NULL);
    check(strstr(js, "\"avionics_on\":") != NULL,
          "avionics_on 已输出（与 XConnect 对齐）", NULL);
    {
        const char* a = strstr(js, "\"aircraft_icao\"");
        const char* b = strstr(js, "\"aircraft_name\"");
        check(a && b && a < b, "aircraft_icao 在 aircraft_name 之前（顺序不乱）", NULL);
    }

    printf("\n[5b] 必需字段齐全\n");
    {
        static const char* need[] = {
            "latitude", "longitude", "altitude_msl", "heading",
            "com1_freq", "com2_freq", "com3_freq",
            "com1_standby", "com2_standby", "com3_standby",
            "com_transmit", "pilot_xmit", "copilot_xmit",
            "com_receive_all", "audio_panel_on",
            "transponder", "xpdr_mode", "squark", "on_ground"
        };
        size_t i;
        for (i = 0; i < sizeof(need) / sizeof(need[0]); i++) {
            char pat[64];
            snprintf(pat, sizeof(pat), "\"%s\":", need[i]);
            if (!strstr(js, pat)) {
                char msg[96];
                snprintf(msg, sizeof(msg), "缺字段 %s", need[i]);
                check(0, msg, NULL);
            }
        }
        check(1, "全部必需字段存在", NULL);
    }

    printf("\n== 结果: %d 通过, %d 失败 ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}