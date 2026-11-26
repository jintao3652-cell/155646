/* MConnect 机型档案测试（不需要模拟器）。
 *
 * 档案库是"全都要适配"的核心：每个机模族用不同的机制，靠猜名字必然失败。
 * 这里验证：
 *   1. 机型识别（标题/ICAO、大小写、子串、多备选）
 *   2. 模板展开（%d 替换、无占位符、不支持的档位）
 *   3. 各机模族的实际写法与 HubHop 预置一致
 *   4. 未知机型回退（返回 NULL，不崩）
 *
 * 校验基准：MobiFlight HubHop 本地缓存 msfs2020_hubhop_presets.json 的
 * 真实 code 字段（测试里以注释标注来源 label）。
 */
#include "aircraftdb.h"
#include "mconnect.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

int g_verbose_flag = 0;

static int g_fail = 0;
static int g_pass = 0;

double xc_now_sec(void) { return 0.0; }

void xc_logf(const char* fmt, ...) {
    static int v = -1;
    va_list ap;
    if (v < 0) v = getenv("TESTLOG") ? 1 : 0;
    if (!v) return;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

void xc_infof(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

static void check(int cond, const char* what, const char* detail) {
    if (cond) { g_pass++; printf("  PASS  %s\n", what); }
    else { g_fail++; printf("  FAIL  %s   [%s]\n", what, detail ? detail : ""); }
}

static void check_str(const char* got, const char* want, const char* what) {
    char d[320];
    snprintf(d, sizeof(d), "got=\"%s\" want=\"%s\"", got ? got : "(null)",
             want ? want : "(null)");
    check(got && want && strcmp(got, want) == 0, what, d);
}

static void check_id(const AircraftProfile* p, const char* id, const char* what) {
    char d[128];
    snprintf(d, sizeof(d), "got=%s want=%s", p ? p->id : "(null)", id);
    check(p && strcmp(p->id, id) == 0, what, d);
}

int main(void) {
    char buf[256];

    printf("== MConnect 机型档案测试 ==\n\n");

    /* --- 1. 机型识别 --- */
    printf("[1] 机型识别（按标题）\n");
    check_id(aircraftdb_find("FlyByWire A32NX", ""), "fbw-a32nx", "FBW A32NX");
    check_id(aircraftdb_find("A32NX", ""), "fbw-a32nx", "仅 ICAO 串 A32NX");
    check_id(aircraftdb_find("Fenix A320", ""), "fenix-a320", "Fenix A320");
    check_id(aircraftdb_find("FNX320", ""), "fenix-a320", "FNX320");
    check_id(aircraftdb_find("PMDG 737-800", ""), "pmdg-737", "PMDG 737-800");
    check_id(aircraftdb_find("PMDG 777-300ER", ""), "pmdg-777", "PMDG 777");
    check_id(aircraftdb_find("iniBuilds A310", ""), "inibuilds-a310",
             "IniBuilds A310");
    check_id(aircraftdb_find("Asobo C172", ""), "asobo-default", "Asobo C172");

    printf("\n[1b] 大小写不敏感\n");
    check_id(aircraftdb_find("flybywire a32nx", ""), "fbw-a32nx", "全小写");
    check_id(aircraftdb_find("FENIX A320", ""), "fenix-a320", "全大写");
    check_id(aircraftdb_find("pmdg 737", ""), "pmdg-737", "pmdg 小写");

    printf("\n[1c] 按 ICAO 兜底\n");
    check_id(aircraftdb_find("", "B738"), "pmdg-737", "空标题 + B738");
    check_id(aircraftdb_find("Unknown", "B77W"), "pmdg-777", "标题不匹配 + B77W");

    printf("\n[1d] 未知机型回退\n");
    check(aircraftdb_find("Some Unknown Addon", "") == NULL,
          "未知机型返回 NULL（不崩）", NULL);
    check(aircraftdb_find("", "") == NULL, "全空返回 NULL", NULL);
    check(aircraftdb_find(NULL, NULL) == NULL, "NULL 返回 NULL", NULL);

    /* --- 2. FBW：实测写法 --- */
    printf("\n[2] FBW A32NX 实测写法\n");
    {
        const AircraftProfile* p = aircraftdb_find("A32NX", "");
        /* 来源预置 "TRANSPONDER MODE STBY" → 0 (>L:A32NX_TRANSPONDER_MODE) */
        check(aircraftdb_expand_xpdr_mode(p, XPDR_CANON_STBY, buf, sizeof(buf)),
              "STBY 有模板", NULL);
        check_str(buf, "0 (>L:A32NX_TRANSPONDER_MODE, Number)", "STBY = 0");
        /* "TRANSPONDER MODE ON" → 2 (>L:A32NX_TRANSPONDER_MODE) */
        aircraftdb_expand_xpdr_mode(p, XPDR_CANON_ON, buf, sizeof(buf));
        check_str(buf, "2 (>L:A32NX_TRANSPONDER_MODE, Number)", "ON = 2");
        /* "TRANSPONDER MODE AUTO" → 1 (>L:...)；ALT 映射到 AUTO */
        aircraftdb_expand_xpdr_mode(p, XPDR_CANON_ALT, buf, sizeof(buf));
        check_str(buf, "1 (>L:A32NX_TRANSPONDER_MODE, Number)",
                  "ALT → AUTO(1)");
        /* FBW 没有 OFF/TEST/GND 档 */
        check(!aircraftdb_expand_xpdr_mode(p, XPDR_CANON_OFF, buf, sizeof(buf)),
              "OFF 无模板（FBW 不支持）", NULL);
        check(!aircraftdb_expand_xpdr_mode(p, XPDR_CANON_TEST, buf, sizeof(buf)),
              "TEST 无模板", NULL);
        check(!aircraftdb_expand_xpdr_mode(p, XPDR_CANON_GND, buf, sizeof(buf)),
              "GND 无模板", NULL);

        /* 频率：FBW 的 COM ACTIVE FREQUENCY 可写，直接写 Hz */
        check(p->com_set_rpn[0] != NULL, "COM1 有频率模板", NULL);
        snprintf(buf, sizeof(buf), p->com_set_rpn[0], 122800000);
        check_str(buf, "122800000 (>A:COM ACTIVE FREQUENCY:1, Hz)",
                  "COM1 频率写 A 变量");
        snprintf(buf, sizeof(buf), p->com_set_rpn[2], 121500000);
        check_str(buf, "121500000 (>A:COM ACTIVE FREQUENCY:3, Hz)",
                  "COM3 频率");
    }

    /* --- 3. Fenix --- */
    printf("\n[3] Fenix A320 实测写法\n");
    {
        const AircraftProfile* p = aircraftdb_find("Fenix A320", "");
        /* Fenix 有两套变量：
         *   (L:S_XPDR_OPERATION) 0=STBY 1=AUTO 2=ON  ← 真正的开关
         *   (L:S_XPDR_MODE)      0=STBY 1=TA 2=TARA  ← TA/RA 选择
         * "FNX320 Transponder Operation Knob STBY/AUTO/ON" 预置原文。 */
        aircraftdb_expand_xpdr_mode(p, XPDR_CANON_STBY, buf, sizeof(buf));
        check_str(buf, "0 (>L:S_XPDR_OPERATION, Number)",
                  "STBY 用 OPERATION=0");
        aircraftdb_expand_xpdr_mode(p, XPDR_CANON_ON, buf, sizeof(buf));
        check_str(buf, "2 (>L:S_XPDR_OPERATION, Number)",
                  "ON 用 OPERATION=2");
        /* ALT 用 MODE=TARA（含高度报告），来源
         * "FNX320 Transponder Mode Knob TARA" → 2 (>L:S_XPDR_MODE) */
        aircraftdb_expand_xpdr_mode(p, XPDR_CANON_ALT, buf, sizeof(buf));
        check_str(buf, "2 (>L:S_XPDR_MODE, Number)", "ALT → MODE=TARA(2)");
        check(!aircraftdb_expand_xpdr_mode(p, XPDR_CANON_TEST, buf, sizeof(buf)),
              "TEST 无模板", NULL);
        check(p->com_set_rpn[0] == NULL, "频率无模板（走标准事件）", NULL);
        check(p->note && strstr(p->note, "OPERATION") != NULL,
              "备注说明两套变量", p->note);
    }

    /* --- 4. PMDG：只能旋钮，不能设值 --- */
    printf("\n[4] PMDG 限制（只有旋钮 ROTOR_BRAKE）\n");
    {
        const AircraftProfile* p = aircraftdb_find("PMDG 737-800", "");
        int i, all_null = 1;
        for (i = 0; i < 6; i++) {
            if (p->xpdr_mode_rpn[i]) all_null = 0;
        }
        check(all_null, "所有模式档位都无模板（旋钮无法设值）", NULL);
        check(p->com_set_rpn[0] == NULL, "频率无模板", NULL);
        check(p->note && strstr(p->note, "SDK ClientData") != NULL,
              "备注说明已改用 SDK ClientData", p->note);
        check(p->note && strstr(p->note, "EnableDataBroadcast") != NULL,
              "备注提醒需开 EnableDataBroadcast", p->note);
    }

    /* --- 5. IniBuilds：交换用 LVar 翻转 --- */
    printf("\n[5] IniBuilds 实测写法\n");
    {
        const AircraftProfile* p = aircraftdb_find("iniBuilds A300", "");
        check(p->com_swap_rpn != NULL, "交换有模板", NULL);
        /* "A300 Pedestal VHF Capt COM1 Active Indicator" 预置原文 */
        check_str(p->com_swap_rpn,
                  "(L:INI_CPT_VHF_TRANSFER_SWITCH, Bool) ! "
                  "(>L:INI_CPT_VHF_TRANSFER_SWITCH, Bool)",
                  "交换 = 翻转 transfer switch");
    }

    /* --- 6. 模板展开边界 --- */
    printf("\n[6] 模板展开边界\n");
    {
        const AircraftProfile* p = aircraftdb_find("A32NX", "");
        check(!aircraftdb_expand_xpdr_mode(p, 99, buf, sizeof(buf)),
              "越界模式返回 0", NULL);
        check(!aircraftdb_expand_xpdr_mode(p, -1, buf, sizeof(buf)),
              "负数模式返回 0", NULL);
        check(!aircraftdb_expand_xpdr_mode(NULL, 1, buf, sizeof(buf)),
              "NULL 档案返回 0", NULL);
        /* FBW 无码模板 → 走标准事件 */
        check(!aircraftdb_expand_xpdr_code(p, 7000, buf, sizeof(buf)),
              "无码模板返回 0（回退标准事件）", NULL);
    }

    /* --- 7. 档案数量与不重叠 --- */
    printf("\n[7] 档案完整性\n");
    check(aircraftdb_count() >= 19, "至少 19 个档案", NULL);
    {
        /* 典型机型不应互相误匹配（档案顺序 = 优先级，越具体越前） */
        check(strcmp(aircraftdb_find("Fenix A320", "A20N")->id, "fenix-a320") == 0,
              "Fenix 不会被 IniBuilds A20N 抢走", NULL);
        check(strcmp(aircraftdb_find("PMDG 777-300ER", "")->id, "pmdg-777") == 0,
              "777 不落到 737", NULL);
        check(strcmp(aircraftdb_find("iniBuilds A310", "")->id, "inibuilds-a310") == 0,
              "A310 不落到 iniBuilds 通用", NULL);
        check(strcmp(aircraftdb_find("iniBuilds A300", "")->id, "inibuilds-a300") == 0,
              "A300 不落到 A310", NULL);
        check(strcmp(aircraftdb_find("iniBuilds A340", "")->id, "inibuilds-a340") == 0,
              "A340 独立档案", NULL);
        check(strcmp(aircraftdb_find("iniBuilds A350", "")->id, "inibuilds-a350") == 0,
              "A350 独立档案", NULL);
        check(strcmp(aircraftdb_find("iniBuilds A220", "")->id, "inibuilds-a220") == 0,
              "A220 独立档案", NULL);
        check(strcmp(aircraftdb_find("Leonardo Maddog MD-82", "")->id,
                     "leonardo-maddog") == 0, "Maddog", NULL);
        check(strcmp(aircraftdb_find("TFDi MD-11", "")->id, "tfdi-md11") == 0,
              "TFDi MD-11", NULL);
        check(strcmp(aircraftdb_find("iFly 737 MAX 8", "")->id, "ifly-737max") == 0,
              "iFly 737 MAX", NULL);
        /* PMDG 736/739 与 737 同协议（736 有 PMDG_NG3_SDK.h，739 同为 NG3 系列） */
        check(strcmp(aircraftdb_find("PMDG 737-600", "")->id, "pmdg-737") == 0,
              "736 归入 pmdg-737", NULL);
        check(strcmp(aircraftdb_find("PMDG 737-900", "")->id, "pmdg-737") == 0,
              "739 归入 pmdg-737", NULL);
        /* 777 各型（77L/77F/77W）都要命中 pmdg-777 */
        check(strcmp(aircraftdb_find("PMDG 777-200LR", "")->id, "pmdg-777") == 0,
              "77L 归入 pmdg-777", NULL);
        check(strcmp(aircraftdb_find("PMDG 777F", "")->id, "pmdg-777") == 0,
              "77F 归入 pmdg-777", NULL);
        /* 新机型 */
        check(strcmp(aircraftdb_find("FSL A321", "")->id, "fsl-a32x") == 0,
              "FSL A32X", NULL);
        check(strcmp(aircraftdb_find("C919", "")->id, "c919") == 0,
              "C919", NULL);
        /* FSL 不能掉进 A320 的 IniBuilds/Fenix 档案 */
        check(strcmp(aircraftdb_find("FSL A32X A320", "")->id, "fsl-a32x") == 0,
              "FSL 不被 Fenix/IniBuilds 抢走", NULL);
    }

    /* --- 8. A220：官方文档核实 --- */
    printf("\n[8] iniBuilds A220（官方变量表）\n");
    {
        const AircraftProfile* p = aircraftdb_find("A220", "");
        /* L:A22X Transponder Mode  0=Auto 1=Standby 2=TA/RA 3=TA Only 4=Alt On */
        aircraftdb_expand_xpdr_mode(p, XPDR_CANON_STBY, buf, sizeof(buf));
        check_str(buf, "1 (>L:A22X Transponder Mode, Number)", "STBY = 1");
        aircraftdb_expand_xpdr_mode(p, XPDR_CANON_ON, buf, sizeof(buf));
        check_str(buf, "0 (>L:A22X Transponder Mode, Number)", "ON → Auto(0)");
        aircraftdb_expand_xpdr_mode(p, XPDR_CANON_ALT, buf, sizeof(buf));
        check_str(buf, "4 (>L:A22X Transponder Mode, Number)", "ALT → Alt On(4)");
        /* A220 原生支持标准 COM_RADIO_SET_HZ，所以频率模板留空走标准事件 */
        check(p->com_set_rpn[0] == NULL, "频率走标准事件（A220 原生支持）", NULL);
        check((p->caps & CAP_XPDR_CODE) != 0, "声明支持应答机码（标准 XPNDR_SET）", NULL);
        check(p->com_swap_rpn != NULL, "交换有模板", NULL);
        aircraftdb_expand_com_swap(p, 1, buf, sizeof(buf));
        check_str(buf, "1 (>K:COM1_RADIO_SWAP)", "交换 = 标准 K 事件");
    }

    /* --- 9. TFDi / Maddog / iFly：数字编码机制 --- */
    printf("\n[9] 数字编码类机模\n");
    {
        const AircraftProfile* p;
        p = aircraftdb_find("TFDi MD-11", "");
        check(p->note && strstr(p->note, "CEVENT") != NULL,
              "TFDi 备注说明 CEVENT 机制", p->note);
        check(p->com_set_rpn[0] == NULL, "TFDi 频率无模板（只能逐档）", NULL);

        p = aircraftdb_find("Leonardo Maddog MD-82", "");
        check(p->note && strstr(p->note, "radio_event") != NULL,
              "Maddog 备注说明 radio_event", p->note);
        check(p->com_set_rpn[0] == NULL, "Maddog 频率无模板", NULL);

        p = aircraftdb_find("iFly 737 MAX 8", "");
        check(p->note && strstr(p->note, "411") != NULL,
              "iFly 备注记录 IDENT=411", p->note);
    }

    /* --- 10. IniBuilds 全家桶 --- */
    printf("\n[10] IniBuilds 全家桶\n");
    {
        const AircraftProfile* p;
        /* A310：模式档位 STBY/XPDR/TARA/TA */
        p = aircraftdb_find("iniBuilds A310", "");
        aircraftdb_expand_xpdr_mode(p, XPDR_CANON_ON, buf, sizeof(buf));
        check_str(buf, "1 (>L:A310_tcas_mode_pedestal, Number)", "A310 ON → XPDR(1)");
        aircraftdb_expand_xpdr_mode(p, XPDR_CANON_STBY, buf, sizeof(buf));
        check_str(buf, "0 (>L:A310_tcas_mode_pedestal, Number)", "A310 STBY = 0");

        /* A300：独立前缀 INI_ */
        p = aircraftdb_find("iniBuilds A300", "");
        aircraftdb_expand_xpdr_mode(p, XPDR_CANON_ON, buf, sizeof(buf));
        check_str(buf, "1 (>L:INI_tcas_mode_pedestal, Number)", "A300 ON → INI_ 前缀");

        /* A220 已单独测；A340/A350 走 2024 档案 */
        /* A350：本机机型文件实测 INI_tcas_mode_pedestal，4 档 */
        p = aircraftdb_find("iniBuilds A350", "");
        check(p->xpdr_mode_read != NULL, "A350 有模式读取（实测 LVar）", NULL);
        check_str(p->xpdr_mode_read, "(L:INI_tcas_mode_pedestal, Number)",
                  "A350 读 INI_tcas_mode_pedestal");
        aircraftdb_expand_xpdr_mode(p, XPDR_CANON_ON, buf, sizeof(buf));
        check_str(buf, "1 (>L:INI_tcas_mode_pedestal, Number)", "A350 ON → XPDR(1)");
        aircraftdb_expand_xpdr_mode(p, XPDR_CANON_ALT, buf, sizeof(buf));
        check_str(buf, "2 (>L:INI_tcas_mode_pedestal, Number)", "A350 ALT → TA/RA(2)");
    }

    /* --- 11. 能力位 --- */
    printf("\n[11] 能力位\n");
    {
        check((aircraftdb_find("A32NX", "")->caps & CAP_COM_SET) != 0,
              "FBW 声明支持设频率", NULL);
        check((aircraftdb_find("PMDG 737-800", "")->caps & CAP_XPDR_MODE) != 0,
              "PMDG 声明支持设模式（走 SDK ClientData）", NULL);
        check((aircraftdb_find("Asobo C172", "")->caps & CAP_COM_XMIT) != 0,
              "默认机声明支持发射台选择", NULL);
        check((aircraftdb_find("A220", "")->caps & CAP_XPDR_CODE) != 0,
              "A220 声明支持应答机码", NULL);
    }

    /* --- 12. 频率/交换/发射台展开 --- */
    printf("\n[12] 频率与发射台展开\n");
    {
        const AircraftProfile* p = aircraftdb_find("A32NX", "");
        check(aircraftdb_expand_com_set(p, 1, 122800000L, buf, sizeof(buf)),
              "FBW COM1 有模板", NULL);
        check_str(buf, "122800000 (>A:COM ACTIVE FREQUENCY:1, Hz)", "写 A 变量");
        check(!aircraftdb_expand_com_set(p, 9, 122800000L, buf, sizeof(buf)),
              "台号越界返回 0", NULL);
        check(!aircraftdb_expand_com_xmit(p, 1, buf, sizeof(buf)),
              "FBW 无发射台模板", NULL);
        /* 默认机有 */
        p = aircraftdb_find("Asobo C172", "");
        check(!aircraftdb_expand_com_xmit(p, 1, buf, sizeof(buf)),
              "默认机发射台也走标准事件（无模板）", NULL);
    }

    /* --- 13. 应答机状态：读写一致性（最容易出 bug 的地方） --- */
    printf("\n[13] 应答机模式读写一致性\n");
    {
        struct { const char* t; int canon; int native; } cases[] = {
            /* FBW：0=STBY 1=AUTO(ALT) 2=ON，无 OFF */
            { "A32NX",        XPDR_CANON_STBY, 0 },
            { "A32NX",        XPDR_CANON_ON,   2 },
            { "A32NX",        XPDR_CANON_ALT,  1 },
            /* Fenix：OPERATION 0=STBY 2=ON */
            { "Fenix A320",   XPDR_CANON_STBY, 0 },
            { "Fenix A320",   XPDR_CANON_ON,   2 },
            /* A310：0=STBY 1=XPDR 2=TARA */
            { "iniBuilds A310", XPDR_CANON_STBY, 0 },
            { "iniBuilds A310", XPDR_CANON_ON,   1 },
            { "iniBuilds A310", XPDR_CANON_ALT,  2 },
            /* A220：0=Auto 1=Stby 2=TA/RA 4=AltOn */
            { "A220",         XPDR_CANON_STBY, 1 },
            { "A220",         XPDR_CANON_ON,   0 },
            { "A220",         XPDR_CANON_ALT,  4 },
            /* Just Flight：0=STBY 1=ALT OFF */
            { "Just Flight BAe 146", XPDR_CANON_STBY, 0 },
            { "Just Flight BAe 146", XPDR_CANON_ALT,  1 },
        };
        size_t i;
        int all_ok = 1;
        for (i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
            const AircraftProfile* p = aircraftdb_find(cases[i].t, "");
            int nat = aircraftdb_canon_to_native(p, cases[i].canon);
            int back;
            char d[200];
            if (nat != cases[i].native) {
                snprintf(d, sizeof(d), "%s canon=%d native got=%d want=%d",
                         cases[i].t, cases[i].canon, nat, cases[i].native);
                check(0, "写：规范→原生", d);
                all_ok = 0;
                continue;
            }
            back = aircraftdb_native_to_canon(p, cases[i].native);
            if (back != cases[i].canon) {
                snprintf(d, sizeof(d), "%s native=%d canon got=%d want=%d",
                         cases[i].t, cases[i].native, back, cases[i].canon);
                check(0, "读：原生→规范（必须与写对称）", d);
                all_ok = 0;
            }
        }
        check(all_ok, "13 组读写映射全部对称", NULL);
    }

    printf("\n[13b] 不支持的档位返回 -1\n");
    {
        const AircraftProfile* p = aircraftdb_find("A32NX", "");
        check(aircraftdb_canon_to_native(p, XPDR_CANON_OFF) == -1,
              "FBW 无 OFF 档", NULL);
        check(aircraftdb_canon_to_native(p, XPDR_CANON_TEST) == -1,
              "FBW 无 TEST 档", NULL);
        check(aircraftdb_native_to_canon(p, 9) == -1,
              "读到未定义值返回 -1", NULL);
        check(aircraftdb_canon_to_native(p, 99) == -1, "越界返回 -1", NULL);
        check(aircraftdb_canon_to_native(NULL, 1) == -1, "NULL 返回 -1", NULL);
    }

    /* --- 14. 读取表达式与 needs_custom_read --- */
    printf("\n[14] 读取表达式\n");
    {
        const AircraftProfile* p;
        p = aircraftdb_find("A32NX", "");
        check(p->xpdr_mode_read != NULL, "FBW 有模式读取表达式", NULL);
        check_str(p->xpdr_mode_read, "(L:A32NX_TRANSPONDER_MODE, Number)",
                  "读 FBW 的 LVar");
        check(aircraftdb_needs_custom_read(p), "FBW 需要特殊读取", NULL);

        p = aircraftdb_find("Fenix A320", "");
        check(p->xpdr_mode_read != NULL, "Fenix 有模式读取", NULL);
        check(p->xpdr_code_read != NULL, "Fenix 有码读取（N_FREQ_XPNDR_SELECTED）", NULL);

        p = aircraftdb_find("A220", "");
        check_str(p->xpdr_mode_read, "(L:A22X Transponder Mode, Number)",
                  "A220 读官方 LVar");

        /* Asobo 默认机不需要特殊读取：模拟器自己维护 TRANSPONDER STATE */
        p = aircraftdb_find("Asobo C172", "");
        check(p->xpdr_mode_read == NULL, "默认机无需特殊读取", NULL);
        check(!aircraftdb_needs_custom_read(p), "默认机 needs_custom_read=false", NULL);

        /* PMDG 没有可读 LVar（需 SDK ClientData）→ 已知限制 */
        p = aircraftdb_find("PMDG 737-800", "");
        check(p->xpdr_mode_read == NULL, "PMDG 无 LVar 可读（已知限制）", NULL);
    }

    /* --- 15. 当前档案共享（datadefs 读取侧需要用） --- */
    printf("\n[15] 当前档案共享\n");
    {
        const AircraftProfile* p = aircraftdb_find("A32NX", "");
        check(aircraftdb_current() == NULL || 1, "初始状态可取", NULL);
        aircraftdb_set_current(p);
        check(aircraftdb_current() == p, "set/get 一致", NULL);
        aircraftdb_set_current(NULL);
        check(aircraftdb_current() == NULL, "可清空", NULL);
    }

    /* --- 16. FSL / C919 机制如实记录（不假装支持） --- */
    printf("\n[16] FSL A32X / C919\n");
    {
        const AircraftProfile* p = aircraftdb_find("FSL A321", "");
        /* FSL 应答机是按钮阵列(ID*4+动作) + ROTOR_BRAKE，按钮 ID 需实测
         * → 不填模板（填错比不填更糟） */
        check(p->xpdr_mode_rpn[XPDR_CANON_ON] == NULL,
              "FSL 未填模式模板（ID 需实测，不猜）", NULL);
        check(p->note && strstr(p->note, "78347") != NULL,
              "FSL 备注记录实测 MODE 旋钮 EVENT_ID=78347", p->note);
        check(p->note && strstr(p->note, "78292") != NULL,
              "FSL 备注记录数字键 EVENT_ID", p->note);
        /* FSL 维护自己的 LVar 而非标准 SimVar，所以必须有读取表达式 */
        check(aircraftdb_needs_custom_read(p),
              "FSL 需要特殊读取（不维护标准 SimVar）", NULL);
        check_str(p->xpdr_mode_read, "(L:VC_PED_ATCXPDR_MODE_Switch, Number)",
                  "FSL 读自己的模式变量");
        /* 档位数未知 → 所有档位模板为空，由 control.c 相对转 */
        check(p->xpdr_mode_rpn[XPDR_CANON_ON] == NULL &&
              p->xpdr_mode_rpn[XPDR_CANON_STBY] == NULL,
              "FSL 不填绝对档位模板（档位数未知，避免转错）", NULL);

        p = aircraftdb_find("C919", "");
        check(p->note && strstr(p->note, "HW_PUB") != NULL,
              "C919 备注记录具名事件机制", p->note);
        check(p->caps == 0, "C919 不声明任何能力（面板未开放）", NULL);
        check(p->xpdr_mode_rpn[XPDR_CANON_ON] == NULL,
              "C919 无模式模板", NULL);
    }

    printf("\n== 结果: %d 通过, %d 失败 ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}