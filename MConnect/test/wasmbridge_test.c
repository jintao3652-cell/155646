/* MConnect WASM 桥单元测试（不需要模拟器、不需要 WASM 模块）。
 *
 * WASM 桥是三级回退里最后也是最强的通道（执行任意 RPN），协议是
 * MobiFlight 的 ClientData 文本命令。这里验证不需要实机的部分：
 *   1. 命令格式（MF.SimVars.Set.<RPN> 等前缀拼装）
 *   2. 命令队列（顺序、满则丢最旧）
 *   3. 状态机（off → registering → ready / failed 超时）
 *   4. 响应解析（MF.Version. / MF.Clients.Add.x.Finished）
 *   5. LVar 数组按槽位回填
 *   6. ClientData 结构体布局
 *
 * 真实"WASM 模块是否响应"需要实机（--dump-wasm 可查）。
 */
#include "wasmbridge.h"
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

static void check(int cond, const char* what, const char* detail) {
    if (cond) { g_pass++; printf("  PASS  %s\n", what); }
    else { g_fail++; printf("  FAIL  %s   [%s]\n", what, detail ? detail : ""); }
}

static void check_int(int got, int want, const char* what) {
    char d[128];
    snprintf(d, sizeof(d), "got=%d want=%d", got, want);
    check(got == want, what, d);
}

/* ---- 捕获 SetClientData 发出的命令字符串 ---- */
#define SENT_MAX 256
static char g_sent[SENT_MAX][256];
static int g_sent_n = 0;
static char g_sent_last[256];
static DWORD g_last_cd_id = 0;

static HRESULT WINAPI stub_set_client_data(HANDLE h, DWORD cd_id, DWORD define,
                                           DWORD flags, DWORD reserved,
                                           DWORD cb, PVOID data) {
    (void)h; (void)define; (void)flags; (void)reserved; (void)cb;
    if (data) {
        /* 只保留最近 SENT_MAX 条，但"最后一条"始终单独记录 ——
         * 队列满测试要断言最末命令，不能被缓冲上限误导。 */
        if (g_sent_n < SENT_MAX) {
            snprintf(g_sent[g_sent_n], sizeof(g_sent[0]), "%s", (const char*)data);
            g_sent_n++;
        }
        snprintf(g_sent_last, sizeof(g_sent_last), "%s", (const char*)data);
    }
    g_last_cd_id = cd_id;
    return S_OK;
}

/* 通道建立一律成功（桩） */
static HRESULT WINAPI stub_map(HANDLE h, const char* name, DWORD id) {
    (void)h; (void)name; (void)id; return S_OK;
}
static HRESULT WINAPI stub_create(HANDLE h, DWORD id, DWORD size, DWORD flags) {
    (void)h; (void)id; (void)size; (void)flags; return S_OK;
}
static HRESULT WINAPI stub_adddef(HANDLE h, DWORD d, DWORD off, DWORD sz,
                                  float eps, DWORD dat) {
    (void)h; (void)d; (void)off; (void)sz; (void)eps; (void)dat; return S_OK;
}
static HRESULT WINAPI stub_request(HANDLE h, DWORD cd, DWORD req, DWORD def,
                                   DWORD per, DWORD fl, DWORD res, DWORD cb) {
    (void)h; (void)cd; (void)req; (void)def; (void)per; (void)fl; (void)res;
    (void)cb; return S_OK;
}

/* 构造一个 CLIENT_DATA 报文 */
typedef struct {
    DWORD dwSize, dwVersion, dwID, dwRequestID;
    DWORD dwDefineID, dwFlags;
    DWORD dwEntryNumber, dwOutOf, dwDefineCount;
    BYTE  data[1];
} TestData;

static void feed_resp(DWORD def_id, DWORD req_id, const char* str) {
    static BYTE buf[1024];
    TestData* d = (TestData*)buf;
    DWORD hdr = (DWORD)(sizeof(TestData) - 1);
    memset(buf, 0, sizeof(buf));
    d->dwSize = hdr + 256;
    d->dwID = SC_RECV_ID_CLIENT_DATA;
    d->dwRequestID = req_id;
    d->dwDefineID = def_id;
    d->dwDefineCount = 1;
    strncpy((char*)d->data, str, 255);
    wasmbridge_on_client_data(buf, d->dwSize);
}

static void feed_floats(DWORD def_id, DWORD req_id, const float* v, int n) {
    static BYTE buf[1024];
    TestData* d = (TestData*)buf;
    DWORD hdr = (DWORD)(sizeof(TestData) - 1);
    memset(buf, 0, sizeof(buf));
    d->dwSize = hdr + (DWORD)n * 4;
    d->dwID = SC_RECV_ID_CLIENT_DATA;
    d->dwRequestID = req_id;
    d->dwDefineID = def_id;
    d->dwDefineCount = (DWORD)n;
    memcpy(d->data, v, (size_t)n * 4);
    wasmbridge_on_client_data(buf, d->dwSize);
}

static void reset_sent(void) {
    g_sent_n = 0;
    memset(g_sent, 0, sizeof(g_sent));
    g_sent_last[0] = '\0';
}

int main(void) {
    char ver[32];

    printf("== MConnect WASM 桥测试 ==\n\n");

    /* --- 1. ClientData 结构体布局 --- */
    printf("[1] 结构体布局\n");
    /* 头是 9 个 DWORD；结构体末尾有 1 字节柔性数组，MSVC 会按 4 字节对齐
     * 补齐，所以 (sizeof - 1) 不等于 36。用 offsetof 检查真实布局。 */
    check(offsetof(TestData, data) == 9 * 4,
          "载荷起始偏移 = 9*4 = 36 字节", NULL);
    check(offsetof(TestData, dwDefineID) == 4 * 4,
          "dwDefineID 偏移 16", NULL);
    check(offsetof(TestData, dwDefineCount) == 8 * 4,
          "dwDefineCount 偏移 32", NULL);

    /* --- 2. 未初始化的行为 --- */
    printf("\n[2] 未初始化\n");
    check_int((int)wasmbridge_state(), (int)WASM_STATE_OFF, "初始 OFF");
    check_int(wasmbridge_exec_rpn("5 (>L:X)"), -1, "OFF 时 exec_rpn 拒绝");
    check_int(wasmbridge_watch("(A:LIGHT LANDING,Bool)", 0), -1,
              "OFF 时 watch 拒绝（待就绪后补发）");
    {
        const char* n = wasmbridge_state_name(WASM_STATE_OFF);
        check(strcmp(n, "off") == 0, "OFF 名字 = off", n);
    }

    /* --- 3. 初始化：应发出 Ping/Version/Clients.Add --- */
    printf("\n[3] 初始化命令序列\n");
    g_sc.SetClientData = stub_set_client_data;
    g_sc.CreateClientData = stub_create;
    g_sc.MapClientDataNameToID = stub_map;
    g_sc.AddToClientDataDefinition = stub_adddef;
    g_sc.RequestClientData = stub_request;
    g_sc_handle = (HANDLE)0x1;

    reset_sent();
    {
        int rc = wasmbridge_init();
        check_int(rc, 0, "init 返回 0（已发出注册）");
        check_int((int)wasmbridge_state(), (int)WASM_STATE_REGISTERING,
                  "状态进入 REGISTERING");
        check_int(g_sent_n >= 3, 1, "至少发出 3 条命令");
        check(strstr(g_sent[0], "MF.Ping") != NULL ||
              strstr(g_sent[1], "MF.Ping") != NULL, "含 MF.Ping（探活）", g_sent[0]);
        {
            int i, found = 0;
            for (i = 0; i < g_sent_n; i++)
                if (strstr(g_sent[i], "MF.Clients.Add.mconnect")) found = 1;
            check(found, "含 MF.Clients.Add.mconnect（注册独立通道）", NULL);
        }
        {
            int i, found = 0;
            for (i = 0; i < g_sent_n; i++)
                if (strstr(g_sent[i], "MF.Version.Get")) found = 1;
            check(found, "含 MF.Version.Get", NULL);
        }
    }

    /* --- 4. 未就绪时 exec_rpn 拒绝（避免静默丢命令） --- */
    printf("\n[4] REGISTERING 阶段\n");
    check_int(wasmbridge_exec_rpn("1 (>L:X)"), -1, "未 ready 时拒绝 RPN");

    /* --- 5. 响应解析：注册完成 --- */
    printf("\n[5] 响应解析\n");
    feed_resp(1000 /*CD_DEF_STR*/, 1000 /*CD_REQ_RESP*/, "MF.Version.0.6.0");
    check_int(wasmbridge_version(ver, sizeof(ver)), 1, "版本已记录");
    check(strcmp(ver, "0.6.0") == 0, "版本 = 0.6.0", ver);

    feed_resp(1000, 1000, "MF.Clients.Add.mconnect.Finished");
    check_int((int)wasmbridge_state(), (int)WASM_STATE_READY,
              "收到 .Finished → READY");

    /* --- 6. RPN 命令格式 --- */
    printf("\n[6] RPN 命令格式\n");
    reset_sent();
    /* 先把 [3] init 与 [8] watch 之外残留的队列排空，避免干扰计数 */
    {
        int i;
        for (i = 0; i < 20; i++) wasmbridge_poll(xc_now_sec());
    }
    reset_sent();
    check_int(wasmbridge_exec_rpn("5 (>L:MY_VAR)"), 0, "入队成功");
    wasmbridge_poll(xc_now_sec());
    check_int(g_sent_n, 1, "poll 后发出 1 条");
    check(strcmp(g_sent[0], "MF.SimVars.Set.5 (>L:MY_VAR)") == 0,
          "格式 = MF.SimVars.Set.<RPN>", g_sent[0]);

    printf("\n[6b] READY 后走自己的 Command 通道\n");
    check_int((int)g_last_cd_id, 1010, "ClientData ID = mconnect.Command(1010)");

    reset_sent();
    wasmbridge_exec_rpn("1 (>B:FOO_Push)");
    wasmbridge_poll(xc_now_sec());
    check(strcmp(g_sent[0], "MF.SimVars.Set.1 (>B:FOO_Push)") == 0,
          "B 事件格式正确", g_sent[0]);

    /* --- 7. 队列顺序与满载 --- */
    printf("\n[7] 命令队列\n");
    reset_sent();
    {
        int i;
        char rpn[64];
        for (i = 0; i < 10; i++) {
            snprintf(rpn, sizeof(rpn), "%d (>L:X)", i);
            wasmbridge_exec_rpn(rpn);
        }
        wasmbridge_poll(xc_now_sec());   /* 每帧最多发 8 条 */
        check_int(g_sent_n, 8, "单帧最多发 8 条（限流）");
        wasmbridge_poll(xc_now_sec());
        check_int(g_sent_n, 10, "剩余 2 条下一帧发完");
        check(strstr(g_sent[9], "9 (>L:X)") != NULL, "顺序保持（最后是 9）",
              g_sent[9]);
    }

    printf("\n[7b] 队列满丢最旧\n");
    reset_sent();
    {
        int i;
        char rpn[64];
        for (i = 0; i < 100; i++) {   /* 超过 WASM_CMD_MAX(64) */
            snprintf(rpn, sizeof(rpn), "%d (>L:X)", i);
            wasmbridge_exec_rpn(rpn);
        }
        /* 全部发完（多轮 poll，每轮限流 8 条） */
        for (i = 0; i < 30; i++) wasmbridge_poll(xc_now_sec());
        /* 队列容量 64 → 实际只保留最后 64 条（36..99），最早 36 条被丢弃。
         * 断言：最后一条是最新命令，且第 0 条不是最早的 0。 */
        check(strstr(g_sent_last, "99 (>L:X)") != NULL,
              "最后一条是最新命令 99", g_sent_last);
        check(strstr(g_sent[0], "0 (>L:X)") == NULL,
              "最早的命令被丢弃（不是 0）", g_sent[0]);
        check(g_sent_n > 0 && g_sent_n <= (int)(sizeof(g_sent)/sizeof(g_sent[0])),
              "捕获数量在缓冲范围内", NULL);
    }

    /* --- 8. LVar 监视与回填 --- */
    printf("\n[8] LVar 监视\n");
    reset_sent();
    check_int(wasmbridge_watch("(A:LIGHT LANDING,Bool)", 0), 0, "注册 slot 0");
    check_int(wasmbridge_watch("(A:GEAR HANDLE POSITION,Percent)", 1), 0,
              "注册 slot 1");
    wasmbridge_poll(xc_now_sec());
    {
        int i, found = 0;
        for (i = 0; i < g_sent_n; i++)
            if (strstr(g_sent[i], "MF.SimVars.Add.(A:LIGHT LANDING,Bool)")) found = 1;
        check(found, "命令格式 = MF.SimVars.Add.<expr>", NULL);
    }

    printf("\n[8b] 值回填\n");
    {
        double v = 0.0;
        check_int(wasmbridge_get_value(0, &v), 0, "回填前无值");
        {
            float vals[2] = { 1.0f, 0.5f };
            feed_floats(1001 /*CD_DEF_FLOAT*/, 1001 /*CD_REQ_LVARS*/, vals, 2);
        }
        check_int(wasmbridge_get_value(0, &v), 1, "回填后有值");
        check(v == 1.0, "slot0 = 1.0", NULL);
        check_int(wasmbridge_get_value(1, &v), 1, "slot1 有值");
        check(v == 0.5, "slot1 = 0.5", NULL);
        check_int(wasmbridge_get_value(9, &v), 0, "未注册槽位无值");
    }

    /* --- 9. 通道 ID 过滤 --- */
    printf("\n[9] 报文过滤\n");
    {
        float vals[1] = { 9.9f };
        double v = 0.0;
        feed_floats(999 /* 非本客户端定义 */, 1001, vals, 1);
        check_int(wasmbridge_get_value(0, &v), 1, "旧值保持不变");
        check(v == 1.0, "未被错误报文覆盖", NULL);
    }

    /* --- 10. 注册超时 → FAILED --- */
    printf("\n[10] 注册超时\n");
    wasmbridge_shutdown();
    reset_sent();
    wasmbridge_init();          /* 重新进入 REGISTERING */
    check_int((int)wasmbridge_state(), (int)WASM_STATE_REGISTERING, "回到 REGISTERING");
    wasmbridge_poll(xc_now_sec() + 10.0);   /* 模拟 10 秒后 */
    check_int((int)wasmbridge_state(), (int)WASM_STATE_FAILED,
              "超时 5s → FAILED（WASM 模块未装）");
    check_int(wasmbridge_exec_rpn("1 (>L:X)"), -1, "FAILED 后拒绝 RPN");

    printf("\n== 结果: %d 通过, %d 失败 ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}