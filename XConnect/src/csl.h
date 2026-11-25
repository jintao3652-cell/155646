/* CSL 模型加载与实例化渲染（双版本兼容：SDK 430 新 API / 旧 SDK 旧 API）。
 *
 * 模型布局（相对 X-System 根，X-CSL 风格）：
 *   Resources/plugins/XConnect/CSL/{ICAO}/xsb_aircraft.txt
 *   Resources/plugins/XConnect/CSL/{ICAO}/{OBJ...}.obj
 *
 * X-CSL 的一架飞机是**复合模型**：一个变体（OBJ8_AIRCRAFT）由多个 OBJ8 组成
 * （机身涂装 + 风扇/发动机等），由 xsb_aircraft.txt 的 OBJ8 行定义，例如：
 *   OBJ8_AIRCRAFT A346_A346FUN
 *   OBJ8 SOLID YES A346:A346fan.obj     -> 发动机/风扇
 *   OBJ8 SOLID YES A346\A346_FUN.obj    -> 机身（含涂装贴图）
 *   AIRLINE A346 FUN
 * 涂装由 AIRLINE/LIVERY 行的第三个字段标识（本工程按呼号前三位匹配）。
 *
 * SDK 版本差异（kXPLM_Version 判定）：
 *   >= 430: XPLMLoadObject + XPLMCreateInstance + XPLMDrawInfo_t
 *   <  430: XPLMInstanceCreate(path, datarefs, count) + float pos[9]
 */
#ifndef XCONNECT_CSL_H
#define XCONNECT_CSL_H

#include "XPLMInstance.h"

#define CSL_MAX_DATAREFS 48  /* XPMP2 标准动画 dataref 全集（libxplanemp/...）*/
#define CSL_MAX_OBJS 4       /* 单个 CSL 变体最多包含的 OBJ 数（风扇+涂装…） */

/* 玩家渲染实例（由主程序持有）。一个 CSL 变体可能含多个 OBJ，逐个实例化。 */
typedef struct {
    XPLMInstanceRef insts[CSL_MAX_OBJS]; /* 各 OBJ 的实例（NULL 未使用） */
    int             inst_count;          /* 已创建实例数（0 = 未加载） */
    int             tried;               /* 是否已尝试加载（失败则不再试） */
    char            variant[16];         /* 已加载变体码（涂装），用于换漆重载 */
    char            variant_name[24];    /* 已加载变体名（xsb 的 OBJ8_AIRCRAFT 名） */
} CslNode;

/* 初始化：解析 CSL 相对/绝对根路径（基于 XPLMGetSystemPath）。 */
void csl_init(void);

/* 释放地形探针等资源（插件退出时）。 */
void csl_shutdown(void);

/* 为指定机型获取实例集合。family/airline/registration 可为 NULL。
 * 优先按 X-CSL 风格解析 CSL/{ICAO}/xsb_aircraft.txt，变体按四级匹配：
 *   1) 注册号（尾号）一致 —— 同一架飞机的专属涂装（变体名/OBJ 名/行尾注记）；
 *   2) 同航司 + 同机型 —— AIRLINE/LIVERY 码精确，再同航司带后缀；
 *   3) 同机型任意涂装 —— ZZZ 白机 → ANY/GEN → 无涂装基础变体 → 包内基础 OBJ；
 *   4) 兜底 —— 机型别名同族模型（csl.c::kTypeAlias，如 FA8X→F900）→ ZZZZ 白机。
 * 成功返回实例数（>0），失败返回 0（结果缓存于 node）。 */
int csl_load(CslNode* node, const char* icao, const char* family,
             const char* airline, const char* registration);

/* 设置实例集合的位置（WGS84）与动画。animValues 长度须 ≥ csl_dataref_count()。 */
void csl_set_position(CslNode* node, double lat, double lon,
                      double alt_m, double heading, double pitch, double roll,
                      const float* animValues, int animCount);

/* 释放指定节点的全部实例（并重置节点缓存状态）。 */
void csl_release_node(CslNode* node);

/* 查询 (lat,lon) 处地形海拔（米 MSL）。命中返回地形高度并置 *ok=1，
 * 否则返回 0.0 且 *ok=0。alt_hint_m 仅用于构造射线起点（须高于地形）。 */
double csl_terrain_msl(double lat, double lon, double alt_hint_m, int* ok);

extern int csl_dataref_count(void);

#endif
