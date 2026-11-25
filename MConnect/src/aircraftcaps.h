/* MConnect — 机型能力探测：告诉客户端"你当前这架飞机能控制什么"。
 *
 * 要解决的问题
 * ------------
 * 之前 control.c 用的是**硬编码候选名**（"COM1_SWAP"、"XPDR_SET"…），
 * 但每个机模给按钮起的名字都不一样：
 *     Fenix A320   XPDR_STBY_Push / COM1_SWAP_Push
 *     PMDG 737     EVT_XPNDR_MODE / EVT_COM1_TRANSFER
 *     Asobo 默认机  TRANSPONDER_MODE_SELECT
 * 写死候选表必然漏机型，用户看到"写入失败"却不知道为什么。
 *
 * 本模块的做法
 * ------------
 * 从**当前机模实际枚举到的 InputEvent 表**里，按语义分类统计，然后把
 * "这架飞机对每个功能支持哪些具体事件"报给客户端。客户端据此：
 *   - 只显示这架飞机真能操作的按钮（不给假选项）
 *   - 让用户从真实存在的名字里挑（而不是猜）
 *   - 机型切换后自动刷新（收到新报文）
 *
 * 三层能力汇总（对应 control.c 的三级回退）
 * ----------------------------------------
 *   standard : 标准 SimEvent 是否可用（几乎所有机模都响应一部分）
 *   inputev  : 按分类统计到的自定义事件数（0 = 机模没暴露，只能用标准事件）
 *   wasm     : WASM 桥是否就绪（能执行任意 RPN，最强）
 *
 * 协议报文（MConnect → 客户端）
 * ---------------------------
 * {"type":"aircraft_caps","icao":"A20N","title":"Fenix A320",
 *  "standard":{"com":true,"xpdr":true},
 *  "inputev":{"com_freq":3,"xpndr_code":4,"xpndr_mode":2,"xpndr_ident":1},
 *  "wasm":{"state":"ready","version":"0.6.0"},
 *  "events":[{"cat":"xpndr_code","name":"XPDR_CODE_SET","hash":123456}, ...]}
 *
 * 变更检测：机型或能力集变化时才重发（见 aircraft_caps_tick）。
 */
#ifndef MCONNECT_AIRCAPS_H
#define MCONNECT_AIRCAPS_H

#include "mconnect.h"

/* 构建 capabilities 报文的 JSON（写入 out）。返回长度，0 = 无可用信息。 */
int  aircaps_build_json(char* out, int size);

/* 主线程每帧调用：检测机型/能力变化，变了就置脏。
 * 返回 1 表示"需要立即重发"，调用方据此推送一次。 */
int  aircaps_tick(void);

/* 机型切换时调用：清空旧能力，等待新机模的 InputEvent 重新枚举。 */
void aircaps_on_aircraft_change(const char* title);

/* 诊断输出。 */
void aircaps_dump(void);

#endif /* MCONNECT_AIRCAPS_H */