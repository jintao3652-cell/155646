/* MConnect 反向控制单元测试（不需要模拟器、不需要 SimConnect.dll）。
 *
 * 覆盖客户端 → 模拟器这条链路上所有**纯逻辑**部分：
 *   1. 频率换算 MHz*1000 → Hz，含波段校验
 *   2. 应答机码解析（八进制逐位校验）
 *   3. BCD 编码（XPNDR_SET 的参数格式）
 *   4. Send:RADIO 报文解析（设主频/设备频/交换/非法值拒收）
 *   5. Send:XPDR 报文解析（字符串码/数字码/模式/IDENT）
 *   6. 命令队列（入队、排空顺序、队列满时丢最旧）
 *
 * 事件是否真的触发需要模拟器，这里只验证"我们发出的参数是对的"。
 */
#include "control.h"
#include "datadefs.h"
#include "inputev.h"
#include "json.h"
#include "sc.h"
#include "mconnect.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

/* control.c / datadefs.c 引用的外部符号 */
ScApi g_sc;
HANDLE g_sc_handle = NULL;
int g_verbose_flag = 0;
void* g_players_lock = NULL;
void* g_sock_lock = NULL;

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
    /* 测试时静默（下面用 TESTLOG 开关控制） */
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

/* datadefs.c 依赖：control 在换机型时会通知读取侧重置 LVar 表达式。
 * 本测试只关心写入路径，重置逻辑给个空实现即可。 */
void datadefs_reset_profile_reads(void) { }

/* control.c 读当前应答机状态做参照（FSL 相对转动用），测试里给个实例 */
FlightRaw g_flight;

/* ---------------- 断言 ---------------- */

static void check(int cond, const char* what, const char* detail) {
    if (cond) { g_pass++; printf("  PASS  %s\n", what); }
    else { g_fail++; printf("  FAIL  %s   [%s]\n", what, detail ? detail : ""); }
}

static void check_long(long got, long want, const char* what) {
    char d[128];
    snprintf(d, sizeof(d), "got=%ld want=%ld", got, want);
    check(got == want, what, d);
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

/* 抓取 control.c 内部发出的命令：用桩替换 TransmitClientEvent 与
 * SetDataOnSimObject，把参数记下来供断言。 */
typedef struct {
    int   n;                /* 本批已捕获条数 */
    DWORD last_evid;
    DWORD last_data;
    int   n_transmit;
    int   n_setdata;
    double setdata_value;
    DWORD  setdata_defid;
    /* InputEvent（Send:INPUTEV 走这条） */
    int   n_input_ev;
    DWORD last_input_hash;
    double last_input_value;
} Capture;

static Capture g_cap;

/* InputEvent 触发桩 */
static HRESULT WINAPI stub_set_input_ev(HANDLE h, DWORD hash, DWORD cb,
                                        PVOID val) {
    (void)h; (void)cb;
    g_cap.n_input_ev++;
    g_cap.last_input_hash = hash;
    if (val) {
        if (cb == sizeof(float)) g_cap.last_input_value = (double)*(float*)val;
        else if (cb == sizeof(double)) g_cap.last_input_value = *(double*)val;
    }
    return S_OK;
}

static HRESULT WINAPI stub_transmit(HANDLE h, SIMCONNECT_OBJECT_ID oid, DWORD evid,
                                    DWORD data, DWORD group, DWORD flags) {
    (void)h; (void)oid; (void)group; (void)flags;
    g_cap.last_evid = evid;
    g_cap.last_data = data;
    g_cap.n_transmit++;
    return S_OK;
}

static HRESULT WINAPI stub_setdata(HANDLE h, DWORD flags, SIMCONNECT_OBJECT_ID oid,
                                   SIMCONNECT_DATA_DEFINITION_ID def, DWORD count,
                                   DWORD unit, void* data) {
    (void)h; (void)flags; (void)oid; (void)count; (void)unit;
    g_cap.setdata_defid = def;
    g_cap.setdata_value = data ? *(double*)data : -1.0;
    g_cap.n_setdata++;
    return S_OK;
}

/* 送一条 JSON 报文进 control_handle_message */
static int feed(const char* json_line) {
    Json* root = json_parse(json_line);
    int handled;
    if (!root) {
        printf("  !! JSON 解析失败: %s\n", json_line);
        return 0;
    }
    handled = control_handle_message(root, NULL);
    json_free(root);
    return handled;
}

/* 重置捕获并排空队列（排空会执行命令） */
static void flush(void) {
    memset(&g_cap, 0, sizeof(g_cap));
    control_drain();
}

int main(void) {
    printf("== MConnect 反向控制测试 ==\n\n");

    /* 用桩替换真实 SimConnect 调用（本测试不连模拟器） */
    g_sc.TransmitClientEvent = stub_transmit;
    g_sc.SetDataOnSimObject = stub_setdata;
    g_sc.SetInputEvent = stub_set_input_ev;

    /* control_init 需要 g_sc_handle；给个假句柄让 LOCK 初始化跑起来。
     * MapClientEventToSimEvent 为 NULL 时 control_init 返回 -1，
     * 但队列锁仍会初始化 —— 这正是本测试需要的。 */
    g_sc_handle = (HANDLE)0x1;
    control_init();
    g_sc_handle = (HANDLE)0x1;

    /* --- 1. 频率换算 --- */
    printf("[1] 频率换算 MHz*1000 → Hz\n");
    check_long(control_mhz1000_to_hz(122800), 122800000L, "122.800 MHz");
    check_long(control_mhz1000_to_hz(118000), 118000000L, "118.000 MHz 下边界");
    check_long(control_mhz1000_to_hz(136975), 136975000L, "136.975 MHz 上边界");
    check_long(control_mhz1000_to_hz(117999), 0, "越下限拒收");
    check_long(control_mhz1000_to_hz(137001), 0, "越上限拒收");
    check_long(control_mhz1000_to_hz(0), 0, "0 拒收");
    check_long(control_mhz1000_to_hz(-100), 0, "负数拒收");

    /* --- 2. 应答机码解析 --- */
    printf("\n[2] 应答机码解析\n");
    check_int(control_parse_squawk("1234"), 1234, "\"1234\"");
    check_int(control_parse_squawk("0000"), 0, "\"0000\"");
    check_int(control_parse_squawk("7777"), 7777, "\"7777\" 全 7");
    check_int(control_parse_squawk("7700"), 7700, "\"7700\" 紧急码");
    check_int(control_parse_squawk("1238"), -1, "含 8 非法（非八进制）");
    check_int(control_parse_squawk("123"), -1, "长度不足");
    check_int(control_parse_squawk("12345"), -1, "长度超出");
    check_int(control_parse_squawk("ABCD"), -1, "非数字");
    check_int(control_parse_squawk(NULL), -1, "NULL");

    /* --- 3. Send:RADIO 设置主频 --- */
    printf("\n[3] Send:RADIO 设置 COM1 主频 122.800\n");
    flush();
    check_int(feed("{\"type\":\"Send:RADIO\",\"com\":1,\"active\":true,\"freq\":122800}"),
              1, "报文被识别为控制报文");
    control_drain();
    check_int(g_cap.n_transmit, 1, "发出 1 个事件");
    check_int((int)g_cap.last_evid, CEV_COM1_SET, "事件 = COM_RADIO_SET_HZ");
    check_long((long)g_cap.last_data, 122800000L, "参数 = 122800000 Hz");

    /* --- 4. 设置备频 --- */
    printf("\n[4] Send:RADIO 设置 COM2 备频 121.500\n");
    flush();
    feed("{\"type\":\"Send:RADIO\",\"com\":2,\"active\":false,\"freq\":121500}");
    control_drain();
    check_int((int)g_cap.last_evid, CEV_COM2_STBY_SET, "事件 = COM2_STBY_RADIO_SET_HZ");
    check_long((long)g_cap.last_data, 121500000L, "参数 = 121500000 Hz");

    /* --- 5. COM3 --- */
    printf("\n[5] Send:RADIO 设置 COM3 主频\n");
    flush();
    feed("{\"type\":\"Send:RADIO\",\"com\":3,\"freq\":133000}");
    control_drain();
    check_int((int)g_cap.last_evid, CEV_COM3_SET, "事件 = COM3_RADIO_SET_HZ");
    check_long((long)g_cap.last_data, 133000000L, "参数 = 133000000 Hz");

    /* --- 6. 主备互换 --- */
    printf("\n[6] Send:RADIO 主备互换\n");
    flush();
    feed("{\"type\":\"Send:RADIO\",\"com\":1,\"swap\":true}");
    control_drain();
    check_int((int)g_cap.last_evid, CEV_COM1_SWAP, "事件 = COM1_RADIO_SWAP");

    /* --- 6b. 选定发射台（XVoice 的 tx_com 对应写侧） --- */
    printf("\n[6b] Send:RADIO 选定发射台\n");
    flush();
    feed("{\"type\":\"Send:RADIO\",\"com\":2,\"xmit\":true}");
    control_drain();
    check_int((int)g_cap.last_evid, CEV_COM2_XMIT, "事件 = COM2_TRANSMIT_SELECT");
    check_long((long)g_cap.last_data, 1L, "参数 = 1（选中）");

    flush();
    feed("{\"type\":\"Send:RADIO\",\"com\":3,\"xmit\":true}");
    control_drain();
    check_int((int)g_cap.last_evid, CEV_COM3_XMIT, "COM3 发射台");

    printf("\n[6c] xmit:false 被忽略（必须有台在发）\n");
    flush();
    feed("{\"type\":\"Send:RADIO\",\"com\":1,\"xmit\":false}");
    control_drain();
    check_int(g_cap.n_transmit, 0, "不产生事件");

    /* --- 6d. 全收开关 --- */
    printf("\n[6d] Send:RADIO 全收\n");
    flush();
    feed("{\"type\":\"Send:RADIO\",\"recv_all\":true}");
    control_drain();
    check_int((int)g_cap.last_evid, CEV_COM_RECV_ALL, "事件 = COM_RECEIVE_ALL_SET");
    check_long((long)g_cap.last_data, 1L, "参数 = 1（开）");

    flush();
    feed("{\"type\":\"Send:RADIO\",\"recv_all\":false}");
    control_drain();
    check_long((long)g_cap.last_data, 0L, "参数 = 0（关）");

    /* --- 7. 非法频率被拒（不产生事件） --- */
    printf("\n[7] 越界频率拒收\n");
    flush();
    feed("{\"type\":\"Send:RADIO\",\"com\":1,\"freq\":100000}");
    control_drain();
    check_int(g_cap.n_transmit, 0, "100.000 MHz（超出 COM 波段）不产生事件");

    printf("\n[8] 非法 COM 通道被拒\n");
    flush();
    feed("{\"type\":\"Send:RADIO\",\"com\":9,\"freq\":122800}");
    control_drain();
    check_int(g_cap.n_transmit, 0, "com=9 不产生事件");

    /* --- 9. 应答机码（字符串） --- */
    printf("\n[9] Send:XPDR 设置码 \"7000\"\n");
    flush();
    feed("{\"type\":\"Send:XPDR\",\"code\":\"7000\"}");
    control_drain();
    check_int((int)g_cap.last_evid, CEV_XPNDR_SET, "事件 = XPNDR_SET");
    check_long((long)g_cap.last_data, 0x7000L, "参数 = BCD 0x7000");

    /* --- 10. 应答机码（数字形式） --- */
    printf("\n[10] Send:XPDR 数字形式 1234 → BCD 0x1234\n");
    flush();
    feed("{\"type\":\"Send:XPDR\",\"code\":1234}");
    control_drain();
    check_long((long)g_cap.last_data, 0x1234L, "参数 = BCD 0x1234");

    /* --- 11. 非法应答机码（含 8/9） --- */
    printf("\n[11] 非法应答机码拒收\n");
    flush();
    feed("{\"type\":\"Send:XPDR\",\"code\":\"8899\"}");
    control_drain();
    check_int(g_cap.n_transmit, 0, "\"8899\" 含 8/9，不产生事件");

    /* --- 12. 应答机模式（走 SetDataOnSimObject 而非事件） --- */
    printf("\n[12] Send:XPDR 设置模式 ALT(3，协议规范)\n");
    flush();
    feed("{\"type\":\"Send:XPDR\",\"state\":3}");
    control_drain();
    check_int(g_cap.n_setdata, 1, "走 SetDataOnSimObject");
    check_int(g_cap.n_transmit, 0, "不产生事件");
    check_int((int)g_cap.setdata_defid, DEF_XPNDR_STATE, "定义 = DEF_XPNDR_STATE");
    check_int((int)g_cap.setdata_value, 4, "规范 ALT(3) → MSFS Alt(4)");

    /* --- 12b. 模式枚举重映射（两平台顺序不同） --- */
    printf("\n[12b] 模式枚举重映射（规范 → MSFS）\n");
    check_int(control_canonical_to_msfs_state(0), 0, "Off 一致");
    check_int(control_canonical_to_msfs_state(1), 1, "Stby 一致");
    check_int(control_canonical_to_msfs_state(2), 3, "规范 On(2) → MSFS On(3)");
    check_int(control_canonical_to_msfs_state(3), 4, "规范 Alt(3) → MSFS Alt(4)");
    check_int(control_canonical_to_msfs_state(4), 2, "规范 Test(4) → MSFS Test(2)");
    check_int(control_canonical_to_msfs_state(5), 5, "Ground 一致");

    printf("\n[12c] 设 ON 不能落到 Test\n");
    flush();
    feed("{\"type\":\"Send:XPDR\",\"state\":2}");
    control_drain();
    check_int((int)g_cap.setdata_value, 3,
              "规范 On(2) 写成 MSFS 3（3 才是 ON；2 是 Test）");

    /* --- 13. 模式越界被夹到 0..5 --- */
    printf("\n[13] 模式越界夹紧\n");
    flush();
    feed("{\"type\":\"Send:XPDR\",\"state\":99}");
    control_drain();
    check_int((int)g_cap.setdata_value, 5, "99 夹到 5（Ground）");

    /* --- 14. IDENT --- */
    printf("\n[14] Send:XPDR IDENT\n");
    flush();
    feed("{\"type\":\"Send:XPDR\",\"ident\":true}");
    control_drain();
    check_int((int)g_cap.last_evid, CEV_XPNDR_IDENT, "事件 = XPNDR_IDENT_SET");
    check_long((long)g_cap.last_data, 1L, "参数 = 1");

    /* --- 15. 非控制报文不被误认 --- */
    printf("\n[15] 非控制报文不被拦截\n");
    check_int(feed("{\"type\":\"Send:OPD:FSD9\",\"data\":[]}"), 0,
              "Send:OPD:FSD9 不归 control 处理");
    check_int(feed("{\"type\":\"Send:ATC\",\"data\":[]}"), 0,
              "Send:ATC 不归 control 处理");

    /* --- 15b. Send:INPUTEV：按机型给的选项触发 --- */
    printf("\n[15b] Send:INPUTEV 按名字触发\n");
    {
        /* 先灌入两个事件（模拟当前机型枚举结果） */
        static BYTE buf[8192];
        struct { DWORD sz, ver, id, req, n, entry, out;
                 SC_INPUT_EVENT_DESCRIPTOR it[1]; }* p = (void*)buf;
        DWORD base = (DWORD)(sizeof(*p) - sizeof(SC_INPUT_EVENT_DESCRIPTOR));

        memset(buf, 0, sizeof(buf));
        p->sz = base + 2 * sizeof(SC_INPUT_EVENT_DESCRIPTOR);
        p->id = SC_RECV_ID_ENUMERATE_INPUT_EVENTS;
        p->req = 9001;
        p->n = 2;
        p->entry = 0;
        p->out = 1;
        strncpy(((SC_INPUT_EVENT_DESCRIPTOR*)(buf + base))[0].Name,
                "XPDR_CODE_SET", 63);
        ((SC_INPUT_EVENT_DESCRIPTOR*)(buf + base))[0].Hash = 0xDEADBEEF;
        ((SC_INPUT_EVENT_DESCRIPTOR*)(buf + base))[0].Type = SC_DATATYPE_FLOAT32;
        strncpy(((SC_INPUT_EVENT_DESCRIPTOR*)(buf + base))[1].Name,
                "COM1_SWAP_Push", 63);
        ((SC_INPUT_EVENT_DESCRIPTOR*)(buf + base))[1].Hash = 0xCAFEBABE;
        ((SC_INPUT_EVENT_DESCRIPTOR*)(buf + base))[1].Type = SC_DATATYPE_FLOAT32;
        inputev_on_enumerate(buf, p->sz);
    }

    flush();
    feed("{\"type\":\"Send:INPUTEV\",\"name\":\"XPDR_CODE_SET\",\"value\":7000}");
    control_drain();
    check_int(g_cap.n_input_ev, 1, "调用了 SetInputEvent");
    check_u32(g_cap.last_input_hash, 0xDEADBEEF, "hash 与枚举值一致");
    check_int((int)g_cap.last_input_value, 7000, "值正确传递");

    printf("\n[15c] 未知名字不触发\n");
    flush();
    feed("{\"type\":\"Send:INPUTEV\",\"name\":\"NO_SUCH_EVENT\",\"value\":1}");
    control_drain();
    check_int(g_cap.n_input_ev, 0, "不在事件表里 → 不调用");

    printf("\n[15d] 缺 name 不崩\n");
    flush();
    feed("{\"type\":\"Send:INPUTEV\",\"value\":1}");
    control_drain();
    check_int(g_cap.n_input_ev, 0, "缺 name 被忽略");

    /* --- 15e. Send:RPN --- */
    printf("\n[15e] Send:RPN（WASM 未就绪时安全拒绝）\n");
    check_int(feed("{\"type\":\"Send:RPN\",\"rpn\":\"5 (>L:X)\"}"), 1,
              "被识别为控制报文");
    check_int(feed("{\"type\":\"Send:RPN\"}"), 1, "缺 rpn 也被识别（内部忽略）");

    /* --- 16. 队列：多条命令按顺序执行 --- */
    printf("\n[16] 队列顺序\n");
    flush();
    feed("{\"type\":\"Send:RADIO\",\"com\":1,\"freq\":118000}");
    feed("{\"type\":\"Send:RADIO\",\"com\":1,\"freq\":119000}");
    feed("{\"type\":\"Send:XPDR\",\"code\":\"4321\"}");
    control_drain();
    check_int(g_cap.n_transmit, 3, "3 条命令全部执行");
    check_long((long)g_cap.last_data, 0x4321L, "最后执行的是应答机码（顺序正确）");

    /* --- 17. 队列满：丢最旧 --- */
    printf("\n[17] 队列满丢最旧\n");
    flush();
    {
        int i;
        char buf[96];
        for (i = 0; i < 80; i++) {          /* 超过 CMD_QUEUE_MAX(64) */
            snprintf(buf, sizeof(buf),
                     "{\"type\":\"Send:RADIO\",\"com\":1,\"freq\":%d}", 118000 + i);
            feed(buf);
        }
        control_drain();
        /* 队列保留最后 64 条：118000+16 .. 118000+79 */
        check_int(g_cap.n_transmit, 64, "只执行 64 条（队列上限）");
        check_long((long)g_cap.last_data, (118000L + 79) * 1000L,
                   "最后一条是最新命令");
    }

    printf("\n== 结果: %d 通过, %d 失败 ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}