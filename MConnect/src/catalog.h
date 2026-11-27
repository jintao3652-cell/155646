/* MConnect — 机型/涂装目录与四级匹配。
 *
 * XConnect 用 CSL 包 + `xsb_aircraft.txt` 做机型匹配；MSFS 没有这个概念，
 * 必须换成「容器标题（container title）+ 涂装名（livery）」：
 *
 *   AICreateNonATCAircraft(h, szContainerTitle, ...)          ← 2020 legacy
 *   AICreateNonATCAircraft_EX1(h, szContainerTitle, szLivery, ...)  ← 2024 模块化
 *
 * 目录来源三处，合并去重：
 *   1. 包扫描：Community / Official 下所有 SimObjects/<机型>/aircraft.cfg
 *      每个 [FLTSIM.N] 节 = 一个可创建的条目（2020 与 2024 legacy 包都适用）
 *      ——标题取自 title=，ICAO 取自 icao_type_designator，航司取自 atc_airline/
 *      icao_airline，尾号取自 atc_id。
 *   2. 2024 模块化：livery.cfg 只给涂装名，容器标题靠 EnumerateSimObjectsAndLiveries
 *      在运行时枚举（权限、加密包等情况只有模拟器自己知道）。
 *   3. 内置兜底表：模拟器自带的 Asobo 机型（未装第三方包时也能显示他机）。
 *
 * 匹配沿用 XConnect 的四级规则（REG → AIRLINE → TYPE → FALLBACK），
 * 只是把"变体"换成"容器标题 + 涂装"。
 */
#ifndef MCONNECT_CATALOG_H
#define MCONNECT_CATALOG_H

#include "mconnect.h"

/* AI 模型库来源。扫描时按包名标记，匹配时按用户选择优先使用。
 * 两套库都装了也不冲突：偏好库优先，未命中才回退其他来源，避免白机。 */
typedef enum {
    CAT_SRC_OTHER = 0,   /* 其它包（PMDG/Fenix 涂装等）/ 内置兜底 */
    CAT_SRC_FSLTL = 1,   /* fsltl-* 包 */
    CAT_SRC_AIG   = 2    /* aig-* / aigaim 包 */
} CatalogSource;

/* 模型库偏好（设置项 msfs_traffic_lib 与 --traffic-lib 共用） */
#define CAT_LIB_AUTO  0
#define CAT_LIB_FSLTL 1
#define CAT_LIB_AIG   2

/* 单条目：一个 (容器标题, 涂装) 组合，代表"能创建出来的一架具体飞机" */
typedef struct {
    char title[112];    /* 容器标题（AICreate 的第 1 个参数） */
    char livery[64];    /* 涂装名（2024 模块化机型用；legacy 为空） */
    char icao[8];       /* 机型 ICAO（icao_type_designator） */
    char airline[8];    /* 航司码（atc_airline / icao_airline） */
    char reg[16];       /* 绑定尾号（atc_id，空 = 通用） */
    unsigned char modular;   /* 1 = 需要走 _EX1 + livery */
    unsigned char source;    /* CatalogSource：来自哪套 AI 模型库 */
} CatalogEntry;

/* 匹配结果 */
typedef struct {
    int  found;
    int  level;              /* 1=REG 2=AIRLINE 3=TYPE 4=FALLBACK，0=未命中 */
    char title[112];
    char livery[64];
    unsigned char modular;
    unsigned char source;    /* CatalogSource：实际命中的模型库 */
} CatalogMatch;

/* 启动目录扫描（后台线程；非阻塞返回）。dir_override 为空则自动探测。 */
void catalog_start_scan(const char* dir_override);

/* 运行时设置 AI 模型库偏好：CAT_LIB_AUTO / FSLTL / AIG。
 * 只影响"优先使用哪套库"，不会丢弃另一套的条目；偏好库无匹配时自动回退。
 * 网络线程可调用（内部原子变量 + 目录锁）。 */
void  catalog_set_lib(int lib);
int   catalog_lib(void);
const char* catalog_lib_name(int lib);
/* 条目来源名（FSLTL / AIG / OTHER），用于 AI 匹配日志。 */
const char* catalog_source_name(int source);

/* 扫描是否完成（未完成时匹配会退到内置兜底表）。 */
int  catalog_ready(void);

/* 条目数量（诊断用）。 */
int  catalog_count(void);

/* 内置兜底表条目数。 */
int  catalog_builtin_count(void);

/* 四级匹配：为 (icao, airline, registration) 选一个可创建条目。
 * 找不到返回 level=0。线程安全。 */
void catalog_match(const char* icao, const char* airline, const char* reg,
                   CatalogMatch* out);

/* 由本机 TITLE 反查 ICAO（机型识别用）。找不到返回 NULL。
 * 内部顺序：目录精确匹配 → 内置表 → title 关键词猜测（见下）。 */
const char* catalog_icao_for_title(const char* title);

/* title 关键词机型猜测（子串匹配，处理 FenixA320 这类连写涂装 title）。
 * 精确反查失败时的兜底，独立暴露便于测试与调用方分级打日志。 */
const char* catalog_icao_from_title_keywords(const char* title);

/* 非标准机型码 → 标准 ICAO（A350-900→A359 等）。查不到返回 NULL，
 * 调用者保留原值。上报服务器前必须过一遍。 */
const char* catalog_canonical_icao(const char* icao);

/* 机型串清洗：仅保留字母数字与 '-'，转大写，2~6 位（ICAO 机型代码格式）。 */
void catalog_sanitize_icao(const char* raw, char* out, int cap);

/* 2024：把 EnumerateSimObjectsAndLiveries 的结果并入目录（由派发回调调用）。 */
void catalog_add_enumerated(const char* title, const char* livery);

/* 让扫描线程退出（程序结束时调用，最多等待 timeout_ms）。 */
void catalog_stop(int timeout_ms);

/* 写诊断：把匹配到的前 N 条与统计写日志（排查"他机不显示"用）。 */
void catalog_dump_stats(void);

#endif /* MCONNECT_CATALOG_H */