/* MConnect PMDG SDK 桥测试（不需要模拟器、不需要 PMDG 装机）。
 *
 * PMDG 是唯一"既没有可读 LVar、又不维护标准 SimVar"的机模，只能走官方
 * SDK 的二进制 ClientData。这里验证不需要实机的部分：
 *
 *   1. SDK 协议常量（通道名/ID/事件偏移）与官方头文件一致
 *   2. 数据解析（按固定偏移取应答机三字段）
 *   3. **模式枚举重映射**（PMDG 原生 ≠ 规范枚举，顺序不同，错位会把
 *      ALT RPTG OFF 显示成 STBY）
 *   4. 状态机（off → waiting → ready / failed 超时）
 *   5. 事件发送（Event + Parameter 8 字节结构）
 *   6. 载荷尺寸校验（SDK 版本不同时拒绝解析，避免读错位）
 *
 * 基准来源：本机官方头文件
 *   L:\MSFS\Community\pmdg-aircraft-737\Documentation\SDK\PMDG_NG3_SDK.h
 */
#include "pmdgbridge.h"
#include "sc.h"
#include "mconnect.h"

#include <stdio.h>
#include <stdlib.h>
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

static void check(int cond, const char* what, const char* detail) {
    if (cond) { g_pass++; printf("  PASS  %s\n", what); }
    else { g_fail++; printf("  FAIL  %s   [%s]\n", what, detail ? detail : ""); }
}

static void check_int(int got, int want, const char* what) {
    char d[128];
    snprintf(d, sizeof(d), "got=%d want=%d", got, want);
    check(got == want, what, d);
}

/* ---- 捕获 SetClientData ---- */
static unsigned g_last_event = 0;
static unsigned g_last_param = 0;
static int g_send_count = 0;
static DWORD g_last_cd_id = 0;

static HRESULT WINAPI stub_set_client_data(HANDLE h, DWORD cd_id, DWORD def,
                                           DWORD fl, DWORD rv, DWORD cb,
                                           PVOID data) {
    (void)h; (void)def; (void)fl; (void)rv;
    if (data && cb == 8) {
        struct { unsigned e, p; }* c = data;
        g_last_event = c->e;
        g_last_param = c->p;
        g_send_count++;
    }
    g_last_cd_id = cd_id;
    return S_OK;
}
static HRESULT WINAPI stub_map(HANDLE h, const char* n, DWORD id) {
    (void)h; (void)n; (void)id; return S_OK;
}
static HRESULT WINAPI stub_create(HANDLE h, DWORD id, DWORD sz, DWORD fl) {
    (void)h; (void)id; (void)sz; (void)fl; return S_OK;
}
static HRESULT WINAPI stub_adddef(HANDLE h, DWORD d, DWORD o, DWORD sz,
                                  float e, DWORD dt) {
    (void)h; (void)d; (void)o; (void)sz; (void)e; (void)dt; return S_OK;
}
static HRESULT WINAPI stub_request(HANDLE h, DWORD cd, DWORD rq, DWORD df,
                                   DWORD pd, DWORD f, DWORD r, DWORD cb) {
    (void)h; (void)cd; (void)rq; (void)df; (void)pd; (void)f; (void)r; (void)cb;
    return S_OK;
}

/* 构造 PMDG 数据广播报文 */
typedef struct {
    DWORD dwSize, dwVersion, dwID, dwRequestID;
    DWORD dwDefineID, dwFlags;
    DWORD dwEntryNumber, dwOutOf, dwDefineCount;
    BYTE  data[1];
} RecvHdr;

/* 按机型族灌入一帧数据。def 用该机型族的 DataDefinition。 */
static void feed_data_for(PmdgType type, int mode, int sel, int extra,
                          int payload_size) {
    static BYTE buf[4096];
    RecvHdr* d = (RecvHdr*)buf;
    DWORD hdr = (DWORD)(sizeof(RecvHdr) - 1);
    int i;

    memset(buf, 0, sizeof(buf));
    d->dwSize = hdr + (DWORD)payload_size;
    d->dwID = SC_RECV_ID_CLIENT_DATA;
    d->dwRequestID = 7001;
    d->dwDefineID = (type == PMDG_TYPE_777) ? 0x504D4448u : 0x4E473332u;
    d->dwDefineCount = 1;
    for (i = 0; i < payload_size && (DWORD)(hdr + i) < sizeof(buf); i++)
        d->data[i] = 0;

    if (type == PMDG_TYPE_777) {
        if (payload_size > PMDG777_OFF_XPDR_IDENT) {
            d->data[PMDG777_OFF_XPDR_SEL] = (BYTE)(sel ? 1 : 0);
            d->data[PMDG777_OFF_XPDR_MODE] = (BYTE)mode;
            d->data[PMDG777_OFF_XPDR_IDENT] = (BYTE)(extra ? 1 : 0);
        }
    } else {
        if (payload_size > PMDG737_OFF_XPDR_FAIL) {
            d->data[PMDG737_OFF_XPDR_SEL] = (BYTE)(sel ? 1 : 0);
            d->data[PMDG737_OFF_XPDR_MODE] = (BYTE)mode;
            d->data[PMDG737_OFF_XPDR_FAIL] = (BYTE)(extra ? 1 : 0);
        }
    }
    pmdgbridge_on_client_data(buf, d->dwSize);
}

/* 兼容旧签名（737） */
static void feed_data(int mode, int sel2, int fail, int payload_size) {
    feed_data_for(PMDG_TYPE_737, mode, sel2, fail, payload_size);
}

int main(void) {
    PmdgXpdr x;

    printf("== MConnect PMDG SDK 桥测试 ==\n\n");

    /* --- 1. 协议常量与官方头文件一致 --- */
    printf("[1] 协议常量（对照 PMDG_NG3_SDK.h）\n");
    check_int(PMDG737_DATA_SIZE, 916, "sizeof(PMDG_NG3_Data) = 916");
    check_int(PMDG737_OFF_XPDR_MODE, 612, "XPDR_ModeSel 偏移 = 612");
    /* 事件基址 69632，EVT_TCAS_MODE = +800 */
    check_int(PMDG737_EVT_TCAS_MODE, 70432, "737 EVT_TCAS_MODE = 70432");
    check_int(69632 + 806, 70438, "737 EVT_TCAS_IDENT = 70438");
    /* 777 是另一套（别想当然只改通道名） */
    check_int(PMDG777_DATA_SIZE, 684, "sizeof(PMDG_777X_Data) = 684");
    check_int(PMDG777_OFF_XPDR_MODE, 449, "777 XPDR_ModeSel 偏移 = 449");
    check_int(69632 + 749, 70381, "777 EVT_TCAS_MODE = 70381");
    check_int(69632 + 746, 70378, "777 EVT_TCAS_IDENT = 70378");
    check(PMDG737_DATA_SIZE != PMDG777_DATA_SIZE, "两机型结构大小不同", NULL);
    check(PMDG737_EVT_TCAS_MODE != PMDG777_EVT_TCAS_MODE,
          "两机型事件号不同", NULL);
    /* PMDG 原生模式枚举 */
    check_int(PMDG_XPDR_STBY, 0, "STBY = 0");
    check_int(PMDG_XPDR_ALT_OFF, 1, "ALT RPTG OFF = 1");
    check_int(PMDG_XPDR_ON, 2, "ON = 2");
    check_int(PMDG_XPDR_ALT, 3, "ALT = 3");
    check_int(PMDG_XPDR_TARA, 4, "TA/RA = 4");

    /* --- 2. 未初始化的行为 --- */
    printf("\n[2] 未初始化\n");
    check_int((int)pmdgbridge_state(), (int)PMDG_STATE_OFF, "初始 OFF");
    check_int(pmdgbridge_get_xpdr(&x), 0, "无数据");
    check_int(pmdgbridge_send_event(1, 1), 0, "OFF 时拒绝发事件");
    check_int(pmdgbridge_set_xpdr_mode(2), 0, "OFF 时拒绝设模式");

    /* --- 3. 初始化 --- */
    printf("\n[3] 初始化\n");
    g_sc.SetClientData = stub_set_client_data;
    g_sc.CreateClientData = stub_create;
    g_sc.MapClientDataNameToID = stub_map;
    g_sc.AddToClientDataDefinition = stub_adddef;
    g_sc.RequestClientData = stub_request;
    g_sc_handle = (HANDLE)0x1;

    check_int(pmdgbridge_init(PMDG_TYPE_737), 0, "init(737) 返回 0");
    check_int((int)pmdgbridge_type(), (int)PMDG_TYPE_737, "机型族 = 737");
    check_int((int)pmdgbridge_state(), (int)PMDG_STATE_WAIT, "进入 WAIT（等广播）");
    check_int(pmdgbridge_set_xpdr_mode(2), 0, "WAIT 时还不能写（无当前值）");

    /* --- 4. 收数据 → READY --- */
    printf("\n[4] 收到 SDK 数据\n");
    feed_data(PMDG_XPDR_STBY, 0, 0, 916);
    check_int((int)pmdgbridge_state(), (int)PMDG_STATE_READY, "进入 READY");
    check_int(pmdgbridge_get_xpdr(&x), 1, "有数据");
    check_int(x.mode, PMDG_XPDR_STBY, "解出 mode = STBY");
    check_int(x.selector, 0, "selector = 1");
    check_int(x.annun_fail, 0, "无 FAIL");

    printf("\n[4b] 字段偏移正确性\n");
    feed_data(PMDG_XPDR_TARA, 1, 1, 916);
    pmdgbridge_get_xpdr(&x);
    check_int(x.mode, PMDG_XPDR_TARA, "mode = TA/RA（偏移 612）");
    check_int(x.selector, 1, "selector = 2（偏移 610）");
    check_int(x.annun_fail, 1, "FAIL（偏移 613）");

    /* --- 5. 模式旋钮：按差值转动 --- */
    printf("\n[5] 模式转动（旋钮语义）\n");
    feed_data(PMDG_XPDR_STBY, 0, 0, 916);   /* 当前 STBY(0) */
    g_send_count = 0;
    check_int(pmdgbridge_set_xpdr_mode(PMDG_XPDR_ON), 1, "STBY → ON 成功");
    /* 0→2 需要顺时针转 2 格 */
    check_int(g_send_count, 2, "转了 2 格");
    check_int((int)g_last_event, 70432, "事件 = EVT_TCAS_MODE");
    check_int((int)g_last_param, 1, "参数 = 1（顺时针）");

    printf("\n[5b] 已是目标档则不动\n");
    feed_data(PMDG_XPDR_ON, 0, 0, 916);
    g_send_count = 0;
    check_int(pmdgbridge_set_xpdr_mode(PMDG_XPDR_ON), 1, "返回成功");
    check_int(g_send_count, 0, "不转（避免多余动作）");

    printf("\n[5c] 逆时针（取最短路径）\n");
    feed_data(PMDG_XPDR_STBY, 0, 0, 916);   /* 0 */
    g_send_count = 0;
    /* 0 → TA/RA(4)：顺时针 4 格，逆时针 1 格 → 应走逆时针 1 格 */
    check_int(pmdgbridge_set_xpdr_mode(PMDG_XPDR_TARA), 1, "STBY → TA/RA");
    check_int(g_send_count, 1, "只转 1 格（最短路径）");
    check_int((int)g_last_param, 0xFFFFFFFF, "参数 = -1（逆时针）");

    printf("\n[5d] 越界参数拒绝\n");
    check_int(pmdgbridge_set_xpdr_mode(-1), -1, "-1 拒绝");
    check_int(pmdgbridge_set_xpdr_mode(9), -1, "9 拒绝");

    /* --- 6. IDENT --- */
    printf("\n[6] IDENT\n");
    g_send_count = 0;
    check_int(pmdgbridge_set_xpdr_ident(), 1, "发送成功");
    check_int(g_send_count, 2, "按下 + 松开（2 次）");
    check_int((int)g_last_event, 70438, "事件 = EVT_TCAS_IDENT");
    check_int((int)g_last_param, 0, "最后是松开(0)");

    /* --- 7. 载荷尺寸不符 → 拒绝解析 --- */
    printf("\n[7] 载荷尺寸防护\n");
    {
        /* 重新初始化以清掉 g_data_size_seen */
        pmdgbridge_shutdown();
        pmdgbridge_init(PMDG_TYPE_737);
        feed_data(2, 0, 0, 500);   /* 比期望 916 小 */
        check_int((int)pmdgbridge_state(), (int)PMDG_STATE_FAILED,
                  "尺寸不符 → FAILED（不读错位）");
        check_int(pmdgbridge_get_xpdr(&x), 0, "无数据");
    }

    /* --- 8. 超时 --- */
    printf("\n[8] 超时判定\n");
    {
        pmdgbridge_shutdown();
        pmdgbridge_init(PMDG_TYPE_737);
        check_int((int)pmdgbridge_state(), (int)PMDG_STATE_WAIT, "回到 WAIT");
        pmdgbridge_poll(xc_now_sec() + 10.0);
        check_int((int)pmdgbridge_state(), (int)PMDG_STATE_FAILED,
                  "10s 无数据 → FAILED（多半没开 EnableDataBroadcast）");
    }

    /* --- 9. 777 协议（与 737 完全不同） --- */
    printf("\n[9] 777X 协议\n");
    {
        pmdgbridge_shutdown();
        check_int(pmdgbridge_init(PMDG_TYPE_777), 0, "init(777) 返回 0");
        check_int((int)pmdgbridge_type(), (int)PMDG_TYPE_777, "机型族 = 777");
        check_int((int)pmdgbridge_state(), (int)PMDG_STATE_WAIT, "进入 WAIT");

        /* 684 字节（不是 916） */
        feed_data_for(PMDG_TYPE_777, 2, 1, 1, PMDG777_DATA_SIZE);
        check_int((int)pmdgbridge_state(), (int)PMDG_STATE_READY,
                  "777 数据 → READY");
        check_int(pmdgbridge_get_xpdr(&x), 1, "有数据");
        check_int(x.mode, 2, "解出 mode = 2（777 语义是 XPNDR）");
        check_int(x.selector, 1, "selector = R（偏移 447）");
        check_int(x.ident_pushed, 1, "ident_pushed（偏移 450，737 无此字段）");
        check_int(x.annun_fail, 0, "777 无 FAIL 字段 → 恒 0");
    }

    printf("\n[9b] 777 事件号与 737 不同\n");
    {
        g_send_count = 0;
        feed_data_for(PMDG_TYPE_777, 0, 0, 0, PMDG777_DATA_SIZE);
        check_int(pmdgbridge_set_xpdr_mode(4), 1, "STBY → TA/RA");
        check_int(g_send_count, 1, "转 1 格（最短路径）");
        check_int((int)g_last_event, PMDG777_EVT_TCAS_MODE,
                  "事件 = 777 的 evt_tcas_mode(70381)，不是 737 的 70432");

        g_send_count = 0;
        pmdgbridge_set_xpdr_ident();
        check_int((int)g_last_event, PMDG777_EVT_TCAS_IDENT,
                  "IDENT 事件 = 70378（737 是 70738）");
    }

    printf("\n[9c] 777 载荷尺寸校验用 684 不是 916\n");
    {
        pmdgbridge_shutdown();
        pmdgbridge_init(PMDG_TYPE_777);
        /* 800 字节：比 684 大，比 916 小 —— 若误用 737 的阈值就会判失败 */
        feed_data_for(PMDG_TYPE_777, 2, 0, 0, 800);
        check_int((int)pmdgbridge_state(), (int)PMDG_STATE_READY,
                  "800 字节对 777 足够（说明用的是 684 阈值）");
        check_int(pmdgbridge_get_xpdr(&x), 1, "有数据");
    }

    printf("\n[9d] 未知机型族\n");
    {
        pmdgbridge_shutdown();
        check_int(pmdgbridge_init(PMDG_TYPE_NONE), -1, "NONE 拒绝初始化");
        check_int((int)pmdgbridge_state(), (int)PMDG_STATE_OFF, "保持 OFF");
    }

    printf("\n== 结果: %d 通过, %d 失败 ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}