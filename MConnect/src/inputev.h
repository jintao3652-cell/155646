/* MConnect — Input Events：按名字触发机模自定义输入事件。
 *
 * 为什么需要它
 * ------------
 * 标准 SimConnect 事件（`COM_RADIO_SET_HZ`、`XPNDR_SET`…）只能驱动**默认机**
 * 和一部分兼容机模。Fenix / PMDG / Inibuilds 这类深度机模把座舱逻辑做成了
 * **自定义 InputEvent**，标准事件对它们完全无效（写入静默失败）。
 *
 * Input Events 是纯 SimConnect 就能用的正解，无需用户安装额外 WASM 包：
 *   1. `SimConnect_EnumerateInputEvents` → 拿到当前机模全部输入事件
 *      （名字 + hash + 类型 + 关联节点），分页返回
 *   2. `SimConnect_EnumerateInputEventParams(hash)` → 该事件的参数签名
 *   3. `SimConnect_SetInputEvent(hash, size, value)` → 触发
 *
 * 与 XVoice 用的 WASimCommander 对比
 * ----------------------------------
 * WASimCommander 走模拟器内 WASM 模块，能力更强（LVar 读写、RPN 执行、
 * CalculatorCode），但要求用户先装 WASM 包。本模块只做 Input Events ——
 * 覆盖"按一下按钮 / 拨一个开关"，这正是应答机与频率输入需要的能力，
 * 且零安装成本。若将来需要 LVar/RPN，再作为可选增强接 WASim。
 *
 * 参数打包（容易踩的坑）
 * ----------------------
 * `SetInputEvent` 只有一个 PVOID + size，多个参数要**自己按 pack(1) 依次拼接**。
 * 例如 EnumerateInputEventParams 返回 ";char;FLOAT64" 时，缓冲区必须是：
 *     struct { char c; double d; } __attribute__((packed));
 * 实测多数按钮类事件是 1~2 个 double（Push=1 / Release=0）。这里统一用
 * "一个 double"（最常见），并在参数签名不同时记日志说明。
 */
#ifndef MCONNECT_INPUTEV_H
#define MCONNECT_INPUTEV_H

#include "mconnect.h"

#define INPUTEV_MAX 4096     /* 单机模输入事件上限（实测 A320 约 600~1500） */
#define INPUTEV_NAME 64

typedef struct {
    char  name[INPUTEV_NAME];
    DWORD hash;
    DWORD type;        /* SIMCONNECT_DATATYPE */
} InputEvent;

/* 输入事件的语义分类。
 *
 * 为什么按语义分类而不是硬编码名字：每个机模给按钮起的名字都不一样
 * （Fenix 叫 `XPDR_STBY_Push`、PMDG 叫 `EVT_XPNDR_MODE`、默认机叫
 * `TRANSPONDER_MODE_SELECT`）。写死一份候选表必然漏机型。
 * 按关键字归类后，客户端可以"问当前机模支持哪些能力"，而不是猜名字。 */
typedef enum {
    IE_CAT_NONE = 0,
    IE_CAT_COM_FREQ,      /* COM 频率相关（旋钮/交换/设值） */
    IE_CAT_COM_SELECT,    /* 发射台/收听选择 */
    IE_CAT_XPNDR_CODE,    /* 应答机码 */
    IE_CAT_XPNDR_MODE,    /* 应答机模式 */
    IE_CAT_XPNDR_IDENT,   /* 应答机 IDENT */
    IE_CAT_AUTOPILOT,     /* 自动驾驶（将来扩展用） */
    IE_CAT_COUNT
} InputEventCategory;

/* 按语义归类（关键字匹配，忽略大小写）。纯函数，便于测试。 */
InputEventCategory inputev_classify(const char* name);

/* 某分类下的事件数量。 */
int  inputev_count_category(InputEventCategory cat);

/* 取某分类下第 idx 个事件（0-based）。返回 1 成功。
 * 用于把"当前机模能做什么"报给客户端。 */
int  inputev_get_category(InputEventCategory cat, int idx,
                          char* name, int cap, DWORD* hash);

const char* inputev_category_name(InputEventCategory cat);

/* 请求枚举（sc_open 之后调用；模拟器异步分页返回，由派发回调填充）。
 * 返回 0 成功发出请求。不支持时返回 -1（调用方应退化为标准事件）。 */
int  inputev_request_enum(void);

/* 处理 SIMCONNECT_RECV_ENUMERATE_INPUT_EVENTS（派发回调转交）。 */
void inputev_on_enumerate(const void* pkt, DWORD cbData);

/* 按名字查 hash。找到返回 1 并写 *hash / *type。不区分大小写。 */
int  inputev_lookup(const char* name, DWORD* hash, DWORD* type);

/* 按名字触发（value 为 double，最常见形态）。返回 1 成功。
 * 这是**主线程**调用（SimConnect 非线程安全）。 */
int  inputev_set_by_name(const char* name, double value);

/* 按 hash 触发。 */
int  inputev_set_by_hash(DWORD hash, double value);

/* 已枚举到的事件数（0 = 还没收到 / 不支持）。 */
int  inputev_count(void);

/* 写一条诊断：把前 N 个事件名写日志（排查"我的按钮叫什么名字"）。 */
void inputev_dump(int max_items);

#endif /* MCONNECT_INPUTEV_H */