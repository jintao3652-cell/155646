/* MConnect — 机型能力探测。见 aircraftcaps.h 的设计说明。 */
#include "aircraftcaps.h"
#include "xstr.h"
#include "aircraftdb.h"
#include "inputev.h"
#include "wasmbridge.h"
#include "datadefs.h"
#include "catalog.h"
#include "control.h"

#include <stdio.h>
#include <string.h>

/* 上报给客户端的事件条数上限。太多会让报文变大且客户端列表刷不完，
 * 每类留前 8 条足够（同一功能的按钮极少超过几个）。 */
#define CAPS_MAX_PER_CAT 8

/* 上次上报时的指纹，用来判断"能力集是否变化" */
static char g_last_fp[512];
static int  g_dirty = 1;

/* 参与上报的分类（顺序即客户端显示顺序） */
static const InputEventCategory kCats[] = {
    IE_CAT_COM_FREQ,
    IE_CAT_COM_SELECT,
    IE_CAT_XPNDR_CODE,
    IE_CAT_XPNDR_MODE,
    IE_CAT_XPNDR_IDENT
};

/* 计算能力指纹：机型标题 + 各分类计数 + 前几个名字。
 * 名字也算进去，因为同款机型不同涂装包可能暴露不同事件集。 */
static void build_fingerprint(char* out, int cap) {
    int i, k, n = 0;
    /* ⚠️ 不能用 `n += snprintf(out + n, cap - n, ...)`：截断时 snprintf
     * 返回"本应写入的长度"，大于剩余空间 → n 越过 cap →
     * 下一轮 `cap - n` 变负数（转 size_t 后巨大）→ 越界写。
     * 而且原来的守卫 `if (n >= cap - 1) break;` 写在**追加之后**，
     * 等它发现时 n 已经越界了，守卫形同虚设。
     * XSTR_APPEND 每次写完立刻把 n 夹回 [0, cap-1]，守卫才有效。
     * 注意 out 是 char*（不是数组），所以显式传 cap。 */
    if (!out || cap <= 0) return;
    out[0] = '\0';
#define FP_APPEND(...)                                                      \
    (n = xstr_cat(n,                                                        \
                  snprintf(out + xstr_safe_off(n, cap),                     \
                           xstr_rem(cap, n), __VA_ARGS__),                  \
                  cap))
    FP_APPEND("%s|%d|", g_flight.title, inputev_count());
    for (i = 0; i < (int)(sizeof(kCats) / sizeof(kCats[0])); i++) {
        int cnt = inputev_count_category(kCats[i]);
        FP_APPEND("%d:", cnt);
        for (k = 0; k < cnt && k < 2; k++) {
            char nm[INPUTEV_NAME];
            if (inputev_get_category(kCats[i], k, nm, sizeof(nm), NULL))
                FP_APPEND("%s,", nm);
        }
        FP_APPEND("|");
        if (n >= cap - 1) break;
    }
#undef FP_APPEND
}

int aircaps_tick(void) {
    char fp[512];

    build_fingerprint(fp, sizeof(fp));
    if (strcmp(fp, g_last_fp) != 0) {
        snprintf(g_last_fp, sizeof(g_last_fp), "%s", fp);
        g_dirty = 1;
    }
    if (g_dirty) {
        g_dirty = 0;
        return 1;
    }
    return 0;
}

void aircaps_on_aircraft_change(const char* title) {
    (void)title;
    /* 清指纹，下一次 tick 必然判定为"变化" → 客户端拿到新机型的能力 */
    g_last_fp[0] = '\0';
    g_dirty = 1;
}

int aircaps_build_json(char* out, int size) {
    char evbuf[2048];
    int el = 0;
    int i, k;
    int total_events = 0;
    char wv[32];
    int  wasm_ready = (wasmbridge_state() == WASM_STATE_READY);

    if (!out || size < 64) return 0;
    out[0] = '\0';
    evbuf[0] = '\0';

    /* 逐分类收集事件名（客户端用它渲染可选项） */
    for (i = 0; i < (int)(sizeof(kCats) / sizeof(kCats[0])); i++) {
        int cnt = inputev_count_category(kCats[i]);
        int lim = cnt < CAPS_MAX_PER_CAT ? cnt : CAPS_MAX_PER_CAT;

        for (k = 0; k < lim; k++) {
            char nm[INPUTEV_NAME];
            DWORD h = 0;
            if (!inputev_get_category(kCats[i], k, nm, sizeof(nm), &h)) break;
            /* XSTR_APPEND 保证 el 不会越过 evbuf（原来 `el += snprintf`
             * 截断时会让 el 越界，而下面的 128 字节守卫判断在追加**之后**，
             * 那时已经晚了）。 */
            XSTR_APPEND(evbuf, el,
                        "%s{\"cat\":\"%s\",\"name\":\"%s\",\"hash\":%lu}",
                        total_events ? "," : "",
                        inputev_category_name(kCats[i]), nm,
                        (unsigned long)h);
            total_events++;
            if (el >= (int)sizeof(evbuf) - 128) break;
        }
        if (el >= (int)sizeof(evbuf) - 128) break;
    }

    wasmbridge_version(wv, sizeof(wv));

    /* standard：标准 SimEvent 永远可用（默认机必备），但深度机模可能只响
     * 应一部分。这里保守报 true —— 真正的判断在写入失败后的回退链里。 */
    snprintf(out, (size_t)size,
        "{\"type\":\"aircraft_caps\","
        "\"icao\":\"%s\",\"title\":\"%s\","
        "\"profile\":\"%s\",\"profile_caps\":%u,"
        "\"standard\":{\"com\":true,\"xpdr\":true},"
        "\"inputev\":{"
        "\"total\":%d,"
        "\"com_freq\":%d,\"com_select\":%d,"
        "\"xpndr_code\":%d,\"xpndr_mode\":%d,\"xpndr_ident\":%d},"
        "\"wasm\":{\"state\":\"%s\",\"version\":\"%s\"},"
        "\"events\":[%s]}",
        datadefs_icao(),
        g_flight.title,
        control_profile_id(),
        control_profile_caps(),
        inputev_count(),
        inputev_count_category(IE_CAT_COM_FREQ),
        inputev_count_category(IE_CAT_COM_SELECT),
        inputev_count_category(IE_CAT_XPNDR_CODE),
        inputev_count_category(IE_CAT_XPNDR_MODE),
        inputev_count_category(IE_CAT_XPNDR_IDENT),
        wasmbridge_state_name(wasmbridge_state()), wv,
        evbuf);

    (void)wasm_ready;
    return (int)strlen(out);
}

void aircaps_dump(void) {
    int i;

    xc_logf("aircaps: 机型 \"%s\" ICAO=%s  InputEvent=%d  WASM=%s",
            g_flight.title, datadefs_icao(), inputev_count(),
            wasmbridge_state_name(wasmbridge_state()));
    for (i = 0; i < (int)(sizeof(kCats) / sizeof(kCats[0])); i++) {
        int cnt = inputev_count_category(kCats[i]);
        int k;
        xc_logf("  %-12s %d 个", inputev_category_name(kCats[i]), cnt);
        for (k = 0; k < cnt && k < CAPS_MAX_PER_CAT; k++) {
            char nm[INPUTEV_NAME];
            DWORD h = 0;
            if (inputev_get_category(kCats[i], k, nm, sizeof(nm), &h))
                xc_logf("      %-44s hash=%lu", nm, (unsigned long)h);
        }
    }
}