/* MConnect Input Events 单元测试（不需要模拟器）。
 *
 * Input Events 是深度机模（Fenix/PMDG）**唯一可写**的通道，标准 SimConnect
 * 事件对它们无效。这里验证不需要模拟器就能测的部分：
 *   1. 结构体布局（pack(1)、字段偏移）—— 布局错了读出来的名字全是乱的
 *   2. 分页枚举报文解析（含截断保护、去重）
 *   3. 按名字查找（大小写不敏感）
 *   4. 触发时发给 SimConnect_SetInputEvent 的 hash 与参数正确
 *   5. 截断报文不会越界读
 *
 * 真实"按钮是否被机模响应"需要实机验证（见 --dump-input）。
 */
#include "inputev.h"
#include "sc.h"
#include "mconnect.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

ScApi g_sc;
HANDLE g_sc_handle = NULL;
int g_verbose_flag = 0;

static int g_fail = 0;
static int g_pass = 0;

double xc_now_sec(void) {
    static LARGE_INTEGER freq;
    static int init = 0;
    LARGE_INTEGER now;
    if (!init) { QueryPerformanceFrequency(&freq); init = 1; }
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

void xc_logf(const char* fmt, ...) {
    static int verbose = -1;
    va_list ap;
    if (verbose < 0) verbose = getenv("TESTLOG") ? 1 : 0;
    if (!verbose) return;
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

static void check_u32(DWORD got, DWORD want, const char* what) {
    char d[128];
    snprintf(d, sizeof(d), "got=0x%08lx want=0x%08lx",
             (unsigned long)got, (unsigned long)want);
    check(got == want, what, d);
}

/* 捕获 SetInputEvent 调用 */
static int   g_set_calls = 0;
static DWORD g_last_hash = 0;
static DWORD g_last_size = 0;
static double g_last_value = 0.0;

static HRESULT WINAPI stub_set_input(HANDLE h, DWORD hash, DWORD cbUnitSize,
                                     PVOID value) {
    (void)h;
    g_set_calls++;
    g_last_hash = hash;
    g_last_size = cbUnitSize;
    if (value) {
        if (cbUnitSize == sizeof(float)) g_last_value = (double)*(float*)value;
        else if (cbUnitSize == sizeof(double)) g_last_value = *(double*)value;
    }
    return S_OK;
}

/* 构造一个分页枚举报文并喂给 inputev_on_enumerate */
typedef struct {
    DWORD dwSize, dwVersion, dwID, dwRequestID;
    DWORD dwArraySize, dwEntryNumber, dwOutOf;
    SC_INPUT_EVENT_DESCRIPTOR items[1];
} TestEnumPkt;

static void feed_page(int entry, int out_of, int count,
                      const char** names, const DWORD* hashes) {
    static BYTE buf[16384];
    TestEnumPkt* p = (TestEnumPkt*)buf;
    DWORD base = (DWORD)(sizeof(TestEnumPkt) - sizeof(SC_INPUT_EVENT_DESCRIPTOR));
    DWORD cb;
    int i;

    memset(buf, 0, sizeof(buf));
    p->dwSize = base + (DWORD)count * (DWORD)sizeof(SC_INPUT_EVENT_DESCRIPTOR);
    p->dwVersion = 5;
    p->dwID = SC_RECV_ID_ENUMERATE_INPUT_EVENTS;
    p->dwRequestID = 9001;
    p->dwArraySize = (DWORD)count;
    p->dwEntryNumber = (DWORD)entry;
    p->dwOutOf = (DWORD)out_of;

    for (i = 0; i < count; i++) {
        SC_INPUT_EVENT_DESCRIPTOR* d =
            (SC_INPUT_EVENT_DESCRIPTOR*)(buf + base +
                (size_t)i * sizeof(SC_INPUT_EVENT_DESCRIPTOR));
        strncpy(d->Name, names[i], sizeof(d->Name) - 1);
        d->Hash = hashes[i];
        d->Type = SC_DATATYPE_FLOAT32;
        strncpy(d->NodeNames, "NODE_A;NODE_B", sizeof(d->NodeNames) - 1);
    }
    cb = p->dwSize;
    inputev_on_enumerate(buf, cb);
}

int main(void) {
    printf("== MConnect Input Events 测试 ==\n\n");

    g_sc.SetInputEvent = stub_set_input;
    /* inputev_set_by_hash 会检查会话句柄非空（防止未连接时静默失败），
     * 测试里给一个假句柄即可 —— 真正的调用已被桩替换。 */
    g_sc_handle = (HANDLE)0x1;

    /* --- 1. 结构体布局 --- */
    printf("[1] 结构体布局（必须 pack(1)，否则字段全错位）\n");
    check_int((int)sizeof(SC_INPUT_EVENT_DESCRIPTOR), 1096,
              "DESCRIPTOR = 64 + 4 + 4 + 1024 = 1096 字节");
    check_int((int)offsetof(SC_INPUT_EVENT_DESCRIPTOR, Hash), 64, "Hash 偏移 64");
    check_int((int)offsetof(SC_INPUT_EVENT_DESCRIPTOR, NodeNames), 72,
              "NodeNames 偏移 72");
    check_int((int)sizeof(SC_RECV_ENUMERATE_INPUT_EVENTS),
              7 * 4 + 1096, "ENUMERATE 头 28 字节 + 1 项");

    /* --- 2. 分页枚举解析 --- */
    printf("\n[2] 分页枚举解析\n");
    {
        const char* names1[] = { "COM1_SWAP", "COM2_SWAP", "XPDR_IDENT_Push" };
        DWORD hashes1[] = { 0xAAAA0001, 0xAAAA0002, 0xAAAA0003 };
        const char* names2[] = { "COM1_TRANSFER", "XPDR_MODE_SET" };
        DWORD hashes2[] = { 0xAAAA0004, 0xAAAA0005 };

        feed_page(0, 2, 3, names1, hashes1);
        check_int(inputev_count(), 3, "第 1 页 3 条");
        feed_page(1, 2, 2, names2, hashes2);
        check_int(inputev_count(), 5, "第 2 页后共 5 条");

        /* 重复枚举（重连场景）不应产生重复项 */
        feed_page(0, 2, 3, names1, hashes1);
        check_int(inputev_count(), 5, "重复页不重复计数（按 hash 去重）");
    }

    /* --- 3. 按名字查找 --- */
    printf("\n[3] 按名字查找（大小写不敏感）\n");
    {
        DWORD h = 0, t = 0;
        check_int(inputev_lookup("COM1_SWAP", &h, &t), 1, "精确命中");
        check_u32(h, 0xAAAA0001, "hash 正确");
        check_u32(t, SC_DATATYPE_FLOAT32, "type 正确");

        check_int(inputev_lookup("com1_swap", &h, NULL), 1, "小写也命中");
        check_int(inputev_lookup("COM2_SWAP", &h, NULL), 1, "第二个");
        check_int(inputev_lookup("NOPE_NOT_EXIST", &h, NULL), 0, "不存在返回 0");
        check_int(inputev_lookup("", &h, NULL), 0, "空名返回 0");
        check_int(inputev_lookup(NULL, &h, NULL), 0, "NULL 返回 0");
    }

    /* --- 4. 触发：hash 与参数 --- */
    printf("\n[4] 按名字触发\n");
    g_set_calls = 0;
    check_int(inputev_set_by_name("COM1_SWAP", 1.0), 1, "触发成功");
    check_int(g_set_calls, 1, "调用了 1 次 SetInputEvent");
    check_u32(g_last_hash, 0xAAAA0001, "传的 hash 正确");
    check_int((int)g_last_size, (int)sizeof(float), "参数按 float 发（最常见）");
    check(g_last_value == 1.0, "参数值 1.0", NULL);

    printf("\n[4b] 未知名字不发送\n");
    g_set_calls = 0;
    check_int(inputev_set_by_name("DOES_NOT_EXIST", 1.0), 0, "返回失败");
    check_int(g_set_calls, 0, "未调用 SetInputEvent");

    /* --- 5. 截断报文不越界 --- */
    printf("\n[5] 截断报文安全（绝不越界读）\n");
    {
        BYTE small[64];
        int before = inputev_count();
        memset(small, 0, sizeof(small));
        /* 声称有 100 项但只给了 64 字节 */
        ((DWORD*)small)[0] = 64;
        ((DWORD*)small)[3] = 9001;    /* dwRequestID */
        ((DWORD*)small)[4] = 100;     /* dwArraySize */
        inputev_on_enumerate(small, 64);
        check_int(inputev_count(), before, "截断报文被忽略，不改变事件表");
    }

    printf("\n[5b] 认证 ID 不符的报文被忽略\n");
    {
        const char* nm[] = { "SHOULD_IGNORE" };
        DWORD hs[] = { 0xBBBB0001 };
        int before = inputev_count();
        static BYTE buf[8192];
        TestEnumPkt* p = (TestEnumPkt*)buf;
        DWORD base = (DWORD)(sizeof(TestEnumPkt) - sizeof(SC_INPUT_EVENT_DESCRIPTOR));

        memset(buf, 0, sizeof(buf));
        p->dwSize = base + (DWORD)sizeof(SC_INPUT_EVENT_DESCRIPTOR);
        p->dwID = SC_RECV_ID_ENUMERATE_INPUT_EVENTS;
        p->dwRequestID = 12345;       /* 不是我们的 9001 */
        p->dwArraySize = 1;
        strncpy(p->items[0].Name, nm[0], sizeof(p->items[0].Name) - 1);
        p->items[0].Hash = hs[0];
        inputev_on_enumerate(buf, p->dwSize);
        check_int(inputev_count(), before, "忽略非本客户端请求的报文");
    }

    printf("\n== 结果: %d 通过, %d 失败 ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}