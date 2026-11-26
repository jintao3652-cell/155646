/* MConnect — 机型档案库：把各机模族的**实测真实写法**固化下来。
 *
 * 为什么不能靠通用规则
 * -------------------
 * 原来 control.c 按"语义分类 + 关键字猜名字"找事件，但实际验证（从
 * MobiFlight HubHop 27382 条社区预置 + 厂商官方文档里提取）发现各机模族
 * 用的机制完全不同，根本不存在通用写法。
 *
 * 数据来源（每条都核对过，不是推测）
 * ---------------------------------
 * ① MobiFlight HubHop 全量预置（本地缓存）
 *    %LOCALAPPDATA%\MobiFlight\MobiFlight Connector\Presets\msfs2020_hubhop_presets.json
 *    27382 条，字段 vendor/aircraft/system/label/code(真实 RPN)
 * ② A220 官方变量表（iniBuilds / SynapticSim）
 *    https://docs.synapticsim.com/pilots/simvars   LVar
 *    https://docs.synapticsim.com/pilots/inputs    InputEvent
 *
 * 已覆盖的机模族（实测机制一览）
 * ----------------------------
 *  FBW A32NX/A380X   模式 (L:A32NX_TRANSPONDER_MODE) 0=STBY 1=AUTO 2=ON
 *                    频率 (A:COM ACTIVE FREQUENCY:n) **可写**
 *  Fenix A320        模式 (L:S_XPDR_MODE) 0=STBY 1=TA 2=TARA
 *                    另有 (L:S_XPDR_OPERATION) 0=STBY 1=AUTO 2=ON
 *                    码   逐位按钮 (L:S_PED_ATC_0..7)
 *  PMDG 737/777      模式 80007/80008 (>K:ROTOR_BRAKE) 魔法数字旋钮
 *                    频率 72607/72608 (>K:ROTOR_BRAKE) 只能逐档
 *  iniBuilds A300/A310  模式 (L:A3x0_tcas_mode_pedestal) 0=STBY 1=XPDR 2=TARA 3=TA
 *  iniBuilds A220    模式 (L:A22X Transponder Mode) 0=Auto 1=Stdby 2=TA/RA
 *                                                  3=TA Only 4=Alt On 5=Alt Off
 *                    码   标准 K 事件 XPNDR_SET（BCD）+ XPNDR_n_INC/DEC
 *  iniBuilds A340/A350(2024)  B 事件 (B:AIRLINER_RMP_*)，LVar 前缀 INI_
 *  iFly 737-MAX      事件 (L:VC_Navigation_trigger_VAL) 数字编码
 *  TFDi MD-11        事件 (L:CEVENT) 数字编码（69632+ 为无线电）
 *  Leonardo Maddog   事件 (L:radio_event) 位运算 8192/16384
 *  Just Flight       模式 (L:PED_AFT_XPDR_MODE)
 *  Aerosoft CRJ      按钮 (L:ASCRJ_XPDR_ATC_SEL)
 *  Asobo 默认机       全部标准 SimEvent
 */
#ifndef MCONNECT_AIRCRAFTDB_H
#define MCONNECT_AIRCRAFTDB_H

#include "mconnect.h"

/* 应答机模式的规范枚举（与协议一致，0..5，见 control.h 的说明） */
#define XPDR_CANON_OFF    0
#define XPDR_CANON_STBY   1
#define XPDR_CANON_ON     2
#define XPDR_CANON_ALT    3
#define XPDR_CANON_TEST   4
#define XPDR_CANON_GND    5

/* 档案能力标记：说明该档案支持哪些操作，供 aircraft_caps 报给客户端。 */
#define CAP_XPDR_CODE  0x01
#define CAP_XPDR_MODE  0x02
#define CAP_COM_SET    0x04
#define CAP_COM_SWAP   0x08
#define CAP_COM_XMIT   0x10

typedef struct {
    const char* id;          /* 档案 ID，仅用于日志 */
    const char* match;       /* 机型标题/ICAO 匹配子串，'|' 分隔，大小写不敏感 */

    /* 应答机码：RPN 模板，%d = 4 位八进制码（如 7000）。
     * NULL = 该机型只能用标准事件 XPNDR_SET（绝大多数情况）。 */
    const char* xpdr_code_rpn;

    /* 应答机模式：RPN 模板，%d = 规范模式枚举。
     * 各机模档位**不同**，模板里直接写该机模自己的取值。 */
    const char* xpdr_mode_rpn[6];

    /* 应答机模式**读取**：calculator 表达式，经 WASM 桥读出该机模的原生档位值。
     * NULL = 该机模会正常更新标准 SimVar TRANSPONDER STATE（只有默认机这样）。
     *
     * 为什么必须单独给读：
     *   FBW 的模式在 (L:A32NX_TRANSPONDER_MODE)，Fenix 在 (L:S_XPDR_OPERATION)，
     *   A220 在 (L:A22X Transponder Mode)。这些机模**都不更新**标准
     *   TRANSPONDER STATE，只读 SimVar 会永远拿到过期值（停在 0=Off），
     *   客户端显示的模式与实际面板位置不一致。 */
    const char* xpdr_mode_read;

    /* 规范枚举 → 该机模原生档位值。-1 表示该档不支持。
     * 用于**写**（选模板）和**读**（把原生值反解回规范枚举）。 */
    int xpdr_mode_native[6];

    /* 应答机码读取：calculator 表达式。NULL = 读标准 SimVar
     * TRANSPONDER CODE:1（多数机模会更新这个 A 变量）。 */
    const char* xpdr_code_read;

    /* COM 频率设置：RPN 模板，%d = Hz。NULL = 用标准事件。 */
    const char* com_set_rpn[3];     /* [0]=COM1 [1]=COM2 [2]=COM3 */

    /* 主备交换：RPN 模板（可含 %d = 台号） */
    const char* com_swap_rpn;

    /* 发射台选择：RPN 模板，%d = 台号(1..3) */
    const char* com_xmit_rpn;

    /* 支持的能力位（CAP_*） */
    unsigned caps;

    /* 备注（写日志用，说明该机型的限制） */
    const char* note;
} AircraftProfile;

/* 按机型标题 / ICAO 查档案。找不到返回 NULL（调用方回退通用逻辑）。 */
const AircraftProfile* aircraftdb_find(const char* title, const char* icao);

/* 加载 MConnect.exe 同目录的 profiles.json（外部档案，热加载）。
 * aircraftdb_init 在启动时调用一次；aircraftdb_tick 在主循环里周期检查。 */
void aircraftdb_init(void);
void aircraftdb_tick(void);

/* 档案数量（诊断用）。 */
int aircraftdb_count(void);

/* 列出全部档案（写入日志）。 */
void aircraftdb_dump(void);

/* 模板展开。返回 1 成功；该档不支持返回 0。 */
int aircraftdb_expand_xpdr_code(const AircraftProfile* p, int code,
                                char* out, int cap);
int aircraftdb_expand_xpdr_mode(const AircraftProfile* p, int mode,
                                char* out, int cap);
int aircraftdb_expand_com_set(const AircraftProfile* p, int com, long hz,
                              char* out, int cap);
int aircraftdb_expand_com_swap(const AircraftProfile* p, int com,
                               char* out, int cap);
int aircraftdb_expand_com_xmit(const AircraftProfile* p, int com,
                               char* out, int cap);

/* 把机模原生档位值反解回规范枚举（用于读取）。
 * 找不到匹配返回 -1。native 值常见为 0..5 的小整数。 */
int aircraftdb_native_to_canon(const AircraftProfile* p, int native);

/* 把规范枚举转成机模原生档位值（用于写入）。不支持返回 -1。 */
int aircraftdb_canon_to_native(const AircraftProfile* p, int canon);

/* 该档案是否需要特殊读取（有 xpdr_mode_read / xpdr_code_read）。 */
int aircraftdb_needs_custom_read(const AircraftProfile* p);

/* 当前生效的档案（control 层解析后缓存）。
 * 从未解析过时返回 NULL。
 *
 * 放在本模块而非 control 是为了避免 datadefs → control → datadefs 循环依赖。 */
const AircraftProfile* aircraftdb_current(void);
void aircraftdb_set_current(const AircraftProfile* p);

#endif /* MCONNECT_AIRCRAFTDB_H */