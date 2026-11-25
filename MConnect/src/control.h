/* MConnect — 反向控制：客户端 → 模拟器（应答机 / 无线电频率）。
 *
 * 与 XConnect 的差异：X-Plane 的应答机/频率可以直写 dataref（`sim/cockpit2/
 * radios/...`），MSFS 完全不行——`COM ACTIVE FREQUENCY`、`TRANSPONDER CODE`
 * 都是**只读 SimVar**，只能通过 **SimConnect 事件**（Event ID）改变：
 *
 *   COM1 主频  COM_RADIO_SET_HZ       参数 = Hz
 *   COM1 备频  COM_STBY_RADIO_SET_HZ  参数 = Hz
 *   COM2/3     同上，前缀 COM2_ / COM3_
 *   应答机码   XPNDR_SET              参数 = BCD 编码
 *   应答机模式 TRANSPONDER STATE       可写 SimVar（Enum 0..5），不是事件
 *
 * 两个坑：
 *   1. `_SET`（不带 _HZ）收的是 BCD16，`_HZ` 收十进制 Hz。BCD 要自己按位编码，
 *      且 8.33kHz 间隔的第三个小数位在 BCD16 里表达不全 —— 统一用 _HZ 更省事，
 *      但 `COM_RADIO_SET_HZ` 在极少数老机型上不响应，故失败时退 BCD 版。
 *   2. 事件必须在有 SimConnect 会话的线程发（SimConnect 非线程安全），
 *      而命令来自网络线程 → 必须过队列，不能直接在网络线程 TransmitClientEvent。
 *
 * 事件注册：SimConnect 要求先用 MapClientEventToSimEvent 把「客户端事件 ID」
 * 映射到「事件名」，再用 TransmitClientEvent 触发。映射只需一次（sc_open 后）。
 */
#ifndef MCONNECT_CONTROL_H
#define MCONNECT_CONTROL_H

#include "mconnect.h"

/* 客户端事件 ID（客户端自定义，与 DEF_/REQ_ 错开） */
#define CEV_COM1_SET        201
#define CEV_COM1_STBY_SET   202
#define CEV_COM2_SET        203
#define CEV_COM2_STBY_SET   204
#define CEV_COM3_SET        205
#define CEV_COM3_STBY_SET   206
#define CEV_XPNDR_SET       207
#define CEV_XPNDR_IDENT     208
#define CEV_COM1_SWAP       209
#define CEV_COM2_SWAP       210
#define CEV_COM3_SWAP       211
#define CEV_COM1_XMIT       212   /* COM1_TRANSMIT_SELECT */
#define CEV_COM2_XMIT       213   /* COM2_TRANSMIT_SELECT */
#define CEV_COM3_XMIT       214   /* COM3_TRANSMIT_SELECT */
#define CEV_COM_RECV_ALL    215   /* COM_RECEIVE_ALL_SET */

/* 命令种类 */
typedef enum {
    CMD_NONE = 0,
    CMD_COM_SET,        /* 设置 COM 主频/备频（active 指定主备） */
    CMD_COM_SWAP,       /* 主备频率互换 */
    CMD_COM_XMIT,       /* 选定发射台（com_index = 1..3） */
    CMD_COM_RECV_ALL,   /* 设置"全收"开关 */
    CMD_XPNDR_CODE,     /* 设置应答机 4 位码 */
    CMD_XPNDR_STATE,    /* 设置应答机模式（OFF/STBY/ON/ALT…） */
    CMD_XPNDR_IDENT,    /* 应答机 IDENT 脉冲 */
    CMD_INPUT_EVENT     /* 按 hash 触发当前机模的 InputEvent */
} CmdKind;

typedef struct {
    CmdKind kind;
    int     com_index;    /* 1..3（CMD_COM_*） */
    int     active;       /* 1 = 主频，0 = 备频 */
    long    hz;           /* 频率（Hz） */
    int     code;         /* 应答机码（十进制 0..7777，逐位八进制） */
    int     state;        /* 应答机模式枚举；INPUT_EVENT 时为触发值 */
    int     on;           /* IDENT 开/关；RECV_ALL 时为开关值 */
    DWORD   hash;         /* CMD_INPUT_EVENT 用：完整 32 位 hash */
} Command;

/* 注册事件映射（sc_open 之后、进入主循环之前调用）。返回 0 成功。 */
int  control_init(void);

/* 网络线程调用：把命令放进队列（非阻塞、线程安全）。返回 0 成功入队。
 * 队列满则丢最旧的一条（控制命令是"最新有效"，丢旧的比丢新的合理）。 */
int  control_post(const Command* cmd);

/* 主线程每帧调用：取出队列里的全部命令并执行（在这里才碰 SimConnect）。 */
void control_drain(void);

/* 机型档案：换机模时调用，重新解析该机型用哪套写法。
 * title 是容器标题，icao 是反查得到的机型代码。
 * 解析不到（未知机型）时置空，控制逻辑退回标准事件 + InputEvent 搜索。 */
void control_set_aircraft(const char* title, const char* icao);

/* 当前生效的档案 ID（诊断用），无档案返回 "none"。 */
const char* control_profile_id(void);

/* 当前档案支持的能力位（CAP_* 来自 aircraftdb.h），无档案返回 0。 */
unsigned control_profile_caps(void);

/* 解析客户端 JSON 行里的控制类报文（Send:RADIO / Send:XPDR）。
 * 返回 1 = 已识别并处理，0 = 不是控制报文（交给别的处理器）。
 * 在**网络线程**调用，只做入队。 */
struct Json;
int  control_handle_message(const struct Json* root, void* unused);

/* 把 MHz*1000（协议单位，如 122800）转 Hz（122800000）。非法值返回 0。 */
long control_mhz1000_to_hz(int mhz1000);

/* 把 4 位十进制应答机串（"1234"）转内部码；非法返回 -1。
 * 允许 0..7 的每一位（八进制）。 */
int  control_parse_squawk(const char* s);

/* 协议规范应答机模式 → MSFS SimVar 枚举。
 * 协议沿用 X-Plane 顺序（0=Off 1=Stby 2=On 3=Alt 4=Test 5=Ground），
 * MSFS 是 0=Off 1=Stby 2=Test 3=On 4=Alt 5=Ground，两者 2/3/4 位置不同，
 * 必须重映射（否则"设为 ON"会把应答机打到 Test，管制看不到你）。 */
int  control_canonical_to_msfs_state(int st);

#endif /* MCONNECT_CONTROL_H */