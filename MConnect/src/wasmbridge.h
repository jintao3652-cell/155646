/* MConnect — WASM 桥：通过 ClientData 与模拟器内 WASM 模块通信，执行任意 RPN。
 *
 * 这是第三条（也是能力最强的）写入/读取通道。三条通道的分工：
 *
 *   ① 标准 SimEvent（control.c）      只对默认机有效
 *   ② Input Events（inputev.c）       能按名字触发机模预定义按钮，纯 SimConnect
 *   ③ WASM 桥（本文件）               执行任意 RPN：LVar 读写、多参数 K 事件、
 *                                     B 事件、复杂计算。需要用户装 WASM 模块
 *
 * 为什么 ③ 不可替代
 * -----------------
 * Fenix / PMDG 的很多操作没有暴露成标准事件或 InputEvent，
 * 只能靠 RPN 直接改 LVar（`5 (>L:MyVar)`）或触发 B 事件（`1 (>B:FOO_Push)`）。
 * 这正是 MobiFlight / XVoice（WASimCommander）都在做的事。
 *
 * 协议（MobiFlight WASM Module，实测本地 modules/MobiFlightWasmModule.wasm 支持）
 * ------------------------------------------------------------------------------
 * 三条共享内存通道，名字固定：
 *   "MobiFlight.Command"    客户端 → WASM（写字符串命令）
 *   "MobiFlight.Response"   WASM → 客户端（写字符串响应）
 *   "MobiFlight.LVars"      WASM → 客户端（float 数组，按 Add 顺序排列）
 *
 * 命令是**纯文本字符串**（不是 JSON）：
 *   MF.Ping                          → MF.Pong
 *   MF.Version.Get                   → MF.Version.0.6.0
 *   MF.SimVars.Set.5 (>L:MyVar)      → 执行 RPN（核心能力）
 *   MF.SimVars.Add.(A:GROUND ALTITUDE,Meters)  → 注册一个变量，返回偏移
 *   MF.SimVars.Clear                 清空已注册变量
 *   MF.LVars.List                    → 列出全部 LVar 名
 *   MF.Clients.Add.<名字>            → 为自己注册独立通道（推荐！）
 *
 * 为什么要 `MF.Clients.Add`
 * -------------------------
 * 默认三条通道是给 MobiFlight Connector 自己用的。如果 MConnect 也直接写
 * "MobiFlight.Command"，就会和用户打开的 MobiFlight Connector **抢通道**
 * （两边的 SimVars.Add 偏移互相踩，读到的值全是错的）。
 * 正确做法是先 `MF.Clients.Add.mconnect`，WASM 会创建
 * "mconnect.LVars" / "mconnect.Command" / "mconnect.Response" 三条独立通道，
 * 收到 `MF.Clients.Add.mconnect.Finished` 后再用它们。
 *
 * 首个命令可能丢失：MobiFlight 文档明确提到"新客户端启动后第一条命令
 * 有时被忽略"，因此注册后先发一条 MF.Ping 探活
 * （datadefs 里对应 --dump-wasm 与自动重试）。
 */
#ifndef MCONNECT_WASMBRIDGE_H
#define MCONNECT_WASMBRIDGE_H

#include "mconnect.h"

#define WASM_CLIENT_NAME   "mconnect"
#define WASM_CMD_MAX       64     /* 命令队列长度 */
#define WASM_RESP_MAX      64     /* 响应 ring 长度 */

/* 桥接状态 */
typedef enum {
    WASM_STATE_OFF = 0,      /* 未初始化（未找到 WASM 模块或 API 不可用） */
    WASM_STATE_REGISTERING,  /* 已发 MF.Clients.Add，等 ...Finished */
    WASM_STATE_READY,        /* 独立通道就绪，可收发 */
    WASM_STATE_FAILED        /* 注册超时/失败（WASM 模块没装？） */
} WasmState;

/* 初始化：映射默认通道 + 发注册命令。返回 0 表示已发出（不代表已就绪）。 */
int  wasmbridge_init(void);

/* 撤销注册并释放通道（退出时调用）。 */
void wasmbridge_shutdown(void);

/* 主线程每帧调用：处理状态机、发送排队命令、解析响应。 */
void wasmbridge_poll(double now);

/* 执行一段 RPN（入队）。返回 0 成功。
 *
 * 例：
 *   wasmbridge_exec_rpn("5 (>L:MY_VAR)")        写 LVar
 *   wasmbridge_exec_rpn("1 (>B:MY_BUTTON_Push)") 触发 B 事件
 *   wasmbridge_exec_rpn("(A:LIGHT LANDING,Bool) (>L:X)")  读 A 存 L
 *
 * 命令是**最新有效**语义（队列满丢最旧），与 control.c 一致。
 * 注意：RPN 里的逗号会被 MobiFlight 解析成命令分隔，参数里要写全。 */
int  wasmbridge_exec_rpn(const char* rpn);

/* 读一个变量（通过 MF.SimVars.Add 注册，值从 LVars 通道周期性返回）。
 * 返回 0 表示已提交请求；值就绪后用 wasmbridge_get_value 取。
 * index 是本客户端内自增的槽位（0..WASM_VAR_MAX-1）。 */
int  wasmbridge_watch(const char* calculator, int slot);

/* 取某槽位最近的读数值。返回 1 表示有值。 */
int  wasmbridge_get_value(int slot, double* out);

/* 当前状态与诊断。 */
WasmState wasmbridge_state(void);
const char* wasmbridge_state_name(WasmState s);
int  wasmbridge_version(char* out, int cap);
void wasmbridge_dump(void);

/* 处理 SIMCONNECT_RECV_CLIENT_DATA（派发回调转交）。 */
void wasmbridge_on_client_data(const void* pkt, DWORD cbData);

#endif /* MCONNECT_WASMBRIDGE_H */