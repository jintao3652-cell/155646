"""机型 ICAO 推断 —— 移植 swift 的级联打分算法。

## 为什么要这个模块

有些机模的 `.acf` 里**根本没有 `acf/_ICAO` 字段**（实测 Laminar `F-14D.acf`
只有 `_name` / `_tailnum`），插件读出来是空串。如果直接上报空机型，会出现
"机型为空但尾号是新的"这种脏数据（服务器上表现为 `equipment: MD82` +
`livery: N988DR`，两架飞机的数据拼在一起）。

swift 的做法不是维护一张手写的"名字→代码"映射表，而是**拿完整的机型数据库
（1 万多条）做级联打分匹配**，取最高分。本模块就是把这套算法搬过来。

## 数据来源

`aircraft_db.b64` 是 swift 0.15 的 `share/shared/dbdata/aircrafticao.json`
快照（数据版本 2024-12-02），原始格式是 `swift:<ts>:<base64(zlib(json))>`，
这里预先解压、精简成 `[designator, model, manufacturer, engines, wtc, rank]`
六元组后重新压成 base64+zlib，体积 424K → 111K。

## 打分规则（对应 swift `CAircraftIcaoCode::calculateScore`）

| 条件 | 分值 |
|---|---|
| designator 完全相同 | +50（rank 0 再 +15，1 再 +12，n 再 +10-n）|
| family 相同 | +40 |
| combinedType 相同 | +30 |
| 否则按引擎数/机型类别逐项 | 至多 +15 |
| 制造商等长匹配 / 含于 | +10 / +5 |
| 军用标志一致 | +8 |

另外 `matchesAnyDescription()` 是**不区分大小写的子串匹配**，用它从机型名
（"F-14 Tomcat"）反查候选，再打分。这是 swift 唯一用于"名字→代码"的匹配方式。
"""

from __future__ import annotations

import base64
import json
import re
import threading
import zlib
from typing import Optional

_DB_FILE = "aircraft_db.b64"
# designator → 引擎类型字母（J/T/P/R）。从 swift aircrafticao.json 的
# `engine` 字段导出，独立成小文件以免改 6 元组主库格式。
_ENGINE_TYPES_FILE = "aircraft_engine_types.b64"
# designator → swift 数据库里的记录 id（用于拼 LIVERY=SWIFT_L..A<id>M..）
_ID_FILE = "_aircraft_ids.b64"
_db_lock = threading.Lock()
_db: Optional[list[tuple]] = None
# 索引：小写 model 子串 → 记录下标。用于加速 contains 查询，
# 避免每次都对 1 万条做全表子串扫描。
_by_token: Optional[dict[str, list[int]]] = None
_ids: Optional[dict[str, int]] = None
_engine_types: Optional[dict[str, str]] = None

# 本地补充表里的机型不在 swift DB 中，单独标注引擎类型。
_LOCAL_ENGINE_TYPES: dict[str, str] = {
    "C919": "J", "AJ27": "T", "Y20": "T", "YY20": "T", "J20": "J",
    "AG60": "T", "MA60": "T", "Z20": "R", "C929": "J",
    "B779": "J", "B778": "J",
}

# ---- 本地补充表（不在 swift 源里的机型）----
#
# 为什么需要：`aircraft_db.b64` 是从 swift 的 `aircrafticao.json`
# 原样导出的，而 swift 那份数据停在 2024-12，**没有 C919**（国产大飞机
# 2023 才商业运营，ICAO 代码 C919 录入更晚）。
# 直接改 b64 文件不行 —— 下次重新导入就被覆盖了。
#
# 所以在这里放"增量条目"，加载时拼到末尾。格式与源一致：
#   (designator, model, manufacturer, engine_count, wtc, rank)
#
# rank 取值参考源数据：常见干线机 1~5，客改货/军警型 10。
# C919 按 A320/B738 同级别取 2（常见机型）。
_LOCAL_EXTRA: list[tuple] = [
    # COMAC C919（2023-05-28 东航首航；LEAP-1C ×2，L2J，WTG D → wtc=M）
    # ICAO Doc 8643 代码就是 "C919"（不是 C-919，那个是厂商写法）
    ("C919", "C919", "COMAC", 2, "M", 2),
    ("C919", "C-919", "COMAC", 2, "M", 2),
    ("C919", "C919-100STD", "COMAC", 2, "M", 5),
    ("C919", "COMAC C919", "COMAC", 2, "M", 5),
    # ARJ21：swift 里只有 AJ27 一条（model "ARJ-21-700 Xiangfeng"），
    # 但机模 title 普遍写 `ARJ21-700` / `ARJ-21`，与 model 里的
    # "ARJ-21-700" 连字符位置不同，容易匹配不上。补两个常见写法。
    # （AJ27 是 ARJ21-700 的正式 ICAO 代码，保持不变）
    ("AJ27", "ARJ21-700", "COMAC", 2, "M", 5),
    ("AJ27", "ARJ21", "COMAC", 2, "M", 5),
    ("AJ27", "ARJ-21", "COMAC", 2, "M", 5),
    # ARJ21 货运/公务改型（C909 是商飞新命名，2024 起启用的市场名）
    ("AJ27", "C909", "COMAC", 2, "M", 10),

    # ---- 国产军/民机（ICAO Doc 8643 代码，来源 doc8643.com）----
    #
    # ⚠️ **必须补，否则会误判成完全不相干的机型**：
    #   `Y-20` → 被配到 GY20（Minicab 轻型机，一架 2 座小飞机）
    #   `J-20` → 被配到 KRAG
    #   `AG600` → 被配到 GA6C
    #   `Z-20` → 被配到 PA34（Piper Seneca）
    # 军机 code 库里基本没有，只能手工补。

    # 运-20 鲲鹏：doc8643 = Y20，L4J，重型 H
    ("Y20", "Y-20", "XIAN", 4, "H", 10),
    ("Y20", "Y-20 Kunpeng", "XIAN", 4, "H", 10),
    ("Y20", "Y-20B", "XIAN", 4, "H", 10),
    ("Y20", "Kunpeng", "XIAN", 4, "H", 10),
    ("Y20", "运-20", "XIAN", 4, "H", 10),
    # 运油-20（加油型，2022 服役）
    ("YY20", "YY-20", "XIAN", 4, "H", 10),
    ("YY20", "Y-20U", "XIAN", 4, "H", 10),

    # 歼-20 威龙：doc8643 = J20，L2J，中型 M
    ("J20", "J-20", "CHENGDU", 2, "M", 10),
    ("J20", "J-20 Mighty Dragon", "CHENGDU", 2, "M", 10),
    ("J20", "歼-20", "CHENGDU", 2, "M", 10),

    # AG600 鲲龙：doc8643 = **AG60**（不是 AG600），L4T 水陆两栖
    ("AG60", "AG600", "CAIGA", 4, "M", 10),
    ("AG60", "AG-600", "CAIGA", 4, "M", 10),
    ("AG60", "AG-600 Kunlong", "CAIGA", 4, "M", 10),
    ("AG60", "Kunlong", "CAIGA", 4, "M", 10),
    ("AG60", "鲲龙", "CAIGA", 4, "M", 10),

    # 新舟 60 / 600（MA60 源里已有 "MA-60"，补 600 与中文名）
    ("MA60", "MA600", "XIAN", 2, "M", 10),
    ("MA60", "新舟60", "XIAN", 2, "M", 10),
    ("MA60", "新舟-60", "XIAN", 2, "M", 10),

    # 直-20 战术直升机
    ("Z20", "Z-20", "HARBIN", 2, "M", 10),
    ("Z20", "直-20", "HARBIN", 2, "M", 10),

    # C929（中俄宽体，研制中，ICAO 尚未正式分配；用 C929 占位，
    # 避免被误配到别的机型。正式代码公布后改这里即可）
    ("C929", "C929", "COMAC", 2, "H", 10),
    ("C929", "C-929", "COMAC", 2, "H", 10),
    ("C929", "CR929", "COMAC", 2, "H", 10),

    # ---- Boeing 777X 系列 ----
    #
    # **777X 是系列名，不是单一机型**：拆成 777-8 和 777-9 两个型号，
    # 以及 777-8F 货机（代号 B778 / B779，ICAO Doc 8643 确认）。
    #
    # ⚠️ 不补的话 `Boeing 777X` 会被配到 **B77L（777-200LR）** ——
    # 完全是另一架飞机。原因是 token `777x` 匹配不上任何 model，
    # 只能靠 `777` 这个数字串去撞，最后撞到了 777-200LR。
    # 在飞行网络里上报错的 designator 比上报空值更糟（对端会画错 CSL 模型）。
    #
    # 系列名的处理：**归到 777-9**（系列里产量与订单最多的客运型，
    # 也是绝大多数机模做的那一款）。777-8 则用精确名匹配。
    ("B779", "777X", "BOEING", 2, "H", 5),
    ("B779", "777-X", "BOEING", 2, "H", 5),
    ("B779", "Boeing 777X", "BOEING", 2, "H", 5),
    ("B779", "777-9X", "BOEING", 2, "H", 5),
    # 777-8 用精确型号名区分（避免被系列名吸走）
    ("B778", "777-8X", "BOEING", 2, "H", 4),
    # 777-8F 货机（2022-05 汉莎货运首订）
    ("B778", "777-8F", "BOEING", 2, "H", 10),
    ("B778", "777X Freighter", "BOEING", 2, "H", 10),
]

# ---- 源数据修正表 ----
#
# 用来修正 swift 源里**本身就有错**的字段（不是我们导入时丢的）。
# 格式：designator → {字段: 正确值}
#
# 已知问题（都经 ICAO Doc 8643 核对）：
#
# ① `B779 | 777-9` 的 enginecount 写成 **0**，实际是两台 GE9X。
#    engines=0 会让 `_score()` 的"引擎数一致"加分永远不生效。
#
# ② `B772 | 777-200F` 的 designator 错了。ICAO 里 777 只有四个代码：
#       B772  777-200 / 777-200ER
#       B773  777-300
#       B77L  777-200LR / **777F（货机）**
#       B77W  777-300ER
#    777 货机（777F）由 -200LR 衍生，官方代码就是 **B77L**，
#    而不是 B772。`B77F` 那个条目是 swift 自造的非官方代码。
#    这里把 777-200F 修到 B77L，避免它靠 B772 的 rank=1 抢分。
_FIELD_FIXES: dict[str, dict] = {
    "B779": {"engines": 2},
}

# 需要**改写 designator** 的条目：源里的 (旧des, model) → 新 des。
# 与 _FIELD_FIXES 分开是因为它会改变主键，得单独处理。
_DES_FIXES: dict[tuple, str] = {
    # 777 货机的官方代码是 B77L（与 777-200LR 共用）
    ("B772", "777-200F"): "B77L",
    # swift 自造的 B77F 也并到 B77L
    ("B77F", "777 Freighter (777F)"): "B77L",
}

# 切词时丢弃的噪声词。这些词在机型名里出现频率极高但不区分机型，
# 留着会让"全中"条件变得过严（例如 "Boeing 747-400" 的 "boeing" 在很多
# 记录里不存在，因为数据库里 model 字段只写 "747-400 (international...)"）。
_NOISE = {
    "the", "aircraft", "airplane", "plane", "livery", "default",
    "boeing", "airbus", "mcdonnell", "douglas", "lockheed", "grumman",
    "cessna", "beechcraft", "embraer", "bombardier", "atr", "piper",
    "robinson", "sikorsky", "north", "american", "de", "havilland",
    "company", "corporation", "inc",
    # 机模作者/厂商名（真实 title 里很常见，库里没有）
    "aerosoft", "pmdg", "zibo", "toliss", "fenix", "fslabs", "inibuilds",
    "flightfactor", "justflight", "carenado", "alabeo", "milviz", "xplane",
    "pro", "v2", "v3", "variation", "name", "winds", "aig", "aigaim",
    "fsltl", "ivao", "vatsim", "generic", "asobo", "workingtitle",
}


# 大写字母 + 连字符 + 数字，但属于**机型代码**而非尾号的前缀。
# 这些不能当尾号删掉，否则机型直接丢失（实测 `MD-82` 就被误删过）。
_NOT_A_REG_PREFIX = {
    "MD", "PA", "AT", "CRJ", "ERJ", "BEC", "DC", "AN", "IL", "TU",
    "BAE", "ATP", "SF", "SA", "AH", "CH", "UH", "MH", "NH", "SH",
}


def _looks_like_registration(t: str) -> bool:
    """判断 token 是否像航空器注册号（尾号）。

    真实机模 title 几乎都带尾号：
        "China Eastern Airlines B-6055"
        "CRJ700ER Lufthansa Regional D-ACSB"
        "BlueBird 757-200 PW United Airlines N563UA"
    这些 token 在机型库里**从来不存在**，留着会把命中率稀释得很低
    （实测 `CSS B737-500` 因 `css` 一个噪声词就从 1.0 掉到 0.6）。
        "BlueBird 757-200 PW United Airlines N563UA"
    这些 token 在机型库里**从来不存在**，留着会把命中率稀释得很低
    （实测 `CSS B737-500` 因 `css` 一个噪声词就从 1.0 掉到 0.6）。

    ⚠️ **必须区分尾号和机型代码**，否则会误删真机型：
        `MD-82`  / `B737-500` / `A321-211` / `PA-28`  ← 机型，不能删
        `B-6055`  / `D-ACSB`  / `F-HMLH` / `N563UA`  ← 尾号，要删

    关键区别：**尾号在 title 里一律是大写字母 + 连字符**，
    而机型代码可大写可小写；且机型代码的字母部分通常是
    A/B/MD/CRJ/PA 这种有意义的缩写，不会只有 1 位。

    因此判据收紧为：
      ① `X-####`（1 位字母 + 连字符 + 数字）—— 且字母是大写
      ② `XX-###`（2 位大写字母 + 连字符 + 数字）—— 但排除 MD/PA/AT 等机型前缀
      ③ `N` 开头 + 字母数字混合（美国）
      ④ `JA` + 4 位 / `B` + 5 位纯数字
    """
    if not t or len(t) < 5:
        return False
    digits = sum(c.isdigit() for c in t)
    if digits < 2:
        return False

    # ① / ② 连字符形态
    if "-" in t:
        head, _, tail = t.partition("-")
        if not tail:
            return False
        # 字母部分必须全大写（`md-82` / `a321-211` 是小写 → 判定为机型）
        if head.isalpha() and head.isupper():
            # 排除常见机型前缀（它们也是大写 + 连字符 + 数字）
            if head.upper() in _NOT_A_REG_PREFIX:
                return False
            if 1 <= len(head) <= 2 and 2 <= len(tail) <= 5:
                return True
        return False

    # ③ 美国 N 号：N + 3~5 位字母数字，且总长 5~6
    if t[0] == "n" and t[1:].isalnum() and 5 <= len(t) <= 6:
        # 排除 n 结尾的普通单词（如 "night"），要求含数字
        if digits >= 2:
            return True
    # ④ JA + 4 位 / B + 5 位纯数字
    if t.startswith("ja") and len(t) == 6 and t[2:].isdigit():
        return True
    if t[0] == "b" and len(t) == 5 and t[1:].isdigit():
        return True
    return False


def _load() -> list[tuple]:
    """惰性加载并解压数据库（首次调用约 20ms）。"""
    global _db, _by_token
    with _db_lock:
        if _db is not None:
            return _db
        import os
        p = os.path.join(os.path.dirname(os.path.abspath(__file__)), _DB_FILE)
        with open(p, "rb") as f:
            blob = b"".join(
                ln for ln in f.read().splitlines() if not ln.startswith(b"#")
            )
        rows = json.loads(zlib.decompress(base64.b64decode(blob)))
        # 每行: [designator, model, manufacturer, engines, wtc, rank]
        _db = [tuple(r) for r in rows]

        # ① 改写错误的 designator（见 _DES_FIXES）。
        #    必须在建索引前做 —— 否则索引还挂在旧 designator 上。
        if _DES_FIXES:
            _renamed = []
            _drop = set()
            for r in _db:
                newdes = _DES_FIXES.get((r[0], r[1]))
                if newdes:
                    # 目标已存在同一 model → 丢弃重复项，避免索引出现两条
                    if (newdes, r[1]) in {(x[0], x[1]) for x in _renamed}:
                        continue
                    r = (newdes,) + tuple(r[1:])
                _renamed.append(r)
            _db = _renamed

        # ② 修正源数据本身的错误字段（见 _FIELD_FIXES 说明）。
        # 修正源数据本身的错误字段（见 _FIELD_FIXES 说明）。
        # 必须在建索引**之前**做，否则 engines 还是旧值。
        if _FIELD_FIXES:
            _fixed = []
            for r in _db:
                fix = _FIELD_FIXES.get(r[0])
                if fix:
                    r = list(r)
                    # 元组布局：0=des 1=model 2=manuf 3=engines 4=wtc 5=rank
                    _slot = {"designator": 0, "model": 1, "manufacturer": 2,
                             "engines": 3, "wtc": 4, "rank": 5}
                    for k, v in fix.items():
                        if k in _slot:
                            r[_slot[k]] = v
                    r = tuple(r)
                _fixed.append(r)
            _db = _fixed
        # 拼上本地补充表（C919 / ARJ21 别名等）。
        # 放在源数据**之后**：源数据优先，补充只填补空缺。
        # 用 (designator, model) 去重，避免与源里已有条目重复建索引。
        _seen = {(r[0], r[1]) for r in _db}
        for r in _LOCAL_EXTRA:
            if (r[0], r[1]) not in _seen:
                _seen.add((r[0], r[1]))
                _db.append(r)
        # 建倒排索引：model 里的每个词 → 记录下标。
        # **必须用与查询侧相同的 _tokens()**，否则 `SR22` 在索引里被拆成
        # `sr`/`22`、查询侧却找 `sr22`，两边对不上 → 永远查不到（踩过）。
        #
        # **designator 本身也要入索引**：很多机型的 model 不含型号码，
        #     A346 | model="A340-600"      ← 没有 "A346"
        #     CRJ7 | model="CL-600 ... CRJ-550"
        # 而机模 title 常直接写型号码（"Aerosoft A346 Pro"）。
        # 不把 designator 入索引，这类就永远 0 候选（实测）。
        idx: dict[str, list[int]] = {}
        for i, r in enumerate(_db):
            for tok in set(_tokens(r[1] + " " + (r[2] or ""))):
                idx.setdefault(tok, []).append(i)
            des = (r[0] or "").lower()
            if des:
                idx.setdefault(des, []).append(i)
        _by_token = idx
        return _db


def _load_engine_types() -> dict[str, str]:
    """惰性加载 designator → 引擎类型（J/T/P/R）表。"""
    global _engine_types
    if _engine_types is not None:
        return _engine_types
    import os
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     _ENGINE_TYPES_FILE)
    with open(p, "rb") as f:
        blob = b"".join(
            ln for ln in f.read().splitlines() if not ln.startswith(b"#")
        )
    data: dict[str, str] = json.loads(zlib.decompress(base64.b64decode(blob)))
    data.update({k.upper(): v for k, v in _LOCAL_ENGINE_TYPES.items()})
    _engine_types = data
    return data


def engine_type_of(designator: str) -> str:
    """查机型引擎类型字母；未知返回空串（调用方再走启发式）。"""
    des = (designator or "").strip().upper()
    if not des:
        return ""
    return _load_engine_types().get(des, "")


def _combined_type(engines: int, wtc: str, designator: str = "") -> str:
    """combinedType（wtc 首字符 + 引擎数 + 引擎类型），如 `L2J`。

    引擎类型优先取 swift 官方 DB 的 `engine` 字段；缺记录时才按
    "运输类多为喷气"兜底（原先恒为 J，把 C172/AT72 全判成喷气）。
    """
    at = (wtc or "L")[:1].upper() or "L"
    n = max(0, min(9, int(engines or 0)))
    et = engine_type_of(designator) or "J"
    return f"{at}{n}{et}"


def _strip_tail_alpha(t: str) -> str:
    """去掉 token 尾部的连续字母，前提是核心仍含数字。

    `737-800x` → `737-800`    （Zibo 的写法）
    `320neo`   → `320`
    `a321lr`   → `a321`
    `zibo`     → ``            （纯字母，去掉后没数字，作废）
    """
    i = len(t)
    while i > 0 and t[i - 1].isalpha():
        i -= 1
    core = t[:i]
    if core and core != t and any(c.isdigit() for c in core):
        return core
    return ""


def _token_weight(t: str) -> int:
    """token 在匹配打分里的权重。

    **含数字的 token 给 6，纯字母给 1。**

    为什么把数字权重提到 6（原为 3）：真实 title 里的航司名
    （`united` / `express` / `cathay`）会意外命中库里的无关机型 ——
    实测 `CRJ550 United Express N504GJ` 因为 `express` 命中
    "Global Express"，把 GLEX 抬到和正确答案 CRJ7 打平，
    最后靠索引顺序选中了 GLEX。机型指纹（含数字）必须**压过**
    航司名（纯字母），否则这类误判无法避免。
    """
    if any(c.isdigit() for c in t):
        return 6
    return 1


# 型号后缀词：与前面的型号词拼接后才是完整型号
# （`A320 Neo` → `a320neo`，`737 MAX` → `737max`）。
_VARIANT_SUFFIX = {
    "neo", "max", "er", "lr", "x", "ng", "sl", "adv", "f", "sf",
}


def _is_cjk_strong(t: str) -> bool:
    """中文片段是否足以作为"认出型号"的依据。

    中文名（`鲲龙` / `新舟60` / `运-20`）信息密度远高于英文：
    2 个汉字通常就是一个完整的机型名，而英文 2 个字母（`bo`/`pa`）
    几乎什么都不能说明。

    没有这个判断，`鲲龙`（2 字）会被 `len >= 6` 的门槛挡掉 ——
    实测 `鲲龙` 匹配不到 AG60。
    """
    cjk = sum(1 for c in t if "\u4e00" <= c <= "\u9fff")
    return cjk >= 2


def _tok_core(t: str) -> str:
    """去掉派生标记 `~`，得到实际用于匹配的 token。

    `~a319` 是从 `fenixa319` 里抽出的片段，本身是有效线索，
    但优先级应低于用户显式写的 token（否则 `Cirrus SR22`
    会因派生出的 `r22` 而错配到 Robinson R22）。
    """
    return t[1:] if t.startswith("~") else t


def _score(cand: tuple, want_engines: int = 0, want_wtc: str = "") -> int:
    """给单个候选打分（简化版 calculateScore，满分约 100）。"""
    des, model, manuf, eng, wtc, rank = cand
    score = 0
    # rank 越小越常见 → 加分（swift: rank 0→+15, 1→+12, n→+10-n）
    r = int(rank or 5)
    if r == 0:
        score += 15
    elif r == 1:
        score += 12
    elif r < 10:
        score += max(0, 10 - r)
    # 引擎数一致
    if want_engines and int(eng or 0) == want_engines:
        score += 8
    # wtc 一致
    if want_wtc and (wtc or "").upper()[:1] == want_wtc.upper()[:1]:
        score += 6
    return score


def _tokens(s: str) -> list[str]:
    """把机型名切成词（小写、去噪声词）。

    关键点：**不要拆掉"字母+数字"连写**。`SR22` / `R44` / `A320` / `B738`
    这类是整个机型代号，拆成 `sr`+`22` 会让 `22` 命中一堆无关机型
    （实测 `Cirrus SR22` 因此配到 `VK3P`）。只把连字符/斜杠/括号当分隔符，
    然后把连字符两侧合成一个 token（`747-400` → `747400` 之外还保留
    `747` 和 `400`，因为数据库里两种写法都有）。
    """
    s = (s or "").lower()
    # 括号、逗号、斜杠、下划线只当分隔符；连字符先保留（下面单独处理）
    for ch in "_/(),.;:'\"[]{}|&#=":
        s = s.replace(ch, " ")
    raw = s.split()
    out: list[str] = []
    for t in raw:
        if t in _NOISE:
            continue
        # 尾号（注册号）在机型库里从不存在，必须剔除，
        # 否则会把命中率稀释（真实 title 几乎都带尾号）。
        if _looks_like_registration(t):
            continue
        # 连字符形式：`747-400` 同时产出 `747-400`、`747`、`400` 三种形态，
        # 因为库里可能是 "747-400"、也可能是 "747400" 或分写。
        if "-" in t:
            parts = [p for p in t.split("-") if p]
            if parts:
                out.append("-".join(parts))
                out.extend(parts)
                out.append("".join(parts))
                # 每一段若形如"数字+尾字母"（`800x` / `320neo`），
                # 额外产出纯数字形态 `800` / `320`。
                #
                # 为什么需要：`Zibo 737-800X` 的 800X 在库里不存在
                # （库里只有 "737-800"），若只保留 `800x`，打分时
                # 除 `737` 外全部落空 → 命中率 3/7=0.43 → 被门槛拒绝，
                # 整个 737 家族 28 个候选全被刷掉（实测）。
                # 补上纯数字后 `800` 能命中，命中率升到 6/7，正确选中 B738。
                for p in parts:
                    core = _strip_tail_alpha(p)
                    if core:
                        out.append(core)
            continue
        if len(t) >= 2 or t.isdigit():
            out.append(t)
            # 单 token 同样处理"数字+尾字母"：`320neo` → 追加 `320`。
            # 保留原 token（`320neo` 在库里确实存在，能精确命中），
            # 只是多加一个更宽的形态提高召回。
            core = _strip_tail_alpha(t)
            if core:
                out.append(core)
    # 去重保序
    seen = set()
    res = []
    for t in out:
        if t not in seen:
            seen.add(t)
            res.append(t)
    # 从"字母+型号"连写里抽出型号片段：`fenixa319` → `a319`，
    # `flybywirea320` → `a320`。
    #
    # 真实机模 title 常把作者名和型号黏在一起（`FenixA319 CFM`、
    # `FlyByWireA320neo_JuneyaoB326J`），只按空格切会得到
    # `fenixa319` 这种整体 token，在库里根本不存在 → 整条失配。
    # 抽出尾部的"1~2 位字母 + 3~4 位数字"（A319 / B738 / CRJ7）即可命中。
    extra: list[str] = []
    for t in res:
        # 从右往左找：优先最短的字母前缀（`a319` 优于 `xa319`）。
        # 必须锚定到**数字段开头**，且前面的字符是字母或行首。
        for m in re.finditer(r"([a-z]{1,2})(\d{2,4}[a-z]{0,3})$", t):
            # 起点前必须是"词边界"（前一个是字母，说明还有更短的前缀可试）
            if m.start() > 0:
                extra.append(m.group(0))
        # 追加一个"只保留 1 位字母前缀"的变体（`fenixa319` → `a319`）
        m2 = re.search(r"([a-z])(\d{2,4}[a-z]{0,3})$", t)
        if m2 and m2.start() > 0:
            extra.append(m2.group(0))
    # ⚠️ 抽取片段只能作为**补充**，不能覆盖原 token 的优先级。
    # `Cirrus SR22` 的 `sr22` 会抽出一个 `r22`，而库里 R22 是
    # Robinson R22（另一架飞机）。若把 `r22` 当同等级 token，
    # 它会和 `sr22` 竞争并让 SR22 丢分（实测退化成了 R22）。
    #
    # 用前缀 `~` 标示"派生 token"，匹配时权重减半。
    for t in extra:
        if t not in seen:
            seen.add(t)
            res.append("~" + t)
    return res


def guess_designator(name: str, engines: int = 0) -> str:
    """按机型显示名推断 ICAO designator。

    算法对应 swift 的 `matchesAnyDescription()` + `calculateScore()`：

    1. **精确 designator**：名字本身就是代码（"F14"）→ 直接用。
    2. **词命中打分**：把输入的机型名切词，对每条记录的 model+manufacturer
       统计**命中的词数**，再叠加引擎数、wtc、rank 等轻量修正。
    3. **要求关键词全中**：像 "Boeing 747-400" 里的 "747" 和 "400" 必须都出现，
       否则 "Pa-28" 这种短串会误配到别处。
    4. 分数太低（< 阈值）就返回空 —— **宁可不给机型，也不要给错的机型**。

    返回空串表示没把握。
    """
    name = (name or "").strip()
    if not name:
        return ""
    db = _load()
    idx = _by_token or {}

    # 1) 名字本身就是 designator（用户手填 "F14"/"B738"）
    up = name.upper().replace(" ", "").replace("-", "")
    if 2 <= len(up) <= 4 and up.isalnum():
        for r in db:
            if r[0] == up:
                return r[0]

    toks = _tokens(name)
    if not toks:
        return ""
    # 取**最后一个 token** 的压缩形式当"疑似 designator"。
    # `Cirrus SR22` 的末词是 `sr22` → 可与库里的 SR22 对上；
    # 而整名压缩成 `CIRRUSSR22` 就永远匹配不上（踩过）。
    # 取**最后一个非派生 token**（跳过 `~r22` 这类抽取片段）。
    # 派生 token 追加在列表末尾，直接取 toks[-1] 会拿到 `~r22`，
    # 导致 tail 变成 "~R22" 永远匹配不上（实测 SR22/A320neo 双双失配）。
    _plain = [t for t in toks if not t.startswith("~")] or toks
    last = _plain[-1].upper().replace("-", "")
    tail_up = last if 2 <= len(last) <= 5 and last.isalnum() else ""
    # 若末词是"型号后缀"（neo / max / er / lr / x），与前一个词拼接成
    # 完整型号词：`A320 Neo` → `a320neo`，`737 MAX 8` → 逐级拼接。
    # 这样 `Airbus A320 Neo`（空格写法）与 `Airbus A320neo`（连写）
    # 会走同一条路径，不会被判成基础型 A320。
    if len(_plain) >= 2 and _plain[-1].lower() in _VARIANT_SUFFIX:
        merged = _plain[-2] + _plain[-1]
        if merged not in toks:
            toks = toks + [merged]

    # 2) 用倒排索引取候选（任一 token 命中即可，后面再按命中率过滤）
    #
    # ⚠️ 关键点：索引是按**库里的 model** 建的，查询侧的 token 常常是
    # 变体写法（`737-800X` / `320neo` / `A321LR`），直接查索引会拿到
    # **0 个候选**，整个匹配直接失败（实测 `Zibo 737-800X` 候选数=0）。
    #
    # 所以对每个 token 做"**去尾字母**"再查一次：
    #   `737-800x` → `737-800`  → 命中库里的 "737-800"
    #   `320neo`   → `320`      → 命中 "A320"
    #   `a321lr`   → `a321`     → 命中 "A321"
    # 只去尾部**连续的字母**，保留数字与连字符结构。
    cand_ids: set[int] = set()
    for t in toks:
        cand_ids.update(idx.get(_tok_core(t), ()))
        # 去尾字母后，再按"连字符拆开"的每一段各查一次。
        # `737-800x` → 去尾字母 `737-800` → 拆成 `737` / `800`，
        # 这两个都在索引里（库里的 model "737-800" 切词后就是它们）。
        s = _strip_tail_alpha(t)
        if s:
            cand_ids.update(idx.get(s, ()))
            for seg in s.split("-"):
                if seg:
                    cand_ids.update(idx.get(seg, ()))
    if not cand_ids:
        return ""

    # 3) 逐候选打分
    #
    # 不用"全中"（太严：`Cessna 172 SP` 的 `sp` 不在库里的 `172 Skyhawk`
    # 中，会直接判死）。改用**加权命中率**：
    #   - 数字 token 权重 3（747 / 400 是区分机型的关键）
    #   - 字母 token 权重 1
    #   - 命中权重 / 总权重 作为主分，再叠 rank/引擎数修正
    # 并设最低门槛，避免"只命中一个泛词"就乱配。
    best: Optional[tuple] = None
    best_score = 0.0
    total_w = sum(
        max(1, _token_weight(_tok_core(t)) // 2) if t.startswith('~')
        else _token_weight(t)
        for t in toks
    )
    # 取**最长的字母数字 token** 作为"型号特征词"。
    # 用最长而非最后一个：`Airbus A320neo` 里 `a320neo`(7) 比 `a320`(4)
    # 更能代表具体型号；`Zibo 737-800X` 里 `737-800x` 同理。
    last_tok = max((_tok_core(t) for t in toks if not _tok_core(t).isdigit()),
                   key=len, default="")
    if total_w == 0:
        return ""
    for i in cand_ids:
        r = db[i]
        # hay 三种形态都拼上：原样、去连字符、去所有分隔符。
        # 必须包含**压缩形式**：库里 `SR22` 的 model 写作 "SR-22"，查询侧
        # token 是 "sr22"，若只比 "sr 22" 会漏配（实测配到 S22T）。
        base = (r[1] + " " + (r[2] or "")).lower()
        hay = base + " " + base.replace("-", " ") + " " + \
            base.replace("-", "").replace(" ", "")
        # **designator 本身也要进 haystack**。
        #
        # 为什么：库里很多机型的 model 字段不含型号码，只有 designator 有：
        #     A346 | model="A340-600"        ← model 里没有 "A346"
        #     CRJ7 | model="CL-600 ... CRJ-550"
        # 机模 title 却常直接写型号码（"Aerosoft A346 Pro"）。
        # 只比 model 永远匹配不上（实测 A346 返回空）。
        hay_des = r[0].lower()
        hit_w = 0
        des_hit = False
        matched: set[str] = set()   # 真正命中的 token（供 core_hit 判断）
        for t in toks:
            core_t = _tok_core(t)
            derived = core_t != t          # 派生 token（`~r22`）
            w = _token_weight(core_t)
            if derived:
                w = max(1, w // 2)         # 派生 token 权重减半
            if core_t in hay:
                hit_w += w
                matched.add(core_t)
                continue
            # designator 精确对上（`a346` == designator "A346"）→ 强信号
            #
            # ⚠️ 但**不能是"家族前缀"**：`CSS B737-500` 切出 token `b737`，
            # 它的大写形式恰好等于 designator `B737`（波音 737 家族里的
            # 某一个具体型号），于是 B737 白拿 +70 分压过真正正确的 B735
            # （B735 靠 strip 匹配拿到 18 权重、92 分，本来是赢家）。
            #
            # 判据：只有当这个 token **本身就长等于 designator**、
            # 且输入里没有更精确的型号 token（如 `b737-500`）时才认。
            # 具体做法：要求 token 长度 == designator 长度，
            # 且 input 里该 token 是"独立的"（不含连字符/更长数字串）。
            if core_t.upper() == r[0] and len(core_t) >= 3:
                # `b737` 后面还有 `-500` 这种更精确的限定 → 不算命中
                more_specific = "-" in core_t or any(
                    o != t and _tok_core(o).startswith(core_t)
                    and len(_tok_core(o)) > len(core_t)
                    for o in toks
                )
                if not more_specific:
                    hit_w += w
                    des_hit = True
                    matched.add(core_t)
                continue
            # 数字前缀式匹配：`737` 应能命中 "737800"（库里的 "737-800"
            # 压缩后）。否则 `737-800X` 这种写法会整体失配。
            if any(c.isdigit() for c in core_t) and core_t in base.replace("-", "").replace(" ", ""):
                hit_w += w
                matched.add(core_t)
                continue
            # `b737` / `a320` 这类"厂商字母 + 数字"的 token，库里常写作
            # 纯数字（`737-500`）。去掉首字母再试一次。
            # 实测 `CSS B737-500` 因 `b737` 匹配不上 `"737-500"` 而选错机型。
            if len(core_t) >= 4 and core_t[0].isalpha() and core_t[1:].isdigit():
                if core_t[1:] in hay:
                    hit_w += w
                    matched.add(core_t)
                    continue
        if hit_w == 0:
            continue
        ratio = hit_w / total_w
        s = ratio * 100.0

        # ---- 门槛：判断是否"认出了型号" ----
        #
        # 真实机模 title 里混着**航司名、涂装名、作者名**，机型库里都没有，
        # 一刀切"命中率>=0.5"会把正确答案全刷掉（实测真实 title 只中 11/20）。
        #
        # 但门槛也不能太松：`Some Random Thing` 的 `thing` 会命中
        # `WT-02 Wild Thing` → 误判 WILT。
        #
        # 判据：命中的 token 必须是**型号指纹**，即
        #   ① 含数字（`737` / `a320` / `crj700` / `e175` / `r44` / `a346`）
        #      —— 这是最可靠的信号，机型号几乎都带数字；
        #   ② 或 designator 被直接命中；
        #   ③ 或（无数字输入时）长 >= 5 的非泛词（"King Air" → B350）。
        #
        # 注意：判"含数字"要用 **any(c.isdigit())** 而不是 `t.isdigit()`。
        # `a320` 内部有数字但不是纯数字 —— 之前写 `t.isdigit()` 导致
        # A320/E175/R44 全部失配（实测回归）。
        def _has_digit(s: str) -> bool:
            return any(c.isdigit() for c in s)

        if any(_has_digit(t) for t in toks):
            # 用 matched（真正命中的 token）而非 `t in hay` —— strip/prefix
            # 匹配成功的 token 不在 hay 里，但确实是有效命中。
            # 之前写 `t in hay` 导致 `CSS B737-500` 的 B735（hit_w=18，
            # 靠 strip 命中）被判 core_hit=False 而丢弃，最后错选 B737。
            core_hit = any(_has_digit(t) for t in matched) or des_hit
            if not core_hit:
                core_hit = any(len(t) >= 5 and t not in _NOISE for t in matched)
        else:
            # 无数字输入（`Some Random Thing` / `King Air`）：
            # 要求命中词**长且具体**。阈值取 6 而非 5 —— `thing`(5)
            # 会命中 "Wild Thing" 导致误判 WILT（实测），
            # 而真正的型号词（`baron` 5 位也有，但它同时在 _NOISE 外的
            # 具体机名里）用 designator 命中或英文全名兜底即可。
            core_hit = des_hit or any(
                (len(t) >= 6 and t not in _NOISE) or _is_cjk_strong(t)
                for t in matched
            )
        if not core_hit:
            continue
        s = ratio * 100.0
        s += _score(r, engines)
        if des_hit:
            s += 70
        # designator 与输入的压缩形式相等 → 强优先（"SR22" → SR22 而不是 S22T）
        if tail_up and r[0] == tail_up:
            s += 60
        # model 压缩后与输入的**型号词**压缩后相等 → 强优先。
        # 这条解决 SR22/S22T：
        #     SR22 | model="SR-22"        → 压缩 "sr22" == tail "sr22"  +40
        #     S22T | model="SR-22 Turbo"  → 压缩 "sr22turbo" ≠          不加
        # 注意要用**末词/tail**而不是整名：输入是 "Cirrus SR22"，
        # 整名压缩成 "cirrussr22" 永远不等于 "sr22"（踩过）。
        model_c = r[1].lower().replace("-", "").replace(" ", "")
        if model_c:
            # 用**型号特征词**（最长非派生 token）而不是末词：
            #   `Cirrus SR22`    最长是 `sr22`    → 对上 model "SR-22"  ✅
            #   `Airbus A320neo` 最长是 `a320neo` → 对上 model "A320neo" ✅
            #   `Airbus A320 Neo` 最长是 `a320`   → 对上 "A320"          ✅
            # 用末词会两头翻车（`A320neo` 的末词是 `a320`，
            # 反而把 A320 Prestige 抬上来）。
            cand_t = last_tok.lower().replace("-", "")
            if cand_t and model_c == cand_t:
                s += 40
        name_c = name.lower().replace("-", "").replace(" ", "")
        if model_c and model_c == name_c:
            s += 40
        # 输入的**末词**被 model 完整包含 → 优先。
        #
        # 为什么需要：`Airbus A320neo` 切词得 ['a320neo','a320']，
        # 两个候选都命中 —— A320 Prestige（rank=2）靠排名压过
        # A320neo（rank=5），但输入明确写了 neo，应该选 A20N。
        # `a320neo` ⊂ "a320neo"(A20N 的 model) 而 ⊄ "a320 prestige"，
        # 用它当判别信号正好把两者分开。
        if last_tok and len(last_tok) >= 4:
            if last_tok in r[1].lower().replace("-", "").replace(" ", ""):
                s += 30
        if s > best_score:
            best_score = s
            best = r
    return best[0] if best else ""


def normalize_icao(raw: str, name: str = "", engines: int = 0) -> str:
    """把模拟器上报的机型 ICAO 规范化成"服务器可用"的标准码。

## 为什么要规范化（2026-11-27 livery=null 的根因）

之前的推断只在 ICAO 为**空**（或 `ATC*` 占位符）时触发。但有些机模
上报的 ICAO **非空却不是标准码**——IniBuilds A350 的 ATC ICAO TYPE
就是 `A350-900`（标准码应为 `A359`）。它原样进入 PI：

    EQUIPMENT=A350-900:AIRLINE=SJX:REG=B-6419      ← 没有 LIVERY 键

`swift_livery("A350-900")` 在 swift 机型库（2606 条）里查不到 id →
LIVERY 键不发 → **站点 JSON 里 livery 恒为 null**（实测 22:52 会话，
equipment 也是非标准码）。空值路径（F-14 空 ICAO）早已修掉，
这一条"非空但不可用"的路径漏了。

## 规则

1. `ATC*` 开头 → 占位符，当未知（走推断）。
2. ICAO 在 swift 机型库**查得到 id** → 原样保留（绝大多数正常机模）。
3. 查不到 → 按机模名称重新推断；推断出来的一定在库里
   （`guess_designator` 的返回值都来自库），`A350-900` → A359。
4. 推断也失败 → 保留原值：equipment 至少还能显示，
   livery 为 null 是 swift 库不覆盖该机型，无法再改善。

    实测：`A350-900`→A359、`Airbus A350-900`→A359、`A350-1000`→A35K。
    """
    icao = (raw or "").strip().upper()
    if icao.startswith("ATC"):
        icao = ""
    if icao and swift_livery(icao):
        return icao                      # 规则 2：库里有 id，直接用
    # 名称缺失时把 ICAO 码本身当名称输入（"A350-900" 自身就能推出 A359）
    guessed = guess_designator(name or icao, engines)
    return guessed or icao               # 规则 3/4


def lookup(designator: str) -> Optional[dict]:
    """按 designator 精确查一条记录（调试/展示用）。"""
    up = (designator or "").strip().upper()
    if not up:
        return None
    for r in _load():
        if r[0] == up:
            return {
                "designator": r[0], "model": r[1], "manufacturer": r[2],
                "engines": r[3], "wtc": r[4], "rank": r[5],
                "engine_type": engine_type_of(r[0]),
                "combined_type": _combined_type(r[3], r[4], r[0]),
            }
    return None


# ---------------- swift LIVERY 生成 ----------------
#
# ## 为什么必须生成 swift 格式的 LIVERY
#
# XFlySim 的 FSD 服务器**从 PI 的 `LIVERY` 字段反查机型**，而不是直接用
# `EQUIPMENT=`。实测证据（2026-11-25）：
#
#     swift 发: #SBSJX010:SERVER:PI:GEN:EQUIPMENT=F14:LIVERY=SWIFT_L181A4278M87561
#     服务器存: "equipment":"F14", "livery":"SWIFT_L181A4278M87561"      ✅
#
#     swift 发: ...EQUIPMENT=A320:LIVERY=SWIFT_L1759A672M87075
#     服务器存: "equipment":"A320"                                        ✅
#
#     swift 发: ...EQUIPMENT=A388:LIVERY=SWIFT_L764A679M89612
#     服务器存: "equipment":"A388"                                        ✅
#
# 三条的 `A<n>` 恰好都是 airfcrafticao.json 里的 `id`，且该 id 的
# designator 与服务器存下来的 equipment **完全一致**：
#
#     4278 → F14     672 → A320     679 → A388
#
# ## 我们原来的做法为什么失败
#
# 之前 packet.plane_info() 把**尾号塞进 LIVERY**（为了让尾号穿透 swift 中继）：
#
#     #SBSJX110:SERVER:PI:GEN:EQUIPMENT=F14:AIRLINE=SJX:LIVERY=N988DR:ENGINES=2:REG=N988DR
#
# `N988DR` 不含 `A<id>`，服务器解析不出机型 →
# **整条 PI 的 aircraft 段被丢弃**（equipment/airline/livery/engines 全 null），
# 但服务器不报错（$ER 里看不到），极难排查。
#
# ## 格式
#
#     SWIFT_L<liveryId>A<aircraftId>M<modelId>
#
# 三段都可省。我们只需要保证 `A<aircraftId>` 存在且对应正确机型；
# 涂装/模型 id 我们无法准确得到，省略即可（swift 自己戴涂装时才会有）。
# 实测 `SWIFT_A10764`（只有 A 段）也能被服务器正确解析成 A20N。
#
# 尾号仍然通过 `REG=` 携带 —— 服务器认这个键（JSON 里 registration 有值），
# 所以不必再占用 LIVERY。


def _load_ids() -> dict[str, int]:
    """加载 designator → swift 记录 id 的映射（惰性，首调用约 3ms）。"""
    global _ids
    with _db_lock:
        if _ids is not None:
            return _ids
        import os
        p = os.path.join(os.path.dirname(os.path.abspath(__file__)), _ID_FILE)
        try:
            with open(p, "rb") as f:
                blob = b"".join(
                    ln for ln in f.read().splitlines() if not ln.startswith(b"#")
                )
            pairs = json.loads(zlib.decompress(base64.b64decode(blob)))
            _ids = {str(k).upper(): int(v) for k, v in pairs}
        except Exception:
            # 映射文件缺失不该让整个客户端起不来 —— 只是 LIVERY 退化成
            # 纯涂装码（老行为），其它功能照常。
            _ids = {}
        return _ids


def swift_livery(designator: str) -> str:
    """按 swift 格式生成 LIVERY 值：`SWIFT_A<id>`。

    返回空串表示映射表里没有这个 designator —— 此时调用方应**不要**
    把尾号塞进 LIVERY（那正是导致服务器丢弃整条 PI 的老 bug），
    而是让 LIVERY 缺失，仅靠 `REG=` 传尾号。
    """
    des = (designator or "").strip().upper()
    if not des:
        return ""
    rid = _load_ids().get(des)
    if rid is None:
        return ""
    return f"SWIFT_A{rid}"