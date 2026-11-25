/* MConnect — Input Events：按名字触发机模自定义输入事件。
 * 见 inputev.h 的设计说明与用法。 */
#include "inputev.h"
#include "sc.h"

#include <stdio.h>
#include <string.h>

static InputEvent g_events[INPUTEV_MAX];
static int g_event_n = 0;
static CRITICAL_SECTION g_lock;
static int g_lock_init = 0;

static void ensure_lock(void) {
    if (!g_lock_init) {
        InitializeCriticalSection(&g_lock);
        g_lock_init = 1;
    }
}

int inputev_request_enum(void) {
    HRESULT hr;
    if (!g_sc_handle || !g_sc.EnumerateInputEvents) return -1;

    /* RequestID 用独立编号，与 DEF_/REQ_/CEV_/AI 全错开 */
    hr = g_sc.EnumerateInputEvents(g_sc_handle, 9001);
    xc_logf("inputev: enumerate request hr=0x%08lx", (unsigned long)hr);
    return SUCCEEDED(hr) ? 0 : -1;
}

/* 名字/节点字段可能不带结尾 0（定长缓冲），统一收尾。 */
static void copy_field(char* dst, int cap, const char* src, int srclen) {
    int n = 0;
    if (cap < 1) return;
    while (n < srclen && n < cap - 1 && src[n]) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
}

void inputev_on_enumerate(const void* pkt, DWORD cbData) {
    const SC_RECV_ENUMERATE_INPUT_EVENTS* d =
        (const SC_RECV_ENUMERATE_INPUT_EVENTS*)pkt;
    DWORD base, i;
    int added = 0;

    if (!d) return;
    if (d->dwRequestID != 9001) return;

    /* 头部固定 7 个 DWORD，之后是 rgData[dwArraySize] */
    base = (DWORD)(sizeof(SC_RECV_ENUMERATE_INPUT_EVENTS) -
                   sizeof(SC_INPUT_EVENT_DESCRIPTOR));
    if (cbData < base) return;

    ensure_lock();
    LOCK_ENTER(&g_lock);
    for (i = 0; i < d->dwArraySize; i++) {
        const SC_INPUT_EVENT_DESCRIPTOR* it = &d->rgData[i];
        size_t need = base + (i + 1) * sizeof(SC_INPUT_EVENT_DESCRIPTOR);
        char name[INPUTEV_NAME];

        if (cbData < need) break;   /* 截断保护：绝不越界读 */
        copy_field(name, sizeof(name), it->Name, (int)sizeof(it->Name));
        if (!name[0]) continue;

        /* 去重（重连/重复枚举时同一条会再来一次） */
        {
            int k, dup = 0;
            for (k = 0; k < g_event_n; k++) {
                if (g_events[k].hash == it->Hash) { dup = 1; break; }
            }
            if (dup) continue;
        }
        if (g_event_n >= INPUTEV_MAX) break;

        snprintf(g_events[g_event_n].name, sizeof(g_events[g_event_n].name),
                 "%s", name);
        g_events[g_event_n].hash = it->Hash;
        g_events[g_event_n].type = it->Type;
        g_event_n++;
        added++;
    }
    {
        int total = g_event_n;
        LOCK_LEAVE(&g_lock);
        if (added) {
            xc_logf("inputev: page %lu/%lu n=%lu -> total %d",
                    (unsigned long)d->dwEntryNumber, (unsigned long)d->dwOutOf,
                    (unsigned long)d->dwArraySize, total);
        }
    }
}

int inputev_lookup(const char* name, DWORD* hash, DWORD* type) {
    int i, found = 0;
    if (!name || !name[0] || !g_lock_init) return 0;

    LOCK_ENTER(&g_lock);
    for (i = 0; i < g_event_n; i++) {
        if (_stricmp(g_events[i].name, name) == 0) {
            if (hash) *hash = g_events[i].hash;
            if (type) *type = g_events[i].type;
            found = 1;
            break;
        }
    }
    LOCK_LEAVE(&g_lock);
    return found;
}

int inputev_set_by_hash(DWORD hash, double value) {
    HRESULT hr;
    float v = (float)value;   /* 实测 SetInputEvent 的常见类型是 FLOAT32 */

    if (!g_sc_handle || !g_sc.SetInputEvent) return 0;

    /* cbUnitSize 必须与参数签名一致。多数按钮是 1 个 float；
     * 少量是 2 个 double（Push/Release），见文件头说明——这里按 1 个 float
     * 发，失败时上层会退到标准事件（若可用）。 */
    hr = g_sc.SetInputEvent(g_sc_handle, hash, (DWORD)sizeof(v), &v);
    if (FAILED(hr)) {
        /* 单个 float 不被接受时，试 double（部分 SU 用 FLOAT64） */
        double d = value;
        hr = g_sc.SetInputEvent(g_sc_handle, hash, (DWORD)sizeof(d), &d);
        if (FAILED(hr)) {
            xc_logf("inputev: SetInputEvent hash=0x%08lx failed hr=0x%08lx",
                    (unsigned long)hash, (unsigned long)hr);
            return 0;
        }
    }
    return 1;
}

int inputev_set_by_name(const char* name, double value) {
    DWORD hash = 0;
    if (!inputev_lookup(name, &hash, NULL)) {
        xc_logf("inputev: \"%s\" not found (%d events enumerated)", name,
                g_event_n);
        return 0;
    }
    return inputev_set_by_hash(hash, value);
}

int inputev_count(void) {
    int n;
    if (!g_lock_init) return 0;
    LOCK_ENTER(&g_lock);
    n = g_event_n;
    LOCK_LEAVE(&g_lock);
    return n;
}

/* ---------------- 语义归类 ----------------
 *
 * 关键字匹配的顺序很重要：先判更具体的类别，避免"XPDR_MODE" 被 XPDR 的
 * 泛匹配吃掉。都在小写化后比对，不区分机模命名大小写风格
 * （Fenix 全大写 + 下划线，PMDG 用 CamelCase，Asobo 用空格分词）。
 */

/* 小写拷贝，下划线与空格统一（不同机模用不同分词符） */
static void norm_name(const char* src, char* dst, int cap) {
    int n = 0;
    if (!dst || cap < 1) return;
    while (*src && n < cap - 1) {
        char c = *src++;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        else if (c == '_' || c == '-' || c == ' ') c = '_';
        dst[n++] = c;
    }
    dst[n] = '\0';
}

static int has(const char* hay, const char* needle) {
    return strstr(hay, needle) != NULL;
}

/* 应答机相关的公共前缀判断 */
static int is_xpndr(const char* s) {
    return has(s, "xpdr") || has(s, "xpndr") || has(s, "transponder") ||
           has(s, "squawk") || has(s, "atc_code") || has(s, "atccode");
}

InputEventCategory inputev_classify(const char* name) {
    char s[128];

    if (!name || !name[0]) return IE_CAT_NONE;
    norm_name(name, s, sizeof(s));

    /* --- 应答机（先判，因为内部还有子类） --- */
    if (is_xpndr(s)) {
        /* IDENT：优先级最高（"XPDR_IDENT" 也含 XPDR） */
        if (has(s, "ident") || has(s, "id_button") || has(s, "idbtn"))
            return IE_CAT_XPNDR_IDENT;
        /* 模式：mode / stby / alt / on / off / test / ta / ra */
        if (has(s, "mode") || has(s, "_alt") || has(s, "standby") ||
            has(s, "_stby") || has(s, "_on") || has(s, "_off") ||
            has(s, "test") || has(s, "tara") || has(s, "_ta"))
            return IE_CAT_XPNDR_MODE;
        /* 码：code / digit / knob / set / 数字位 */
        if (has(s, "code") || has(s, "digit") || has(s, "knob") ||
            has(s, "set") || has(s, "1") || has(s, "2") ||
            has(s, "3") || has(s, "4"))
            return IE_CAT_XPNDR_CODE;
        return IE_CAT_XPNDR_MODE;   /* 兜底归到模式 */
    }

    /* --- 无线电 --- */
    if (has(s, "com1") || has(s, "com2") || has(s, "com3") ||
        has(s, "com_") || has(s, "radio") || has(s, "vhf") || has(s, "freq")) {
        /* 发射/收听选择 */
        if (has(s, "transmit") || has(s, "xmit") || has(s, "select") ||
            has(s, "receive") || has(s, "recv") || has(s, "audio") ||
            has(s, "listening"))
            return IE_CAT_COM_SELECT;
        /* 频率：交换/旋钮/设值 */
        return IE_CAT_COM_FREQ;
    }

    /* --- 自动驾驶（预留） --- */
    if (has(s, "ap_") || has(s, "autopilot") || has(s, "fcc_") ||
        has(s, "athr") || has(s, "autothrottle"))
        return IE_CAT_AUTOPILOT;

    return IE_CAT_NONE;
}

const char* inputev_category_name(InputEventCategory cat) {
    switch (cat) {
    case IE_CAT_NONE:        return "none";
    case IE_CAT_COM_FREQ:    return "com_freq";
    case IE_CAT_COM_SELECT:  return "com_select";
    case IE_CAT_XPNDR_CODE:  return "xpndr_code";
    case IE_CAT_XPNDR_MODE:  return "xpndr_mode";
    case IE_CAT_XPNDR_IDENT: return "xpndr_ident";
    case IE_CAT_AUTOPILOT:   return "autopilot";
    default:                 return "?";
    }
}

int inputev_count_category(InputEventCategory cat) {
    int i, n = 0;
    if (!g_lock_init) return 0;
    LOCK_ENTER(&g_lock);
    for (i = 0; i < g_event_n; i++) {
        if (inputev_classify(g_events[i].name) == cat) n++;
    }
    LOCK_LEAVE(&g_lock);
    return n;
}

int inputev_get_category(InputEventCategory cat, int idx,
                         char* name, int cap, DWORD* hash) {
    int i, seen = 0;
    int found = 0;
    if (!g_lock_init || idx < 0) return 0;

    LOCK_ENTER(&g_lock);
    for (i = 0; i < g_event_n; i++) {
        if (inputev_classify(g_events[i].name) != cat) continue;
        if (seen++ != idx) continue;
        if (name && cap > 0)
            snprintf(name, (size_t)cap, "%s", g_events[i].name);
        if (hash) *hash = g_events[i].hash;
        found = 1;
        break;
    }
    LOCK_LEAVE(&g_lock);
    return found;
}

void inputev_dump(int max_items) {
    int i;
    if (!g_lock_init) {
        xc_logf("inputev: 未枚举（模拟器未连接或不支持）");
        return;
    }
    LOCK_ENTER(&g_lock);
    xc_logf("inputev: 共 %d 个输入事件，前 %d 个：", g_event_n,
            max_items < g_event_n ? max_items : g_event_n);
    for (i = 0; i < g_event_n && i < max_items; i++) {
        xc_logf("  IE  %-48s hash=0x%08lx type=%lu", g_events[i].name,
                (unsigned long)g_events[i].hash,
                (unsigned long)g_events[i].type);
    }
    LOCK_LEAVE(&g_lock);
}