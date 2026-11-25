/* MConnect — 机型档案库。见 aircraftdb.h 的数据来源与实测机制一览。
 *
 * 所有 RPN 均核对自：
 *   ① MobiFlight HubHop 本地全量预置 msfs2020_hubhop_presets.json（27382 条）
 *   ② A220 官方变量表 docs.synapticsim.com
 * 引用处的注释标明来源 label，便于日后随厂商更新复核。
 */
#include "aircraftdb.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <windows.h>

/* ============================================================
 * 档案表
 *
 * 字段顺序：id, match, xpdr_code_rpn, xpdr_mode_rpn[6],
 *          com_set_rpn[3], com_swap_rpn, com_xmit_rpn, caps, note
 *
 * 排列顺序 = 匹配优先级（越具体的档案越靠前，避免被通用档案抢走）。
 * ============================================================ */

static const AircraftProfile kProfiles[] = {

/* ---------------- FlyByWire A32NX / A380X ----------------
 * 写：预置 "TRANSPONDER MODE STBY/AUTO/ON" → 0/1/2 (>L:A32NX_TRANSPONDER_MODE)
 * 读：同 LVar（FBW 不更新标准 TRANSPONDER STATE，必须读 LVar）
 * 频率：FBW 官方 Flight Deck API 标注 COM ACTIVE FREQUENCY 为 R/W。 */
{
    "fbw-a32nx", "A32NX|A380X|flybywire|fly by wire",
    NULL,
    {
        NULL,                                    /* OFF：FBW 无此档 */
        "0 (>L:A32NX_TRANSPONDER_MODE, Number)", /* STBY */
        "2 (>L:A32NX_TRANSPONDER_MODE, Number)", /* ON  */
        "1 (>L:A32NX_TRANSPONDER_MODE, Number)", /* ALT → AUTO */
        NULL, NULL
    },
    "(L:A32NX_TRANSPONDER_MODE, Number)",
    { -1, 0, 2, 1, -1, -1 },
    NULL,
    {
        "%ld (>A:COM ACTIVE FREQUENCY:1, Hz)",
        "%ld (>A:COM ACTIVE FREQUENCY:2, Hz)",
        "%ld (>A:COM ACTIVE FREQUENCY:3, Hz)"
    },
    "1 (>K:COM%d_RADIO_SWAP)",
    NULL,
    CAP_XPDR_MODE | CAP_COM_SET | CAP_COM_SWAP,
    "应答机只有 STBY/AUTO/ON；ALT 在 FBW 语境即 AUTO"
},

/* ---------------- Fenix A320 ----------------
 * 写：OPERATION 0=STBY 1=AUTO 2=ON（"FNX320 Transponder Operation Knob" 预置）
 *     MODE     0=STBY 1=TA 2=TARA（TA/RA 选择）
 * 读：OPERATION（真正的开关状态）
 * 码：N_FREQ_XPNDR_SELECTED（FenixQuartz 提供的"已选定码"） */
{
    "fenix-a320", "FNX320|Fenix|fenix",
    NULL,
    {
        "0 (>L:S_XPDR_OPERATION, Number)",  /* OFF → OPERATION=STBY */
        "0 (>L:S_XPDR_OPERATION, Number)",  /* STBY */
        "2 (>L:S_XPDR_OPERATION, Number)",  /* ON   */
        "2 (>L:S_XPDR_MODE, Number)",       /* ALT  → MODE=TARA */
        NULL, NULL
    },
    "(L:S_XPDR_OPERATION, Number)",
    { -1, 0, 2, -1, -1, -1 },
    "(L:N_FREQ_XPNDR_SELECTED, Number)",
    { NULL, NULL, NULL },
    NULL,
    NULL,
    CAP_XPDR_MODE | CAP_COM_SET | CAP_COM_SWAP,
    "OPERATION(0=STBY 1=AUTO 2=ON) 与 MODE(0=STBY 1=TA 2=TARA) 两套；"
    "ALT 映射到 MODE=TARA"
},

/* ---------------- PMDG 737 NG3 ----------------
 * 只有旋钮 ROTOR_BRAKE（模式 80007/80008、频率 72607/72608），无法直接设值。
 * PMDG 不更新标准 TRANSPONDER STATE，但也没有可读的 LVar（需 SDK ClientData），
 * 因此读取也走标准变量（会读到过期值，属已知限制）。 */
{
    "pmdg-737", "PMDG 737|PMDG_737|pmdg-aircraft-737|B736|B737|B738|B739",
    NULL,
    { NULL, NULL, NULL, NULL, NULL, NULL },
    NULL,
    { -1, -1, -1, -1, -1, -1 },
    NULL,
    { NULL, NULL, NULL },
    NULL, NULL,
    CAP_XPDR_MODE | CAP_COM_SET | CAP_COM_SWAP,
    "应答机状态走官方 SDK ClientData（PMDG_NG3_Data），需 737NG3_Options.ini "
    "设 EnableDataBroadcast=1；模式是 5 档旋钮，内部按差值转"
},

/* ---------------- PMDG 777 ---------------- */
{
    "pmdg-777", "PMDG 777|PMDG_777|pmdg-aircraft-77|B77W|B772|B77L|B77F|777-200|777-300",
    NULL,
    { NULL, NULL, NULL, NULL, NULL, NULL },
    NULL,
    { -1, -1, -1, -1, -1, -1 },
    NULL,
    { NULL, NULL, NULL },
    NULL, NULL,
    CAP_XPDR_MODE | CAP_COM_SET | CAP_COM_SWAP,
    "同 737：应答机走官方 SDK ClientData（PMDG_777 通道），需 EnableDataBroadcast=1"
},

/* ---------------- PMDG DC-6 ---------------- */
{
    "pmdg-dc6", "PMDG DC-6|pmdg-aircraft-dc6|DC6",
    NULL,
    { NULL, NULL, NULL, NULL, NULL, NULL },
    NULL,
    { -1, -1, -1, -1, -1, -1 },
    NULL,
    { NULL, NULL, NULL },
    NULL, NULL,
    CAP_COM_SET,
    "DC-6 走标准事件"
},

/* ---------------- iniBuilds A310 ----------------
 * 写/读：(L:A310_tcas_mode_pedestal) 0=STBY 1=XPDR 2=TARA 3=TA
 * 预置："A310 TCAS Mode Selector STBY/XPDR/TARA/TA" */
{
    "inibuilds-a310", "A310",
    NULL,
    {
        NULL,
        "0 (>L:A310_tcas_mode_pedestal, Number)",  /* STBY */
        "1 (>L:A310_tcas_mode_pedestal, Number)",  /* ON  → XPDR */
        "2 (>L:A310_tcas_mode_pedestal, Number)",  /* ALT → TARA */
        NULL, NULL
    },
    "(L:A310_tcas_mode_pedestal, Number)",
    { -1, 0, 1, 2, -1, -1 },
    NULL,
    { NULL, NULL, NULL },
    "(L:INI_CPT_VHF_TRANSFER_SWITCH, Bool) ! "
        "(>L:INI_CPT_VHF_TRANSFER_SWITCH, Bool)",
    NULL,
    CAP_XPDR_MODE | CAP_COM_SET | CAP_COM_SWAP,
    "模式档位 STBY/XPDR/TARA/TA；ALT 映射到 TARA"
},

/* ---------------- iniBuilds A300 ----------------
 * 前缀 INI_（与 A310 的 A310_ 不同） */
{
    "inibuilds-a300", "A300",
    NULL,
    {
        NULL,
        "0 (>L:INI_tcas_mode_pedestal, Number)",
        "1 (>L:INI_tcas_mode_pedestal, Number)",
        "2 (>L:INI_tcas_mode_pedestal, Number)",
        NULL, NULL
    },
    "(L:INI_tcas_mode_pedestal, Number)",
    { -1, 0, 1, 2, -1, -1 },
    NULL,
    { NULL, NULL, NULL },
    "(L:INI_CPT_VHF_TRANSFER_SWITCH, Bool) ! "
        "(>L:INI_CPT_VHF_TRANSFER_SWITCH, Bool)",
    NULL,
    CAP_XPDR_MODE | CAP_COM_SET | CAP_COM_SWAP,
    "前缀 INI_；模式档位同 A310"
},

/* ---------------- iniBuilds A220 ----------------
 * 官方变量表（docs.synapticsim.com）：
 *   L:A22X Transponder Mode  Enum
 *       0=Auto 1=Standby 2=TA/RA 3=TA Only 4=Alt On 5=Alt Off
 *   原生支持标准 K 事件 XPNDR_SET / COM*_RADIO_SET_HZ / XPNDR_IDENT_ON
 * 这是最标准的现代机模：写用标准事件，读模式用 LVar。 */
{
    "inibuilds-a220", "A220|BCS3|A22X|SynapticSim|synapticsim",
    NULL,
    {
        NULL,
        "1 (>L:A22X Transponder Mode, Number)",  /* STBY */
        "0 (>L:A22X Transponder Mode, Number)",  /* ON  → Auto */
        "4 (>L:A22X Transponder Mode, Number)",  /* ALT → Alt On */
        NULL, NULL
    },
    "(L:A22X Transponder Mode, Number)",
    { 5, 1, 0, 4, -1, -1 },   /* OFF→AltOff(5) STBY→1 ON→Auto(0) ALT→AltOn(4) */
    NULL,
    { NULL, NULL, NULL },   /* 频率走标准 COM*_RADIO_SET_HZ */
    "1 (>K:COM%d_RADIO_SWAP)",
    NULL,
    CAP_XPDR_CODE | CAP_XPDR_MODE | CAP_COM_SET | CAP_COM_SWAP,
    "A220 原生支持标准 K 事件；模式用 L:A22X Transponder Mode"
    "(0=Auto 1=Stby 2=TA/RA 4=Alt On)"
},

/* ---------------- iniBuilds A350 ----------------
 * 从本机机型文件实测（比 HubHop 权威）：
 *   ModelBehaviorDefs/iniBuilds/A350/Interior/A350_Pedestal_Components.xml
 *   模式旋钮 <KNOB_VAR>INI_tcas_mode_pedestal</KNOB_VAR>，NUM_STATES=4
 *     TA_ONLY 按钮：2=TA/RA  3=TA ONLY
 *     （0=STBY 1=XPDR 与 A300 同族，取值含义一致）
 *   应答机码：无限旋钮
 *     1 (>L:INI_TRANSPONDER_OUTER_KNOB_TURNED_CLOCKWISE)  / ANTICLOCKWISE
 *     1 (>L:INI_TRANSPONDER_INNER_KNOB_TURNED_CLOCKWISE)  / ANTICLOCKWISE
 *   频率：B 事件 (B:AIRLINER_RMP_*)
 *   应答机 1/2 选择：(L:INI_XPDR_SYS)  1/2 */
{
    "inibuilds-a350", "A350|A359|A35K",
    NULL,
    {
        NULL,
        "0 (>L:INI_tcas_mode_pedestal, Number)",  /* STBY */
        "1 (>L:INI_tcas_mode_pedestal, Number)",  /* ON  → XPDR */
        "2 (>L:INI_tcas_mode_pedestal, Number)",  /* ALT → TA/RA */
        NULL, NULL
    },
    "(L:INI_tcas_mode_pedestal, Number)",
    { -1, 0, 1, 2, -1, -1 },
    NULL,
    { NULL, NULL, NULL },
    "(L:INI_CPT_VHF_TRANSFER_SWITCH, Bool) ! "
        "(>L:INI_CPT_VHF_TRANSFER_SWITCH, Bool)",
    NULL,
    CAP_XPDR_MODE | CAP_COM_SET | CAP_COM_SWAP,
    "A350 模式用 INI_tcas_mode_pedestal（0=STBY 1=XPDR 2=TA/RA 3=TA ONLY）；"
    "频率走 B 事件(B:AIRLINER_RMP_*)或标准事件"
},

/* ---------------- iniBuilds A340 (MSFS 2024) ----------------
 * A340 与 A350 同期发布，机制同族（本机未装 A340，按 A350 类推）。
 * 若有 A340 的机型文件可复核 ModelBehaviorDefs 里的变量名。 */
{
    "inibuilds-a340", "A340|A346|A345|A343",
    NULL,
    {
        NULL,
        "0 (>L:INI_tcas_mode_pedestal, Number)",
        "1 (>L:INI_tcas_mode_pedestal, Number)",
        "2 (>L:INI_tcas_mode_pedestal, Number)",
        NULL, NULL
    },
    "(L:INI_tcas_mode_pedestal, Number)",
    { -1, 0, 1, 2, -1, -1 },
    NULL,
    { NULL, NULL, NULL },
    "(L:INI_CPT_VHF_TRANSFER_SWITCH, Bool) ! "
        "(>L:INI_CPT_VHF_TRANSFER_SWITCH, Bool)",
    NULL,
    CAP_XPDR_MODE | CAP_COM_SET | CAP_COM_SWAP,
    "按 A350 同族类推（未装 A340 实机，未复核）"
},

/* ---------------- iFly 737 MAX ----------------
 * 所有按键通过 (L:VC_Navigation_trigger_VAL) 传数字编码（IDENT=411） */
{
    "ifly-737max", "iFly|IFLY|737-MAX|737MAX|B38M|B39M",
    NULL,
    { NULL, NULL, NULL, NULL, NULL, NULL },
    NULL,
    { -1, -1, -1, -1, -1, -1 },
    NULL,
    { NULL, NULL, NULL },
    NULL, NULL,
    CAP_COM_SET | CAP_COM_SWAP,
    "iFly 用 (L:VC_Navigation_trigger_VAL) 数字编码（IDENT=411）；"
    "应答机码/模式码位需实测，暂走标准事件"
},

/* ---------------- TFDi MD-11 ----------------
 * 单一 (L:CEVENT) 通道传事件号（无线电 69632+、应答机 69854+） */
{
    "tfdi-md11", "TFDi|MD-11|MD11",
    NULL,
    { NULL, NULL, NULL, NULL, NULL, NULL },
    NULL,
    { -1, -1, -1, -1, -1, -1 },
    NULL,
    { NULL, NULL, NULL },
    NULL, NULL,
    CAP_COM_SET | CAP_COM_SWAP,
    "TFDi 用单一 (L:CEVENT) 通道传事件号（本机行为文件实测："
    "应答机 69854~69865、无线电 69632~69651）；频率只能逐档增减"
},

/* ---------------- Leonardo Maddog MD-82 ----------------
 * 位运算 (L:radio_event)：8192+N=INC、16384+N=DEC */
{
    "leonardo-maddog", "MadDog|MD-82|MD82|Leonardo|Fly the Maddog",
    NULL,
    { NULL, NULL, NULL, NULL, NULL, NULL },
    NULL,
    { -1, -1, -1, -1, -1, -1 },
    NULL,
    { NULL, NULL, NULL },
    NULL, NULL,
    CAP_COM_SET | CAP_COM_SWAP,
    "位运算 (L:radio_event)：8192+N=INC、16384+N=DEC，只能逐档转"
},

/* ---------------- Just Flight ----------------
 * 预置："Transponder/TCAS Mode select knob STBY" → 0 (>L:PED_AFT_XPDR_MODE)
 *       "Transponder/TCAS Mode select knob ALT OFF" → 1 (>L:PED_AFT_XPDR_MODE) */
{
    "justflight", "Just Flight|JF_|BAe 146|PA28|Hawk|Vulcan|KT76",
    NULL,
    {
        NULL,
        "0 (>L:PED_AFT_XPDR_MODE, Number)",   /* STBY */
        NULL,
        "1 (>L:PED_AFT_XPDR_MODE, Number)",   /* ALT（ALT OFF 档） */
        NULL, NULL
    },
    "(L:PED_AFT_XPDR_MODE, Number)",
    { -1, 0, -1, 1, -1, -1 },
    NULL,
    { NULL, NULL, NULL },
    NULL, NULL,
    CAP_XPDR_MODE | CAP_COM_SET | CAP_COM_SWAP,
    "档位含义因机型而异，仅映射 STBY/ALT"
},

/* ---------------- FSL A32X（Flight Sim Labs A320/A321）----------------
 * 从本机机型文件实测（比 HubHop 权威）：
 *   ModelBehaviorDefs/Interior/FSLA32X_Interior_Common_Templates.xml
 *
 * 应答机是**按钮阵列 + ROTOR_BRAKE**（和 PMDG 同思路，但编码方式不同）：
 *   #EVENT_ID# (>L:ATCXPDRBtnID, number)
 *   (L:ATCXPDRBtnID) + 0/1/2/3  (>K:ROTOR_BRAKE)
 *       +0 = LeftSingle（左键按下）
 *       +1 = RightSingle（右键按下）
 *       +2 = LeftRelease（左键松开）
 *       +3 = RightRelease（右键松开）
 * 也就是说最终值是 `按钮ID*4 + 动作`，动作 0..3。
 *
 * 应答机各按钮的 EVENT_ID 绑定在 SimObjects 的行为 XML 里（本机该目录未
 * 包含绑定定义，可能编译进 model 内部），因此**具体按钮 ID 需实测**。
 * 这里只记录机制，不填模板（填错不如不填）。
 * 频率：FSL 有自己的 RMP 逻辑，同样未在可见文件中暴露 LVar → 走标准事件。 */
{
    "fsl-a32x", "FSL|fsl-a32x|A32X|Flight Sim Labs",
    NULL,
    {
        /* MODE 旋钮 EVENT_ID=78347。按 FSL_ClickAndDrag_Knob_Template 算法：
         *   (EVENT_ID - 1) (>L:CD_InfKnobID)   → 78346
         *   逆时针一格 +1 → 78347 ；顺时针一格 +2 → 78348
         * 所以「转一格」的 RPN 就是 78347 或 78348 直接送 ROTOR_BRAKE。 */
        NULL, NULL, NULL, NULL, NULL, NULL
    },
    "(L:VC_PED_ATCXPDR_MODE_Switch, Number)",
    { -1, -1, -1, -1, -1, -1 },   /* 档位数未知 → 不做映射，由 control.c 相对转 */
    NULL,
    { NULL, NULL, NULL },
    NULL, NULL,
    CAP_XPDR_MODE | CAP_COM_SET | CAP_COM_SWAP,
    "FSL 应答机：MODE 旋钮 EVENT_ID=78347（+1=逆时针 +2=顺时针，需按差值 "
    "转）；码用数字键 78292/78297/78302/78307/78312/78317/78322/78327(0)/78332(CLR)"
},

/* ---------------- 中国商飞 C919（fycyc）----------------
 * 从本机机型文件实测：
 *   Documentation/ClientEventDefinitionPublicHardware.h
 *
 * 它用的是**具名客户端事件**（第三种机制，与 K 事件/LVar 都不同）：
 *   SimConnect_MapClientEventToSimEvent(h, ID, "HW_PUB.FMCP_AT_BTN")
 *   事件编号从 72000 起（EVENT_HW_PUB_START = 72000）
 *
 * 但该头文件里**没有任何应答机/无线电相关事件**（只有 FMCP/DCP/...
 * 的自动驾驶与显示面板按钮），行为文件里也只有两个 L:C919X_* 变量。
 * 说明 C919 的无线电面板未对外开放 → 只能走标准事件（它实现了标准
 * COM/XPDR 事件与否需实测）。 */
{
    "c919", "C919|fycyc|c919x",
    NULL,
    { NULL, NULL, NULL, NULL, NULL, NULL },
    NULL,
    { -1, -1, -1, -1, -1, -1 },
    NULL,
    { NULL, NULL, NULL },
    NULL, NULL,
    0,
    "C919 用具名客户端事件(HW_PUB.*, 编号 72000 起)，但公开清单里无应答机/"
    "无线电事件；无线电面板未开放，暂走标准事件（需实测）"
},

/* ---------------- Aerosoft CRJ ----------------
 * 按钮 (L:ASCRJ_XPDR_ATC_SEL)，档位需多次按 */
{
    "aerosoft-crj", "Aerosoft|ASCRJ|CRJ",
    NULL,
    { NULL, NULL, NULL, NULL, NULL, NULL },
    "(L:ASCRJ_XPDR_ATC_SEL, Number)",
    { -1, 0, -1, -1, -1, -1 },
    NULL,
    { NULL, NULL, NULL },
    NULL, NULL,
    CAP_COM_SET | CAP_COM_SWAP,
    "应答机是按钮 (ASCRJ_XPDR_ATC_SEL)，档位需多次按，无法直接设值"
},

/* ---------------- iniBuilds 通用兜底（A320neo/A321/A400M/AN225） ---------------- */
{
    "inibuilds-generic", "IniBuilds|iniBuilds|A20N|A320neo|A321|A400M|AN225|A330",
    NULL,
    { NULL, NULL, NULL, NULL, NULL, NULL },
    NULL,
    { -1, -1, -1, -1, -1, -1 },
    NULL,
    { NULL, NULL, NULL },
    "(L:INI_CPT_VHF_TRANSFER_SWITCH, Bool) ! "
        "(>L:INI_CPT_VHF_TRANSFER_SWITCH, Bool)",
    NULL,
    CAP_COM_SET | CAP_COM_SWAP,
    "IniBuilds 通用：频率交换用 INI_ 前缀 LVar 翻转，其余走标准事件"
},

/* ---------------- Asobo / Microsoft 默认机 ----------------
 * 全部走标准事件；标量 SimVar TRANSPONDER STATE 由模拟器自己维护，可直接读。 */
{
    "asobo-default",
    "Asobo|Microsoft|Generic|C172|C152|DA40|TBM|B748|B58|A320neo Asobo",
    NULL,
    { NULL, NULL, NULL, NULL, NULL, NULL },
    NULL,
    { 0, 1, 3, 4, 2, 5 },   /* 标准 MSFS 枚举，无需重映射 */
    NULL,
    { NULL, NULL, NULL },
    NULL, NULL,
    CAP_XPDR_CODE | CAP_XPDR_MODE | CAP_COM_SET | CAP_COM_SWAP | CAP_COM_XMIT,
    "默认机全部走标准 SimEvent，TRANSPONDER STATE 由模拟器直接维护"
}
};

static const int kProfileCount =
    (int)(sizeof(kProfiles) / sizeof(kProfiles[0]));

/* ============================================================
 * 外部档案：profiles.json（热加载）
 *
 * 放在 MConnect.exe 同目录。结构见 MConnect/profiles.example.json：
 *   {"aircraft":[{"id":"...","match":"...","caps":31,
 *                 "xpdr_code_rpn":"...", "xpdr_mode_rpn":["..." x6],
 *                 "xpdr_mode_read":"...", "xpdr_mode_native":[0..5],
 *                 "xpdr_code_read":"...", "com_set_rpn":["..." x3],
 *                 "com_swap_rpn":"...", "com_xmit_rpn":"...",
 *                 "note":"..."}]}
 *
 * 外部档案优先于内置表；文件修改后 2s 内自动重载。
 * 重载时不释放旧档案内存：aircraftdb_current() 可能仍指向旧条目。
 * ============================================================ */

#define EXT_MAX 64

static AircraftProfile* g_ext = NULL;
static int g_ext_n = 0;
static char g_ext_path[MAX_PATH] = "";
static int g_ext_have = 0;
static time_t g_ext_mtime = 0;
static long g_ext_size = 0;

static char* dup_str(const char* s) {
    size_t n;
    char* p;
    if (!s) return NULL;
    n = strlen(s) + 1;
    p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static const char* jstr(const Json* obj, const char* key) {
    const Json* v = json_get(obj, key);
    return (v && v->type == JSON_STR) ? v->sval : NULL;
}

static int jint(const Json* obj, const char* key, int def) {
    const Json* v = json_get(obj, key);
    return (v && v->type == JSON_NUM) ? (int)v->num : def;
}

static int parse_profile(const Json* item, AircraftProfile* p) {
    const Json* arr;
    int i;
    unsigned derived = 0;

    memset(p, 0, sizeof(*p));
    p->id = dup_str(jstr(item, "id"));
    p->match = dup_str(jstr(item, "match"));
    if (!p->id || !p->match) return 0;

    p->xpdr_code_rpn = dup_str(jstr(item, "xpdr_code_rpn"));
    p->xpdr_mode_read = dup_str(jstr(item, "xpdr_mode_read"));
    p->xpdr_code_read = dup_str(jstr(item, "xpdr_code_read"));
    p->com_swap_rpn = dup_str(jstr(item, "com_swap_rpn"));
    p->com_xmit_rpn = dup_str(jstr(item, "com_xmit_rpn"));
    p->note = dup_str(jstr(item, "note"));

    arr = json_get(item, "xpdr_mode_rpn");
    for (i = 0; i < 6; i++) {
        const Json* v = json_at(arr, i);
        if (v && v->type == JSON_STR)
            p->xpdr_mode_rpn[i] = dup_str(v->sval);
        p->xpdr_mode_native[i] = -1;
    }
    arr = json_get(item, "com_set_rpn");
    for (i = 0; i < 3; i++) {
        const Json* v = json_at(arr, i);
        if (v && v->type == JSON_STR)
            p->com_set_rpn[i] = dup_str(v->sval);
    }
    arr = json_get(item, "xpdr_mode_native");
    for (i = 0; i < 6; i++) {
        const Json* v = json_at(arr, i);
        if (v && v->type == JSON_NUM)
            p->xpdr_mode_native[i] = (int)v->num;
    }

    if (p->xpdr_code_rpn) derived |= CAP_XPDR_CODE;
    for (i = 0; i < 6; i++)
        if (p->xpdr_mode_rpn[i]) { derived |= CAP_XPDR_MODE; break; }
    if (p->com_set_rpn[0] || p->com_set_rpn[1] || p->com_set_rpn[2])
        derived |= CAP_COM_SET;
    if (p->com_swap_rpn) derived |= CAP_COM_SWAP;
    if (p->com_xmit_rpn) derived |= CAP_COM_XMIT;
    p->caps = (unsigned)jint(item, "caps", 0);
    if (!p->caps) p->caps = derived;
    return 1;
}

static void load_profiles(void) {
    FILE* f;
    long sz;
    char* buf;
    char* text;
    Json* root;
    const Json* arr;
    AircraftProfile* ext;
    int n, i, m = 0;

    f = fopen(g_ext_path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4L * 1024L * 1024L) { fclose(f); return; }
    buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return;
    }
    fclose(f);
    buf[sz] = '\0';
    text = buf;
    if ((unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        text += 3;

    root = json_parse(text);
    free(buf);
    if (!root) {
        xc_logf("profilejson: parse failed: %s", g_ext_path);
        return;
    }
    arr = json_get(root, "aircraft");
    n = arr ? json_len(arr) : 0;
    if (n > EXT_MAX) n = EXT_MAX;
    if (n <= 0) {
        json_free(root);
        return;
    }
    ext = (AircraftProfile*)calloc((size_t)n, sizeof(*ext));
    if (!ext) { json_free(root); return; }
    for (i = 0; i < n; i++) {
        const Json* item = json_at(arr, i);
        if (item && item->type == JSON_OBJ && parse_profile(item, &ext[m]))
            m++;
    }
    json_free(root);
    if (m <= 0) {
        free(ext);
        xc_logf("profilejson: no valid profiles in %s", g_ext_path);
        return;
    }
    /* 旧 g_ext 故意不释放：当前档案指针可能仍指向它 */
    g_ext = ext;
    g_ext_n = m;
    xc_logf("profilejson: loaded %d external profile(s) from %s",
            m, g_ext_path);
}

static void resolve_profiles_path(void) {
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
    struct _stat st;

    if (n > 0 && n < sizeof(exe)) {
        char* slash = strrchr(exe, '\\');
        if (slash) {
            slash[1] = '\0';
            snprintf(g_ext_path, sizeof(g_ext_path), "%sprofiles.json", exe);
            if (_stat(g_ext_path, &st) == 0) return;
        }
    }
    snprintf(g_ext_path, sizeof(g_ext_path), "profiles.json");
    if (_stat(g_ext_path, &st) != 0)
        g_ext_path[0] = '\0';   /* 文件不存在：保留空路径，tick 再试 */
}

static void load_if_changed(int force) {
    struct _stat st;
    if (!g_ext_path[0]) resolve_profiles_path();
    if (!g_ext_path[0]) return;
    if (_stat(g_ext_path, &st) != 0) {
        g_ext_have = 0;
        return;
    }
    if (!force && g_ext_have &&
        st.st_mtime == g_ext_mtime && (long)st.st_size == g_ext_size)
        return;
    g_ext_mtime = st.st_mtime;
    g_ext_size = (long)st.st_size;
    g_ext_have = 1;
    load_profiles();
}

void aircraftdb_init(void) {
    resolve_profiles_path();
    load_if_changed(1);
}

void aircraftdb_tick(void) {
    static ULONGLONG next = 0;
    ULONGLONG now = GetTickCount64();
    if (now < next) return;
    next = now + 2000;   /* 2s 检查一次 */
    load_if_changed(0);
}

/* ============================================================
 * 查询
 * ============================================================ */

/* 大小写不敏感的子串查找 */
static int icontains(const char* hay, const char* needle) {
    size_t nl;
    const char* p;

    if (!hay || !needle || !needle[0]) return 0;
    nl = strlen(needle);
    for (p = hay; *p; p++) {
        size_t i;
        for (i = 0; i < nl; i++) {
            char a = p[i], b = needle[i];
            if (!a) return 0;
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (i == nl) return 1;
    }
    return 0;
}

/* match 字段用 '|' 分隔多个备选子串 */
static int match_any(const char* match, const char* text) {
    char buf[512];
    char* tok;
    char* p;

    if (!match || !text || !text[0]) return 0;
    snprintf(buf, sizeof(buf), "%s", match);
    p = buf;
    while (p && *p) {
        tok = p;
        while (*p && *p != '|') p++;
        if (*p == '|') { *p = '\0'; p++; }
        else p = NULL;
        if (tok[0] && icontains(text, tok)) return 1;
    }
    return 0;
}

const AircraftProfile* aircraftdb_find(const char* title, const char* icao) {
    int i;

    /* 外部 profiles.json 优先于内置表 */
    for (i = 0; i < g_ext_n; i++) {
        if (match_any(g_ext[i].match, title)) return &g_ext[i];
    }
    for (i = 0; i < g_ext_n; i++) {
        if (match_any(g_ext[i].match, icao)) return &g_ext[i];
    }

    /* 表已按"具体 → 通用"排序，顺序扫描即可。
     * 例："A310" 档案排在 "IniBuilds|iniBuilds" 之前，不会被子串抢走。 */
    for (i = 0; i < kProfileCount; i++) {
        if (match_any(kProfiles[i].match, title)) return &kProfiles[i];
    }
    for (i = 0; i < kProfileCount; i++) {
        if (match_any(kProfiles[i].match, icao)) return &kProfiles[i];
    }
    return NULL;
}

int aircraftdb_count(void) { return kProfileCount + g_ext_n; }

void aircraftdb_dump(void) {
    int i;
    xc_logf("aircraftdb: %d 个机型档案（外部 %d）",
            kProfileCount + g_ext_n, g_ext_n);
    for (i = 0; i < g_ext_n; i++) {
        xc_logf("  [ext] %-14s caps=0x%02x match=\"%s\"",
                g_ext[i].id ? g_ext[i].id : "?",
                g_ext[i].caps, g_ext[i].match ? g_ext[i].match : "");
        if (g_ext[i].note && g_ext[i].note[0])
            xc_logf("        备注: %s", g_ext[i].note);
    }
    for (i = 0; i < kProfileCount; i++) {
        xc_logf("  %-18s caps=0x%02x match=\"%s\"", kProfiles[i].id,
                kProfiles[i].caps, kProfiles[i].match);
        if (kProfiles[i].note && kProfiles[i].note[0])
            xc_logf("      备注: %s", kProfiles[i].note);
    }
}

/* ============================================================
 * 模板展开
 * ============================================================ */

/* 把模板里的第一个 %ld/%d 替换成 v。没有占位符时原样返回。 */
static int expand_int(const char* tmpl, long v, char* out, int cap) {
    const char* pct;

    if (!tmpl || !out || cap < 1) return 0;
    pct = strstr(tmpl, "%ld");
    if (pct) {
        snprintf(out, (size_t)cap, "%.*s%ld%s", (int)(pct - tmpl), tmpl, v,
                 pct + 3);
        return 1;
    }
    pct = strstr(tmpl, "%d");
    if (pct) {
        snprintf(out, (size_t)cap, "%.*s%d%s", (int)(pct - tmpl), tmpl, (int)v,
                 pct + 2);
        return 1;
    }
    snprintf(out, (size_t)cap, "%s", tmpl);
    return 1;
}

int aircraftdb_expand_xpdr_code(const AircraftProfile* p, int code,
                                char* out, int cap) {
    if (!p || !p->xpdr_code_rpn) return 0;
    return expand_int(p->xpdr_code_rpn, code, out, cap);
}

int aircraftdb_expand_xpdr_mode(const AircraftProfile* p, int mode,
                                char* out, int cap) {
    if (!p || mode < 0 || mode > 5) return 0;
    if (!p->xpdr_mode_rpn[mode]) return 0;   /* 该档此机型不支持 */
    return expand_int(p->xpdr_mode_rpn[mode], mode, out, cap);
}

int aircraftdb_expand_com_set(const AircraftProfile* p, int com, long hz,
                              char* out, int cap) {
    if (!p || com < 1 || com > 3) return 0;
    if (!p->com_set_rpn[com - 1]) return 0;
    return expand_int(p->com_set_rpn[com - 1], hz, out, cap);
}

int aircraftdb_expand_com_swap(const AircraftProfile* p, int com,
                               char* out, int cap) {
    if (!p || !p->com_swap_rpn) return 0;
    return expand_int(p->com_swap_rpn, com, out, cap);
}

int aircraftdb_expand_com_xmit(const AircraftProfile* p, int com,
                               char* out, int cap) {
    if (!p || !p->com_xmit_rpn) return 0;
    return expand_int(p->com_xmit_rpn, com, out, cap);
}

/* ---------------- 模式读写映射 ----------------
 *
 * 两个方向的映射必须成对使用同一张 xpdr_mode_native 表，否则会出现
 * "写 ON 有效、读回来却是别的档"这种前后不一致。
 *
 * native 表的值 -1 表示该档此机模不支持（例如 FBW 没有 OFF）。
 * 反解时若多个规范档映射到同一 native 值（如 FBW 的 OFF 和 STBY 都是 -1，
 * Fenix 的 OFF/STBY 都是 0），按表中**靠前**的规范档返回。
 */

int aircraftdb_native_to_canon(const AircraftProfile* p, int native) {
    int i;

    if (!p || !p->xpdr_mode_read) return -1;   /* 该机型不需要特殊反解 */
    /* 先在"支持的档位"里找精确匹配 */
    for (i = 0; i < 6; i++) {
        if (p->xpdr_mode_native[i] >= 0 && p->xpdr_mode_native[i] == native)
            return i;
    }
    return -1;   /* 读到未定义的档位值 */
}

int aircraftdb_canon_to_native(const AircraftProfile* p, int canon) {
    if (!p || canon < 0 || canon > 5) return -1;
    return p->xpdr_mode_native[canon];
}

int aircraftdb_needs_custom_read(const AircraftProfile* p) {
    if (!p) return 0;
    return (p->xpdr_mode_read != NULL) || (p->xpdr_code_read != NULL);
}

/* ---------------- 当前档案 ----------------
 *
 * 档案解析结果（哪个机型命中哪条档案）放在本模块里共享，
 * 而不是只存在 control.c —— 因为 datadefs（读取侧）也需要它，
 * 若从 control 拿会形成 datadefs → control → datadefs 循环依赖。
 */
static const AircraftProfile* g_current = NULL;

void aircraftdb_set_current(const AircraftProfile* p) { g_current = p; }
const AircraftProfile* aircraftdb_current(void) { return g_current; }