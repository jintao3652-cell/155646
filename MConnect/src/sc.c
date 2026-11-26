/* MConnect — SimConnect 动态装载与 API 表。见 sc.h 的设计说明。 */
#include "sc.h"
#include "mconnect.h"

#include <string.h>

/* 进程/模块枚举：用于"从模拟器进程里拿 SimConnect.dll 真实路径"。
 * tlhelp32.h 是 Windows SDK 的标准头，无需额外依赖。 */
#include <tlhelp32.h>

ScApi g_sc;
HANDLE g_sc_handle = NULL;

/* ---------------- 路径查找 ---------------- */

/* 从正在运行的模拟器进程里找出 SimConnect.dll 的**真实路径**。
 *
 * ## 为什么这是最可靠的办法（借鉴 swift 的做法）
 *
 * SimConnect.dll 的落地位置随安装方式而变，光靠猜路径要覆盖：
 *   Steam 版 / MS Store(WindowsApps) / XboxGames / 便携版 / 用户自选目录
 * 而且 WindowsApps 目录**默认不可枚举**（FindFirstFile 直接拒绝访问），
 * 只能靠注册表拿到确切版本号再拼全路径 —— 版本一变就要重算。
 *
 * 但有个事实可以利用：**只要模拟器在跑，正确的那个 SimConnect dll 必然
 * 已经加载进它的进程空间**（模拟器自己就是 SimConnect 的服务端）。所以
 * 直接枚举模拟器进程的模块表，就能拿到系统认可的、与当前运行版本严格
 * 配套的那一份 —— 不用猜、不怕版本错配、不受目录权限限制。
 *
 * ## 必须同时认两个文件名
 *
 * 2024 的安装目录里**没有** `SimConnect.dll`，只有 `SimConnect_internal.dll`
 * （前者由 MSI 装进 WinSxS，后者随模拟器本体分发）。实测本机：
 *
 *     F:\SteamLibrary\...\Limitless\SimConnect_internal.dll  ex1=1  ✅ 连得上
 *     L:\XboxGames\...\Content\SimConnect.dll                ex1=0  ❌ Open 失败
 *
 * 旧路径搜索曾经抓到 XboxGames 那份旧 dll，结果 `SimConnect_Open` 直接
 * 失败。这里两个名字都认，谁在进程里用谁。
 *
 * ## 限制
 *
 * 需要**目标进程与当前进程位数相同**（都是 x86 或都是 x64）。
 * 我们的 MConnect 是 x64，MSFS 也是 x64，满足（TH32CS_SNAPMODULE32
 * 兜底允许跨位数枚举一部分场景）。
 * 若模拟器未运行则返回 0，调用方继续走原来的路径搜索。
 */
static int find_dll_in_sim_process(char* out, int cap) {
    HANDLE snap;
    PROCESSENTRY32 pe;
    int found = 0;
    static const char* kExe[] = {
        "FlightSimulator.exe",          /* 2020 与 2024 都叫这个 */
        "FlightSimulator2024.exe",
    };
    /* 优先 _internal：2024 只有它；2020 两个都有时 _internal 也与本体配套 */
    static const char* kDll[] = {
        "SimConnect_internal.dll",
        "SimConnect.dll",
    };
    size_t k, d;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    memset(&pe, 0, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (!Process32First(snap, &pe)) {
        CloseHandle(snap);
        return 0;
    }
    do {
        int match = 0;
        for (k = 0; k < sizeof(kExe) / sizeof(kExe[0]); k++) {
            if (_stricmp(pe.szExeFile, kExe[k]) == 0) { match = 1; break; }
        }
        if (!match) continue;

        /* 找到模拟器进程 → 枚举它的模块表，按 kDll 的优先级找 */
        for (d = 0; d < sizeof(kDll) / sizeof(kDll[0]) && !found; d++) {
            HANDLE msnap = CreateToolhelp32Snapshot(
                TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pe.th32ProcessID);
            MODULEENTRY32 me;
            if (msnap == INVALID_HANDLE_VALUE) break;

            memset(&me, 0, sizeof(me));
            me.dwSize = sizeof(me);
            if (Module32First(msnap, &me)) {
                do {
                    if (_stricmp(me.szModule, kDll[d]) == 0) {
                        snprintf(out, (size_t)cap, "%s", me.szExePath);
                        found = 1;
                        break;
                    }
                } while (Module32Next(msnap, &me));
            }
            CloseHandle(msnap);
        }
        if (found) break;
    } while (Process32Next(snap, &pe));

    CloseHandle(snap);
    if (found) {
        xc_logf("sc: 从模拟器进程内定位到 %s", out);
    }
    return found;
}

/* 从注册表读 MSFS 安装目录（SimConnect.dll 在模拟器根目录下）。
 * 2020 与 2024 的键不同，两个都试。 */
static int reg_get_path(HKEY root, const char* sub, const char* value,
                        char* out, DWORD cap) {
    HKEY k;
    DWORD type = 0, size = cap;
    LONG rc = RegOpenKeyExA(root, sub, 0, KEY_READ, &k);
    if (rc != ERROR_SUCCESS) return 0;
    rc = RegQueryValueExA(k, value, NULL, &type, (LPBYTE)out, &size);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return 0;
    out[cap - 1] = '\0';
    return out[0] != '\0';
}

/* 拼接目录 + 文件名，检查是否存在。 */
static int try_file(const char* dir, const char* name, char* out, int cap) {
    FILE* f;
    if (!dir || !dir[0]) return 0;
    snprintf(out, (size_t)cap, "%s\\%s", dir, name);
    f = fopen(out, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* 在常见安装布局里找一个候选 SimConnect.dll。命中返回 1。
 *
 * 为什么需要这一步：注册表 `HKLM\SOFTWARE\Microsoft\Flight Simulator\10.0`
 * 只有装了「legacy SimConnect 客户端」的机器才有，而 MSFS 本体自带的
 * SimConnect.dll 就放在游戏目录里。实测一台只装了 MS Store 版 2020 的
 * 机器上，注册表三个键全不存在、System32 里也没有——只能靠目录探测。
 * 每台驱动器上做几次 stat，成本可忽略。 */
typedef struct {
    const char* pattern;   /* 含 %c（盘符）与可选的 * 通配，用 FindFirstFile 展开 */
} DllSearch;

static int try_candidate(char* out, int cap, const char* dir, const char* name) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s", dir, name);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return 0;
    snprintf(out, (size_t)cap, "%s", path);
    return 1;
}

/* 展开 dir_wild 里的通配，逐个尝试拼 name。命中即返回。 */
static int try_wild(char* out, int cap, const char* dir_wild, const char* name) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(dir_wild, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        char dir[MAX_PATH];
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;
        /* dir_wild 形如 "L:\XboxGames\Microsoft Flight Simulator*"，
         * 需要把最后一段换成实际目录名 */
        {
            char prefix[MAX_PATH];
            const char* slash = strrchr(dir_wild, '\\');
            size_t n = slash ? (size_t)(slash - dir_wild) : 0;
            if (n >= sizeof(prefix)) n = sizeof(prefix) - 1;
            memcpy(prefix, dir_wild, n);
            prefix[n] = '\0';
            snprintf(dir, sizeof(dir), "%s\\%s", prefix, fd.cFileName);
        }
        if (try_candidate(out, cap, dir, name)) { FindClose(h); return 1; }
        /* 有些布局多一层（...\Content\SimConnect.dll 已覆盖；
         * ...\<name>\SimConnect.dll 也试一下） */
        {
            char sub[MAX_PATH];
            snprintf(sub, sizeof(sub), "%s\\Content", dir);
            if (try_candidate(out, cap, sub, name)) { FindClose(h); return 1; }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
}

/* 扫描所有逻辑驱动器上的常见安装位置。 */
static int find_in_install_dirs(char* out, int cap) {
    DWORD mask = GetLogicalDrives();
    char root[8];
    int i;

    for (i = 0; i < 26; i++) {
        char wild[MAX_PATH];

        if (!(mask & (1u << i))) continue;
        snprintf(root, sizeof(root), "%c:", 'A' + i);

        /* WindowsApps（MS Store 真实安装目录）**优先**。
         *
         * 为什么排第一：WindowsApps 里的 dll 与实际安装的模拟器版本严格
         * 一致（`Microsoft.FlightSimulator_1.39.12.0_...` / `Microsoft.Limitless_*`），
         * 而 `XboxGames\` 下那份可能属于另一个（已卸载/已升级的）版本 ——
         * 实测本机 `L:\XboxGames\...\SimConnect.dll` 不认
         * `SIMCONNECT_OPEN_CONFIGINDEX_LOCAL`（返回 E_INVALIDARG），
         * 说明它比 WindowsApps 里那份旧。旧 dll 连新模拟器会出现
         * 版本协商不一致，宁可优先用"和当前安装版本配套"的那份。
         *
         * ⚠️ WindowsApps 目录**默认不可枚举**（Permission denied），
         * `FindFirstFile` 会直接失败，所以 try_wild 在这里多半走不通。
         * 因此下面还有 find_store_pkg_dll() 通过注册表拿到确切版本号再拼全路径。 */
        snprintf(wild, sizeof(wild),
                 "%s\\Program Files\\WindowsApps\\Microsoft.FlightSimulator_*", root);
        if (try_wild(out, cap, wild, "SimConnect.dll")) return 1;
        snprintf(wild, sizeof(wild),
                 "%s\\Program Files\\WindowsApps\\Microsoft.Limitless_*", root);
        if (try_wild(out, cap, wild, "SimConnect.dll")) return 1;

        /* MS Store / Xbox 版（后备）：<盘>\XboxGames\Microsoft Flight Simulator*\Content\ */
        snprintf(wild, sizeof(wild), "%s\\XboxGames\\Microsoft Flight Simulator*", root);
        if (try_wild(out, cap, wild, "SimConnect.dll")) return 1;

        /* 2024 的 Xbox 目录名不同（Microsoft Flight Simulator 2024）——
         * 上面的通配已覆盖，但 2024 有时直接放在 <盘>\XboxGames\ 下 */
        snprintf(wild, sizeof(wild), "%s\\XboxGames\\*", root);
        if (try_wild(out, cap, wild, "SimConnect.dll")) return 1;

        /* Steam 版（2020：MicrosoftFlightSimulator） */
        snprintf(wild, sizeof(wild),
                 "%s\\Program Files (x86)\\Steam\\steamapps\\common\\MicrosoftFlightSimulator",
                 root);
        if (try_candidate(out, cap, wild, "SimConnect.dll")) return 1;
        snprintf(wild, sizeof(wild),
                 "%s\\SteamLibrary\\steamapps\\common\\MicrosoftFlightSimulator", root);
        if (try_candidate(out, cap, wild, "SimConnect.dll")) return 1;

        /* Steam 版（**2024：Limitless**）。
         *
         * 2024 的目录里**没有** `SimConnect.dll`，只有 `SimConnect_internal.dll`。
         * 这是 2024 的架构变化：对外 API 由系统级 SimConnect Client
         * （MSI 装进 WinSxS）提供，而 `SimConnect_internal.dll` 是模拟器
         * 内部使用的那份 —— 但它同样**导出了全部 121 个 SimConnect_* API**
         * （含 SimConnect_Open，实测 85504 B 的文件里能查到），
         * 所以可以直接当客户端 dll 用。 */
        snprintf(wild, sizeof(wild),
                 "%s\\Program Files (x86)\\Steam\\steamapps\\common\\Limitless",
                 root);
        if (try_candidate(out, cap, wild, "SimConnect_internal.dll")) return 1;
        snprintf(wild, sizeof(wild),
                 "%s\\SteamLibrary\\steamapps\\common\\Limitless", root);
        if (try_candidate(out, cap, wild, "SimConnect_internal.dll")) return 1;
    }
    return 0;
}

/* 从注册表拿 MS Store 版 MSFS 的**确切安装目录**，再拼 SimConnect.dll。
 *
 * 为什么需要：`C:\Program Files\WindowsApps` 目录**默认禁止枚举**
 * （非管理员连列目录都 Permission denied），所以 FindFirstFile 通配
 * 拿不到里面的包目录名。但**已知完整路径是可以直接读的**。
 *
 * AppModel Repository 在 HKCU 下记录每个已安装包的 PackageFullName，
 * 拼成 `WindowsApps\<PackageFullName>\SimConnect.dll` 即可。
 * 这是"不改任何模拟器文件、也不需要管理员权限"的可行做法。 */
static int find_store_pkg_dll(char* out, int cap) {
    static const char* kFamilies[] = {
        /* 2024 Store 版包名是 Limitless；2020 是 FlightSimulator */
        "Microsoft.Limitless_8wekyb3d8bbwe",
        "Microsoft.FlightSimulator_8wekyb3d8bbwe"
    };
    const char* repo =
        "Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows"
        "\\CurrentVersion\\AppModel\\Repository\\Families\\";
    size_t i;

    for (i = 0; i < sizeof(kFamilies) / sizeof(kFamilies[0]); i++) {
        char key[512];
        HKEY hk;
        char full[512];
        DWORD type = 0, sz = sizeof(full);

        snprintf(key, sizeof(key), "%s%s", repo, kFamilies[i]);
        if (RegOpenKeyExA(HKEY_CURRENT_USER, key, 0, KEY_READ, &hk) != ERROR_SUCCESS)
            continue;
        /* PackageFullName 形如
         * Microsoft.FlightSimulator_1.39.12.0_x64__8wekyb3d8bbwe */
        if (RegQueryValueExA(hk, "PackageFullName", NULL, &type,
                             (LPBYTE)full, &sz) == ERROR_SUCCESS &&
            type == REG_SZ && full[0]) {
            char path[MAX_PATH * 2];
            RegCloseKey(hk);
            snprintf(path, sizeof(path),
                     "C:\\Program Files\\WindowsApps\\%s\\SimConnect.dll", full);
            if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
                snprintf(out, (size_t)cap, "%s", path);
                return 1;
            }
            continue;
        }
        RegCloseKey(hk);
    }
    return 0;
}

/* 按优先级找 SimConnect.dll。 */
static void find_dll(char* out, int cap) {
    static const char* kRegKeys[] = {
        "SOFTWARE\\Microsoft\\Flight Simulator\\10.0",
        "SOFTWARE\\Microsoft\\Flight Simulator2024\\10.0",
        "SOFTWARE\\WOW6432Node\\Microsoft\\Flight Simulator\\10.0"
    };
    char dir[1024];
    char sys[MAX_PATH];
    const char* env;
    size_t i;

    /* 1. 环境变量显式指定（目录或 dll 全路径，调试替换用） */
    env = getenv("MCONNECT_SIMCONNECT");
    if (env && env[0]) {
        size_t n = strlen(env);
        if (n > 4 && _stricmp(env + n - 4, ".dll") == 0) {
            FILE* f = fopen(env, "rb");
            if (f) { fclose(f); snprintf(out, (size_t)cap, "%s", env); return; }
        } else if (try_file(env, "SimConnect.dll", out, cap)) {
            return;
        }
    }

    /* 2. **从模拟器进程里拿已加载模块的路径**（最可靠）。
     *
     * 放在注册表/猜路径之前，因为这是唯一能保证"与当前运行的模拟器版本
     * 严格配套"的来源。注册表 AppPath 可能指向旧版本残留目录，
     * System32 那份可能是安装器随手放的旧 dll —— 用它们连新版模拟器
     * 会出现版本协商失败（实测 E_INVALIDARG）。
     *
     * 模拟器没运行时这一步自动跳过（返回 0），继续走后面的搜索。 */
    if (find_dll_in_sim_process(out, cap)) return;

    /* 3. 注册表记录的模拟器安装目录 */
    for (i = 0; i < sizeof(kRegKeys) / sizeof(kRegKeys[0]); i++) {
        if (reg_get_path(HKEY_LOCAL_MACHINE, kRegKeys[i], "AppPath",
                         dir, sizeof(dir)) &&
            try_file(dir, "SimConnect.dll", out, cap)) {
            return;
        }
    }

    /* 4. 系统目录：部分安装方式会把 SimConnect.dll 放进 System32 */
    if (GetSystemDirectoryA(sys, sizeof(sys)) &&
        try_file(sys, "SimConnect.dll", out, cap)) {
        return;
    }

    /* 5. MS Store 包的确切目录（WindowsApps 不可枚举，但可按全路径访问）*/
    if (find_store_pkg_dll(out, cap)) return;

    /* 6. exe 同目录（随包分发/调试替换） */
    if (try_file(xc_exe_dir(), "SimConnect.dll", out, cap)) return;

    /* 7. 遍历驱动器找模拟器安装位置（覆盖 MS Store / Xbox / Steam） */
    if (find_in_install_dirs(out, cap)) return;

    /* 8. 交给系统搜索路径 */
    snprintf(out, (size_t)cap, "SimConnect.dll");
}

/* ---------------- 装载 ---------------- */

#define RESOLVE(field, name)                                            \
    do {                                                                \
        *(FARPROC*)&g_sc.field = GetProcAddress(g_sc.dll, name);        \
        if (!g_sc.field) {                                              \
            snprintf(err, (size_t)errcap, "缺少导出 %s", name);          \
            return -1;                                                  \
        }                                                               \
    } while (0)

#define RESOLVE_OPT(field, name)                                        \
    (*(FARPROC*)&g_sc.field = GetProcAddress(g_sc.dll, name))

int sc_load(char* err, int errcap) {
    char path[1024];

    memset(&g_sc, 0, sizeof(g_sc));
    if (errcap > 0) err[0] = '\0';

    find_dll(path, sizeof(path));
    g_sc.dll = LoadLibraryA(path);
    if (!g_sc.dll) {
        snprintf(err, (size_t)errcap,
                 "无法加载 SimConnect.dll（%s，GetLastError=%lu）",
                 path, (unsigned long)GetLastError());
        return -1;
    }

    /* 必需导出：2020 与 2024 都有 */
    RESOLVE(Open,                     "SimConnect_Open");
    RESOLVE(Close,                    "SimConnect_Close");
    RESOLVE(CallDispatch,             "SimConnect_CallDispatch");
    RESOLVE(AddToDataDefinition,      "SimConnect_AddToDataDefinition");
    RESOLVE(ClearDataDefinition,      "SimConnect_ClearDataDefinition");
    RESOLVE(RequestDataOnSimObject,   "SimConnect_RequestDataOnSimObject");
    RESOLVE(SetDataOnSimObject,       "SimConnect_SetDataOnSimObject");
    RESOLVE(RequestSystemState,       "SimConnect_RequestSystemState");
    RESOLVE(SubscribeToSystemEvent,   "SimConnect_SubscribeToSystemEvent");
    RESOLVE(MapClientEventToSimEvent, "SimConnect_MapClientEventToSimEvent");
    RESOLVE(TransmitClientEvent,      "SimConnect_TransmitClientEvent");
    RESOLVE(AICreateNonATCAircraft,   "SimConnect_AICreateNonATCAircraft");
    RESOLVE(AIRemoveObject,           "SimConnect_AIRemoveObject");

    /* 可选导出：2024 SU1+ 才有。缺失不是错误。 */
    RESOLVE_OPT(AICreateNonATCAircraft_EX1,
                "SimConnect_AICreateNonATCAircraft_EX1");
    RESOLVE_OPT(EnumerateSimObjectsAndLiveries,
                "SimConnect_EnumerateSimObjectsAndLiveries");
    RESOLVE_OPT(AIReleaseControl, "SimConnect_AIReleaseControl");
    RESOLVE_OPT(GetNextDispatch,  "SimConnect_GetNextDispatch");

    /* Input Events（2020 SU 起就有；缺了不影响基础功能，只是深度机模写不动） */
    RESOLVE_OPT(EnumerateInputEvents,      "SimConnect_EnumerateInputEvents");
    RESOLVE_OPT(EnumerateInputEventParams, "SimConnect_EnumerateInputEventParams");
    RESOLVE_OPT(SetInputEvent,             "SimConnect_SetInputEvent");

    /* ClientData：与 WASM 模块（MobiFlight 等）通信 */
    RESOLVE_OPT(MapClientDataNameToID,     "SimConnect_MapClientDataNameToID");
    RESOLVE_OPT(CreateClientData,          "SimConnect_CreateClientData");
    RESOLVE_OPT(AddToClientDataDefinition, "SimConnect_AddToClientDataDefinition");
    RESOLVE_OPT(SetClientData,             "SimConnect_SetClientData");
    RESOLVE_OPT(RequestClientData,         "SimConnect_RequestClientData");

    xc_logf("sc: dll=%s ex1=%d enumLivery=%d inputEvents=%d clientData=%d", path,
            g_sc.AICreateNonATCAircraft_EX1 != NULL,
            g_sc.EnumerateSimObjectsAndLiveries != NULL,
            g_sc.SetInputEvent != NULL,
            g_sc.SetClientData != NULL);
    return 0;
}

/* ---------------- 连接 ---------------- */

/* 版本推断：SIMCONNECT_RECV_OPEN 给的是**模拟器服务端**的 SimConnect 版本，
 * 不是本机 DLL 的版本。构建号在各机器上会漂移，因此采取「能力优先」判断：
 *   有 _EX1 + 涂装枚举 → 2024 SU1 起，支持模块化机型；
 *   没有              → 按 2020 处理（只能加载 legacy 机型）。
 * 这样即使版本号判错，功能路径也不会错。 */
void sc_on_open_info(const SC_RECV_OPEN* o) {
    g_sc.build_major = (int)o->dwSimConnectVersionMajor;
    g_sc.build_minor = (int)o->dwSimConnectVersionMinor;

    if (g_sc.AICreateNonATCAircraft_EX1) {
        g_sc.sim_version_major = 2024;
        g_sc.supports_modular = 1;
    } else {
        g_sc.sim_version_major = 2020;
        g_sc.supports_modular = 0;
    }
    xc_logf("sc: server ver %lu.%lu build %lu.%lu app=\"%.40s\" -> MSFS %d",
            (unsigned long)o->dwSimConnectVersionMajor,
            (unsigned long)o->dwSimConnectVersionMinor,
            (unsigned long)o->dwSimConnectBuildMajor,
            (unsigned long)o->dwSimConnectBuildMinor,
            o->szApplicationName, g_sc.sim_version_major);
}

/* SimConnect_Open 的 ConfigIndex 特殊值：**强制本地连接，忽略一切 cfg**。
 *
 * 官方定义（SimConnect.h / SDK 文档）：
 *     static const DWORD SIMCONNECT_OPEN_CONFIGINDEX_LOCAL = -1;
 *     "ignore SimConnect.cfg settings, and force local connection"
 *
 * 为什么必须用它：
 *
 * MSFS 的本地连接默认要先找一个 **客户端侧** SimConnect.cfg
 * （在 exe 同目录 / 用户目录），找不到才退到"本地直连"。而：
 *   ① 我们**不能也不该**去改 MSFS 的 SimConnect.xml / SimConnect.cfg
 *      （那是模拟器的文件，升级会覆盖，且多版本共存时会互相干扰）；
 *   ② 很多机器上根本没有客户端 cfg（实测本机 2020/2024 都没有），
 *      于是 ConfigIndex=0 会走到"读 cfg 第 0 节"的分支，
 *      行为取决于库里有没有那份文件，不可预测；
 *   ③ 用 0 还有个坑：如果用户别处放过一份指向远程机器的 cfg，
 *      我们会莫名其妙连到别的机器上。
 *
 * 传 -1 就明确告诉库："我要本机、走命名管道、别读任何配置文件"。
 * 这是 SkyDolly 等项目遇到同样问题后采用的做法（社区已广泛验证）。
 *
 * 注意参数是 DWORD，-1 就是 0xFFFFFFFF，不要写成 0x7FFFFFFF。 */
#define SC_OPEN_CONFIGINDEX_LOCAL ((DWORD)-1)

int sc_open(SC_DISPATCH_PROC on_recv, void* ctx) {
    HRESULT hr;
    int i;
    /* 依次尝试的 ConfigIndex：
     *   ①  0 → 默认本地配置（模拟器自动写入注册表的管道/端口）
     *   ② -1 → 强制本地、忽略一切 cfg（标准做法，但**老版 dll 会返回
     *          E_INVALIDARG 0x80070057**，实测 L:\XboxGames 那份就不认）
     *   ③  1 → 少数机器把本地连接放在第 1 节
     *
     * ⚠️ 顺序是 0 → -1 → 1，不是把 -1 放第一。
     * 因为 -1 在老 dll 上会直接报 E_INVALIDARG，放第一位会让每次连接
     * 先吃一个错误；而 0 在新旧 dll 上都能正确表达"本地默认连接"
     * （没有 cfg 时库自己走本地管道），所以 0 更稳。
     * -1 作为第二选择，用于"用户机器上意外存在一份远程 cfg"的情况。 */
    static const DWORD kIdx[] = { 0, SC_OPEN_CONFIGINDEX_LOCAL, 1 };

    if (!g_sc.dll || !g_sc.Open) return -1;
    if (g_sc_handle) return 0;

    /* 先尝试一轮；模拟器仍在加载时可能返回 E_FAIL，重试几秒。
     * hWnd = NULL + 回调模式：SimConnect 会在我们调 CallDispatch 时
     * 把消息交回来，不依赖窗口消息循环（本进程没有 UI）。 */
    hr = E_FAIL;
    for (i = 0; i < 20; i++) {
        size_t k;
        for (k = 0; k < sizeof(kIdx) / sizeof(kIdx[0]); k++) {
            HRESULT one;
            one = g_sc.Open(&g_sc_handle, MCONNECT_NAME, NULL, 0, NULL, kIdx[k]);
            if (SUCCEEDED(one) && g_sc_handle) {
                xc_logf("sc: connected (ConfigIndex=%ld)", (long)kIdx[k]);
                goto opened;
            }
            g_sc_handle = NULL;
            /* 错误码取舍：E_INVALIDARG 只说明"该索引在 cfg 里不存在"，
             * **不代表连不上模拟器**，信息量最低，不该拿来当结论。
             *
             * ⚠️ 这里踩过一个坑。原来的写法：
             *     if (one != E_INVALIDARG) hr = one;
             *     else if (hr == E_FAIL)  hr = one;   ← 错
             * 实测三个 dll 的返回（模拟器未运行时）：
             *     index  0 → E_FAIL
             *     index -1 → E_FAIL
             *     index  1 → E_INVALIDARG
             * 走到 index=1 时 hr 仍是 E_FAIL，第二个分支成立 →
             * hr 被覆盖成 E_INVALIDARG，日志把"模拟器没运行"误报成
             * "ConfigIndex 不被支持"。我一度据此错判"旧 dll 不认 -1"，
             * 其实**三个 dll 都接受 0 和 -1**，这是实测确认的。
             *
             * 正确做法：只记录**第一个**非 E_INVALIDARG 的错误，之后不再覆盖。 */
            if (one != HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER) &&
                hr == E_FAIL) {
                hr = one;
            }
        }
        Sleep(500);
    }
    if (!g_sc_handle) {
        xc_logf("sc: Open failed hr=0x%08lx（已尝试 ConfigIndex 0/-1/1）",
                (unsigned long)hr);
        return -1;
    }

opened:

    /* 立刻派发一次，把 RECV_OPEN 收下来（版本探测依赖它） */
    if (on_recv) {
        for (i = 0; i < 50; i++) {
            g_sc.CallDispatch(g_sc_handle, on_recv, ctx);
            if (g_sc.sim_version_major != 0) break;
            Sleep(20);
        }
    }
    xc_logf("sc: opened (MSFS %s)", sc_sim_name());
    return 0;
}

void sc_close(void) {
    if (g_sc_handle && g_sc.Close) {
        g_sc.Close(g_sc_handle);
        g_sc_handle = NULL;
    }
    if (g_sc.dll) {
        FreeLibrary(g_sc.dll);
        g_sc.dll = NULL;
    }
    memset(&g_sc, 0, sizeof(g_sc));
}

const char* sc_sim_name(void) {
    if (g_sc.sim_version_major == 2024) return "2024";
    if (g_sc.sim_version_major == 2020) return "2020";
    return "unknown";
}