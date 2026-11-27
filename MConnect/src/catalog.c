/* MConnect — 机型/涂装目录与四级匹配。见 catalog.h 的设计说明。 */
#include "catalog.h"
#include "ini.h"
#include "sc.h"

#include <stdio.h>
#include <string.h>

/* 目录容量：FSLTL 基础包就有 3600+ 个飞机文件夹，每个文件夹若干 FLTSIM 节，
 * 真实机器上容易到 2 万条。给 4 万条（约 40000 × 232B ≈ 9MB）留足余量，
 * 超出后只保留前 4 万条（不影响常用机型，且避免无限吃内存）。 */
#define CATALOG_MAX 40000

static CatalogEntry* g_cat = NULL;
static int  g_cat_n = 0;
static int  g_cat_ready = 0;
static volatile LONG g_cat_stop = 0;
static HANDLE g_cat_thread = NULL;
static CRITICAL_SECTION g_cat_lock;
static int g_cat_lock_init = 0;
/* 用户选择的 AI 模型库偏好（CAT_LIB_*）。原子读写，网络线程可随时切换。 */
static volatile LONG g_cat_lib = CAT_LIB_AUTO;

/* ---------------- AI 模型库偏好 ---------------- */

const char* catalog_lib_name(int lib) {
    switch (lib) {
    case CAT_LIB_FSLTL: return "FSLTL";
    case CAT_LIB_AIG:   return "AIG";
    default:            return "AUTO";
    }
}

const char* catalog_source_name(int source) {
    switch (source) {
    case CAT_SRC_FSLTL: return "FSLTL";
    case CAT_SRC_AIG:   return "AIG";
    default:            return "OTHER";
    }
}

int catalog_lib(void) {
    return (int)InterlockedCompareExchange(&g_cat_lib, 0, 0);
}

void catalog_set_lib(int lib) {
    int old = catalog_lib();
    if (lib != CAT_LIB_AUTO && lib != CAT_LIB_FSLTL && lib != CAT_LIB_AIG) {
        lib = CAT_LIB_AUTO;
    }
    InterlockedExchange(&g_cat_lib, lib);
    if (old != lib) {
        xc_logf("catalog: AI 模型库偏好 %s -> %s（优先使用，缺失时回退其它库）",
                catalog_lib_name(old), catalog_lib_name(lib));
    }
}

/* 从 Community 包名判断来源。FSLTL 基础包是 fsltl-traffic-base，
 * AIG 是 aig-aitraffic-* / AIG_AIManager / aigaim-*，与大小写无关。
 * 两种库的 title 前缀也分别有 FSLTL_ / AIGAIM_，但包名更稳：涂装包
 * 里的 title 可能继承机模名，不一定带库前缀。 */
static int source_from_package(const char* pkg) {
    char low[128];
    size_t i, n = 0;
    if (!pkg || !*pkg) return CAT_SRC_OTHER;
    for (i = 0; pkg[i] && n < sizeof(low) - 1; i++)
        low[n++] = (char)tolower((unsigned char)pkg[i]);
    low[n] = '\0';
    if (strstr(low, "fsltl")) return CAT_SRC_FSLTL;
    if (strstr(low, "aigaim") || strstr(low, "aig-aitraffic") ||
        strncmp(low, "aig_", 4) == 0) return CAT_SRC_AIG;
    return CAT_SRC_OTHER;
}

/* ---------------- 字符串工具 ---------------- */

/* 忽略大小写与 '-'、空格的比较（尾号/航司码比对用）。
 * B-1234 与 B1234、b-1234 视为相同。 */
static int norm_eq(const char* a, const char* b) {
    while (*a && *b) {
        while (*a == '-' || *a == ' ') a++;
        while (*b == '-' || *b == ' ') b++;
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 0;
        if (*a) a++;
        if (*b) b++;
    }
    while (*a == '-' || *a == ' ') a++;
    while (*b == '-' || *b == ' ') b++;
    return *a == '\0' && *b == '\0';
}

/* 检查 hay 里是否包含 needle（忽略大小写与 '-'），用于"变体名含尾号"这类匹配。 */
static int norm_contains(const char* hay, const char* needle) {
    char nh[256], nn[64];
    int i = 0, j = 0, k;
    if (!hay || !needle || !needle[0]) return 0;
    while (*hay && i < (int)sizeof(nh) - 1) {
        if (*hay != '-' && *hay != ' ') nh[i++] = (char)toupper((unsigned char)*hay);
        hay++;
    }
    nh[i] = '\0';
    while (*needle && j < (int)sizeof(nn) - 1) {
        if (*needle != '-' && *needle != ' ') nn[j++] = (char)toupper((unsigned char)*needle);
        needle++;
    }
    nn[j] = '\0';
    if (!nn[0]) return 0;
    for (k = 0; nh[k]; k++) {
        int m = 0;
        while (nn[m] && nh[k + m] == nn[m]) m++;
        if (!nn[m]) return 1;
    }
    return 0;
}

void catalog_sanitize_icao(const char* raw, char* out, int cap) {
    int n = 0;
    if (!out || cap < 2) return;
    out[0] = '\0';
    if (!raw) return;
    while (*raw && n < cap - 1) {
        unsigned char c = (unsigned char)*raw;
        if (isalnum(c) || c == '-') out[n++] = (char)toupper(c);
        else if (n > 0) break;   /* 遇到分隔符即截断（"B738/W" → "B738"） */
        raw++;
    }
    out[n] = '\0';
    if (n < 2) out[0] = '\0';    /* 太短不可信（"1"、"A"） */
}

/* ---------------- 内置兜底表 ---------------- */

/* 模拟器自带机型的容器标题（2020 与 2024 通用，实测名称一致）。
 * 用途：未安装任何第三方机型包（FSLTL/AIG）时，至少能把他机显示成
 * 对应级别的通用机型，而不是什么都不显示。
 *
 * 注意 2024 的 `Asobo PassiveAircraft *` 系列需要 _EX1 + 涂装名，
 * 这里只在 supports_modular 时启用（见 match 逻辑）。 */
typedef struct {
    const char* icao;
    const char* title;
} BuiltinEntry;

static const BuiltinEntry kBuiltin[] = {
    /* --- 波音 --- */
    { "B736", "Boeing 737-600 Asobo" },
    { "B738", "Boeing 737-800 Asobo" },
    { "B739", "Boeing 737-900 Asobo" },
    { "B37M", "Boeing 737 MAX 8 Asobo" },
    { "B38M", "Boeing 737 MAX 8 Asobo" },
    { "B744", "Boeing 747-8i Asobo" },
    { "B748", "Boeing 747-8i Asobo" },
    { "B78X", "Boeing 787-10 Asobo" },
    /* --- 空客 --- */
    { "A20N", "Airbus A320 Neo Asobo" },
    { "A319", "Airbus A319 Asobo" },
    { "A320", "Airbus A320 Neo Asobo" },
    { "A321", "Airbus A321 Asobo" },
    { "A310", "Airbus A310-300 Asobo" },
    { "A333", "Airbus A330-300 Asobo" },
    { "A339", "Airbus A330-900 Asobo" },
    /* --- 支线 / 涡桨 --- */
    { "AT76", "ATR 72-600 Asobo" },
    { "AT75", "ATR 72-600 Asobo" },
    { "AT46", "ATR 42-600 Asobo" },
    { "DH8D", "De Havilland Canada DHC-8-400 Asobo" },
    { "E170", "Embraer E170 Asobo" },
    { "E175", "Embraer E175 Asobo" },
    { "E190", "Embraer E190 Asobo" },
    { "E195", "Embraer E195 Asobo" },
    { "CRJ7", "Bombardier CRJ-700 Asobo" },
    { "CRJ9", "Bombardier CRJ-900 Asobo" },
    { "CRJ2", "Bombardier CRJ-200 Asobo" },
    /* --- 通用航空 --- */
    { "C172", "Cessna Skyhawk G1000 Asobo" },
    { "C152", "Cessna 152 Asobo" },
    { "C208", "Cessna 208B Grand Caravan EX Asobo" },
    { "TBM9", "Daher TBM 930 Asobo" },
    { "BE58", "Beechcraft Baron G58 Asobo" },
    { "B350", "Beechcraft King Air 350i Asobo" },
    { "SR22", "Cirrus SR22 Asobo" },
    { "DA62", "DA62 Asobo" },
    { "DA40", "DA40 NG Asobo" },
    { "PC12", "Pilatus PC-12 NGX Asobo" },
    { "PC6P", "Pilatus Porter PC-6 Asobo" },
    { "C25C", "Cessna Citation CJ4 Asobo" },
    { "C700", "Cessna Citation Longitude Asobo" },
    { "T38", "Boeing T-38 Asobo" },
    /* --- 通用兜底（任何机型都能用） --- */
    { "ZZZZ", "Generic Airliner Twin Engines Asobo 00" }
};

/* 机型别名表：他机上报的 ICAO 在本机不存在时，落到同族机型。
 * 与 XConnect 的 kTypeAlias 同源（186 条），但目标换成"MSFS 里真实存在的
 * 容器标题对应机型"——MSFS 自带的机型集远小于 X-CSL 的 500+ 型号，
 * 所以这里的目标更少、更集中。 */
typedef struct {
    const char* type;
    const char* target;
} TypeAlias;

static const TypeAlias kAlias[] = {
    /* 波音 737 家族 → B738 */
    { "B731", "B738" }, { "B732", "B738" }, { "B733", "B738" },
    { "B734", "B738" }, { "B735", "B738" }, { "B736", "B738" },
    { "B737", "B738" }, { "B73G", "B738" }, { "B73X", "B738" },
    { "B732", "B738" }, { "B735", "B738" }, { "B739", "B738" },
    { "B39M", "B38M" }, { "B3XM", "B38M" }, { "B37M", "B38M" },
    /* 波音 747/757/767/777 → B748 */
    { "B741", "B748" }, { "B742", "B748" }, { "B743", "B748" },
    { "B744", "B748" }, { "B74S", "B748" }, { "B748", "B748" },
    { "B752", "B738" }, { "B753", "B738" },
    { "B762", "B78X" }, { "B763", "B78X" }, { "B764", "B78X" },
    { "B772", "B78X" }, { "B773", "B78X" }, { "B77L", "B78X" },
    { "B77W", "B78X" }, { "B788", "B78X" }, { "B789", "B78X" },
    { "B78X", "B78X" },
    /* 空客 A320 家族 → A20N */
    { "A318", "A20N" }, { "A319", "A20N" }, { "A320", "A20N" },
    { "A321", "A20N" }, { "A20N", "A20N" }, { "A21N", "A20N" },
    /* 空客宽体 */
    { "A306", "A310" }, { "A30B", "A310" }, { "A310", "A310" },
    { "A332", "A333" }, { "A333", "A333" }, { "A337", "A333" },
    { "A338", "A339" }, { "A339", "A339" }, { "A342", "A333" },
    { "A343", "A333" }, { "A345", "A333" }, { "A346", "A333" },
    { "A359", "A339" }, { "A35K", "A339" }, { "A388", "B748" },
    { "A225", "B748" },
    /* 巴航工业 */
    { "E135", "E175" }, { "E145", "E175" }, { "E45X", "E175" },
    { "E170", "E175" }, { "E75L", "E175" }, { "E75S", "E175" },
    { "E175", "E175" }, { "E190", "E195" }, { "E195", "E195" },
    /* E19L 是 E190LR 在部分 VATSIM/航司数据里的写法 */
    { "E19L", "E195" },
    { "E290", "E195" }, { "E295", "E195" }, { "E55P", "C25C" },
    { "E50P", "C25C" }, { "E545", "C25C" }, { "E550", "C25C" },
    { "E35L", "C25C" },
    /* 庞巴迪 / 三菱 */
    { "CRJ1", "CRJ2" }, { "CRJ2", "CRJ2" }, { "CRJ7", "CRJ7" },
    { "CRJ9", "CRJ9" }, { "CRJX", "CRJ9" }, { "CL30", "C25C" },
    { "CL35", "C25C" }, { "CL60", "C25C" }, { "GL5T", "C700" },
    { "GL7T", "C700" }, { "GLEX", "C700" }, { "C Series 100", "E195" },
    { "BCS1", "E195" }, { "BCS3", "E195" },
    /* ATR / 冲量 */
    { "AT43", "AT46" }, { "AT45", "AT46" }, { "AT46", "AT46" },
    { "AT72", "AT76" }, { "AT75", "AT76" }, { "AT76", "AT76" },
    { "DH8A", "DH8D" }, { "DH8B", "DH8D" }, { "DH8C", "DH8D" },
    { "DH8D", "DH8D" }, { "D328", "DH8D" }, { "J328", "DH8D" },
    /* 公务机 / 涡桨 */
    { "FA8X", "C700" }, { "FA7X", "C700" }, { "FA50", "C25C" },
    { "FA20", "C25C" }, { "F2TH", "C25C" }, { "F900", "C700" },
    { "GLF4", "C700" }, { "GLF5", "C700" }, { "GLF6", "C700" },
    { "G280", "C25C" }, { "G450", "C700" }, { "G550", "C700" },
    { "G650", "C700" }, { "LJ45", "C25C" }, { "LJ60", "C25C" },
    { "LJ75", "C25C" }, { "PC24", "C25C" }, { "HDJT", "C25C" },
    { "PRM1", "C25C" }, { "BE40", "C25C" }, { "C25A", "C25C" },
    { "C25B", "C25C" }, { "C25C", "C25C" }, { "C501", "C25C" },
    { "C525", "C25C" }, { "C550", "C25C" }, { "C560", "C25C" },
    { "C56X", "C25C" }, { "C680", "C25C" }, { "C68A", "C25C" },
    { "C700", "C700" }, { "C750", "C700" }, { "C510", "C25C" },
    /* 塞斯纳 / 比奇 / 派珀 */
    { "C150", "C152" }, { "C152", "C152" }, { "C72R", "C172" },
    { "C172", "C172" }, { "C182", "C172" }, { "C206", "C208" },
    { "C207", "C208" }, { "C208", "C208" }, { "C210", "C208" },
    { "BE20", "B350" }, { "BE30", "B350" }, { "BE9L", "B350" },
    { "BE99", "B350" }, { "B350", "B350" }, { "B190", "B350" },
    { "BE36", "BE58" }, { "BE58", "BE58" }, { "BE55", "BE58" },
    { "PA28", "C172" }, { "PA32", "C172" }, { "PA34", "BE58" },
    { "PA46", "TBM9" }, { "P28A", "C172" }, { "P28B", "C172" },
    { "M20P", "C172" }, { "SR20", "SR22" }, { "SR22", "SR22" },
    { "DA40", "DA40" }, { "DA42", "DA62" }, { "DA62", "DA62" },
    { "DV20", "DA40" }, { "PC12", "PC12" }, { "PC6P", "PC6P" },
    { "PC6T", "PC6P" }, { "TBM7", "TBM9" }, { "TBM8", "TBM9" },
    { "TBM9", "TBM9" }, { "TEX2", "T38" }, { "T38", "T38" }
};

/* ---------------- 非标准机型码规范化 ---------------- */

/* catalog 反查/ATC MODEL 给出的"机型"并不总是标准 ICAO 码。
 * 实测 iniBuilds A350 MSFS 的 aircraft.cfg icao_type_designator 就是
 * "A350-900"（非标，标准码是 A359）；Fenix A320 干脆没有
 * icao_type_designator，ATC MODEL 返回通用值 "ATCCOM"。
 * 这里把已知的长码/连写映射成标准 ICAO，供识别输出前统一调用。
 * 注意与 kAlias 区分：kAlias 的目标是"本机 MSFS 里真实存在的容器机型"
 * （用于 AI 建模回退），本表的目标是**标准 ICAO 码**（用于上报服务器）。 */
typedef struct {
    const char* raw;
    const char* icao;
} CanonEntry;

static const CanonEntry kCanonical[] = {
    /* 空客 */
    { "A350-900", "A359" }, { "A350-1000", "A35K" }, { "A350F", "A359" },
    { "A350-900F", "A359" }, { "A350", "A359" },
    { "A330-900", "A339" }, { "A330-800", "A338" },
    { "A330-300", "A333" }, { "A330-200", "A332" }, { "A330", "A333" },
    { "A340-600", "A346" }, { "A340-500", "A345" },
    { "A340-300", "A343" }, { "A340-200", "A342" }, { "A340", "A343" },
    { "A380-800", "A388" }, { "A380", "A388" },
    { "A320-200", "A320" }, { "A320-100", "A320" }, { "A320NEO", "A20N" },
    { "A321-200", "A321" }, { "A321NEO", "A21N" },
    { "A319-100", "A319" }, { "A318-100", "A318" },
    { "A220-300", "BCS3" }, { "A220-100", "BCS1" }, { "A220", "BCS3" },
    { "A310-300", "A310" }, { "A310-200", "A310" },
    { "A300-600", "A306" }, { "A300B", "A306" }, { "A300", "A306" },
    /* 波音 */
    { "B737-800", "B738" }, { "737-800", "B738" },
    { "B737-700", "B737" }, { "737-700", "B737" },
    { "B737-600", "B736" }, { "B737-900", "B739" }, { "737-900", "B739" },
    { "B737-900ER", "B739" },
    { "B737-8", "B38M" }, { "737-8", "B38M" }, { "B737-9", "B39M" },
    { "737-9", "B39M" }, { "B737MAX8", "B38M" }, { "737MAX", "B38M" },
    { "737 MAX 8", "B38M" }, { "737 MAX 9", "B39M" },
    { "B747-8", "B748" }, { "747-8", "B748" }, { "B747-400", "B744" },
    { "747-400", "B744" }, { "B747", "B744" }, { "747", "B744" },
    { "B777-300ER", "B77W" }, { "777-300ER", "B77W" },
    { "B777-300", "B773" }, { "777-300", "B773" },
    { "B777-200", "B772" }, { "777-200", "B772" }, { "B777", "B77W" },
    { "777", "B77W" },
    { "B767-400", "B764" }, { "B767-300", "B763" }, { "767-300", "B763" },
    { "B767-200", "B762" }, { "B767", "B763" }, { "767", "B763" },
    { "B757-300", "B753" }, { "B757-200", "B752" }, { "757-200", "B752" },
    { "B757", "B752" }, { "757", "B752" },
    { "B787-10", "B78X" }, { "787-10", "B78X" }, { "B787-9", "B789" },
    { "787-9", "B789" }, { "B787-8", "B788" }, { "787-8", "B788" },
    { "B787", "B789" }, { "787", "B789" },
    /* 支线 / 其它常见 */
    { "ATR72", "AT76" }, { "ATR-72", "AT76" }, { "ATR 72", "AT76" },
    { "ATR42", "AT46" }, { "ATR-42", "AT46" }, { "ATR 42", "AT46" },
    { "DHC-8", "DH8D" }, { "DASH8", "DH8D" }, { "DASH-8", "DH8D" },
    { "CRJ-700", "CRJ7" }, { "CRJ-900", "CRJ9" }, { "CRJ-200", "CRJ2" },
    { "E-JET", "E175" }, { "E-JETS", "E175" },
    { "MD-11", "MD11" }, { "MD11", "MD11" }, { "MD-82", "MD82" },
    { "MD-83", "MD83" }, { "MD-88", "MD88" }, { "MD-90", "MD90" }
};

/* 标准化：查非标表；查不到则原样返回 NULL（调用者保留原值）。 */
const char* catalog_canonical_icao(const char* icao) {
    size_t i;
    if (!icao || !icao[0]) return NULL;
    for (i = 0; i < sizeof(kCanonical) / sizeof(kCanonical[0]); i++) {
        if (_stricmp(kCanonical[i].raw, icao) == 0)
            return kCanonical[i].icao;
    }
    return NULL;
}

/* ---------------- title 关键词机型识别 ---------------- */

/* catalog 反查（title 精确匹配）失败时的兜底：从 title 文本里找机型
 * 关键词。实测教训 —— Fenix A320 涂装的 title 是连写的
 * "FenixA320 IAE SL Anon x Mika TRINITY AIRLINES"：
 *   - "FenixA320" 与任何已安装目录 title 都精确不等 → 反查失败；
 *   - atc_model 又没设 → 回退 "ATCCOM" 上报 → 对端 CSL/AI 匹配全灭。
 * 关键词按子串匹配（大小写不敏感），**长词在前**防止 "A320" 抢先吃掉
 * "A320NEO" / "A350" 抢先吃掉 "A350-900"。
 * 只覆盖常见干线/支线；通航机型依赖目录扫描即可。 */
typedef struct {
    const char* keyword;
    const char* icao;
} KwEntry;

static const KwEntry kTitleKeywords[] = {
    /* 空客：长的先 */
    { "A350-1000", "A35K" }, { "A350-900", "A359" }, { "A350F", "A359" },
    { "A350", "A359" },
    { "A330-900", "A339" }, { "A330-800", "A338" },
    { "A330-300", "A333" }, { "A330-200", "A332" }, { "A330", "A333" },
    { "A340-600", "A346" }, { "A340-300", "A343" }, { "A340", "A343" },
    { "A380", "A388" },
    { "A321", "A321" }, { "A320", "A320" },
    { "A319", "A319" }, { "A318", "A318" },
    { "A220-300", "BCS3" }, { "A220-100", "BCS1" }, { "A220", "BCS3" },
    { "CS100", "BCS1" }, { "CS300", "BCS3" },
    { "A310", "A310" }, { "A300", "A306" },
    /* 波音：737 MAX 优先于 737 */
    { "737MAX10", "B3XM" }, { "737MAX9", "B39M" }, { "737MAX8", "B38M" },
    { "737 MAX", "B38M" }, { "737MAX", "B38M" },
    { "737-900", "B739" }, { "737-800", "B738" }, { "737-700", "B737" },
    { "737-600", "B736" }, { "B737", "B738" },
    { "787-10", "B78X" }, { "787-9", "B789" }, { "787-8", "B788" },
    { "787", "B789" },
    { "747-8", "B748" }, { "747-400", "B744" }, { "747", "B744" },
    { "777-300", "B77W" }, { "777-200", "B772" }, { "777", "B77W" },
    { "767-400", "B764" }, { "767-300", "B763" }, { "767-200", "B762" },
    { "767", "B763" },
    { "757-300", "B753" }, { "757-200", "B752" }, { "757", "B752" },
    /* 支线 / 其它 */
    { "ATR72", "AT76" }, { "ATR-72", "AT76" }, { "ATR 72", "AT76" },
    { "ATR42", "AT46" }, { "ATR-42", "AT46" }, { "ATR 42", "AT46" },
    { "DHC-8", "DH8D" }, { "DASH", "DH8D" },
    { "CRJ-700", "CRJ7" }, { "CRJ-900", "CRJ9" }, { "CRJ-200", "CRJ2" },
    { "CRJ700", "CRJ7" }, { "CRJ900", "CRJ9" }, { "CRJ200", "CRJ2" },
    { "E190", "E190" }, { "E195", "E195" }, { "E175", "E175" },
    { "E170", "E170" },
    { "MD-11", "MD11" }, { "MD11", "MD11" },
    { "A400", "A400M" }, { "C-17", "C17" }, { "C17", "C17" },
    { "C130", "C130" }, { "KC-135", "K135" }, { "KC135", "K135" },
    { "B-52", "B52H" }, { "B52", "B52H" },
    { "AN-22", "A225" }, { "AN22", "A225" }, { "AN-12", "A124" },
    { "AN12", "A124" }, { "AN-1", "A124" },
    { "Y-20", "Y20" }, { "Y20", "Y20" }, { "Y-9", "Y9" },
    { "IL-76", "IL76" }, { "IL76", "IL76" }
};

/* 从 title 关键词猜 ICAO。大小写不敏感的子串匹配，命中即返回。 */
const char* catalog_icao_from_title_keywords(const char* title) {
    size_t i;
    char low[512];
    size_t n;

    if (!title || !title[0]) return NULL;
    /* 转小写副本（title ≤ 256，512 安全） */
    for (n = 0; title[n] && n < sizeof(low) - 1; n++)
        low[n] = (char)((title[n] >= 'A' && title[n] <= 'Z')
                            ? title[n] + 32 : title[n]);
    low[n] = '\0';

    for (i = 0; i < sizeof(kTitleKeywords) / sizeof(kTitleKeywords[0]); i++) {
        /* 关键词表里的拼写是大写（"A320"/"B737"…），而 low 是小写 title。
         * 旧代码直接 strstr(low, k) → **永远匹配不上**，Fenix A320 连写
         * 涂装标题反查失败、ATC TYPE 又是 ATCCOM 占位符 → 本机机型识别
         * 彻底失效（2026-11-27 修复）。两侧都转小写后再比较。 */
        char kw[32];
        size_t j, m = 0;
        const char* k = kTitleKeywords[i].keyword;
        for (j = 0; k[j] && m < sizeof(kw) - 1; j++)
            kw[m++] = (char)((k[j] >= 'A' && k[j] <= 'Z')
                                 ? k[j] + 32 : k[j]);
        kw[m] = '\0';
        /* 关键词已在表内保证长词在前，直接 strstr */
        if (strstr(low, kw)) return kTitleKeywords[i].icao;
    }
    return NULL;
}

/* ---------------- 目录存储 ---------------- */

/* 添加条目（按 title+livery 去重）。持锁调用或扫描线程独占时调用。 */
static void cat_add_raw(const char* title, const char* livery, const char* icao,
                        const char* airline, const char* reg, int modular,
                        int source) {
    int i;
    if (!title || !title[0]) return;
    if (g_cat_n >= CATALOG_MAX) return;

    /* 去重：同一 (title, livery) 只留一条。同一架飞机在 base 包与 livery 包
     * 里可能各出现一次（2020 的 variation 继承），重复会浪费匹配时间。 */
    for (i = g_cat_n - 1; i >= 0 && i > g_cat_n - 64; i--) {
        if (_stricmp(g_cat[i].title, title) == 0 &&
            _stricmp(g_cat[i].livery, livery ? livery : "") == 0) {
            /* 补齐已有条目缺失的字段（后出现的包可能带更完整的航司/尾号） */
            if (!g_cat[i].airline[0] && airline && airline[0])
                snprintf(g_cat[i].airline, sizeof(g_cat[i].airline), "%s", airline);
            if (!g_cat[i].reg[0] && reg && reg[0])
                snprintf(g_cat[i].reg, sizeof(g_cat[i].reg), "%s", reg);
            return;
        }
    }

    {
        CatalogEntry* e = &g_cat[g_cat_n++];
        memset(e, 0, sizeof(*e));
        snprintf(e->title, sizeof(e->title), "%s", title);
        if (livery && livery[0])
            snprintf(e->livery, sizeof(e->livery), "%s", livery);
        if (icao && icao[0])
            snprintf(e->icao, sizeof(e->icao), "%s", icao);
        if (airline && airline[0])
            snprintf(e->airline, sizeof(e->airline), "%s", airline);
        if (reg && reg[0])
            snprintf(e->reg, sizeof(e->reg), "%s", reg);
        e->modular = (unsigned char)(modular ? 1 : 0);
        e->source = (unsigned char)source;
    }
}

/* ---------------- 包扫描 ---------------- */

/* 从 aircraft.cfg 里抽 ICAO：
 *   1. [GENERAL] icao_type_designator（最准，2020/2024 都有）
 *   2. [GENERAL] atc_model（很多机型直接给 A20N/B738，也可能是本地化引用）
 * 注意 [FLTSIM.N] 节**不**含 icao_type_designator，只在 [GENERAL]。 */
static void icao_from_cfg(Ini* ini, char* out, int cap) {
    char v[128];

    out[0] = '\0';

    if (ini_goto(ini, "GENERAL") && ini_get(ini, "icao_type_designator", v, sizeof(v))) {
        catalog_sanitize_icao(v, out, cap);
        if (out[0]) return;
    }
    if (ini_goto(ini, "GENERAL") && ini_get(ini, "atc_model", v, sizeof(v))) {
        /* atc_model 可能是 "TT:ATCCOM.AC_MODEL_A20N.0.text" 这种本地化引用 */
        const char* p = strstr(v, "AC_MODEL_");
        if (p) {
            char tmp[32];
            int n = 0;
            p += 9;
            while (*p && *p != '.' && *p != ' ' && n < (int)sizeof(tmp) - 1)
                tmp[n++] = *p++;
            tmp[n] = '\0';
            catalog_sanitize_icao(tmp, out, cap);
        } else {
            catalog_sanitize_icao(v, out, cap);
        }
    }
}

/* 航司码清洗：只接受 2~4 位纯字母数字（排除 "TT:..." 本地化引用与航司全名） */
static void airline_from_cfg(Ini* ini, char* out, int cap) {
    char v[128];
    out[0] = '\0';
    if (!ini_get(ini, "icao_airline", v, sizeof(v)) || !v[0]) {
        if (!ini_get(ini, "atc_airline", v, sizeof(v))) return;
    }
    if (!v[0]) return;
    {
        size_t n = strlen(v);
        size_t k;
        if (n < 2 || n > 4) return;
        if (strncmp(v, "TT:", 3) == 0) return;
        for (k = 0; k < n; k++)
            if (!isalnum((unsigned char)v[k])) return;
        snprintf(out, (size_t)cap, "%s", v);
    }
}

/* 解析一个 aircraft.cfg：
 *   [GENERAL] icao_type_designator / atc_model
 *   [FLTSIM.N] title / atc_id / atc_airline / icao_airline / texture
 * 每个 FLTSIM 节产出一条条目。 */
static void scan_aircraft_cfg(const char* cfg_path, int source) {
    Ini* ini;
    char icao[16] = "";
    int has_fltsim = 0;

    ini = ini_open(cfg_path);
    if (!ini) return;

    icao_from_cfg(ini, icao, sizeof(icao));

    /* 是否有 FLTSIM 节 */
    ini_rewind(ini);
    {
        const char* sec;
        while ((sec = ini_next_section(ini)) != NULL) {
            if (_strnicmp(sec, "FLTSIM", 6) == 0) { has_fltsim = 1; break; }
        }
    }

    if (!has_fltsim) {
        /* 没有 FLTSIM 的包（部分 AI 包只有 [GENERAL]，2024 模块化包的
         * aircraft.cfg 也是 preset 形式）→ 用 GENERAL 的 title 直接建条目。
         * AICreate 只认 title，因此这样也能创建。 */
        char title[128] = "", airline[16] = "", reg[16] = "";
        if (ini_goto(ini, "GENERAL")) {
            ini_get(ini, "title", title, sizeof(title));
            airline_from_cfg(ini, airline, sizeof(airline));
            if (!ini_get(ini, "atc_id", reg, sizeof(reg))) reg[0] = '\0';
        }
        if (title[0]) {
            LOCK_ENTER(&g_cat_lock);
            cat_add_raw(title, "", icao, airline, reg, 0, source);
            LOCK_LEAVE(&g_cat_lock);
        }
        ini_close(ini);
        return;
    }

    /* 有 FLTSIM：逐节读 */
    ini_rewind(ini);
    {
        const char* sec;
        while ((sec = ini_next_section(ini)) != NULL) {
            char title[128] = "", airline[16] = "", reg[16] = "";
            char livery[64] = "", tex[64] = "";

            if (_strnicmp(sec, "FLTSIM", 6) != 0) continue;

            if (!ini_get(ini, "title", title, sizeof(title)) || !title[0])
                continue;

            airline_from_cfg(ini, airline, sizeof(airline));

            if (ini_get(ini, "atc_id", reg, sizeof(reg))) {
                size_t n = strlen(reg);
                if (n < 3 || n > 12) reg[0] = '\0';
            }

            /* texture= 不是涂装名，但在 2020 legacy 包里它是区分同机型
             * 多涂装的唯一标识（FSLTL 的 FSLTL_A20N_ZZZZ 一个文件夹一个
             * texture）。2024 模块化才用 livery.cfg 的 name。 */
            if (ini_get(ini, "texture", tex, sizeof(tex)) && tex[0])
                snprintf(livery, sizeof(livery), "%s", tex);

            LOCK_ENTER(&g_cat_lock);
            cat_add_raw(title, livery, icao, airline, reg, 0, source);
            LOCK_LEAVE(&g_cat_lock);
        }
    }

    ini_close(ini);
}

/* 解析 2024 的 livery.cfg：[GENERAL] name 给涂装名，容器标题需要靠运行时
 * 枚举（catalog_add_enumerated 配对）。这里先以"空标题"占位记录涂装名，
 * 待 2024 的 EnumerateSimObjectsAndLiveries 回来后再补标题。 */
static void scan_livery_cfg(const char* cfg_path, int source) {
    Ini* ini = ini_open(cfg_path);
    char name[128];
    if (!ini) return;
    if (!ini_goto(ini, "GENERAL") || !ini_get(ini, "name", name, sizeof(name))) {
        ini_close(ini);
        return;
    }
    if (name[0]) {
        const char* p = name;
        while (*p == '!') p++;   /* "!Airline" 的强制排序前缀 */
        LOCK_ENTER(&g_cat_lock);
        cat_add_raw("", p, "", "", "", 1, source);
        LOCK_LEAVE(&g_cat_lock);
    }
    ini_close(ini);
}

static void scan_dir(const char* dir, int depth, int source);

/* 递归找 aircraft.cfg 与 livery.cfg。depth 上限：MSFS 的包结构一般是
 * <pkg>/SimObjects/Airplanes/<name>/aircraft.cfg（4 层），给 8 层足够，
 * 同时避免在超深目录上浪费时间。
 *
 * source 沿目录树传递：任何一层目录名命中 FSLTL/AIG 包名，其子树的
 * 全部条目都标记为对应来源（Official/OneStore/<pkg> 这种多层布局也适用）。 */
static void scan_dir(const char* dir, int depth, int source) {
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char pattern[MAX_PATH];
    char sub[MAX_PATH];
    const char* slash;
    int guard = 0;

    if (depth > 8) return;
    if (InterlockedCompareExchange(&g_cat_stop, 1, 1) == 1) return;

    /* 目录自身（如 --scan 指向的 fsltl-traffic-base）也算一次包名 */
    slash = strrchr(dir, '\\');
    if (!slash) slash = strrchr(dir, '/');
    {
        int own = source_from_package(slash ? slash + 1 : dir);
        if (own != CAT_SRC_OTHER) source = own;
    }

    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        int child_source = source;
        if (fd.cFileName[0] == '.') continue;
        snprintf(sub, sizeof(sub), "%s\\%s", dir, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            /* 符号链接目录（Addons Linker）照常跟进，靠 depth 防环 */
            child_source = source_from_package(fd.cFileName);
            if (child_source == CAT_SRC_OTHER) child_source = source;
            scan_dir(sub, depth + 1, child_source);
        } else {
            if (_stricmp(fd.cFileName, "aircraft.cfg") == 0) {
                scan_aircraft_cfg(sub, source);
            } else if (_stricmp(fd.cFileName, "livery.cfg") == 0) {
                scan_livery_cfg(sub, source);
            }
        }
        if (++guard > 200000) break;   /* 防御性上限 */
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* ---------------- Community / Official 定位 ---------------- */

/* 从 UserCfg.opt 的 InstalledPackagesPath 得到 MSFS 内容根目录。
 *
 * ⚠️ **两个版本要都收，不能"找到第一个就用"**。
 *
 * 原来的实现是命中第一个 `UserCfg.opt` 就 return，而 2020 的路径排在
 * 2024 前面 —— 结果装了双版本的机器上，**2024 的 Community 永远扫不到**。
 * 实测本机：2020 → `L:\MSFS`，2024 → `F:\Community for MFS2024`，
 * 扫描只进了前者，2024 的机型一个没进来。
 *
 * 现在收集所有命中的根目录（去重），逐个扫描。
 * 这样双版本共存时两边机型都在，谁在跑都能匹配上。 */
#define PKG_ROOT_MAX 4
static int find_all_packages_paths(char roots[][MAX_PATH], int maxRoots) {
    static const char* kOpts[] = {
        /* 2020：MS Store / Xbox */
        "%LOCALAPPDATA%\\Packages\\Microsoft.FlightSimulator_8wekyb3d8bbwe\\LocalCache\\UserCfg.opt",
        "%APPDATA%\\Microsoft Flight Simulator\\UserCfg.opt",
        /* 2024：MS Store 包名是 Limitless；Steam/自定义安装走 APPDATA */
        "%LOCALAPPDATA%\\Packages\\Microsoft.Limitless_8wekyb3d8bbwe\\LocalCache\\UserCfg.opt",
        "%APPDATA%\\Microsoft Flight Simulator 2024\\UserCfg.opt"
    };
    char path[MAX_PATH];
    int n = 0;
    size_t i;

    for (i = 0; i < sizeof(kOpts) / sizeof(kOpts[0]); i++) {
        DWORD envn = ExpandEnvironmentStringsA(kOpts[i], path, sizeof(path));
        char root[MAX_PATH];
        int dup = 0, j;

        if (envn == 0 || envn > sizeof(path)) continue;
        if (!ini_read_global(path, "InstalledPackagesPath", root, sizeof(root)))
            continue;
        if (!root[0]) continue;
        /* 去重：双版本有时指向同一目录 */
        for (j = 0; j < n; j++) {
            if (_stricmp(roots[j], root) == 0) { dup = 1; break; }
        }
        if (dup) continue;
        if (n >= maxRoots) break;
        snprintf(roots[n], MAX_PATH, "%s", root);
        n++;
        xc_logf("catalog: packages root[%d] = %s", n - 1, root);
    }
    return n;
}

/* 尝试把 <root>/Community 与 <root>/Official/* 加进扫描队列。
 * 找不到就退回注册表里的安装目录。 */
static void scan_all_packages(void) {
    char roots[PKG_ROOT_MAX][MAX_PATH];
    char sub[MAX_PATH];
    int nroot, i;
    int scanned = 0;

    nroot = find_all_packages_paths(roots, PKG_ROOT_MAX);

    for (i = 0; i < nroot; i++) {
        static const char* kSubs[] = {
            "Community",        /* 2020 标准 */
            "Community2024",    /* 2024 常见（用户自建） */
            "Official",         /* 2020：Official/OneStore 或 Official/Steam */
            "Official2020",     /* 2024 布局下 2020 的官方包 */
            "Official2024",     /* 2024 官方包 */
            "StreamedPackages"  /* 2024 云流式包（含部分机型清单） */
        };
        size_t k;
        for (k = 0; k < sizeof(kSubs) / sizeof(kSubs[0]); k++) {
            snprintf(sub, sizeof(sub), "%s\\%s", roots[i], kSubs[k]);
            if (GetFileAttributesA(sub) != INVALID_FILE_ATTRIBUTES) {
                scan_dir(sub, 0, CAT_SRC_OTHER);
                scanned++;
            }
        }
    }

    if (scanned > 0) {
        xc_logf("catalog: 扫描了 %d 个内容目录（%d 个包根）", scanned, nroot);
        return;
    }

    /* 兜底：注册表 AppPath（模拟器根目录，Official 就在其下） */
    {
        static const char* kKeys[] = {
            "SOFTWARE\\Microsoft\\Flight Simulator\\10.0",
            "SOFTWARE\\Microsoft\\Flight Simulator2024\\10.0"
        };
        size_t i;
        for (i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); i++) {
            HKEY k;
            char app[MAX_PATH];
            DWORD type = 0, sz = sizeof(app);
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, kKeys[i], 0, KEY_READ, &k)
                != ERROR_SUCCESS) continue;
            if (RegQueryValueExA(k, "AppPath", NULL, &type, (LPBYTE)app, &sz)
                == ERROR_SUCCESS && type == REG_SZ) {
                RegCloseKey(k);
                snprintf(sub, sizeof(sub), "%s\\Community", app);
                if (GetFileAttributesA(sub) != INVALID_FILE_ATTRIBUTES)
                    scan_dir(sub, 0, CAT_SRC_OTHER);
                snprintf(sub, sizeof(sub), "%s\\Official", app);
                if (GetFileAttributesA(sub) != INVALID_FILE_ATTRIBUTES)
                    scan_dir(sub, 0, CAT_SRC_OTHER);
                return;
            }
            RegCloseKey(k);
        }
    }
    xc_logf("catalog: 未找到 MSFS 内容目录，仅使用内置机型表");
}

static THREAD_RET catalog_thread(THREAD_ARG arg) {
    const char* override_dir = (const char*)arg;
    DWORD t0 = GetTickCount();

    xc_logf("catalog: scan start");
    if (override_dir && override_dir[0]) {
        scan_dir(override_dir, 0, CAT_SRC_OTHER);
    } else {
        scan_all_packages();
    }

    LOCK_ENTER(&g_cat_lock);
    g_cat_ready = 1;
    {
        int n = g_cat_n;
        LOCK_LEAVE(&g_cat_lock);
        xc_logf("catalog: scan done in %lums, %d entries (+%d builtin)",
                (unsigned long)(GetTickCount() - t0), n,
                (int)(sizeof(kBuiltin) / sizeof(kBuiltin[0])));
    }
    return 0;
}

void catalog_start_scan(const char* dir_override) {
    static char dir_buf[MAX_PATH];

    if (!g_cat_lock_init) {
        InitializeCriticalSection(&g_cat_lock);
        g_cat_lock_init = 1;
    }
    if (!g_cat) {
        g_cat = (CatalogEntry*)calloc(CATALOG_MAX, sizeof(CatalogEntry));
        if (!g_cat) {
            xc_logf("catalog: 分配目录表失败（%d 条），仅内置表可用", CATALOG_MAX);
            return;
        }
    }
    if (g_cat_thread) return;

    /* 参数必须在本函数返回后仍然有效（线程异步读取），拷到静态缓冲 */
    if (dir_override && dir_override[0]) {
        snprintf(dir_buf, sizeof(dir_buf), "%s", dir_override);
    } else {
        dir_buf[0] = '\0';
    }
    InterlockedExchange(&g_cat_stop, 0);
    g_cat_thread = CreateThread(NULL, 0, catalog_thread,
                                dir_buf[0] ? dir_buf : NULL, 0, NULL);
}

void catalog_stop(int timeout_ms) {
    if (!g_cat_thread) return;
    InterlockedExchange(&g_cat_stop, 1);
    WaitForSingleObject(g_cat_thread, (DWORD)timeout_ms);
    CloseHandle(g_cat_thread);
    g_cat_thread = NULL;
}

int catalog_ready(void) { return g_cat_ready; }

int catalog_count(void) {
    int n;
    if (!g_cat_lock_init) return 0;
    LOCK_ENTER(&g_cat_lock);
    n = g_cat_n;
    LOCK_LEAVE(&g_cat_lock);
    return n;
}

int catalog_builtin_count(void) {
    return (int)(sizeof(kBuiltin) / sizeof(kBuiltin[0]));
}

/* ---------------- 内置表查找 ---------------- */

/* 在扫描结果里按 ICAO 找（优先非模块化：2020 上模块化条目创建不出来）。 */
static int find_by_icao(const char* icao, char* title, int tcap,
                        char* livery, int lcap, unsigned char* modular) {
    int i, best = -1;
    if (!g_cat || !icao || !icao[0]) return 0;
    LOCK_ENTER(&g_cat_lock);
    for (i = 0; i < g_cat_n; i++) {
        if (_stricmp(g_cat[i].icao, icao) != 0) continue;
        /* 优先 legacy（modular=0）：它在 2020/2024 都能创建 */
        if (best < 0 || (g_cat[best].modular && !g_cat[i].modular)) best = i;
        if (!g_cat[i].modular) break;
    }
    if (best >= 0) {
        snprintf(title, (size_t)tcap, "%s", g_cat[best].title);
        if (livery) snprintf(livery, (size_t)lcap, "%s", g_cat[best].livery);
        if (modular) *modular = g_cat[best].modular;
    }
    LOCK_LEAVE(&g_cat_lock);
    return best >= 0;
}

static int find_builtin(const char* icao, char* title, int cap) {
    size_t i;
    for (i = 0; i < sizeof(kBuiltin) / sizeof(kBuiltin[0]); i++) {
        if (_stricmp(kBuiltin[i].icao, icao) == 0) {
            snprintf(title, (size_t)cap, "%s", kBuiltin[i].title);
            return 1;
        }
    }
    return 0;
}

static const char* alias_target(const char* icao) {
    size_t i;
    for (i = 0; i < sizeof(kAlias) / sizeof(kAlias[0]); i++)
        if (_stricmp(kAlias[i].type, icao) == 0) return kAlias[i].target;
    return NULL;
}

const char* catalog_icao_for_title(const char* title) {
    int i;
    if (!title || !title[0]) return NULL;
    if (g_cat && g_cat_lock_init) {
        LOCK_ENTER(&g_cat_lock);
        for (i = 0; i < g_cat_n; i++) {
            if (!g_cat[i].icao[0]) continue;
            if (_stricmp(g_cat[i].title, title) == 0) {
                /* ⚠️ 目录条目的 ICAO 可能是机模 cfg 里填的占位符
                 * （ATCCOM/ATCPVT…，机模作者没写 icao_type_designator 时
                 * MSFS 给的默认值，实测 Fenix A320）。占位符**不算识别
                 * 成功** —— 不能直接 return，否则会绕过下面的内置表与
                 * 关键词兜底，把 "ATCCOM" 原样上报导致对端机型匹配全灭
                 * （2026-11-26 22:56 会话实证）。跳过这条继续找。 */
                if (_strnicmp(g_cat[i].icao, "ATC", 3) == 0) continue;
                {
                    static char out[16];
                    snprintf(out, sizeof(out), "%s", g_cat[i].icao);
                    LOCK_LEAVE(&g_cat_lock);
                    return out;
                }
            }
        }
        LOCK_LEAVE(&g_cat_lock);
    }
    /* 内置表反查（未扫描完时也能识别本机机型） */
    {
        size_t k;
        for (k = 0; k < sizeof(kBuiltin) / sizeof(kBuiltin[0]); k++) {
            if (_stricmp(kBuiltin[k].title, title) == 0 &&
                _strnicmp(kBuiltin[k].icao, "ATC", 3) != 0)
                return kBuiltin[k].icao;
        }
    }
    /* 关键词兜底：FenixA320 连写 / 涂装名打头等场景（见 kTitleKeywords） */
    return catalog_icao_from_title_keywords(title);
}

void catalog_add_enumerated(const char* title, const char* livery) {
    char icao[16] = "";
    int i;

    if (!title || !title[0]) return;
    if (!g_cat || !g_cat_lock_init) return;

    /* 枚举不给 ICAO，只能靠同名 aircraft.cfg 反查；查不到就留空，
     * 该条目只能参与"标题精确匹配"，不能参与按 ICAO 的匹配。 */
    {
        const char* found = catalog_icao_for_title(title);
        if (found) snprintf(icao, sizeof(icao), "%s", found);
    }

    LOCK_ENTER(&g_cat_lock);
    /* 2024 枚举结果覆盖之前无标题的占位条目（scan_livery_cfg 留下的） */
    for (i = 0; i < g_cat_n; i++) {
        if (!g_cat[i].title[0] && _stricmp(g_cat[i].livery, livery ? livery : "") == 0) {
            snprintf(g_cat[i].title, sizeof(g_cat[i].title), "%s", title);
            if (icao[0]) snprintf(g_cat[i].icao, sizeof(g_cat[i].icao), "%s", icao);
            g_cat[i].modular = 1;
            LOCK_LEAVE(&g_cat_lock);
            return;
        }
    }
    {
        /* 2024 枚举只给标题/涂装，用标题前缀补一个来源标记 */
        int src = CAT_SRC_OTHER;
        if (_strnicmp(title, "FSLTL", 5) == 0) src = CAT_SRC_FSLTL;
        else if (_strnicmp(title, "AIGAIM", 6) == 0 ||
                 _strnicmp(title, "AIG_", 4) == 0) src = CAT_SRC_AIG;
        cat_add_raw(title, livery, icao, "", "", 1, src);
    }
    LOCK_LEAVE(&g_cat_lock);
}

/* ---------------- 四级匹配 ---------------- */

/* 在目录里按 (icao, airline) 找涂装；reg 非空时优先精确尾号。
 * 返回 1 命中。level 由调用方标注。 */
static int match_in_catalog(const char* icao, const char* airline,
                            const char* reg, CatalogMatch* out, int lib) {
    int i;
    int best = -1;
    int best_score = -1;

    if (!g_cat || !g_cat_lock_init) return 0;

    LOCK_ENTER(&g_cat_lock);
    for (i = 0; i < g_cat_n; i++) {
        int score = -1;
        const CatalogEntry* e = &g_cat[i];

        if (!e->title[0]) continue;
        if (lib && e->source != lib) continue;   /* 用户指定的模型库优先 */
        if (_stricmp(e->icao, icao) != 0) continue;

        /* 第 1 级：尾号一致（涂装名/标题含同一尾号，或 atc_id 精确相同） */
        if (reg && reg[0]) {
            if (e->reg[0] && norm_eq(e->reg, reg)) score = 100;
            else if (norm_contains(e->livery, reg) ||
                     norm_contains(e->title, reg)) score = 90;
        }
        /* 第 2 级：同航司 */
        if (score < 0 && airline && airline[0] && e->airline[0] &&
            (norm_eq(e->airline, airline) ||
             /* 同航司带后缀（AAL → AALWL） */
             (strlen(e->airline) > strlen(airline) &&
              _strnicmp(e->airline, airline, strlen(airline)) == 0))) {
            score = 60;
            /* 同航司多套涂装时优先**不绑定尾号**的那套（通用涂装） */
            if (e->reg[0]) score -= 5;
        }
        /* 第 3 级：同机型任意涂装。白机（ZZZ/ANY/GEN）优先，
         * 避免给未知航司挂别家涂装。 */
        if (score < 0) {
            if (norm_eq(e->livery, "ZZZZ") || norm_eq(e->icao, "ZZZZ") ||
                norm_eq(e->livery, "ZZZ") || norm_contains(e->livery, "ZZZ"))
                score = 30;
            else if (norm_eq(e->livery, "ANY") || norm_eq(e->livery, "GEN"))
                score = 25;
            else if (!e->livery[0])
                score = 20;
            else
                score = 10;
            /* 模块化条目在 2020 上创建不出来；非模块化优先 */
            if (e->modular && !g_sc.supports_modular) score -= 100;
        } else if (e->modular && !g_sc.supports_modular) {
            score -= 100;
        }

        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }

    if (best >= 0 && best_score > 0) {
        snprintf(out->title, sizeof(out->title), "%s", g_cat[best].title);
        snprintf(out->livery, sizeof(out->livery), "%s", g_cat[best].livery);
        out->modular = g_cat[best].modular;
        out->source = g_cat[best].source;
        out->found = 1;
        if (best_score >= 90) out->level = 1;
        else if (best_score >= 55) out->level = 2;
        else out->level = 3;
        LOCK_LEAVE(&g_cat_lock);
        return 1;
    }
    LOCK_LEAVE(&g_cat_lock);
    return 0;
}

/* 按指定来源集合跑完整四级匹配；lib=0 表示不限制来源。 */
static int match_round(const char* icao, const char* airline, const char* reg,
                       CatalogMatch* out, int lib) {
    const char* alias;

    /* 第 1~3 级：尾号 → 航司 → 机型 */
    if (match_in_catalog(icao, airline, reg, out, lib)) return 1;

    /* 空航司时也试一次"只按机型"（有些包装的航司码是 ZZZZ） */
    if (airline && airline[0] &&
        match_in_catalog(icao, "", reg, out, lib)) return 1;

    /* 第 4 级：机型别名 → 同族机型再试 */
    alias = alias_target(icao);
    if (alias && _stricmp(alias, icao) != 0) {
        if (match_in_catalog(alias, airline, reg, out, lib) ||
            match_in_catalog(alias, "", "", out, lib)) {
            out->level = 4;
            return 1;
        }
    }
    return 0;
}

void catalog_match(const char* icao_in, const char* airline, const char* reg,
                   CatalogMatch* out) {
    char icao[16];
    const char* alias;
    const char* level_icao;
    int lib;

    if (!out) return;
    memset(out, 0, sizeof(*out));

    catalog_sanitize_icao(icao_in, icao, sizeof(icao));
    if (!icao[0]) snprintf(icao, sizeof(icao), "ZZZZ");

    /* 先把偏好库的四级全跑完，再回退全部来源。
     * 不能逐级别"先偏好后全局"：那样偏好库里的白机会被另一套库的
     * 航司涂装抢走，等于用户的选择没生效。 */
    lib = catalog_lib();
    if (lib && match_round(icao, airline, reg, out, lib)) return;
    if (match_round(icao, airline, reg, out, 0)) return;

    alias = alias_target(icao);
    level_icao = (alias && _stricmp(alias, icao) != 0) ? alias : icao;

    /* 扫描目录完全不可用（未装第三方包）→ 内置兜底表 */
    {
        char title[112];
        if (find_builtin(level_icao, title, sizeof(title))) {
            snprintf(out->title, sizeof(out->title), "%s", title);
            out->livery[0] = '\0';
            out->modular = 0;
            out->found = 1;
            out->level = (alias ? 4 : 3);
            return;
        }
        /* 别名也没命中 → 通用双发客机 */
        if (find_builtin("ZZZZ", title, sizeof(title))) {
            snprintf(out->title, sizeof(out->title), "%s", title);
            out->livery[0] = '\0';
            out->modular = 0;
            out->found = 1;
            out->level = 4;
        }
    }
}

void catalog_dump_stats(void) {
    int total, with_icao = 0, modular = 0, i;
    int n_fsltl = 0, n_aig = 0, n_other = 0;
    if (!g_cat || !g_cat_lock_init) return;

    LOCK_ENTER(&g_cat_lock);
    total = g_cat_n;
    for (i = 0; i < g_cat_n; i++) {
        if (g_cat[i].icao[0]) with_icao++;
        if (g_cat[i].modular) modular++;
        if (g_cat[i].source == CAT_SRC_FSLTL) n_fsltl++;
        else if (g_cat[i].source == CAT_SRC_AIG) n_aig++;
        else n_other++;
    }
    LOCK_LEAVE(&g_cat_lock);

    xc_logf("catalog: stats total=%d withIcao=%d modular=%d builtin=%d "
            "supportsModular=%d",
            total, with_icao, modular, catalog_builtin_count(),
            g_sc.supports_modular);
    /* 按来源分开统计：一眼确认 FSLTL / AIG 两套库是否都扫描到了 */
    xc_logf("catalog: AI 库条目 FSLTL=%d AIG=%d 其它=%d（当前偏好=%s）",
            n_fsltl, n_aig, n_other, catalog_lib_name(catalog_lib()));
}