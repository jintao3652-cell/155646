/* MConnect — PMDG SDK 桥。协议与偏移来源见 pmdgbridge.h。
 *
 * 实现要点：把两个机型族的差异收在一张协议表里，读写逻辑只写一遍。
 * 这样加第三个机型族时只加一行表项，不会在多处 switch 里漏改。 */
#include "pmdgbridge.h"
#include "sc.h"

#include <stdio.h>
#include <string.h>

/* ---------------- 协议描述表 ---------------- */

typedef struct {
    PmdgType    type;
    const char* name;
    /* 数据通道 */
    const char* data_name;
    unsigned    data_id;
    unsigned    data_def;
    unsigned    data_size;
    int         off_mode;      /* XPDR_ModeSel 字节偏移 */
    int         off_sel;       /* 应答机 1/2 或 L/R 选择；-1 = 无 */
    int         off_fail;      /* FAIL 灯；-1 = 该机型无此字段 */
    int         off_ident;     /* IDENT 按钮；-1 = 无此字段 */
    /* 控制通道 */
    const char* ctrl_name;
    unsigned    ctrl_id;
    unsigned    ctrl_def;
    /* 事件 */
    unsigned    evt_tcas_mode;
    unsigned    evt_tcas_ident;
    /* 超时时提示用户改哪个 ini */
    const char* ini_hint;
} PmdgProto;

static const PmdgProto kProto[] = {
    {
        PMDG_TYPE_737, "737NG3",
        "PMDG_NG3_Data",  0x4E473331u, 0x4E473332u,
        PMDG737_DATA_SIZE,
        PMDG737_OFF_XPDR_MODE, PMDG737_OFF_XPDR_SEL, PMDG737_OFF_XPDR_FAIL,
        -1,                                   /* 737 数据里无 IDENT 字段 */
        "PMDG_NG3_Control", 0x4E473333u, 0x4E473334u,
        PMDG737_EVT_TCAS_MODE, PMDG737_EVT_TCAS_IDENT,
        "737NG3_Options.ini"
    },
    {
        PMDG_TYPE_777, "777X",
        "PMDG_777X_Data", 0x504D4447u, 0x504D4448u,
        PMDG777_DATA_SIZE,
        PMDG777_OFF_XPDR_MODE, PMDG777_OFF_XPDR_SEL, -1,
        PMDG777_OFF_XPDR_IDENT,
        "PMDG_777X_Control", 0x504D4449u, 0x504D444Au,
        PMDG777_EVT_TCAS_MODE, PMDG777_EVT_TCAS_IDENT,
        "777_Options.ini"
    }
};

static const int kProtoCount = (int)(sizeof(kProto) / sizeof(kProto[0]));

static const PmdgProto* proto_for(PmdgType t) {
    int i;
    for (i = 0; i < kProtoCount; i++)
        if (kProto[i].type == t) return &kProto[i];
    return NULL;
}

/* ---------------- 状态 ---------------- */

#define PMDG_DATA_REQ   7001

static PmdgState g_state = PMDG_STATE_OFF;
static PmdgType  g_type = PMDG_TYPE_NONE;
static const PmdgProto* g_proto = NULL;
static double g_init_at = 0.0;
static PmdgXpdr g_xpdr;
static unsigned g_last_mode_raw = 0xFFFFFFFFu;   /* 日志去重 */
static int g_data_size_seen = 0;

/* ---------------- 生命周期 ---------------- */

int pmdgbridge_init(PmdgType type) {
    const PmdgProto* p = proto_for(type);
    HRESULT hr;

    g_type = type;
    g_proto = p;

    if (!g_sc_handle) return -1;
    if (!p) {
        xc_logf("pmdg: 未知机型族 %d，SDK 桥不启用", (int)type);
        g_state = PMDG_STATE_OFF;
        return -1;
    }
    if (!g_sc.MapClientDataNameToID || !g_sc.CreateClientData ||
        !g_sc.AddToClientDataDefinition || !g_sc.RequestClientData) {
        xc_logf("pmdg: SimConnect 不支持 ClientData，SDK 桥不可用");
        g_state = PMDG_STATE_OFF;
        return -1;
    }

    /* --- 数据通道（PMDG → 我们） --- */
    hr = g_sc.MapClientDataNameToID(g_sc_handle, p->data_name, p->data_id);
    if (FAILED(hr)) {
        xc_logf("pmdg: MapClientDataNameToID(\"%s\") hr=0x%08lx",
                p->data_name, (unsigned long)hr);
        g_state = PMDG_STATE_OFF;
        return -1;
    }
    hr = g_sc.CreateClientData(g_sc_handle, p->data_id, p->data_size,
                               0x1 /*ALLOW_SHARED*/);
    if (FAILED(hr)) {
        xc_logf("pmdg: CreateClientData(Data) hr=0x%08lx", (unsigned long)hr);
        g_state = PMDG_STATE_FAILED;
        return -1;
    }
    hr = g_sc.AddToClientDataDefinition(g_sc_handle, p->data_def, 0,
                                        p->data_size, 0.0f, 0);
    if (FAILED(hr)) {
        xc_logf("pmdg: AddToClientDataDefinition(Data) hr=0x%08lx",
                (unsigned long)hr);
        g_state = PMDG_STATE_FAILED;
        return -1;
    }
    /* SIMCONNECT_PERIOD_SIM_FRAME(1)：PMDG 按帧广播，用 ON_SET(2) 会漏掉
     * 中间变化（旋钮快速转动时）。应答机状态变化不频繁，按帧更可靠。 */
    hr = g_sc.RequestClientData(g_sc_handle, p->data_id, PMDG_DATA_REQ,
                                p->data_def, 1 /*SIM_FRAME*/, 0, 0,
                                p->data_size);
    if (FAILED(hr)) {
        xc_logf("pmdg: RequestClientData(Data) hr=0x%08lx", (unsigned long)hr);
        g_state = PMDG_STATE_FAILED;
        return -1;
    }

    /* --- 控制通道（我们 → PMDG） --- */
    hr = g_sc.MapClientDataNameToID(g_sc_handle, p->ctrl_name, p->ctrl_id);
    if (FAILED(hr)) {
        xc_logf("pmdg: MapClientDataNameToID(\"%s\") hr=0x%08lx",
                p->ctrl_name, (unsigned long)hr);
        g_state = PMDG_STATE_FAILED;
        return -1;
    }
    hr = g_sc.CreateClientData(g_sc_handle, p->ctrl_id, 8, 0x1);
    if (FAILED(hr)) {
        xc_logf("pmdg: CreateClientData(Control) hr=0x%08lx", (unsigned long)hr);
        g_state = PMDG_STATE_FAILED;
        return -1;
    }
    hr = g_sc.AddToClientDataDefinition(g_sc_handle, p->ctrl_def, 0, 8, 0.0f, 0);
    if (FAILED(hr)) {
        xc_logf("pmdg: AddToClientDataDefinition(Control) hr=0x%08lx",
                (unsigned long)hr);
        g_state = PMDG_STATE_FAILED;
        return -1;
    }

    g_state = PMDG_STATE_WAIT;
    g_init_at = xc_now_sec();
    g_xpdr.valid = 0;
    g_data_size_seen = 0;          /* 每次 init 都要重新校验载荷尺寸 */
    g_last_mode_raw = 0xFFFFFFFFu;
    xc_logf("pmdg: [%s] 已请求 SDK 数据广播（等首帧；若一直没数据请确认 %s "
            "的 [SDK] 段 EnableDataBroadcast=1）", p->name, p->ini_hint);
    return 0;
}

void pmdgbridge_poll(double now) {
    if (g_state != PMDG_STATE_WAIT) return;
    /* 5 秒没收到任何广播 → 判定失败（通常是 EnableDataBroadcast 没开） */
    if (now - g_init_at > 5.0) {
        xc_logf("pmdg: 5 秒未收到 SDK 数据 → 判定不可用。"
                "请在 %s 的 [SDK] 段设置 EnableDataBroadcast=1 后重启模拟器",
                g_proto ? g_proto->ini_hint : "Options.ini");
        g_state = PMDG_STATE_FAILED;
    }
}

void pmdgbridge_shutdown(void) {
    g_state = PMDG_STATE_OFF;
    g_type = PMDG_TYPE_NONE;
    g_proto = NULL;
    g_xpdr.valid = 0;
    /* 必须重置：否则重新 init（换机型）后 g_data_size_seen 仍非 0，
     * 尺寸校验分支会被跳过，SDK 版本不符时照样按旧偏移解析 → 读错位。 */
    g_data_size_seen = 0;
    g_last_mode_raw = 0xFFFFFFFFu;
}

PmdgState pmdgbridge_state(void) { return g_state; }
PmdgType  pmdgbridge_type(void) { return g_type; }

const char* pmdgbridge_state_name(PmdgState s) {
    switch (s) {
    case PMDG_STATE_OFF:    return "off";
    case PMDG_STATE_WAIT:   return "waiting";
    case PMDG_STATE_READY:  return "ready";
    case PMDG_STATE_FAILED: return "failed";
    default:                return "?";
    }
}

const char* pmdgbridge_type_name(PmdgType t) {
    switch (t) {
    case PMDG_TYPE_NONE: return "none";
    case PMDG_TYPE_737:  return "737";
    case PMDG_TYPE_777:  return "777";
    default:             return "?";
    }
}

int pmdgbridge_get_xpdr(PmdgXpdr* out) {
    if (!out || !g_xpdr.valid) return 0;
    *out = g_xpdr;
    return 1;
}

/* ---------------- 发送事件 ---------------- */

int pmdgbridge_send_event(unsigned event, unsigned param) {
    struct { unsigned Event; unsigned Parameter; } ctrl;

    if (g_state != PMDG_STATE_READY) return 0;
    if (!g_proto || !g_sc.SetClientData) return 0;

    ctrl.Event = event;
    ctrl.Parameter = param;
    if (FAILED(g_sc.SetClientData(g_sc_handle, g_proto->ctrl_id,
                                  g_proto->ctrl_def, 0, 0,
                                  (DWORD)sizeof(ctrl), &ctrl))) {
        xc_logf("pmdg: 发送事件 %u/%u 失败", event, param);
        return 0;
    }
    if (event == g_proto->evt_tcas_mode)
        xc_logf("pmdg: [%s] EVT_TCAS_MODE param=%u", g_proto->name, param);
    return 1;
}

/* 把模式旋钮从 cur 转到 target。
 *
 * PMDG 的模式是**旋钮**，Event 的 Parameter 是转动量（正=加、负=减），
 * 不是目标值。因此需要算差值并逐格转。
 * 两个机型的模式序列都是 5 档且从 STBY 开始：
 *   0=STBY → 1=ALT RPTG OFF → 2=(737)ON/(777)XPNDR
 *   → 3=(737)ALT/(777)TA ONLY → 4=TA/RA
 * 是循环的（继续转会回到 STBY），所以差值按 5 取模取最短路径。 */
static int rotate_mode_to(unsigned cur, unsigned target) {
    int diff;
    unsigned steps;
    unsigned p;

    if (cur == target) return 1;
    diff = (int)target - (int)cur;
    if (diff > 2) diff -= 5;
    if (diff < -2) diff += 5;

    steps = (unsigned)(diff < 0 ? -diff : diff);
    p = (diff > 0) ? 1u : 0xFFFFFFFFu;   /* 1=顺时针，0xFFFFFFFF=-1（补码） */

    while (steps--) {
        if (!pmdgbridge_send_event(g_proto->evt_tcas_mode, p)) return 0;
    }
    return 1;
}

int pmdgbridge_set_xpdr_mode(int pmdg_mode) {
    unsigned cur;

    if (pmdg_mode < 0 || pmdg_mode > PMDG_XPDR_TARA) return -1;
    if (g_state != PMDG_STATE_READY || !g_xpdr.valid || !g_proto) return 0;

    cur = (unsigned)g_xpdr.mode;
    if (cur == (unsigned)pmdg_mode) {
        xc_logf("pmdg: 模式已是 %d，无需转动", pmdg_mode);
        return 1;
    }
    xc_logf("pmdg: 模式 %u → %d（转旋钮）", cur, pmdg_mode);
    return rotate_mode_to(cur, (unsigned)pmdg_mode);
}

int pmdgbridge_set_xpdr_ident(void) {
    if (!g_proto) return 0;
    if (!pmdgbridge_send_event(g_proto->evt_tcas_ident, 1)) return 0;
    pmdgbridge_send_event(g_proto->evt_tcas_ident, 0);
    return 1;
}

/* ---------------- 接收解析 ---------------- */

/* SIMCONNECT_RECV_CLIENT_DATA 头：与 SIMOBJECT_DATA 相同布局 */
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
    BYTE  data[1];        /* 载荷起点 */
} PmdgRecvHdr;

void pmdgbridge_on_client_data(const void* pkt, DWORD cbData) {
    const PmdgRecvHdr* d = (const PmdgRecvHdr*)pkt;
    DWORD hdr;

    if (!d || !g_proto) return;
    hdr = (DWORD)(sizeof(PmdgRecvHdr) - 1);
    if (cbData < hdr) return;
    if (d->dwDefineID != g_proto->data_def) return;   /* 不是本机型的数据 */

    {
        DWORD payload = cbData - hdr;
        const unsigned char* p = d->data;

        if (!g_data_size_seen) {
            g_data_size_seen = (int)payload;
            xc_logf("pmdg: [%s] 首帧数据 %lu 字节（期望 %lu）", g_proto->name,
                    (unsigned long)payload, (unsigned long)g_proto->data_size);
            if (payload < g_proto->data_size) {
                xc_logf("pmdg: [!] 载荷比期望小，SDK 版本可能不同 —— "
                        "拒绝解析以免读错位");
                g_state = PMDG_STATE_FAILED;
                return;
            }
        }
        if (payload <= (DWORD)g_proto->off_mode) return;

        {
            int mode = p[g_proto->off_mode];
            unsigned raw = (unsigned)mode;

            if (raw != g_last_mode_raw) {
                const char* label;
                /* 2/3 号档两个机型语义不同：737 的 ON/ALT vs 777 的 XPNDR/TA ONLY */
                if (g_type == PMDG_TYPE_737) {
                    label = mode == PMDG_XPDR_STBY    ? "STBY" :
                            mode == PMDG_XPDR_ALT_OFF ? "ALT RPTG OFF" :
                            mode == PMDG_XPDR_ON      ? "ON" :
                            mode == PMDG_XPDR_ALT     ? "ALT" :
                            mode == PMDG_XPDR_TARA    ? "TA/RA" : "未知";
                } else {
                    label = mode == PMDG_XPDR_STBY    ? "STBY" :
                            mode == PMDG_XPDR_ALT_OFF ? "ALT RPTG OFF" :
                            mode == PMDG_XPDR_ON      ? "XPNDR" :
                            mode == PMDG_XPDR_ALT     ? "TA ONLY" :
                            mode == PMDG_XPDR_TARA    ? "TA/RA" : "未知";
                }
                g_last_mode_raw = raw;
                xc_logf("pmdg: [%s] 应答机模式 = %d（%s）", g_proto->name,
                        mode, label);
            }

            g_xpdr.mode = mode;
            g_xpdr.selector = (g_proto->off_sel >= 0 &&
                               payload > (DWORD)g_proto->off_sel)
                            ? (p[g_proto->off_sel] ? 1 : 0) : 0;
            g_xpdr.annun_fail = (g_proto->off_fail >= 0 &&
                                 payload > (DWORD)g_proto->off_fail)
                            ? (p[g_proto->off_fail] ? 1 : 0) : 0;
            g_xpdr.ident_pushed = (g_proto->off_ident >= 0 &&
                                   payload > (DWORD)g_proto->off_ident)
                            ? (p[g_proto->off_ident] ? 1 : 0) : 0;
            g_xpdr.valid = 1;
        }

        if (g_state == PMDG_STATE_WAIT) {
            g_state = PMDG_STATE_READY;
            xc_logf("pmdg: [%s] SDK 数据已就绪，可读可写", g_proto->name);
        }
    }
}

void pmdgbridge_dump(void) {
    xc_logf("pmdg: type=%s state=%s payload=%d",
            pmdgbridge_type_name(g_type), pmdgbridge_state_name(g_state),
            g_data_size_seen);
    if (g_xpdr.valid) {
        xc_logf("  xpdr mode=%d selector=%d fail=%d ident=%d", g_xpdr.mode,
                g_xpdr.selector, g_xpdr.annun_fail, g_xpdr.ident_pushed);
    } else {
        xc_logf("  xpdr 无数据");
        if (g_proto)
            xc_logf("  提示：确认 %s 的 [SDK] 段 EnableDataBroadcast=1",
                    g_proto->ini_hint);
    }
}