"""注册号（尾号）归一化与来源优先级解析。

客户端四处需要尾号：`#SB PI` 的发送/接收、CSL 变体匹配、界面显示。统一放这里，
避免 fsd / model / 应用层各写一套规则。

**取值优先级**（越靠前越可信）：
1. `manual`  —— 用户在飞行计划里手填（`config.registration`）；
2. `sim`    —— 插件读模拟器：`acf_tailnum`，缺失时从涂装目录名提取；
3. `cached` —— 同一机型上次成功得到的值（`config.registration_cache[ICAO]`），
   用于模拟器读数尚未到达（插件刚上线/切换机模）时的短暂空档；
4. 全部为空 → 返回空串：**不伪造尾号**。此时 PI 的 `LIVERY` 退回航司码，对端
   CSL 匹配自动从第 2 级（同航司同机型）开始。
"""

from __future__ import annotations

SOURCE_LABELS = {
    "manual": "手动填写",
    "sim": "模拟器",
    "tailnum": "模拟器尾号",
    "livery": "模拟器涂装",
    "cache": "上次记录",
    "": "未设置",
}


def norm_registration(raw: str) -> str:
    """注册号归一化：大写，仅保留字母数字与 '-'，长度 3~10（B-1234 / N829PL）。"""
    s = "".join(ch for ch in (raw or "").upper()
                if ch.isalnum() or ch == "-").strip("-")
    return s if 3 <= len(s) <= 10 else ""


def looks_like_registration(raw: str) -> bool:
    """判断一个字符串更像注册号还是涂装/航司码（也用于从自由文本里挑尾号）。

    判据（与插件 `datarefs.c::tail_like` 保持一致）：
      - 归一化后 5~10 位（`DLH`/`CCA`/`A320`/`B738` 这类短码一律不算）；
      - 含数字时：还须含字母或 '-'（`B-1234` / `N829PL` / `VH-OEI`）；
      - 纯字母时：须带 '-' 且字母 ≥4 个（欧陆格式 `D-AIKN` / `G-EUUU`），
        这样既收下官方 A330 这类无数字尾号，又不会把 `DLH` 认成尾号；
      - 带 '_' 或以 swift 开头的是 swift 内部 CSL 映射 ID，直接排除。
    """
    s = (raw or "").strip().upper()
    if not s or "_" in s or s.startswith("SWIFT"):
        return False
    s = norm_registration(s)
    if not (5 <= len(s) <= 10):
        return False
    has_digit = any(ch.isdigit() for ch in s)
    has_alpha = any(ch.isalpha() for ch in s)
    if has_digit:
        return has_alpha or "-" in s
    # 纯字母：必须是 D-AIKN 这类带连字符的欧陆尾号
    return "-" in s and sum(ch.isalpha() for ch in s) >= 4


def tail_from_text(text: str) -> str:
    """从自由文本里挑出第一个像尾号的 token。

    典型输入：涂装目录名 `B-1234 China Southern`、`N829PL_Livery`、
    FSInn 描述串 `DLH A359 B-1234`、X-Plane 涂装路径末段。
    """
    for chunk in (text or "").replace("\\", "/").replace("/", " ").split():
        for token in chunk.split("_") if "_" in chunk else [chunk]:
            if looks_like_registration(token):
                return norm_registration(token)
    return ""


def resolve_registration(manual: str = "", sim: str = "",
                         cached: str = "", sim_src: str = "") -> tuple[str, str]:
    """按优先级取生效尾号，返回 `(注册号, 来源)`；来源见 SOURCE_LABELS。

    `sim_src` 用于把模拟器来源细分为 `tailnum`（`acf_tailnum`）与
    `livery`（从涂装目录名提取），仅用于日志/界面显示。
    """
    v = norm_registration(manual)
    if v:
        return v, "manual"
    v = norm_registration(sim)
    if v:
        return v, (sim_src if sim_src in ("tailnum", "livery") else "sim")
    v = norm_registration(cached)
    if v:
        return v, "cache"
    return "", ""


def source_label(src: str) -> str:
    return SOURCE_LABELS.get(src, src or SOURCE_LABELS[""])
