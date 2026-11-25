/* MConnect — PMDG SDK 桥：通过官方 ClientData 读写 PMDG 737/777 的状态与事件。
 *
 * 为什么必须单独做一条通道
 * ----------------------
 * PMDG 是唯一"既不更新标准 SimVar、也没有可读 LVar"的机模：
 *   - 它的应答机模式存在自己的内部结构里，标准 TRANSPONDER STATE 恒为过期值
 *   - HubHop 里能写（80007/80008 >K:ROTOR_BRAKE）但**读不到**
 *   - 没有暴露任何 PMDG_* 的 LVar（MobiFlight 也是靠 SDK 才能读）
 * 所以只有走官方 SDK 的二进制 ClientData 才能读到真实状态。
 *
 * 两个机型族的协议**完全不同**（别想当然以为只改个通道名）
 * ------------------------------------------------------
 *                    737 NG3 (NG3)              777X (777)
 *   数据通道名        "PMDG_NG3_Data"            "PMDG_777X_Data"
 *   数据 ID           0x4E473331                 0x504D4447
 *   数据 DEF          0x4E473332                 0x504D4448
 *   控制通道名        "PMDG_NG3_Control"         "PMDG_777X_Control"
 *   控制 ID           0x4E473333                 0x504D4449
 *   控制 DEF          0x4E473334                 0x504D444A
 *   sizeof(Data)      916                        684
 *   模式字段偏移      612                          449
 *   EVT_TCAS_MODE     69632+800                  69632+749
 *   EVT_TCAS_IDENT    69632+806                  69632+746
 *
 * 应答机模式枚举也**不同**：
 *   737: 0=STBY 1=ALT RPTG OFF 2=ON      3=ALT     4=TA/RA
 *   777: 0=STBY 1=ALT RPTG OFF 2=XPNDR   3=TA ONLY 4=TA/RA
 *       （2 号档语义名不同，3 号档 737 是 ALT、777 是 TA ONLY）
 *
 * 来源（本机官方头文件，逐项核实）
 *   737: L:\MSFS\Community\pmdg-aircraft-737\Documentation\SDK\PMDG_NG3_SDK.h
 *   777: L:\MSFS\Community\pmdg-aircraft-77l\Documentation\SDK\PMDG_777X_SDK.h
 *
 * 前置条件（必须由用户开启，否则通道不出现）
 * -----------------------------------------
 *   737: <MSFS>\Packages\pmdg-aircraft-737\work\737NG3_Options.ini
 *   777: <MSFS>\Packages\pmdg-aircraft-777\work\777_Options.ini
 *   [SDK]
 *   EnableDataBroadcast=1
 * 未开启时 MapClientDataNameToID 成功但收不到任何广播 —— 表现为"通道建立
 * 成功但状态一直是 0"，因此需要超时判定并给出明确提示。
 */
#ifndef MCONNECT_PMDGBRIDGE_H
#define MCONNECT_PMDGBRIDGE_H

#include "mconnect.h"

/* PMDG 应答机模式（737 SDK 原生枚举；777 的 2/3 号档语义不同，见下） */
#define PMDG_XPDR_STBY      0   /* 737: STBY         777: STBY        */
#define PMDG_XPDR_ALT_OFF   1   /* 737: ALT RPTG OFF 777: ALT RPTG OFF */
#define PMDG_XPDR_ON        2   /* 737: ON           777: XPNDR       */
#define PMDG_XPDR_ALT       3   /* 737: ALT          777: TA ONLY     */
#define PMDG_XPDR_TARA      4   /* 737: TA/RA        777: TA/RA       */

/* 机型族：决定用哪套通道/偏移/事件号 */
typedef enum {
    PMDG_TYPE_NONE = 0,
    PMDG_TYPE_737,      /* 737 NG3（含 736/737/738/739） */
    PMDG_TYPE_777       /* 777X（含 772/77L/77W/77F） */
} PmdgType;

typedef enum {
    PMDG_STATE_OFF = 0,      /* 未初始化（非 PMDG 机型 / API 不可用） */
    PMDG_STATE_WAIT,         /* 已请求，等第一条广播 */
    PMDG_STATE_READY,        /* 收到数据，可读可写 */
    PMDG_STATE_FAILED        /* 超时：多半是没开 EnableDataBroadcast */
} PmdgState;

/* 应答机状态（从 SDK 数据里解出来的） */
typedef struct {
    int mode;            /* PMDG_XPDR_*（SDK 原生） */
    int selector;        /* 737: 1或2   777: 0=L 1=R */
    int annun_fail;      /* FAIL 灯（777 无此字段，恒 0） */
    int ident_pushed;    /* IDENT 按钮按下（仅 777 有此字段） */
    int valid;           /* 是否收到过数据 */
} PmdgXpdr;

/* 初始化：按机型族选协议、映射通道并订阅。返回 0 表示已发出请求。
 * type 由调用方根据档案判定（PMDG_TYPE_737 / PMDG_TYPE_777）。 */
int  pmdgbridge_init(PmdgType type);

/* 主线程每帧调用：状态机推进（含超时判定）。 */
void pmdgbridge_poll(double now);

/* 关闭 */
void pmdgbridge_shutdown(void);

/* 取应答机状态。返回 1 表示数据有效。 */
int  pmdgbridge_get_xpdr(PmdgXpdr* out);

/* 发一个 SDK 事件（Event + Parameter）。返回 0 成功。
 *
 * 注意 EVT_TCAS_MODE 这类旋钮事件的 Parameter 语义是"转动量"，
 * 不是"目标值" —— PMDG 面板本质是旋钮，只能一格一格转。
 * 因此设具体模式需要：先读当前 mode，再按差值转对应格数。 */
int  pmdgbridge_send_event(unsigned event, unsigned param);

/* 便捷封装：把应答机切到指定 PMDG 模式（内部按差值转旋钮）。
 * 返回 1 成功，0 未就绪，-1 参数非法。 */
int  pmdgbridge_set_xpdr_mode(int pmdg_mode);

/* 便捷封装：IDENT（瞬时脉冲）。 */
int  pmdgbridge_set_xpdr_ident(void);

/* 状态与诊断 */
PmdgState pmdgbridge_state(void);
PmdgType  pmdgbridge_type(void);
const char* pmdgbridge_state_name(PmdgState s);
const char* pmdgbridge_type_name(PmdgType t);
void pmdgbridge_dump(void);

/* 处理 SIMCONNECT_RECV_CLIENT_DATA（派发回调转交）。 */
void pmdgbridge_on_client_data(const void* pkt, DWORD cbData);

/* SDK 结构期望大小与关键偏移，供测试校验
 * （与官方头文件 sizeof/offsetof 实测一致） */
#define PMDG737_DATA_SIZE        916
#define PMDG737_OFF_XPDR_MODE    612
#define PMDG737_OFF_XPDR_SEL     610
#define PMDG737_OFF_XPDR_FAIL    613
#define PMDG737_EVT_TCAS_MODE    70432   /* 69632 + 800 */
#define PMDG737_EVT_TCAS_IDENT   70438   /* 69632 + 806 */

#define PMDG777_DATA_SIZE        684
#define PMDG777_OFF_XPDR_MODE    449
#define PMDG777_OFF_XPDR_SEL     447
#define PMDG777_OFF_XPDR_IDENT   450
#define PMDG777_EVT_TCAS_MODE    70381   /* 69632 + 749 */
#define PMDG777_EVT_TCAS_IDENT   70378   /* 69632 + 746 */

#endif /* MCONNECT_PMDGBRIDGE_H */