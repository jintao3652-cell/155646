/* MConnect — WASM 桥：通过 ClientData 与模拟器内 WASM 模块通信，执行任意 RPN。
 * 见 wasmbridge.h 的协议说明与三条通道的分工。 */
#include "wasmbridge.h"
#include "sc.h"

#include <stdio.h>
#include <string.h>

/* ClientData ID 分配（本项目内唯一）
 * 用 1000 起，避开与 DEF_/REQ_/CEV_ 的冲突；独立客户端通道用 1100 段。 */
#define CD_ID_CMD      1000   /* MobiFlight.Command（默认通道，仅用于注册） */
#define CD_ID_RESP     1001   /* MobiFlight.Response */
#define CD_ID_LVARS    1002   /* MobiFlight.LVars */
#define CD_ID_MY_CMD   1010   /* mconnect.Command（注册后用自己的） */
#define CD_ID_MY_RESP  1011   /* mconnect.Response */
#define CD_ID_MY_LVARS 1012   /* mconnect.LVars */

/* 数据定义 ID */
#define CD_DEF_STR     1000   /* 命令/响应：一个 256 字节字符串 */
#define CD_DEF_FLOAT   1001   /* LVars：一个 float */

#define CD_REQ_RESP    1000   /* 订阅响应的 RequestID */
#define CD_REQ_LVARS   1001

#define WASM_STR_LEN   256
#define WASM_VAR_MAX   32     /* 本客户端最多同时监视 32 个变量 */

static WasmState g_state = WASM_STATE_OFF;
static char g_version[32];
static double g_register_at = 0.0;   /* 发出注册命令的时刻（超时判断） */

/* 命令队列 */
static char g_cmdq[WASM_CMD_MAX][160];
static int g_cmd_head = 0;
static int g_cmd_count = 0;

/* 监视的变量：calculator 表达式 + 最近值 */
static char g_watch_expr[WASM_VAR_MAX][128];
static double g_watch_val[WASM_VAR_MAX];
static unsigned char g_watch_set[WASM_VAR_MAX];
static int g_watch_n = 0;
/* 注册映射：WASM 的 LVars 数组按 MF.SimVars.Add 的先后顺序返回，
 * g_reg_slot[i] 表示"返回数组第 i 项对应的槽位"。没有这层映射时，
 * 使用显式槽位（如 30/31）会错误地读到数组第 0/1 项。 */
static int g_reg_n = 0;
static int g_reg_slot[WASM_VAR_MAX];
static int g_need_resync = 0;
static double g_last_req_lvars = 0.0;

static void reset_reg_map(void) {
    int i;
    g_reg_n = 0;
    for (i = 0; i < WASM_VAR_MAX; i++) g_reg_slot[i] = -1;
}

/* ---------------- 工具 ---------------- */

/* 把字符串写进定长缓冲（去掉末尾 NUL/空格） */
static void copy_str(char* dst, int cap, const char* src, int srclen) {
    int n = 0;
    if (cap < 1) return;
    while (n < srclen && n < cap - 1 && src[n]) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
    while (n > 0 && dst[n - 1] == ' ') dst[--n] = '\0';
}

const char* wasmbridge_state_name(WasmState s) {
    switch (s) {
    case WASM_STATE_OFF:         return "off";
    case WASM_STATE_REGISTERING: return "registering";
    case WASM_STATE_READY:       return "ready";
    case WASM_STATE_FAILED:      return "failed";
    default:                     return "?";
    }
}

WasmState wasmbridge_state(void) { return g_state; }
int wasmbridge_version(char* out, int cap) {
    if (!out || cap < 1) return 0;
    snprintf(out, (size_t)cap, "%s", g_version);
    return g_version[0] ? 1 : 0;
}

/* ---------------- 通道建立 ---------------- */

/* 映射并创建一条 ClientData 通道，然后订阅。返回 1 成功。 */
static int setup_channel(const char* name, DWORD id, DWORD def_id, DWORD req_id,
                         DWORD size, DWORD dtype, DWORD period) {
    HRESULT hr;

    if (!g_sc.MapClientDataNameToID || !g_sc.CreateClientData) return 0;

    hr = g_sc.MapClientDataNameToID(g_sc_handle, name, id);
    if (FAILED(hr)) {
        xc_logf("wasm: MapClientDataNameToID(\"%s\") hr=0x%08lx", name,
                (unsigned long)hr);
        return 0;
    }
    /* CREATE_FLAG_ALLOW_SHARED：区域内存在多客户端（MobiFlight Connector
     * 也在用默认通道）时必须带这个标志，否则 Create 会失败。 */
    hr = g_sc.CreateClientData(g_sc_handle, id, size, 0x00000001 /*ALLOW_SHARED*/);
    if (FAILED(hr)) {
        xc_logf("wasm: CreateClientData(\"%s\", %lu) hr=0x%08lx", name,
                (unsigned long)size, (unsigned long)hr);
        return 0;
    }
    if (g_sc.AddToClientDataDefinition) {
        hr = g_sc.AddToClientDataDefinition(g_sc_handle, def_id, 0, dtype, 0.0f,
                                            SC_UNUSED);
        if (FAILED(hr)) {
            xc_logf("wasm: AddToClientDataDefinition(%lu) hr=0x%08lx",
                    (unsigned long)def_id, (unsigned long)hr);
            return 0;
        }
    }
    if (g_sc.RequestClientData) {
        /* period: ON_SET(2) 表示"值变化时推" */
        hr = g_sc.RequestClientData(g_sc_handle, id, req_id, def_id, period,
                                    0, SC_UNUSED, size);
        if (FAILED(hr)) {
            xc_logf("wasm: RequestClientData(%lu) hr=0x%08lx",
                    (unsigned long)id, (unsigned long)hr);
            return 0;
        }
    }
    xc_logf("wasm: channel \"%s\" ready (id=%lu size=%lu)", name,
            (unsigned long)id, (unsigned long)size);
    return 1;
}

/* 注册自己的独立通道（避免与 MobiFlight Connector 抢默认通道）。 */
static int register_own_channels(void) {
    int ok = 1;
    ok &= setup_channel(WASM_CLIENT_NAME ".Command", CD_ID_MY_CMD,
                        CD_DEF_STR, 0, WASM_STR_LEN,
                        SC_DATATYPE_STRING256, 0 /* NEVER，只写不订阅 */);
    ok &= setup_channel(WASM_CLIENT_NAME ".Response", CD_ID_MY_RESP,
                        CD_DEF_STR, CD_REQ_RESP, WASM_STR_LEN,
                        SC_DATATYPE_STRING256, 2 /* ON_SET */);
    ok &= setup_channel(WASM_CLIENT_NAME ".LVars", CD_ID_MY_LVARS,
                        CD_DEF_FLOAT, CD_REQ_LVARS, WASM_VAR_MAX * 4,
                        SC_DATATYPE_FLOAT32, 2 /* ON_SET */);
    return ok;
}

/* 往命令通道写一条字符串命令。 */
static int send_cmd(const char* cmd) {
    char buf[WASM_STR_LEN];
    DWORD id;

    if (!g_sc.SetClientData || !g_sc_handle) return 0;
    memset(buf, 0, sizeof(buf));
    snprintf(buf, sizeof(buf) - 1, "%s", cmd);

    /* 注册阶段走默认通道（自己的还没建好） */
    id = (g_state == WASM_STATE_READY) ? CD_ID_MY_CMD : CD_ID_CMD;
    if (FAILED(g_sc.SetClientData(g_sc_handle, id, CD_DEF_STR, 0, 0,
                                  (DWORD)sizeof(buf), buf))) {
        xc_logf("wasm: send \"%s\" failed", cmd);
        return 0;
    }
    return 1;
}

/* ---------------- 生命周期 ---------------- */

int wasmbridge_init(void) {
    reset_reg_map();
    g_need_resync = 0;
    if (!g_sc_handle) return -1;
    if (!g_sc.SetClientData || !g_sc.CreateClientData) {
        xc_logf("wasm: SimConnect 不支持 ClientData，桥接不可用");
        g_state = WASM_STATE_OFF;
        return -1;
    }

    /* 先建默认通道（用于发注册命令），再请求独立通道 */
    if (!setup_channel("MobiFlight.Command", CD_ID_CMD, CD_DEF_STR, 0,
                       WASM_STR_LEN, SC_DATATYPE_STRING256, 0)) {
        xc_logf("wasm: 默认 Command 通道建立失败（WASM 模块未安装？）");
        g_state = WASM_STATE_FAILED;
        return -1;
    }
    if (!setup_channel("MobiFlight.Response", CD_ID_RESP, CD_DEF_STR, CD_REQ_RESP,
                       WASM_STR_LEN, SC_DATATYPE_STRING256, 2)) {
        xc_logf("wasm: 默认 Response 通道建立失败");
        g_state = WASM_STATE_FAILED;
        return -1;
    }

    if (!register_own_channels()) {
        xc_logf("wasm: 独立通道建立失败，将复用过默认通道");
        /* 降级：继续用默认通道（与 MobiFlight Connector 共存时可能冲突，
         * 但比完全不可用好） */
    }

    /* MobiFlight 文档提到：新客户端启动后第一条命令有时被忽略，
     * 因此先发一条 Ping 探活。 */
    g_state = WASM_STATE_REGISTERING;
    g_register_at = xc_now_sec();
    send_cmd("MF.Ping");
    send_cmd("MF.Version.Get");
    send_cmd("MF.Clients.Add." WASM_CLIENT_NAME);

    xc_logf("wasm: 已请求注册为 \"%s\"", WASM_CLIENT_NAME);
    return 0;
}

void wasmbridge_shutdown(void) {
    if (g_state == WASM_STATE_READY) {
        /* 尽力通知 WASM 释放（失败也无所谓，模拟器退出会清） */
        send_cmd("MF.Config.MAX_VARS_PER_FRAME.Set.0");
    }
    g_state = WASM_STATE_OFF;
    g_cmd_count = 0;
    g_need_resync = 0;
    reset_reg_map();
}

/* ---------------- RPN 执行 ---------------- */

int wasmbridge_exec_rpn(const char* rpn) {
    int dropped = 0;
    if (!rpn || !rpn[0]) return -1;
    if (g_state != WASM_STATE_READY) return -1;   /* 未就绪，调用方应回退 */

    /* MobiFlight 的命令格式是 `MF.SimVars.Set.<RPN>`，RPN 直接跟在点后
     * （注意：不是括号；不同版本文档写法略有差异，实测点号后接原文有效）。 */
    if (g_cmd_count >= WASM_CMD_MAX) {
        g_cmd_head = (g_cmd_head + 1) % WASM_CMD_MAX;   /* 丢最旧 */
        g_cmd_count--;
        dropped = 1;
    }
    {
        int slot = g_cmd_head;
        snprintf(g_cmdq[slot], sizeof(g_cmdq[slot]), "MF.SimVars.Set.%s", rpn);
        g_cmd_head = (g_cmd_head + 1) % WASM_CMD_MAX;
        g_cmd_count++;
    }
    if (dropped) xc_logf("wasm: 命令队列满，丢弃最旧");
    return 0;
}

int wasmbridge_watch(const char* calculator, int slot) {
    if (!calculator || !calculator[0]) return -1;
    if (slot < 0) slot = g_watch_n;
    if (slot >= WASM_VAR_MAX) return -1;

    snprintf(g_watch_expr[slot], sizeof(g_watch_expr[slot]), "%s", calculator);
    g_watch_set[slot] = 0;
    if (g_watch_n <= slot) g_watch_n = slot + 1;

    if (g_state != WASM_STATE_READY) return -1;
    /* WASM 的 SimVars 列表只能追加、不能原地替换表达式。表达式变化后旧注册项
     * 仍会占住返回数组前面的位置，因此统一标记 resync：由 poll 先 Clear，
     * 再按槽位顺序重新 Add，并建立 返回下标→槽位 的映射。 */
    g_need_resync = 1;
    return 0;
}

int wasmbridge_get_value(int slot, double* out) {
    if (slot < 0 || slot >= WASM_VAR_MAX || !g_watch_set[slot]) return 0;
    if (out) *out = g_watch_val[slot];
    return 1;
}

/* ---------------- 主循环 ---------------- */

void wasmbridge_poll(double now) {
    if (g_state == WASM_STATE_OFF) return;

    /* 注册超时：5 秒没等到 Finished 就判失败（WASM 模块没装/版本太老） */
    if (g_state == WASM_STATE_REGISTERING && now - g_register_at > 5.0) {
        xc_logf("wasm: 注册超时（%.1fs）→ 判定 WASM 模块不可用", 
                now - g_register_at);
        g_state = WASM_STATE_FAILED;
        return;
    }

    /* 表达式变化 / 注册完成：Clear 后按槽位顺序重新注册所有 watch。
     * WASM 只按 Add 顺序返回数组，用 g_reg_slot 建立 返回下标→槽位 映射，
     * 修复显式槽位（如 30/31）与数组第 0/1 项错位的问题。 */
    if (g_state == WASM_STATE_READY && g_need_resync) {
        int i, need = 1;   /* Clear 本身占一条 */
        for (i = 0; i < g_watch_n; i++)
            if (g_watch_expr[i][0]) need++;
        /* 队列放不下"Clear + 全部 Add"就本帧跳过，等发送循环排空后重试：
         * 否则 Clear 成功但部分 Add 被丢，返回数组与槽位映射会错乱。 */
        if (g_cmd_count + need <= WASM_CMD_MAX) {
            g_need_resync = 0;
            reset_reg_map();
            snprintf(g_cmdq[g_cmd_head], sizeof(g_cmdq[g_cmd_head]),
                     "%s", "MF.SimVars.Clear");
            g_cmd_head = (g_cmd_head + 1) % WASM_CMD_MAX;
            g_cmd_count++;
            for (i = 0; i < g_watch_n; i++) {
                if (!g_watch_expr[i][0]) continue;
                snprintf(g_cmdq[g_cmd_head], sizeof(g_cmdq[g_cmd_head]),
                         "MF.SimVars.Add.%s", g_watch_expr[i]);
                g_cmd_head = (g_cmd_head + 1) % WASM_CMD_MAX;
                g_cmd_count++;
                if (g_reg_n < WASM_VAR_MAX) g_reg_slot[g_reg_n++] = i;
            }
            xc_logf("wasm: resync %d watch var(s)", g_reg_n);
        }
    }

    /* 把排队的命令发出去（每帧最多 8 条，避免刷爆共享内存） */
    {
        int sent = 0;
        while (g_cmd_count > 0 && sent < 8) {
            int tail = (g_cmd_head - g_cmd_count + WASM_CMD_MAX) % WASM_CMD_MAX;
            if (!send_cmd(g_cmdq[tail])) break;
            g_cmd_count--;
            sent++;
        }
    }
}

/* ---------------- 接收解析 ---------------- */

/* SIMCONNECT_RECV_CLIENT_DATA 与 SIMOBJECT_DATA 结构相同（dwData 起是载荷） */
typedef struct {
    DWORD dwSize;
    DWORD dwVersion;
    DWORD dwID;
    DWORD dwRequestID;
    DWORD dwDefineID;
    DWORD dwFlags;
    DWORD dwEntryNumber;
    DWORD dwOutOf;
    DWORD dwDefineCount;
    DWORD dwData;
} TestClientDataHdr;

/* 处理响应字符串（来自 XXX.Response 通道） */
static void handle_response(const char* s) {
    if (!s || !s[0]) return;

    if (strncmp(s, "MF.Version.", 11) == 0) {
        copy_str(g_version, sizeof(g_version), s + 11, (int)strlen(s) - 11);
        xc_logf("wasm: WASM 模块版本 %s", g_version);
        return;
    }
    if (strstr(s, ".Finished") != NULL) {
        /* MF.Clients.Add.mconnect.Finished → 独立通道可用 */
        if (g_state != WASM_STATE_READY) {
            g_state = WASM_STATE_READY;
            xc_logf("wasm: 注册完成，独立通道就绪（state=ready）");
            /* 之前因未就绪而未注册的 watch 由 poll 统一 Clear + 重新 Add */
            g_need_resync = 1;
        }
        return;
    }
    if (strncmp(s, "MF.Pong", 7) == 0) return;
    if (strncmp(s, "MF.LVars.List.Start", 19) == 0) return;
    if (strncmp(s, "MF.LVars.List.End", 17) == 0) return;
    if (strstr(s, "ERROR") || strstr(s, "Error")) {
        xc_logf("wasm: 响应含错误：%s", s);
    }
}

void wasmbridge_on_client_data(const void* pkt, DWORD cbData) {
    const TestClientDataHdr* d = (const TestClientDataHdr*)pkt;
    DWORD hdr = (DWORD)(sizeof(TestClientDataHdr) - sizeof(DWORD));
    if (!d || cbData < hdr) return;

    /* 响应通道：字符串 */
    if (d->dwDefineID == CD_DEF_STR &&
        (d->dwRequestID == CD_REQ_RESP)) {
        handle_response((const char*)&d->dwData);
        return;
    }

    /* LVars 通道：float 数组，按 watch 顺序排列 */
    if (d->dwDefineID == CD_DEF_FLOAT && d->dwRequestID == CD_REQ_LVARS) {
        DWORD n = (cbData - hdr) / sizeof(float);
        const float* v = (const float*)&d->dwData;
        DWORD i;
        if (n > (DWORD)g_reg_n) n = (DWORD)g_reg_n;
        for (i = 0; i < n; i++) {
            int slot = g_reg_slot[i];
            if (slot < 0 || slot >= WASM_VAR_MAX) continue;
            g_watch_val[slot] = (double)v[i];
            g_watch_set[slot] = 1;
        }
        return;
    }
}

void wasmbridge_dump(void) {
    xc_logf("wasm: state=%s version=\"%s\" watching=%d",
            wasmbridge_state_name(g_state), g_version, g_watch_n);
    {
        int i;
        for (i = 0; i < g_watch_n; i++) {
            if (!g_watch_expr[i][0]) continue;
            xc_logf("  watch[%d] %-40s = %.4f%s", i, g_watch_expr[i],
                    g_watch_val[i], g_watch_set[i] ? "" : " (no value)");
        }
    }
}