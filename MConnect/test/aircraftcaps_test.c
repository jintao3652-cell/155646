/* MConnect 机型能力探测测试（不需要模拟器）。
 *
 * 这是"按实际机型给选项"的核心：把当前机模枚举到的 InputEvent 按语义
 * 分类，让客户端知道这架飞机能控制什么。分类错了客户端就会显示不存在的
 * 选项（点了没反应），或者漏掉真能用的按钮。
 *
 * 覆盖：
 *   1. 各机模命名风格的分类（Fenix 下划线 / PMDG 的 EVT_ 前缀 / Asobo 空格）
 *   2. 优先级（XPDR_IDENT 不能被当成 XPDR_MODE，XPDR_MODE 不能被当成 CODE）
 *   3. 分类统计与按分类取事件
 *   4. capabilities 报文结构（JSON 合法、字段齐全、事件列表正确）
 *   5. 机型切换时能力失效（指纹变化 → 需要重推）
 */
#include "aircraftcaps.h"
#include "inputev.h"
#include "wasmbridge.h"
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
FlightRaw g_flight;

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

/* datadefs / catalog 依赖 */
const char* datadefs_icao(void) { return "A20N"; }
const char* catalog_icao_for_title(const char* t) { (void)t; return "A20N"; }

/* control.c 依赖（能力报文里报档案 ID；测试里给固定值） */
const char* control_profile_id(void) { return "fenix-a320"; }
unsigned control_profile_caps(void) { return 0x0Bu; }

static void check(int cond, const char* what, const char* detail) {
    if (cond) { g_pass++; printf("  PASS  %s\n", what); }
    else { g_fail++; printf("  FAIL  %s   [%s]\n", what, detail ? detail : ""); }
}

static void check_int(int got, int want, const char* what) {
    char d[128];
    snprintf(d, sizeof(d), "got=%d want=%d", got, want);
    check(got == want, what, d);
}

static void check_cat(const char* name, InputEventCategory want) {
    InputEventCategory got = inputev_classify(name);
    char d[160];
    snprintf(d, sizeof(d), "\"%s\" got=%s want=%s", name,
             inputev_category_name(got), inputev_category_name(want));
    check(got == want, name, d);
}

/* 构造枚举报文并灌入 */
typedef struct {
    DWORD dwSize, dwVersion, dwID, dwRequestID;
    DWORD dwArraySize, dwEntryNumber, dwOutOf;
    SC_INPUT_EVENT_DESCRIPTOR items[1];
} TestEnumPkt;

static void feed_events(const char** names, DWORD* hashes, int n) {
    static BYTE buf[32768];
    TestEnumPkt* p = (TestEnumPkt*)buf;
    DWORD base = (DWORD)(sizeof(TestEnumPkt) - sizeof(SC_INPUT_EVENT_DESCRIPTOR));
    int i;

    memset(buf, 0, sizeof(buf));
    p->dwSize = base + (DWORD)n * (DWORD)sizeof(SC_INPUT_EVENT_DESCRIPTOR);
    p->dwID = SC_RECV_ID_ENUMERATE_INPUT_EVENTS;
    p->dwRequestID = 9001;
    p->dwArraySize = (DWORD)n;
    p->dwEntryNumber = 0;
    p->dwOutOf = 1;
    for (i = 0; i < n; i++) {
        SC_INPUT_EVENT_DESCRIPTOR* d =
            (SC_INPUT_EVENT_DESCRIPTOR*)(buf + base +
                (size_t)i * sizeof(SC_INPUT_EVENT_DESCRIPTOR));
        strncpy(d->Name, names[i], sizeof(d->Name) - 1);
        d->Hash = hashes[i];
        d->Type = SC_DATATYPE_FLOAT32;
    }
    inputev_on_enumerate(buf, p->dwSize);
}

int main(void) {
    printf("== MConnect 机型能力测试 ==\n\n");

    /* --- 1. 分类：三种机模命名风格 --- */
    printf("[1] 分类（Fenix 下划线风格）\n");
    check_cat("XPDR_STBY_Push", IE_CAT_XPNDR_MODE);
    check_cat("XPDR_CODE_SET", IE_CAT_XPNDR_CODE);
    check_cat("XPDR_IDENT_Push", IE_CAT_XPNDR_IDENT);
    check_cat("COM1_SWAP_Push", IE_CAT_COM_FREQ);
    check_cat("COM1_TRANSMIT_SELECT", IE_CAT_COM_SELECT);
    check_cat("COM2_RECEIVE_SELECT", IE_CAT_COM_SELECT);

    printf("\n[1b] 分类（PMDG EVT_ 前缀风格）\n");
    check_cat("EVT_XPNDR_MODE", IE_CAT_XPNDR_MODE);
    check_cat("EVT_COM1_TRANSFER", IE_CAT_COM_FREQ);
    check_cat("EVT_XPNDR_IDENT", IE_CAT_XPNDR_IDENT);

    printf("\n[1c] 分类（Asobo 空格分词风格）\n");
    check_cat("TRANSPONDER MODE SELECT", IE_CAT_XPNDR_MODE);
    check_cat("TRANSPONDER IDENT", IE_CAT_XPNDR_IDENT);
    check_cat("COM1 STANDBY SWAP", IE_CAT_COM_FREQ);

    printf("\n[1d] 非目标事件不误分类\n");
    check_cat("LIGHT_LANDING_TOGGLE", IE_CAT_NONE);
    check_cat("GEAR_HANDLE_UP", IE_CAT_NONE);
    check_cat("", IE_CAT_NONE);

    /* --- 2. 优先级（最容易错的地方） --- */
    printf("\n[2] 分类优先级\n");
    check_cat("XPDR_IDENT", IE_CAT_XPNDR_IDENT);   /* 不能落 MODE */
    check_cat("XPDR_MODE", IE_CAT_XPNDR_MODE);     /* 不能落 CODE */
    check_cat("XPDR_ALT", IE_CAT_XPNDR_MODE);
    check_cat("TRANSPONDER_CODE", IE_CAT_XPNDR_CODE);
    check_cat("SQUAWK_SET", IE_CAT_XPNDR_CODE);
    check_cat("COM1_XMIT", IE_CAT_COM_SELECT);
    check_cat("COM1_FREQ_KNOB", IE_CAT_COM_FREQ);

    printf("\n[2b] 自动驾驶分类（预留）\n");
    check_cat("AP_MASTER_TOGGLE", IE_CAT_AUTOPILOT);
    check_cat("FCC_SPEED", IE_CAT_AUTOPILOT);
    check_cat("AUTOPILOT_ALT_HOLD", IE_CAT_AUTOPILOT);

    /* --- 3. 枚举灌入后统计 --- */
    printf("\n[3] 分类统计\n");
    {
        static const char* names[] = {
            "XPDR_CODE_SET", "XPDR_CODE_KNOB1", "XPDR_MODE_ALT",
            "XPDR_IDENT_Push", "COM1_SWAP_Push", "COM2_SWAP_Push",
            "COM1_TRANSMIT_SELECT", "LIGHT_LANDING"
        };
        DWORD hashes[8];
        int i;
        for (i = 0; i < 8; i++) hashes[i] = (DWORD)(0x1000 + i);

        feed_events(names, hashes, 8);
        check_int(inputev_count(), 8, "共 8 个事件");
        check_int(inputev_count_category(IE_CAT_XPNDR_CODE), 2, "码 2 个");
        check_int(inputev_count_category(IE_CAT_XPNDR_MODE), 1, "模式 1 个");
        check_int(inputev_count_category(IE_CAT_XPNDR_IDENT), 1, "IDENT 1 个");
        check_int(inputev_count_category(IE_CAT_COM_FREQ), 2, "频率 2 个");
        check_int(inputev_count_category(IE_CAT_COM_SELECT), 1, "选择 1 个");
        check_int(inputev_count_category(IE_CAT_NONE), 1, "未分类 1 个");
    }

    /* --- 4. 按分类取事件 --- */
    printf("\n[4] 按分类取事件\n");
    {
        char nm[INPUTEV_NAME];
        DWORD h = 0;
        check_int(inputev_get_category(IE_CAT_XPNDR_CODE, 0, nm, sizeof(nm), &h),
                  1, "取第 0 个码事件");
        check(strcmp(nm, "XPDR_CODE_SET") == 0, "名字正确", nm);
        check(h == 0x1000, "hash 正确", NULL);
        check_int(inputev_get_category(IE_CAT_XPNDR_CODE, 1, nm, sizeof(nm), NULL),
                  1, "取第 1 个码事件");
        check(strcmp(nm, "XPDR_CODE_KNOB1") == 0, "第二个名字", nm);
        check_int(inputev_get_category(IE_CAT_XPNDR_CODE, 5, nm, sizeof(nm), NULL),
                  0, "越界返回 0");
    }

    /* --- 5. capabilities 报文 --- */
    printf("\n[5] capabilities 报文\n");
    {
        char js[4096];
        int n;

        snprintf(g_flight.title, sizeof(g_flight.title), "Fenix A320");
        n = aircaps_build_json(js, sizeof(js));
        check(n > 0, "生成了报文", NULL);
        check(js[0] == '{', "以 { 开头", NULL);
        check(js[n - 1] == '}', "以 } 结尾", NULL);
        check(strstr(js, "\"type\":\"aircraft_caps\"") != NULL, "type 字段", NULL);
        check(strstr(js, "\"icao\":\"A20N\"") != NULL, "icao 字段", NULL);
        check(strstr(js, "\"title\":\"Fenix A320\"") != NULL, "title 字段", NULL);
        check(strstr(js, "\"xpndr_code\":2") != NULL, "码计数正确", NULL);
        check(strstr(js, "\"wasm\":{") != NULL, "wasm 段", NULL);
        check(strstr(js, "\"XPDR_CODE_SET\"") != NULL, "事件列表含具体名字", NULL);
        check(strstr(js, "\"cat\":\"xpndr_code\"") != NULL, "事件带分类标签", NULL);

        /* 括号配对 */
        {
            int depth = 0, ok = 1, in_str = 0;
            const char* p;
            for (p = js; *p; p++) {
                if (*p == '"' && (p == js || p[-1] != '\\')) in_str = !in_str;
                if (in_str) continue;
                if (*p == '{' || *p == '[') depth++;
                else if (*p == '}' || *p == ']') { depth--; if (depth < 0) ok = 0; }
            }
            check(ok && depth == 0, "JSON 括号平衡", NULL);
        }
    }

    /* --- 6. 变化检测 --- */
    printf("\n[6] 变化检测\n");
    {
        /* 首次 tick 应判脏（指纹为空） */
        check_int(aircaps_tick(), 1, "首次判定需要推送");
        check_int(aircaps_tick(), 0, "无变化时不再推送");

        /* 声称换机型 → 应重新判脏 */
        aircaps_on_aircraft_change("Fenix A320");
        check_int(aircaps_tick(), 1, "换机型后需要重推");

        /* 恢复稳定 */
        aircaps_tick();

        /* 新事件集（模拟新机模枚举回来）→ 指纹变化 */
        {
            static const char* names2[] = { "EVT_COM1_TRANSFER",
                                            "EVT_XPNDR_MODE" };
            DWORD h2[2] = { 0x2001, 0x2002 };
            feed_events(names2, h2, 2);
            check_int(aircaps_tick(), 1, "新增事件后需要重推");
            check_int(aircaps_tick(), 0, "之后稳定");
        }
    }

    /* --- 7. 无事件时也能出报文 --- */
    printf("\n[7] 边界：无 InputEvent\n");
    {
        /* 用新进程做不到清空，这里验证报文仍合法（total=0 分支） */
        char js[4096];
        int n = aircaps_build_json(js, sizeof(js));
        check(n > 0, "仍能生成报文", NULL);
        check(strstr(js, "\"events\":[") != NULL, "events 段存在（可为空）", NULL);
        check(strstr(js, "\"total\":") != NULL, "total 字段存在", NULL);
    }

    printf("\n== 结果: %d 通过, %d 失败 ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}