/* CSL 模型加载与实例化渲染（双版本兼容：SDK 430 新 API / 旧 SDK 旧 API）。
 *
 * 模型布局（相对 X-System 根）：
 *   Resources/plugins/XConnect/CSL/{ICAO}.obj             单层
 *   Resources/plugins/XConnect/CSL/{family}/{ICAO}.obj    两层（X-CSL 风格）
 *
 * SDK 版本差异（kXPLM_Version 判定）：
 *   >= 430: XPLMLoadObject + XPLMCreateInstance + XPLMDrawInfo_t
 *   <  430: XPLMInstanceCreate(path, datarefs, count) + float pos[9]
 */
#ifndef XCONNECT_CSL_H
#define XCONNECT_CSL_H

#include "XPLMInstance.h"

#define CSL_MAX_DATAREFS 48  /* XPMP2 标准动画 dataref 全集（libxplanemp/...）*/

/* 玩家渲染实例（由主程序持有） */
typedef struct {
    XPLMInstanceRef inst;   /* NULL 表示未加载 */
    int             tried;  /* 是否已尝试加载（失败则不再试） */
} CslNode;

/* 初始化：解析 CSL 相对路径（基于 XPLMGetSystemPath）。 */
void csl_init(void);

/* 释放全部已加载实例（插件退出时）。 */
void csl_shutdown(void);

/* 为指定机型获取实例。family 可为 NULL。
 * 成功返回 XPLMInstanceRef，失败返回 NULL（结果缓存于 node）。 */
XPLMInstanceRef csl_load(CslNode* node, const char* icao, const char* family);

/* 设置实例位置（WGS84）与动画。animValues 长度须 ≥ csl_dataref_count()。 */
void csl_set_position(XPLMInstanceRef inst, double lat, double lon,
                      double alt_m, double heading, double pitch, double roll,
                      const float* animValues, int animCount);

/* 释放指定实例（并重置节点缓存状态）。 */
void csl_release_node(CslNode* node);

extern int csl_dataref_count(void);

#endif