/* MConnect 机型匹配单元测试（不需要模拟器、不需要 SimConnect.dll）。
 *
 * 覆盖 MSFS 与 X-Plane 差异最大的部分：容器标题 + 涂装的四级匹配。
 * 在临时目录造 4 个假机型包，断言：
 *   1. 尾号精确命中（REG）
 *   2. 同航司命中（AIRLINE），且同航司多套涂装时优先不绑尾号的那套
 *   3. 同机型白机兜底（TYPE）
 *   4. 机型别名 → 同族（FALLBACK）
 *   5. 找不到机型 → 内置通用表
 *   6. 2020 环境下模块化条目被跳过（只能选 legacy）
 *
 * 编译见 CMakeLists.txt 的 catalog_test target；直接运行输出每条断言结果，
 * 全通过退出码 0。
 */
#include "catalog.h"
#include "sc.h"
#include "mconnect.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

/* catalog.c 引用的外部符号 */
ScApi g_sc;
void* g_players_lock = NULL;
void* g_sock_lock = NULL;

static int g_fail = 0;
static int g_pass = 0;

double xc_now_sec(void) {
    static LARGE_INTEGER freq;
    static int init = 0;
    LARGE_INTEGER now;
    if (!init) { QueryPerformanceFrequency(&freq); init = 1; }
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

void xc_logf(const char* fmt, ...) {
    /* 测试时把日志打到 stdout，便于定位失败 */
    va_list ap;
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

static const char* g_exe = ".";

const char* xc_exe_dir(void) { return g_exe; }

/* ---------------- 断言 ---------------- */

static void check(int cond, const char* what, const char* detail) {
    if (cond) {
        g_pass++;
        printf("  PASS  %s\n", what);
    } else {
        g_fail++;
        printf("  FAIL  %s   [%s]\n", what, detail ? detail : "");
    }
}

static void check_str(const char* got, const char* want, const char* what) {
    char d[256];
    int ok = (got && want && _stricmp(got, want) == 0);
    snprintf(d, sizeof(d), "got=\"%s\" want=\"%s\"", got ? got : "(null)",
             want ? want : "(null)");
    check(ok, what, d);
}

/* ---------------- 假机型包 ---------------- */

static void mkdir_p(const char* path) {
    char tmp[MAX_PATH];
    char* p;
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (p = tmp + 3; *p; p++) {
        if (*p == '\\') {
            *p = '\0';
            CreateDirectoryA(tmp, NULL);
            *p = '\\';
        }
    }
    CreateDirectoryA(tmp, NULL);
}

static void write_file(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    if (!f) {
        printf("  !! 无法写入 %s\n", path);
        return;
    }
    fputs(text, f);
    fclose(f);
}

/* 造一个 2020 风格机型包：aircraft.cfg 含 [GENERAL] + 多个 [FLTSIM.N] */
static void make_package(const char* root, const char* folder,
                         const char* icao, const char* fltsim_blocks,
                         const char* extra_general) {
    char dir[MAX_PATH], cfg[MAX_PATH], body[8192];

    snprintf(dir, sizeof(dir), "%s\\SimObjects\\Airplanes\\%s", root, folder);
    mkdir_p(dir);
    snprintf(cfg, sizeof(cfg), "%s\\aircraft.cfg", dir);

    snprintf(body, sizeof(body),
             "[VERSION]\r\nmajor = 1\r\nminor = 0\r\n\r\n"
             "[GENERAL]\r\n"
             "atc_model = \"TT:ATCCOM.AC_MODEL_%s.0.text\"\r\n"
             "icao_type_designator = \"%s\"\r\n"
             "%s"
             "\r\n%s",
             icao, icao, extra_general ? extra_general : "", fltsim_blocks);
    write_file(cfg, body);
}

/* 造一个 FLTSIM 节 */
static const char* fltsim(const char* title, const char* texture,
                          const char* icao_airline, const char* atc_id) {
    static char buf[8][1024];
    static int idx = 0;
    char* b = buf[idx % 8];
    idx++;
    snprintf(b, 1024,
             "[FLTSIM.%d]\r\n"
             "title = \"%s\" ; name\r\n"
             "texture = \"%s\" ; tex\r\n"
             "atc_id = \"%s\" ; tail\r\n"
             "icao_airline = \"%s\" ; airline\r\n"
             "ui_variation = \"%s\"\r\n"
             "\r\n",
             idx - 1, title, texture, atc_id ? atc_id : "",
             icao_airline ? icao_airline : "", texture);
    return b;
}

int main(void) {
    char root[MAX_PATH];
    char g[4096];
    CatalogMatch m;

    snprintf(root, sizeof(root), "%s\\mconnect_catalog_test_%lu",
             getenv("TEMP") ? getenv("TEMP") : ".",
             (unsigned long)GetCurrentProcessId());

    printf("== MConnect catalog 匹配测试 ==\n");
    printf("   假包目录: %s\n\n", root);

    /* 包 1：B738，含白机 + 两家航司 + 一个绑定尾号的专属涂装 */
    g[0] = '\0';
    {
        char all[4096] = "";
        strncat(all, fltsim("PMDG 737-800 ZZZZ", "ZZZZ", "", ""),
                sizeof(all) - strlen(all) - 1);
        strncat(all, fltsim("PMDG 737-800 CCA", "CCA", "CCA", ""),
                sizeof(all) - strlen(all) - 1);
        strncat(all, fltsim("PMDG 737-800 CCA B-1234", "CCA_B1234", "CCA", "B-1234"),
                sizeof(all) - strlen(all) - 1);
        strncat(all, fltsim("PMDG 737-800 CES", "CES", "CES", ""),
                sizeof(all) - strlen(all) - 1);
        make_package(root, "PMDG_B738", "B738", all, "");
    }

    /* 包 2：A20N */
    {
        char all[2048] = "";
        strncat(all, fltsim("FSLTL A320 ZZZZ", "ZZZZ", "ZZZZ", ""),
                sizeof(all) - strlen(all) - 1);
        strncat(all, fltsim("FSLTL A320 CSN", "CSN", "CSN", ""),
                sizeof(all) - strlen(all) - 1);
        make_package(root, "FSLTL_A20N", "A20N", all, "");
    }

    /* 包 2b：AIG 同名机型（A20N 的 CCA 涂装），用于验证库偏好 */
    {
        char all[2048] = "";
        strncat(all, fltsim("AIG A320 CCA", "CCA", "CCA", ""),
                sizeof(all) - strlen(all) - 1);
        make_package(root, "AIG_A20N", "A20N", all, "");
    }

    /* 包 3：白机机型（只用于别名回退的落点） */
    {
        char all[1024] = "";
        strncat(all, fltsim("FSLTL E175 ZZZZ", "ZZZZ", "ZZZZ", ""),
                sizeof(all) - strlen(all) - 1);
        make_package(root, "FSLTL_E175", "E175", all, "");
    }

    /* 包 4：2024 模块化机型（只有 livery.cfg，无 FLTSIM）——用于验证
     * 2020 模式下不会被选中 */
    {
        char dir[MAX_PATH], cfg[MAX_PATH];
        snprintf(dir, sizeof(dir), "%s\\SimObjects\\Airplanes\\MODULAR_TEST", root);
        mkdir_p(dir);
        snprintf(cfg, sizeof(cfg), "%s\\aircraft.cfg", dir);
        write_file(cfg,
                   "[VERSION]\r\nmajor = 1\r\nminor = 0\r\n\r\n"
                   "[GENERAL]\r\n"
                   "icao_type_designator = \"A359\"\r\n"
                   "title = \"Modular A350 Test\"\r\n");
    }

    /* 包 5：占位符 ICAO 包 —— 机模作者没写 icao_type_designator 时 MSFS
     * 给的默认值就是 ATCCOM（实测 Fenix A320）。目录条目会原样存下
     * "ATCCOM"，反查时**不能**把它当识别结果返回。 */
    {
        char all[2048] = "";
        strncat(all, fltsim("FenixA320 Placeholder Livery", "PH1", "", ""),
                sizeof(all) - strlen(all) - 1);
        strncat(all, fltsim("No Keyword Placeholder", "PH2", "", ""),
                sizeof(all) - strlen(all) - 1);
        make_package(root, "PLACEHOLDER_TEST", "ATCCOM", all, "");
    }

    /* 扫描 */
    g_sc.supports_modular = 0;     /* 先按 2020 测 */
    catalog_start_scan(root);
    {
        int waited = 0;
        while (!catalog_ready() && waited < 5000) {
            Sleep(50);
            waited += 50;
        }
        printf("   扫描完成: 条目数 %d（内置表 %d）\n\n",
               catalog_count(), catalog_builtin_count());
    }
    catalog_dump_stats();

    /* --- 1. 尾号精确匹配（REG） --- */
    printf("\n[1] 尾号匹配（B738 / CCA / B-1234）\n");
    catalog_match("B738", "CCA", "B-1234", &m);
    check(m.found == 1, "命中", NULL);
    check(m.level == 1, "级别 = REG(1)", NULL);
    check_str(m.title, "PMDG 737-800 CCA B-1234", "选中绑定尾号的专属涂装");

    /* --- 2. 同航司（AIRLINE） --- */
    printf("\n[2] 同航司匹配（B738 / CCA / 无尾号）\n");
    catalog_match("B738", "CCA", "", &m);
    check(m.found == 1, "命中", NULL);
    check(m.level == 2, "级别 = AIRLINE(2)", NULL);
    check_str(m.title, "PMDG 737-800 CCA", "同航司多套涂装时优先不绑尾号的那套");

    /* --- 2b. 别的航司不串漆 --- */
    printf("\n[2b] 未知航司不串漆（B738 / XXX）\n");
    catalog_match("B738", "XXX", "", &m);
    check(m.found == 1, "命中", NULL);
    check(m.level == 3, "级别 = TYPE(3)", NULL);
    check(strcmp(m.title, "PMDG 737-800 CCA") != 0
          && strcmp(m.title, "PMDG 737-800 CES") != 0,
          "未给未知航司挂别家涂装", m.title);

    /* --- 3. 同机型白机（TYPE） --- */
    printf("\n[3] 同机型白机（A20N / 空航司）\n");
    catalog_match("A20N", "", "", &m);
    check(m.found == 1, "命中", NULL);
    check_str(m.title, "FSLTL A320 ZZZZ", "落到白机");

    /* --- 4. 机型别名回退（FALLBACK） --- */
    printf("\n[4] 别名回退（E75L → E175）\n");
    catalog_match("E75L", "", "", &m);
    check(m.found == 1, "命中", NULL);
    check(m.level == 4, "级别 = FALLBACK(4)", NULL);
    check_str(m.title, "FSLTL E175 ZZZZ", "同族机型");

    /* --- 4b. 别名回退 + 航司保留（B735 → B738，且保留 CCA） --- */
    printf("\n[4b] 别名回退 + 航司保留（B735 / CCA）\n");
    catalog_match("B735", "CCA", "", &m);
    check(m.found == 1, "命中", NULL);
    check(m.level == 4, "级别 = FALLBACK(4)", NULL);
    check_str(m.title, "PMDG 737-800 CCA", "别名落到 B738 且保留同航司");

    /* --- 4b2. E19L（E190LR 写法）走别名回退到 E195 同族 --- */
    printf("\n[4b2] E19L 别名回退\n");
    catalog_match("E19L", "", "", &m);
    check(m.found == 1, "命中", NULL);
    check(m.level == 4, "级别 = FALLBACK(4)", NULL);
    check_str(m.title, "Embraer E195 Asobo", "E19L -> E195 同族");

    /* --- 4c. 内置表有精确机型时优于别名（B38M 是 737 MAX 8） --- */
    printf("\n[4c] 精确机型优先于别名（B38M）\n");
    catalog_match("B38M", "CCA", "", &m);
    check(m.found == 1, "命中", NULL);
    check_str(m.title, "Boeing 737 MAX 8 Asobo", "有精确机型时不用同族替代");

    /* --- 5. 完全未知机型 → 内置通用表 --- */
    printf("\n[5] 未知机型（XXXX / 空）\n");
    catalog_match("XXXX", "", "", &m);
    check(m.found == 1, "命中", NULL);
    check(m.level == 4, "级别 = FALLBACK(4)", NULL);
    check(m.title[0] != '\0', "有可创建标题（非空）", m.title);

    /* --- 6. 2020 环境跳过模块化条目 --- */
    printf("\n[6] 2020 环境不选模块化机型（A359）\n");
    catalog_match("A359", "", "", &m);
    if (m.found) {
        check(m.modular == 0 || _stricmp(m.title, "Modular A350 Test") != 0,
              "2020 下不选需要 _EX1 的模块化条目", m.title);
    } else {
        check(1, "未命中模块化机型（可接受）", NULL);
    }

    /* --- 7. 2024 环境可用模块化条目 --- */
    printf("\n[7] 2024 环境可用模块化机型（A359）\n");
    g_sc.supports_modular = 1;
    catalog_match("A359", "", "", &m);
    check(m.found == 1, "命中", NULL);
    check_str(m.title, "Modular A350 Test", "选中模块化机型标题");

    /* --- 8. 机型串清洗 --- */
    printf("\n[8] 机型串清洗\n");
    {
        char out[16];
        catalog_sanitize_icao(" b738/w ", out, sizeof(out));
        check_str(out, "B738", "\" b738/w \" → B738");
        catalog_sanitize_icao("A20N", out, sizeof(out));
        check_str(out, "A20N", "A20N 保持");
        catalog_sanitize_icao("1", out, sizeof(out));
        check_str(out, "", "过短丢弃");
    }

    /* --- 9. 本机 TITLE 反查 ICAO --- */
    printf("\n[9] TITLE 反查 ICAO\n");
    check_str(catalog_icao_for_title("PMDG 737-800 CCA"), "B738",
              "目录条目反查");
    check_str(catalog_icao_for_title("Cessna Skyhawk G1000 Asobo"), "C172",
              "内置表反查");
    /* 关键词兜底：标题连写、涂装名打头时仍要认出机型。
     * 关键词表是大写、title 先转小写，两边都必须归一化（否则永远不命中）。 */
    check_str(catalog_icao_for_title(
                  "FenixA320 IAE SL Anon x Mika TRINITY AIRLINES"), "A320",
              "Fenix A320 连写涂装标题（关键词兜底）");
    check_str(catalog_icao_for_title("iniBuilds A350-900 Air China"), "A359",
              "A350-900 长词优先，不能被 A350/A320 抢先");
    check(catalog_icao_for_title("Unknown Aircraft XYZ") == NULL,
          "未知机型不误判", NULL);

    /* --- 10. AI 模型库偏好 --- */
    printf("\n[10] AI 模型库偏好（FSLTL / AIG / 回退）\n");
    catalog_set_lib(CAT_LIB_FSLTL);
    catalog_match("A20N", "CCA", "", &m);
    check(m.found == 1, "偏好 FSLTL 时命中", NULL);
    check_str(m.title, "FSLTL A320 ZZZZ", "偏好库优先于 AIG 的航司涂装");
    check(m.source == CAT_SRC_FSLTL, "来源标记为 FSLTL", NULL);

    catalog_set_lib(CAT_LIB_AIG);
    catalog_match("A20N", "CCA", "", &m);
    check(m.found == 1, "偏好 AIG 时命中", NULL);
    check_str(m.title, "AIG A320 CCA", "偏好 AIG 时选中 AIG 涂装");
    check(m.source == CAT_SRC_AIG, "来源标记为 AIG", NULL);

    /* 偏好库没有该机型 → 回退另一套库，不能退化成白机 */
    catalog_match("E175", "", "", &m);
    check(m.found == 1, "偏好库缺失时回退命中", NULL);
    check_str(m.title, "FSLTL E175 ZZZZ", "回退到 FSLTL 机型");
    catalog_set_lib(CAT_LIB_AUTO);

    /* 汇总 */
    printf("\n== 结果: %d 通过, %d 失败 ==\n", g_pass, g_fail);
    catalog_stop(2000);

    /* 清理临时目录（不递归删除用户目录，仅删除本测试创建的树） */
    {
        char cmd[MAX_PATH + 64];
        snprintf(cmd, sizeof(cmd), "cmd /c rmdir /s /q \"%s\"", root);
        system(cmd);
    }

    return g_fail == 0 ? 0 : 1;
}