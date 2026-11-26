/* MConnect — 反向控制：应答机 / 无线电频率（客户端 → 模拟器）。
 * 见 control.h 的设计说明与事件表。 */
#include "control.h"
#include "aircraftdb.h"
#include "datadefs.h"
#include "inputev.h"
#include "pmdgbridge.h"
#include "wasmbridge.h"
#include "json.h"
#include "sc.h"

#include <stdio.h>
#include <string.h>

/* 命令队列：网络线程生产、主线程消费。
 * 64 条足够——正常操作每秒钟最多几条（旋钮拨动/交换频率），
 * 用户按住按键刷屏也只会丢最旧的，不会卡住。 */
#define CMD_QUEUE_MAX 64

/* 详细日志开关（--verbose）。定义在 mconnect.c。 */
extern int g_verbose_flag;

static Command g_queue[CMD_QUEUE_MAX];
static int g_q_head = 0;    /* 下一个写入位置 */
static int g_q_count = 0;
static CRITICAL_SECTION g_q_lock;
static int g_q_lock_init = 0;

/* 应答机模式：协议用**规范枚举**（= X-Plane 的顺序），MSFS 的顺序不同，
 * 必须重映射。不重映射的话 "设为 ON" 会把应答机打到 Test 位——飞行中
 * 应答机不响应询问，管制看不到你，属于会让航班"凭空消失"的错误。
 *
 *   规范（协议 / X-Plane）  0=Off 1=Stby 2=On   3=Alt 4=Test 5=Ground
 *   MSFS SimVar 枚举        0=Off 1=Stby 2=Test 3=On 4=Alt  5=Ground
 */
static int canonical_to_msfs_state(int st);   /* 见下方定义 */

static int g_evt_ready = 0;   /* 事件映射是否已注册成功 */

/* ---------------- 基础换算 ---------------- */

/* 协议里频率是 MHz*1000（122800 = 122.800 MHz），事件要 Hz（122800000）。 */
long control_mhz1000_to_hz(int mhz1000) {
    /* COM 波段：118.000 ~ 136.975 MHz。8.33kHz 间隔下最小步进 0.005，
     * 因此合法范围按 118000..137000（MHz*1000）判断。
     * 越界值直接拒（客户端可能把 NAV 频率误发到 COM 通道）。 */
    if (mhz1000 < 118000 || mhz1000 > 137000) return 0;
    return (long)mhz1000 * 1000L;
}

/* "1234" → 1234（内部按十进制存，写事件时转 BCD）。
 * 每位必须是 0..7（应答机八进制）。 */
int control_parse_squawk(const char* s) {
    int i, v = 0;
    if (!s) return -1;
    for (i = 0; i < 4; i++) {
        if (s[i] < '0' || s[i] > '7') return -1;
        v = v * 10 + (s[i] - '0');
    }
    /* 末尾必须结束（"12345" 非法） */
    if (s[4] != '\0') return -1;
    return v;
}

/* 协议规范枚举 → MSFS SimVar 枚举。 */
static int canonical_to_msfs_state(int st) {
    switch (st) {
    case 2:  return 3;   /* On   */
    case 3:  return 4;   /* Alt  */
    case 4:  return 2;   /* Test */
    default: return st;  /* Off/Stby/Ground 两侧一致 */
    }
}

/* 导出版本（测试与外部模块用）。 */
int control_canonical_to_msfs_state(int st) {
    return canonical_to_msfs_state(st);
}

/* 十进制 1234 → BCD 0x1234（XPNDR_SET 的参数格式） */
static DWORD squawk_to_bcd(int code) {
    int d0 = (code / 1000) % 10;
    int d1 = (code / 100) % 10;
    int d2 = (code / 10) % 10;
    int d3 = code % 10;
    return (DWORD)((d0 << 12) | (d1 << 8) | (d2 << 4) | d3);
}

/* ---------------- 事件注册 ---------------- */

/* MapClientEventToSimEvent：把客户端事件 ID 绑定到模拟器事件名。
 * 只需一次；绑定后 TransmitClientEvent 才能触发。 */
static int map_event(DWORD client_id, const char* name) {
    HRESULT hr;
    if (!g_sc.MapClientEventToSimEvent) return 0;
    hr = g_sc.MapClientEventToSimEvent(g_sc_handle, client_id, name);
    if (FAILED(hr)) {
        xc_logf("control: map \"%s\" failed hr=0x%08lx", name,
                (unsigned long)hr);
        return 0;
    }
    return 1;
}

/* 事件里给的是"客户端事件 ID"，TransmitClientEvent 的 GroupID 参数在
 * EVENT_FLAG_GROUPID_IS_PRIORITY 标志下被解释为**优先级数值**，
 * 因此不需要真的建通知组——直接传 HIGHEST 即可。 */
#define EVT_FLAG_GROUP_IS_PRIORITY 0x00000010
#define PRIO_HIGHEST               1

int control_init(void) {
    int ok = 1;

    if (!g_sc_handle) return -1;
    if (!g_q_lock_init) {
        InitializeCriticalSection(&g_q_lock);
        g_q_lock_init = 1;
    }

    ok &= map_event(CEV_COM1_SET,      "COM_RADIO_SET_HZ");
    ok &= map_event(CEV_COM1_STBY_SET, "COM_STBY_RADIO_SET_HZ");
    ok &= map_event(CEV_COM2_SET,      "COM2_RADIO_SET_HZ");
    ok &= map_event(CEV_COM2_STBY_SET, "COM2_STBY_RADIO_SET_HZ");
    ok &= map_event(CEV_COM3_SET,      "COM3_RADIO_SET_HZ");
    ok &= map_event(CEV_COM3_STBY_SET, "COM3_STBY_RADIO_SET_HZ");
    ok &= map_event(CEV_XPNDR_SET,     "XPNDR_SET");
    ok &= map_event(CEV_XPNDR_IDENT,   "XPNDR_IDENT_SET");
    ok &= map_event(CEV_COM1_SWAP,     "COM1_RADIO_SWAP");
    ok &= map_event(CEV_COM2_SWAP,     "COM2_RADIO_SWAP");
    ok &= map_event(CEV_COM3_SWAP,     "COM3_RADIO_SWAP");
    /* 发射台选择与"全收"：语音通讯必需（XVoice 靠读 COM TRANSMIT /
     * PILOT TRANSMITTER TYPE 判断，写侧用这四个事件）。 */
    ok &= map_event(CEV_COM1_XMIT,     "COM1_TRANSMIT_SELECT");
    ok &= map_event(CEV_COM2_XMIT,     "COM2_TRANSMIT_SELECT");
    ok &= map_event(CEV_COM3_XMIT,     "COM3_TRANSMIT_SELECT");
    ok &= map_event(CEV_COM_RECV_ALL,  "COM_RECEIVE_ALL_SET");
    /* 收听开关（XConnect monitor 字段互通，逐台 toggle） */
    ok &= map_event(CEV_COM1_RECV,     "COM1_RECEIVE_SELECT");
    ok &= map_event(CEV_COM2_RECV,     "COM2_RECEIVE_SELECT");
    ok &= map_event(CEV_COM3_RECV,     "COM3_RECEIVE_SELECT");
    /* 内置机模标签开关（呼号标签显示，--labels 用；失败只影响该功能） */
    map_event(CEV_TOGGLE_LABELS,       "TOGGLE_AIRCRAFT_LABELS");

    g_evt_ready = ok ? 1 : 0;
    xc_logf("control: event mapping %s", ok ? "ok" : "PARTIAL (see above)");
    return ok ? 0 : -1;
}

/* ---------------- 队列 ---------------- */

int control_post(const Command* cmd) {
    int dropped = 0;

    if (!cmd || cmd->kind == CMD_NONE) return -1;
    if (!g_q_lock_init) return -1;

    LOCK_ENTER(&g_q_lock);
    if (g_q_count >= CMD_QUEUE_MAX) {
        /* 队列满：丢最旧的一条（控制命令"最新有效"） */
        g_q_head = (g_q_head + 1) % CMD_QUEUE_MAX;
        g_q_count--;
        dropped = 1;
    }
    g_queue[g_q_head] = *cmd;
    g_q_head = (g_q_head + 1) % CMD_QUEUE_MAX;
    g_q_count++;
    LOCK_LEAVE(&g_q_lock);

    if (dropped) xc_logf("control: queue full, dropped oldest command");
    return 0;
}

/* 第零级：机型档案（最高优先级）。
 *
 * 为什么放在标准事件之前：档案里的写法是从 HubHop 27382 条社区预置中
 * 核对过的**实测可用**写法，比"发标准事件然后祈祷机模响应"可靠得多。
 * 典型例子：FBW 的应答机模式必须写 (L:A32NX_TRANSPONDER_MODE)，发标准
 * 事件完全无效；PMDG 的按钮名是 switch_800_73X，靠猜名字永远猜不到。
 *
 * 当前机型的档案在 control_init 时解析并缓存，避免每帧查表。 */
#define g_profile (aircraftdb_current())

/* 用档案里的 RPN 模板执行。返回 1 = 已成功交给 WASM 桥。 */
/* FSL A32X 应答机旋钮（定义在文件下方，这里前向声明） */
static int fsl_rotate_xpdr(int clockwise);

static int try_profile(const Command* c) {
    char rpn[200];

    /* PMDG 优先：它既没有可写 LVar 也没有响应标准事件的可靠保证，
     * 官方 SDK ClientData 是唯一正解（含应答机模式与 IDENT）。
     * 放在最前面，避免被后面的通用路径抢先尝试而静默失败。 */
    if (pmdgbridge_state() == PMDG_STATE_READY) {
        switch (c->kind) {
        case CMD_XPNDR_STATE: {
            /* 规范枚举 → PMDG 原生枚举。两个机型都是 5 档，但 2/3 号档语义不同：
             *   737: 0=STBY 1=ALT RPTG OFF 2=ON      3=ALT     4=TA/RA
             *   777: 0=STBY 1=ALT RPTG OFF 2=XPNDR   3=TA ONLY 4=TA/RA
             * 对应到规范枚举的 ON/ALT 时两边取值相同，故无需分支。
             * PMDG 无独立 OFF/TEST/GND 档 → 用最接近的档代替。 */
            int pmdg_mode = -1;
            switch (c->state) {
            case 0: pmdg_mode = PMDG_XPDR_STBY;    break;  /* OFF  → STBY */
            case 1: pmdg_mode = PMDG_XPDR_STBY;    break;  /* STBY */
            case 2: pmdg_mode = PMDG_XPDR_ON;      break;  /* ON   */
            case 3: pmdg_mode = PMDG_XPDR_ALT;     break;  /* ALT  */
            case 4: pmdg_mode = PMDG_XPDR_ALT_OFF; break;  /* TEST → ALT RPTG OFF */
            case 5: pmdg_mode = PMDG_XPDR_ALT_OFF; break;  /* GND  → ALT RPTG OFF */
            default: break;
            }
            if (pmdg_mode >= 0 && pmdgbridge_set_xpdr_mode(pmdg_mode) > 0) {
                xc_logf("control: [pmdg-sdk] 应答机模式 → %d", pmdg_mode);
                return 1;
            }
            break;
        }
        case CMD_XPNDR_IDENT:
            if (c->on) {
                if (pmdgbridge_set_xpdr_ident()) {
                    xc_logf("control: [pmdg-sdk] IDENT");
                    return 1;
                }
            }
            return 1;   /* IDENT 释放无需处理，视为成功 */
        default:
            break;
        }
    }

    if (!g_profile) return 0;
    if (wasmbridge_state() != WASM_STATE_READY) {
        /* 档案里的写法几乎都是 RPN/LVar，必须经 WASM 桥才能执行。
         * 未装 WASM 模块时只能退回标准事件（下面 exec_command 会走）。 */
        return 0;
    }

    switch (c->kind) {
    case CMD_XPNDR_CODE:
        if (aircraftdb_expand_xpdr_code(g_profile, c->code, rpn, sizeof(rpn)) &&
            wasmbridge_exec_rpn(rpn) == 0) {
            xc_logf("control: [%s] 码 %04d → %s", g_profile->id, c->code, rpn);
            return 1;
        }
        break;

    case CMD_XPNDR_STATE:
        /* FSL A32X 专用：MODE 旋钮只能相对转，档位数未知，
         * 因此按"目标 > 当前 则顺时针，否则逆时针"转一格。
         * 调用方重复发送即可逐步逼近（客户端轮询时会看到状态变化）。 */
        if (aircraftdb_current() &&
            strcmp(aircraftdb_current()->id, "fsl-a32x") == 0) {
            /* 用标准 SimVar 的当前值做参照（FSL 会维护它，若不维护则恒 0，
             * 最多转错方向，不会死循环） */
            int cur = g_flight.xpdr_state;
            if (cur == c->state) return 1;
            if (fsl_rotate_xpdr(c->state > cur)) {
                xc_logf("control: [fsl-a32x] 应答机转一格 %s（当前 %d 目标 %d）",
                        c->state > cur ? "顺时针" : "逆时针", cur, c->state);
                return 1;
            }
        }
        /* 档案里的规范枚举 → 原生档位映射；读取侧用同一张表反解。 */
        if (aircraftdb_expand_xpdr_mode(g_profile, c->state, rpn, sizeof(rpn)) &&
            wasmbridge_exec_rpn(rpn) == 0) {
            xc_logf("control: [%s] 模式 %d（原生 %d）→ %s", g_profile->id,
                    c->state, aircraftdb_canon_to_native(g_profile, c->state),
                    rpn);
            return 1;
        }
        break;

    case CMD_COM_SET:
        /* 只处理主频（档案里的 com_set_rpn 是主频模板）；备频走标准事件。 */
        if (c->active &&
            aircraftdb_expand_com_set(g_profile, c->com_index, c->hz,
                                      rpn, sizeof(rpn)) &&
            wasmbridge_exec_rpn(rpn) == 0) {
            xc_logf("control: [%s] COM%d 频率 %ld → %s", g_profile->id,
                    c->com_index, c->hz, rpn);
            return 1;
        }
        break;

    case CMD_COM_SWAP:
        if (aircraftdb_expand_com_swap(g_profile, c->com_index, rpn,
                                       sizeof(rpn)) &&
            wasmbridge_exec_rpn(rpn) == 0) {
            xc_logf("control: [%s] COM%d 交换 → %s", g_profile->id,
                    c->com_index, rpn);
            return 1;
        }
        break;

    case CMD_COM_XMIT:
        if (aircraftdb_expand_com_xmit(g_profile, c->com_index, rpn,
                                       sizeof(rpn)) &&
            wasmbridge_exec_rpn(rpn) == 0) {
            xc_logf("control: [%s] 发射台 COM%d → %s", g_profile->id,
                    c->com_index, rpn);
            return 1;
        }
        break;

    default:
        break;
    }
    return 0;
}

/* FSL A32X 应答机写入。
 *
 * 机制（从本机机型文件实测，见 aircraftdb.c 的 fsl-a32x 档案）：
 *   应答机面板按钮/旋钮都走 (L:CD_InfKnobID / ATCXPDRBtnID) + (>K:ROTOR_BRAKE)
 *   MODE 旋钮 EVENT_ID = 78347，按 FSL_ClickAndDrag_Knob_Template 算法：
 *       (EVENT_ID - 1) (>L:CD_InfKnobID)      → 78346
 *       逆时针一格 +1                          → 78347
 *       顺时针一格 +2                          → 78348
 *   所以"转一格"就是 78347 / 78348 送 ROTOR_BRAKE。
 *
 * 为什么不用档案的 xpdr_mode_rpn 模板：FSL 的 MODE 旋钮 ANIM_LENGTH=20，
 * 但**实际档位数不明**（A320 面板通常是 STBY/AUTO/ON 三档循环，也可能是
 * 四档）。档位数未知时无法算出"从当前到目标转几格"，硬写模板会转错档。
 * 因此这里只做"相对转动"，由调用方决定转几格。
 *
 * 已知限制：不能一次跳到目标档，只能一格一格转。调用方需要循环调用。 */
#define FSL_EVT_ROTOR_BASE  78347u   /* MODE 旋钮：逆时针一格的 EVENT_ID */

static int fsl_rotate_xpdr(int clockwise) {
    char rpn[96];

    if (!aircraftdb_current() ||
        strcmp(aircraftdb_current()->id, "fsl-a32x") != 0) return 0;
    if (wasmbridge_state() != WASM_STATE_READY) return 0;

    /* 顺时针 = BASE+1，逆时针 = BASE */
    snprintf(rpn, sizeof(rpn), "%lu (>L:CD_InfKnobID, Number) %lu (>K:ROTOR_BRAKE)",
             (unsigned long)(FSL_EVT_ROTOR_BASE + (clockwise ? 1 : 0)),
             (unsigned long)(FSL_EVT_ROTOR_BASE + (clockwise ? 1 : 0)));
    return wasmbridge_exec_rpn(rpn) == 0;
}

/* 深度机模回退：标准事件不响应时，改按 InputEvent 名字触发。
 *
 * 何时会走到这里：TransmitClientEvent 返回失败。但更常见的情况是
 * **事件发送成功、机模却忽略它**（Fenix/PMDG 的典型表现）——那种情况
 * HRESULT 是 S_OK，我们无法感知，只能靠"回读值没变"来间接发现。
 * 因此本函数同时也用于"主动优先走 InputEvent"的模式（见 g_prefer_input）。
 *
 * 命名约定（按各机模实际事件名）：
 *   COM 频率面板按钮   机模自定义，通常叫 COM1_SWAP_Push / COM_RADIO_...
 *   应答机面板按钮     XPDR_... / ATC_..._Push
 * 这些名字因机模而异，无法硬编码，所以这里做的是**名字前缀搜索**：
 * 找出所有含关键字的 InputEvent，按常见命名优先，逐个尝试。
 */
/* 第三级回退：WASM 桥执行 RPN。
 *
 * 为什么这是最强的一级：前面两级都受机模实现限制（事件未实现 / 按钮没暴露），
 * 而 RPN 能直接改 LVar、触发 B 事件，几乎任何操作都能做到。
 * 代价是需要用户装 MobiFlight WASM 模块，所以放在最后。
 *
 * RPN 写法（K 事件不能带括号参数，必须用 RPN 语法）：
 *   设频率       122800000 (>K:COM_RADIO_SET_HZ)
 *   设应答机码   (A:TRANSPONDER CODE:1,Bco16)  ... 这里用事件更简单
 *   写 LVar      122 (>L:MY_COM1_FREQ)
 *   触发按钮     1 (>B:A32NX_ATSUBTN_PUSH)
 *   B 事件按下/松开要成对：1 (>B:X_Push) 0 (>B:X_Release)
 */
static int try_rpn_fallback(const Command* c) {
    char rpn[160];

    if (wasmbridge_state() != WASM_STATE_READY) return 0;

    rpn[0] = '\0';
    switch (c->kind) {
    case CMD_COM_SET:
        /* COM1 的事件名是 COM_RADIO_SET_HZ / COM_STBY_RADIO_SET_HZ（不带数字），
         * COM2/COM3 是 COM2_RADIO_SET_HZ 形式，因此分开拼。 */
        if (c->com_index == 1) {
            snprintf(rpn, sizeof(rpn), "%ld (>K:%s)", c->hz,
                     c->active ? "COM_RADIO_SET_HZ" : "COM_STBY_RADIO_SET_HZ");
        } else {
            snprintf(rpn, sizeof(rpn), "%ld (>K:COM%d_RADIO_%s)", c->hz,
                     c->com_index, c->active ? "SET_HZ" : "STBY_SET_HZ");
        }
        break;

    case CMD_COM_SWAP:
        snprintf(rpn, sizeof(rpn), "1 (>K:COM%d_RADIO_SWAP)", c->com_index);
        break;

    case CMD_COM_XMIT:
        snprintf(rpn, sizeof(rpn), "1 (>K:COM%d_TRANSMIT_SELECT)", c->com_index);
        break;

    case CMD_COM_RECV_ALL:
        snprintf(rpn, sizeof(rpn), "%d (>K:COM_RECEIVE_ALL_SET)", c->on ? 1 : 0);
        break;

    case CMD_XPNDR_CODE: {
        /* 应答机码：XPNDR_SET 要 BCD。RPN 里用 hex 字面量 0x 前缀不通用，
         * 因此按十进制算好 BCD 值再传。 */
        int d0 = (c->code / 1000) % 10, d1 = (c->code / 100) % 10;
        int d2 = (c->code / 10) % 10, d3 = c->code % 10;
        int bcd = (d0 << 12) | (d1 << 8) | (d2 << 4) | d3;
        snprintf(rpn, sizeof(rpn), "%d (>K:XPNDR_SET)", bcd);
        break;
    }

    case CMD_XPNDR_IDENT:
        snprintf(rpn, sizeof(rpn), "%d (>K:XPNDR_IDENT_SET)", c->on ? 1 : 0);
        break;

    case CMD_INPUT_EVENT: {
        /* InputEvent 在 WASM 上下文里没有直接对应语法，最接近的是
         * "按名字触发"——但 RPN 无法按 InputEvent 名字触发，
         * 因此这里只能放弃（如实返回 0，让上层记录失败）。 */
        return 0;
    }

    default:
        return 0;
    }

    if (!rpn[0]) return 0;
    if (wasmbridge_exec_rpn(rpn) == 0) {
        xc_logf("control: RPN 回退 → %s", rpn);
        return 1;
    }
    return 0;
}

static int try_input_event_fallback(const Command* c);

/* 在某个语义分类下，找名字里含指定关键字的第一个事件。
 *
 * 为什么改成"按分类+关键字搜索"而不是硬编码候选名：每个机模命名风格不同
 * （Fenix `XPDR_STBY_Push`、PMDG `EVT_XPNDR_MODE`、Asobo `TRANSPONDER_...`），
 * 写死一份候选表必然漏机型。分类由 inputev_classify 按语义判定，
 * 再在本类里用关键字挑出最贴合的那个。 */
static int find_in_category(InputEventCategory cat, const char* const* keys,
                            int nkeys, DWORD* hash) {
    int total = inputev_count_category(cat);
    int i, k;

    for (k = 0; k < nkeys; k++) {
        if (!keys[k] || !keys[k][0]) continue;
        for (i = 0; i < total; i++) {
            char nm[INPUTEV_NAME];
            DWORD h = 0;
            char low[INPUTEV_NAME];
            const char* p;
            int j;

            if (!inputev_get_category(cat, i, nm, sizeof(nm), &h)) break;
            /* 小写化后子串匹配（关键字表也用小写） */
            for (j = 0; nm[j] && j < (int)sizeof(low) - 1; j++) {
                char ch = nm[j];
                low[j] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
            }
            low[j] = '\0';
            p = strstr(low, keys[k]);
            if (p) {
                if (hash) *hash = h;
                return 1;
            }
        }
    }
    return 0;
}

static int try_input_event_fallback(const Command* c) {
    DWORD hash = 0;
    int ok = 0;

    if (inputev_count() == 0) return 0;   /* 未枚举，无从回退 */

    switch (c->kind) {
    case CMD_COM_SET:
    case CMD_COM_SWAP: {
        /* 频率类：只有"交换"有对应按钮（面板旋钮只能一档一档转，
         * 没有"设为 122.800"这种整值事件）。按台号挑 swap/transfer。 */
        static const char* kSwap[] = { "swap", "transfer", "stby" };
        if (c->kind == CMD_COM_SWAP) {
            char want[16];
            /* 优先找带台号的（com1_swap），找不到再退回通用的 */
            snprintf(want, sizeof(want), "com%d", c->com_index);
            {
                int total = inputev_count_category(IE_CAT_COM_FREQ);
                int i;
                for (i = 0; i < total; i++) {
                    char nm[INPUTEV_NAME];
                    DWORD h = 0;
                    char low[INPUTEV_NAME];
                    int j;
                    if (!inputev_get_category(IE_CAT_COM_FREQ, i, nm,
                                              sizeof(nm), &h)) break;
                    for (j = 0; nm[j] && j < (int)sizeof(low) - 1; j++) {
                        char ch = nm[j];
                        low[j] = (ch >= 'A' && ch <= 'Z')
                               ? (char)(ch - 'A' + 'a') : ch;
                    }
                    low[j] = '\0';
                    if (strstr(low, want) &&
                        (strstr(low, "swap") || strstr(low, "transfer"))) {
                        hash = h;
                        ok = 1;
                        break;
                    }
                }
            }
            if (!ok) ok = find_in_category(IE_CAT_COM_FREQ, kSwap, 3, &hash);
        }
        break;
    }

    case CMD_COM_XMIT:
        /* 发射台选择：找 transmit/select + 台号 */
        {
            static const char* kXmit[] = { "transmit", "xmit", "select" };
            int total = inputev_count_category(IE_CAT_COM_SELECT);
            int i;
            char want[16];
            snprintf(want, sizeof(want), "com%d", c->com_index);
            for (i = 0; i < total; i++) {
                char nm[INPUTEV_NAME];
                DWORD h = 0;
                char low[INPUTEV_NAME];
                int j;
                if (!inputev_get_category(IE_CAT_COM_SELECT, i, nm,
                                          sizeof(nm), &h)) break;
                for (j = 0; nm[j] && j < (int)sizeof(low) - 1; j++) {
                    char ch = nm[j];
                    low[j] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
                }
                low[j] = '\0';
                if (strstr(low, want)) { hash = h; ok = 1; break; }
            }
            if (!ok) ok = find_in_category(IE_CAT_COM_SELECT, kXmit, 3, &hash);
        }
        break;

    case CMD_XPNDR_CODE: {
        /* 应答机码：找 set/code 类整值事件（旋钮式无法直接设值） */
        static const char* kCode[] = { "set", "code" };
        ok = find_in_category(IE_CAT_XPNDR_CODE, kCode, 2, &hash);
        break;
    }

    case CMD_XPNDR_STATE: {
        static const char* kMode[] = { "mode", "alt", "stby", "on" };
        ok = find_in_category(IE_CAT_XPNDR_MODE, kMode, 4, &hash);
        break;
    }

    case CMD_XPNDR_IDENT: {
        static const char* kIdent[] = { "ident", "id" };
        ok = find_in_category(IE_CAT_XPNDR_IDENT, kIdent, 2, &hash);
        break;
    }

    default:
        break;
    }

    if (!ok || !hash) {
        /* 二级失败 → 三级：WASM 桥执行 RPN（最强，需装 WASM 模块） */
        if (try_rpn_fallback(c)) return 1;
        xc_logf("control: 无 InputEvent / RPN 回退（kind=%d com=%d）",
                (int)c->kind, c->com_index);
        return 0;
    }

    /* 按钮类事件按"按下=1，稍后松开=0"处理；这里只发按下，
     * 面板逻辑通常自己处理复位（对 SWAP 类够用）。 */
    {
        double v = (c->kind == CMD_XPNDR_CODE) ? (double)c->code
                 : (c->kind == CMD_XPNDR_STATE) ? (double)c->state
                 : (c->kind == CMD_XPNDR_IDENT) ? (c->on ? 1.0 : 0.0)
                 : 1.0;
        if (inputev_set_by_hash(hash, v)) {
            xc_logf("control: InputEvent 回退成功 hash=0x%08lx value=%.0f",
                    (unsigned long)hash, v);
            return 1;
        }
    }
    return 0;
}

/* 主线程执行一条命令。返回 1 成功。
 *
 * 执行顺序（可靠性从高到低）：
 *   0. 机型档案（实测的精准写法，需 WASM 桥）
 *   1. 标准 SimEvent（默认机必备）
 *   2. InputEvent 按语义搜索（机模暴露了自定义按钮时）
 *   3. WASM RPN 兜底
 */
static int exec_command(const Command* c) {
    HRESULT hr;
    DWORD evid = 0;
    DWORD data = 0;

    /* 第 0 级：机型档案。这是唯一对深度机模可靠的方式。 */
    if (try_profile(c)) return 1;

    switch (c->kind) {
    case CMD_COM_SET: {
        long hz = c->hz;
        if (hz <= 0) hz = control_mhz1000_to_hz((int)(c->hz / 1000));
        if (hz <= 0) {
            xc_logf("control: COM%d %s invalid freq", c->com_index,
                    c->active ? "active" : "standby");
            return 0;
        }
        if (c->com_index == 1)
            evid = c->active ? CEV_COM1_SET : CEV_COM1_STBY_SET;
        else if (c->com_index == 2)
            evid = c->active ? CEV_COM2_SET : CEV_COM2_STBY_SET;
        else
            evid = c->active ? CEV_COM3_SET : CEV_COM3_STBY_SET;
        data = (DWORD)hz;
        break;
    }
    case CMD_COM_SWAP:
        evid = (c->com_index == 2) ? CEV_COM2_SWAP
             : (c->com_index == 3) ? CEV_COM3_SWAP : CEV_COM1_SWAP;
        data = 0;
        break;

    case CMD_COM_XMIT:
        /* 选定发射台。事件名是 COM{n}_TRANSMIT_SELECT，data 必须给 1
         * （给 0 在多数机型上无效，个别机型会解释为"取消"）。 */
        evid = (c->com_index == 2) ? CEV_COM2_XMIT
             : (c->com_index == 3) ? CEV_COM3_XMIT : CEV_COM1_XMIT;
        data = 1;
        break;

    case CMD_COM_RECV_ALL:
        evid = CEV_COM_RECV_ALL;
        data = c->on ? 1 : 0;
        break;

    case CMD_COM_RECV:
        /* 收听开关 toggle（XP monitor 语义）：按一次切一次，
         * 状态回读靠 flight_data 的 audio_sel_comN（COM RECEIVE:n）。 */
        evid = (c->com_index == 2) ? CEV_COM2_RECV
             : (c->com_index == 3) ? CEV_COM3_RECV : CEV_COM1_RECV;
        data = 0;
        break;

    case CMD_XPNDR_CODE:
        evid = CEV_XPNDR_SET;
        data = squawk_to_bcd(c->code);
        break;

    case CMD_XPNDR_IDENT:
        evid = CEV_XPNDR_IDENT;
        data = c->on ? 1 : 0;
        break;

    case CMD_INPUT_EVENT:
        /* 客户端点名要的 InputEvent（名字来自 aircraft_caps 报文）。
         * 直接按 hash 触发，不走标准事件——这是"按实际机型给选项"的写入端。 */
        if (inputev_set_by_hash(c->hash, (double)c->state)) {
            if (g_verbose_flag)
                xc_logf("control: INPUT_EVENT hash=0x%08lx value=%d ok",
                        (unsigned long)c->hash, c->state);
            return 1;
        }
        /* 失败 → 试 WASM 桥（有些机模的 InputEvent 只在 WASM 上下文有效） */
        return try_rpn_fallback(c);

    case CMD_XPNDR_STATE: {
        /* 应答机模式没有对应事件，写可写 SimVar TRANSPONDER STATE。
         * 定义在启动时建好（datadefs_register_xpdr），避免每条命令临时建定义。
         * 注意枚举顺序与协议规范不同，必须重映射（见 canonical_to_msfs_state）。 */
        int st = canonical_to_msfs_state(c->state);
        if (st < 0) st = 0;
        if (st > 5) st = 5;
        {
            double v = (double)st;
            if (!g_sc.SetDataOnSimObject) return 0;
            hr = g_sc.SetDataOnSimObject(g_sc_handle, SC_DATA_SET_FLAG_DEFAULT,
                                         SC_OBJECT_ID_USER, DEF_XPNDR_STATE, 1,
                                         (DWORD)sizeof(double), &v);
            xc_logf("control: XPNDR state canonical=%d -> msfs=%d hr=0x%08lx",
                    c->state, st, (unsigned long)hr);
            return SUCCEEDED(hr);
        }
    }

    default:
        return 0;
    }

    if (!g_sc.TransmitClientEvent) return 0;
    hr = g_sc.TransmitClientEvent(g_sc_handle, SC_OBJECT_ID_USER, evid, data,
                                  PRIO_HIGHEST, EVT_FLAG_GROUP_IS_PRIORITY);
    if (FAILED(hr)) {
        xc_logf("control: transmit evt=%lu data=%lu failed hr=0x%08lx",
                (unsigned long)evid, (unsigned long)data, (unsigned long)hr);
        /* 标准事件被拒 → 试深度机模的 InputEvent 回退 */
        return try_input_event_fallback(c);
    }
    if (g_verbose_flag) {
        xc_logf("control: evt=%lu data=%lu ok", (unsigned long)evid,
                (unsigned long)data);
    }
    return 1;
}

void control_set_aircraft(const char* title, const char* icao) {
    const AircraftProfile* p = aircraftdb_find(title, icao);

    if (p == aircraftdb_current()) return;   /* 没变，不刷日志 */

    aircraftdb_set_current(p);       /* 共享给 datadefs 的读取侧 */
    datadefs_reset_profile_reads();  /* 换机型 → 旧的读取表达式作废 */
    if (p) {
        xc_logf("control: 机型档案 \"%s\"（匹配 \"%s\"）", p->id, title);
        if (p->note && p->note[0]) xc_logf("         %s", p->note);
        if (aircraftdb_needs_custom_read(p))
            xc_logf("         应答机状态走 LVar 读取（标准 SimVar 对此机模无效）");
    } else {
        xc_logf("control: 无机型档案（\"%s\"），走通用逻辑", title);
    }
}

const char* control_profile_id(void) {
    const AircraftProfile* p = aircraftdb_current();
    return p ? p->id : "none";
}

unsigned control_profile_caps(void) {
    const AircraftProfile* p = aircraftdb_current();
    return p ? p->caps : 0u;
}

int control_toggle_aircraft_labels(void) {
    if (!g_evt_ready || !g_sc.TransmitClientEvent) return 0;
    {
        HRESULT hr = g_sc.TransmitClientEvent(
            g_sc_handle, SC_OBJECT_ID_USER, CEV_TOGGLE_LABELS, 0,
            PRIO_HIGHEST, EVT_FLAG_GROUP_IS_PRIORITY);
        if (FAILED(hr)) {
            xc_logf("control: TOGGLE_AIRCRAFT_LABELS failed hr=0x%08lx",
                    (unsigned long)hr);
            return 0;
        }
    }
    xc_logf("control: 已翻转 MSFS 内置机模标签（TOGGLE_AIRCRAFT_LABELS）");
    return 1;
}

void control_drain(void) {
    Command batch[CMD_QUEUE_MAX];
    int n = 0, i;

    if (!g_q_lock_init) return;

    /* 先把队列整体取空（短暂持锁），再在锁外执行 ——
     * 执行要调 SimConnect，可能耗时，不能占着锁让网络线程阻塞。 */
    LOCK_ENTER(&g_q_lock);
    while (g_q_count > 0 && n < CMD_QUEUE_MAX) {
        int tail = (g_q_head - g_q_count + CMD_QUEUE_MAX) % CMD_QUEUE_MAX;
        batch[n++] = g_queue[tail];
        g_q_count--;
    }
    LOCK_LEAVE(&g_q_lock);

    for (i = 0; i < n; i++) exec_command(&batch[i]);
}

/* ---------------- 协议解析（网络线程） ---------------- */

static int jnum2(const Json* obj, const char* key, double* out) {
    const Json* t = json_get(obj, key);
    if (!t) return 0;
    if (t->type == JSON_NUM) { *out = t->num; return 1; }
    if (t->type == JSON_BOOL) { *out = t->bval ? 1.0 : 0.0; return 1; }
    return 0;
}

static int jstr2(const Json* obj, const char* key, char* dst, int cap) {
    const Json* t = json_get(obj, key);
    if (dst && cap > 0) dst[0] = '\0';
    if (!dst || cap < 1) return 0;
    if (t && t->type == JSON_STR) {
        snprintf(dst, (size_t)cap, "%s", t->sval);
        return 1;
    }
    return 0;
}

/* Send:RADIO
 *   {"type":"Send:RADIO","com":1,"active":true,"freq":122800}
 *   {"type":"Send:RADIO","com":1,"swap":true}
 *   {"type":"Send:RADIO","com":1,"flip":true}        同上（XConnect 别名）
 *   {"type":"Send:RADIO","com":1,"tx":true}          切发话通道（XConnect 字段互通）
 *   {"type":"Send:RADIO","com":1,"xmit":true}        选定 COM2 为发射台
 *   {"type":"Send:RADIO","com":1,"monitor":true}     收听开关 toggle（XConnect 字段互通）
 *   {"type":"Send:RADIO","recv_all":true}            全收开关
 *   {"type":"Send:RADIO","com":1,"power":0}          COM 电源（MSFS 无对应物，忽略）
 * freq 单位 MHz*1000（与 flight_data 的 com*_freq 一致，便于客户端原样回填）。 */
static void handle_radio(const Json* root) {
    Command c;
    double v;
    int com = 1;
    int swap = 0;
    int active = 1;

    memset(&c, 0, sizeof(c));

    /* 全收不涉及频率，先单独处理（不需要 com 字段） */
    if (jnum2(root, "recv_all", &v)) {
        c.kind = CMD_COM_RECV_ALL;
        c.on = v != 0.0;
        control_post(&c);
        return;
    }

    if (jnum2(root, "com", &v)) com = (int)v;
    if (com < 1 || com > 3) {
        xc_logf("control: RADIO bad com=%d", com);
        return;
    }

    /* 切发话通道（XConnect 的 "tx" 是 toggle 语义；MSFS 的
     * COMn_TRANSMIT_SELECT 是"选定"，重复发送无副作用，语义兼容） */
    if (jnum2(root, "tx", &v) && v != 0.0) {
        c.kind = CMD_COM_XMIT;
        c.com_index = com;
        control_post(&c);
        return;
    }

    /* 选定发射台（XVoice 读 PILOT TRANSMITTER TYPE，写侧对应本字段） */
    if (jnum2(root, "xmit", &v)) {
        if (v != 0.0) {
            c.kind = CMD_COM_XMIT;
            c.com_index = com;
            control_post(&c);
        }
        return;   /* xmit:false 无意义（必须有一台在发），忽略 */
    }

    /* 收听开关 toggle（XConnect "monitor"，XP 命令就是 toggle 语义） */
    if (jnum2(root, "monitor", &v) && v != 0.0) {
        c.kind = CMD_COM_RECV;
        c.com_index = com;
        control_post(&c);
        return;
    }

    /* COM 电源：MSFS 没有单台 COM 电源 SimVar/事件（XP 的
     * sim/radios/power_comN_on/off 无对应物）。显式记日志，别静默吞掉。 */
    if (json_get(root, "power")) {
        xc_logf("control: RADIO power 字段 MSFS 不支持（忽略）");
        return;
    }

    jnum2(root, "active", &v);
    {
        /* active 可能是 bool 也可能是 0/1；缺省按主频处理 */
        const Json* t = json_get(root, "active");
        if (t) active = (t->type == JSON_BOOL) ? (t->bval ? 1 : 0)
                                               : (int)(t->num != 0.0);
    }
    if (jnum2(root, "swap", &v) && v != 0.0) swap = 1;
    if (jnum2(root, "flip", &v) && v != 0.0) swap = 1;   /* XConnect 别名 */

    if (swap) {
        c.kind = CMD_COM_SWAP;
        c.com_index = com;
        control_post(&c);
        return;
    }

    if (!jnum2(root, "freq", &v) || v <= 0.0) {
        xc_logf("control: RADIO missing freq");
        return;
    }
    {
        long hz = control_mhz1000_to_hz((int)(v + 0.5));
        if (hz <= 0) {
            xc_logf("control: RADIO freq out of band: %.0f", v);
            return;
        }
        c.kind = CMD_COM_SET;
        c.com_index = com;
        c.active = active;
        c.hz = hz;
        control_post(&c);
    }
}

/* Send:XPDR
 *   {"type":"Send:XPDR","code":"1234"}         → 设置码
 *   {"type":"Send:XPDR","code":1234}           → 同上（数字形式）
 *   {"type":"Send:XPDR","state":3}             → 设置模式（0=Off..5=Gnd）
 *   {"type":"Send:XPDR","ident":true}          → IDENT 脉冲 */
static void handle_xpdr(const Json* root) {
    Command c;
    double v;
    char buf[16];

    memset(&c, 0, sizeof(c));

    if (jnum2(root, "code", &v)) {
        int code = (int)(v + 0.5);
        /* 数字形式：必须 0..7777 且逐位 <=7（八进制语义） */
        if (code >= 0 && code <= 7777) {
            char tmp[8];
            snprintf(tmp, sizeof(tmp), "%04d", code);
            code = control_parse_squawk(tmp);
        } else {
            code = -1;
        }
        if (code < 0) {
            xc_logf("control: XPDR bad code=%d", (int)(v + 0.5));
            return;
        }
        c.kind = CMD_XPNDR_CODE;
        c.code = code;
        control_post(&c);
        return;
    }

    if (jstr2(root, "code", buf, sizeof(buf))) {
        int code = control_parse_squawk(buf);
        if (code < 0) {
            xc_logf("control: XPDR bad code string \"%s\"", buf);
            return;
        }
        c.kind = CMD_XPNDR_CODE;
        c.code = code;
        control_post(&c);
        return;
    }

    if (jnum2(root, "state", &v)) {
        int st = (int)(v + 0.5);
        /* MSFS TRANSPONDER STATE 只有 0..5，而 X-Plane 侧还有 6=TA only、
         * 7=TA/RA。归一化到 MSFS 能表达的档位：
         *   - 6/7 是 TCAS 模式，应答机本身仍正常发射 → 用 Alt(3) 承载，
         *     不能夹成 Ground(5)（那样管制看不到你）；
         *   - 其余越界沿用旧行为夹到 Ground(5)（control_test 已锁定）。 */
        if (st < 0) st = 0;
        if (st > 7) st = 5;
        else if (st > 5) st = 3;
        c.kind = CMD_XPNDR_STATE;
        c.state = st;
        control_post(&c);
        return;
    }

    if (jnum2(root, "ident", &v)) {
        c.kind = CMD_XPNDR_IDENT;
        c.on = v != 0.0;
        control_post(&c);
        return;
    }

    xc_logf("control: XPDR no actionable field");
}

/* Send:INPUTEV —— 按名字触发当前机模的 InputEvent。
 *   {"type":"Send:INPUTEV","name":"XPDR_CODE_SET","value":1234}
 * name 来自 aircraft_caps 报文里报给客户端的真实事件名，
 * 这样客户端不需要猜命名（每个机模都不一样）。 */
static void handle_inputev(const Json* root) {
    char name[INPUTEV_NAME];
    double v = 1.0;
    DWORD hash = 0, type = 0;
    Command c;

    if (!jstr2(root, "name", name, sizeof(name)) || !name[0]) {
        xc_logf("control: INPUTEV 缺 name");
        return;
    }
    jnum2(root, "value", &v);

    if (!inputev_lookup(name, &hash, &type)) {
        xc_logf("control: INPUTEV \"%s\" 不在当前机型事件表里（共 %d 个）",
                name, inputev_count());
        return;
    }

    /* 入队一条"按 hash 触发"的命令。hash 是完整的 32 位值
     *（aircraft_caps 报文里原样报给客户端，客户端再原样传回来），
     * 不能截断——截断会导致碰撞到别的事件。 */
    memset(&c, 0, sizeof(c));
    c.kind = CMD_INPUT_EVENT;
    c.hash = hash;
    c.state = (int)(v + 0.5);
    control_post(&c);
}

/* Send:RPN —— 直接执行一段 RPN（需要 WASM 桥就绪）。
 * RPN 字符串长度超过 Command 结构的定长字段，因此不走命令队列，
 * 直接交给 wasmbridge（它内部有自己的线程安全队列）。 */
static void handle_rpn(const Json* root) {
    char rpn[160];

    if (!jstr2(root, "rpn", rpn, sizeof(rpn)) || !rpn[0]) {
        xc_logf("control: RPN 缺 rpn");
        return;
    }
    if (wasmbridge_exec_rpn(rpn) == 0) return;
    xc_logf("control: RPN 未执行（WASM 桥不可用）: %s", rpn);
}

int control_handle_message(const Json* root, void* unused) {
    const Json* jt;
    (void)unused;
    if (!root) return 0;

    jt = json_get(root, "type");
    if (!jt || jt->type != JSON_STR) return 0;

    if (strcmp(jt->sval, "Send:RADIO") == 0) {
        handle_radio(root);
        return 1;
    }
    if (strcmp(jt->sval, "Send:XPDR") == 0) {
        handle_xpdr(root);
        return 1;
    }
    if (strcmp(jt->sval, "Send:INPUTEV") == 0) {
        handle_inputev(root);
        return 1;
    }
    if (strcmp(jt->sval, "Send:RPN") == 0) {
        handle_rpn(root);
        return 1;
    }
    return 0;
}