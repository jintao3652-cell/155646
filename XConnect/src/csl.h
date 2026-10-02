/* CSL 模型加载与 XPLMInstance 渲染。
 * 模型布局（插件目录 Resources/plugins/XConnect/CSL/）：
 *   CSL/{ICAO}.obj             单层
 *   CSL/{family}/{ICAO}.obj    两层（X-CSL 风格包，family 为包名）
 * 动画 dataref 遵循 XPMP2 / X-CSL 约定。
 */
#ifndef XCONNECT_CSL_H
#define XCONNECT_CSL_H

#include "XPLMInstance.h"

#define CSL_MAX_DATAREFS 12

/* 玩家渲染实例（由主程序持有） */
typedef struct {
    XPLMInstanceRef inst;   /* NULL 表示未加载 */
    int               tried;  /* 是否已尝试加载（失败则不再试） */
} CslNode;

/* 初始化：记录 CSL 根目录。base 为 "插件目录/CSL"。 */
void csl_init(const char* cslRoot);

/* 释放全部已加载实例（插件退出时）。 */
void csl_shutdown(void);

/* 为指定机型获取实例。family 可为 NULL。
 * 成功返回 XPLMInstanceRef，失败返回 NULL（结果缓存于 node）。 */
XPLMInstanceRef csl_load(CslNode* node, const char* icao, const char* family);

/* 设置实例位置与动画。values 长度须 ≥ CSL_DATAREF_COUNT。 */
void csl_set_position(XPLMInstanceRef inst, double lat, double lon,
                      double alt_m, double heading, double pitch, double roll,
                      const float* animValues, int animCount);

/* 释放指定实例（并重置节点缓存状态）。 */
void csl_release_node(CslNode* node);

extern int csl_dataref_count(void);

#endif