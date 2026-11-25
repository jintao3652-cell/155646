"""后端日志/提示文案 i18n（与前端 vue-i18n 目录对应）。

语言取值与前端一致：en / zh-CN / zh-TW，默认英语。
webapp.py 按 config.json 的 language 字段调用 translate()。
"""

from __future__ import annotations

LOG_MESSAGES: dict[str, dict[str, str]] = {
    "en": {
        "[xconnect] 数据通道就绪": "[xconnect] data channel ready",
        "[xconnect] 未发现模拟机": "[xconnect] simulator not found",
        "[xconnect] 已发现模拟机": "[xconnect] simulator found",
        "[fsd] 插件离线，自动断开 FSD 连接": "[fsd] plugin offline, auto disconnect FSD",
        "[xconnect] roster 推送失败": "[xconnect] roster push failed",
        "FSD 连接意外断开": "FSD connection dropped unexpectedly",
        "自动提交飞行计划失败": "failed to auto-submit flight plan",
        "已发送登录请求，呼号": "login request sent, callsign",
        "已自动提交已保存飞行计划": "saved flight plan auto-submitted",
        "[fsd] 注册号记录": "[fsd] registration cached",
        "[fsd] 注册号": "[fsd] registration",
        "手动填写": "manual",
        "模拟器尾号": "simulator tail number",
        "模拟器涂装": "simulator livery",
        "模拟器": "simulator",
        "上次记录": "last used",
        "未设置": "not set",
        "设置注册号失败": "failed to set registration",
    },
    "zh-CN": {},
    "zh-TW": {
        "[xconnect] 数据通道就绪": "[xconnect] 數據通道就緒",
        "[xconnect] 未发现模拟机": "[xconnect] 未發現模擬機",
        "[xconnect] 已发现模拟机": "[xconnect] 已發現模擬機",
        "[fsd] 插件离线，自动断开 FSD 连接": "[fsd] 插件離線，自動斷開 FSD 連接",
        "[xconnect] roster 推送失败": "[xconnect] roster 推送失敗",
        "FSD 连接意外断开": "FSD 連接意外斷開",
        "自动提交飞行计划失败": "自動提交飛行計劃失敗",
        "已发送登录请求，呼号": "已發送登錄請求，呼號",
        "已自动提交已保存飞行计划": "已自動提交已保存飛行計劃",
        "[fsd] 注册号记录": "[fsd] 註冊號記錄",
        "[fsd] 注册号": "[fsd] 註冊號",
        "手动填写": "手動填寫",
        "模拟器尾号": "模擬器尾號",
        "模拟器涂装": "模擬器塗裝",
        "模拟器": "模擬器",
        "上次记录": "上次記錄",
        "未设置": "未設定",
        "设置注册号失败": "設定註冊號失敗",
    },
}


def translate(language: str, text: str) -> str:
    """把后端日志/提示翻译成指定语言；无匹配时原样返回。

    按子串替换（保留动态后缀，如呼号/异常信息）。
    """
    if language not in LOG_MESSAGES:
        return text
    table = LOG_MESSAGES[language]
    if not table:
        return text
    out = text
    for src, dst in table.items():
        if src in out:
            out = out.replace(src, dst)
    return out
