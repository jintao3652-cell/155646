/* XConnect — 远程玩家呼叫号标签模块（2D 顶层绘制）。 */
#ifndef XCONNECT_LABELS_H
#define XCONNECT_LABELS_H

/* 注册 2D 绘制回调（xplm_Phase_LastCockpit，位于 3D 场景之上）。返回 1 成功。 */
int labels_init(void);

/* 注销绘制回调（插件退出时）。 */
void labels_shutdown(void);

#endif /* XCONNECT_LABELS_H */