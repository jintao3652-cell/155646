/* WIN32_LEAN_AND_MEAN 必须在任何 windows.h 之前定义（含 XPLM 头间接引入的），
 * 否则 windows.h 会先拉进 winsock.h，与 net.h 的 winsock2.h 冲突。 */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include "csl.h"
#include "xconnect.h"   /* xconnect_logf：机型/涂装回退诊断日志 */

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>   /* FindFirstFileA 目录枚举 */
#define csl_stricmp _stricmp
#else
#include <dirent.h>
#include <strings.h>
#define csl_stricmp strcasecmp
#endif

#include "XPLMUtilities.h"
#include "XPLMDataAccess.h" /* XPLMRegisterDataAccessor / XPLMGetDataRefTypes */
#include "XPLMGraphics.h"   /* XPLMWorldToLocal / XPLMLocalToWorld（XPLMDrawInfo_t 经 XPLMInstance.h 引入） */
#include "XPLMScenery.h"    /* XPLMCreateProbe / XPLMProbeTerrainXYZ（地面钳制） */

/* 实例动画 dataref（libxplanemp 命名空间，XPMP2 约定）。
 *
 * XPLMInstance 按「精确字符串」把实例 dataref 名与 OBJ 的 ANIM 键匹配，
 * 因此必须包含模型实际使用的全部形态（含 gear_ratio[0..2]、tire_*[0..2]、
 * engine_rotation_angle_deg[0] 等数组索引形式）。形态核对见
 * CSL动画dataref核对.md（242 个模型实扫，29 个键全覆盖）。 */
static const char* kCslDataRefs[CSL_MAX_DATAREFS] = {
    "libxplanemp/controls/gear_ratio",              /* 0  */
    "libxplanemp/controls/gear_ratio[0]",           /* 1  */
    "libxplanemp/controls/gear_ratio[1]",           /* 2  */
    "libxplanemp/controls/gear_ratio[2]",           /* 3  */
    "libxplanemp/controls/flap_ratio",              /* 4  */
    "libxplanemp/controls/spoiler_ratio",           /* 5  */
    "libxplanemp/controls/speed_brake_ratio",       /* 6  */
    "libxplanemp/controls/slat_ratio",              /* 7  */
    "libxplanemp/controls/wing_sweep_ratio",        /* 8  */
    "libxplanemp/controls/thrust_ratio",            /* 9  */
    "libxplanemp/controls/yoke_pitch_ratio",        /* 10 */
    "libxplanemp/controls/yoke_heading_ratio",      /* 11 */
    "libxplanemp/controls/yoke_roll_ratio",         /* 12 */
    "libxplanemp/controls/thrust_revers",           /* 13 */
    "libxplanemp/controls/taxi_lites_on",           /* 14 */
    "libxplanemp/controls/landing_lites_on",        /* 15 */
    "libxplanemp/controls/beacon_lites_on",         /* 16 */
    "libxplanemp/controls/strobe_lites_on",         /* 17 */
    "libxplanemp/controls/nav_lites_on",            /* 18 */
    "libxplanemp/controls/nws_ratio",               /* 19 */
    "libxplanemp/engines/thrust_reverser_deploy_ratio", /* 20 */
    "libxplanemp/engines/prop_rotation_angle_deg",  /* 21 */
    "libxplanemp/engines/engine_rotation_angle_deg",/* 22 */
    "libxplanemp/engines/engine_rotation_angle_deg[0]", /* 23 */
    "libxplanemp/engines/engine_rotation_speed_rad_sec", /* 24 */
    "libxplanemp/engines/engine_rotation_speed_rad_sec[0]", /* 25 */
    "libxplanemp/gear/tire_rotation_angle_deg",     /* 26 */
    "libxplanemp/gear/tire_rotation_angle_deg[0]",  /* 27 */
    "libxplanemp/gear/tire_rotation_angle_deg[1]",  /* 28 */
    "libxplanemp/gear/tire_rotation_angle_deg[2]",  /* 29 */
    "libxplanemp/gear/tire_vertical_deflection_mtr[0]", /* 30 */
    "libxplanemp/gear/tire_vertical_deflection_mtr[1]", /* 31 */
    "libxplanemp/gear/tire_vertical_deflection_mtr[2]", /* 32 */
    "libxplanemp/misc/touch_down",                  /* 33 */
    /* 很多 X-CSL（如 A321）的风扇/螺旋桨用全局运行时间驱动旋转，
     * thrust_ratio 只负责切换静态/高速扇叶层。不提供这个值 → 扇叶恒 0 不转。 */
    "sim/time/total_running_time_sec",              /* 34 */
    "libxplanemp/controls/logo_lites_on",           /* 35 */
    /* 36..42：engine_rotation_angle_deg[1..7]
     * 43..49：engine_rotation_speed_rad_sec[1..7]
     * 50..56：prop_rotation_angle_deg[1..7]
     * 三组必须各自连续，且顺序与 csl.h 的 *_N1 基址一致。 */
    "libxplanemp/engines/engine_rotation_angle_deg[1]", /* 36 */
    "libxplanemp/engines/engine_rotation_angle_deg[2]", /* 37 */
    "libxplanemp/engines/engine_rotation_angle_deg[3]", /* 38 */
    "libxplanemp/engines/engine_rotation_angle_deg[4]", /* 39 */
    "libxplanemp/engines/engine_rotation_angle_deg[5]", /* 40 */
    "libxplanemp/engines/engine_rotation_angle_deg[6]", /* 41 */
    "libxplanemp/engines/engine_rotation_angle_deg[7]", /* 42 */
    "libxplanemp/engines/engine_rotation_speed_rad_sec[1]", /* 43 */
    "libxplanemp/engines/engine_rotation_speed_rad_sec[2]", /* 44 */
    "libxplanemp/engines/engine_rotation_speed_rad_sec[3]", /* 45 */
    "libxplanemp/engines/engine_rotation_speed_rad_sec[4]", /* 46 */
    "libxplanemp/engines/engine_rotation_speed_rad_sec[5]", /* 47 */
    "libxplanemp/engines/engine_rotation_speed_rad_sec[6]", /* 48 */
    "libxplanemp/engines/engine_rotation_speed_rad_sec[7]", /* 49 */
    "libxplanemp/engines/prop_rotation_angle_deg[1]", /* 50 */
    "libxplanemp/engines/prop_rotation_angle_deg[2]", /* 51 */
    "libxplanemp/engines/prop_rotation_angle_deg[3]", /* 52 */
    "libxplanemp/engines/prop_rotation_angle_deg[4]", /* 53 */
    "libxplanemp/engines/prop_rotation_angle_deg[5]", /* 54 */
    "libxplanemp/engines/prop_rotation_angle_deg[6]", /* 55 */
    "libxplanemp/engines/prop_rotation_angle_deg[7]", /* 56 */
};
#define CSL_DATAREF_COUNT ((int)(sizeof(kCslDataRefs) / sizeof(kCslDataRefs[0])))

/* ---------------------------------------------------------------------------
 * libxplanemp/... 自定义 dataref 注册
 *
 * XPLMInstance.h 明确要求：OBJ 里用到的自定义 dataref **必须在 XPLMLoadObject
 * 之前注册**，即使数值是通过 XPLMCreateInstance 的实例 dataref 列表提供的。
 * 之前插件从未注册这些名字 —— X-Plane 的 DataRefs.txt 里 0 条 libxplanemp ——
 * 于是 XPLMInstanceSetPosition 传进去的动画值没有任何 ANIM 绑定：机身位置/涂装
 * 照常显示（走 info 结构体），但起落架/襟翼/灯光/发动机全部不动。这正是
 * "看得见飞机、部件全不显示"的根因。
 *
 * 每个 dataref 给一个独立的 float 存储槽：X-Plane 会读这些 accessor（部分
 * 版本在实例之外也解析 OBJ），读写都落到槽里；实例数据仍由 SetPosition 覆盖。
 * ------------------------------------------------------------------------- */
static XPLMDataRef g_csl_owned_refs[CSL_MAX_DATAREFS];  /* 本插件注册的句柄 */
static int         g_csl_owned_count = 0;
static float       g_csl_dref_slots[CSL_MAX_DATAREFS];

static float csl_dref_get_f(void* refcon) {
    int idx = (int)(intptr_t)refcon;
    return (idx >= 0 && idx < CSL_MAX_DATAREFS) ? g_csl_dref_slots[idx] : 0.0f;
}
static void csl_dref_set_f(void* refcon, float value) {
    int idx = (int)(intptr_t)refcon;
    if (idx >= 0 && idx < CSL_MAX_DATAREFS) g_csl_dref_slots[idx] = value;
}

/* 在 csl_init() 里调用：必须早于任何 XPLMLoadObject。 */
static void csl_register_datarefs(void) {
    int i;
    memset(g_csl_owned_refs, 0, sizeof(g_csl_owned_refs));
    memset(g_csl_dref_slots, 0, sizeof(g_csl_dref_slots));
    g_csl_owned_count = 0;
    for (i = 0; i < CSL_DATAREF_COUNT; ++i) {
        const char* name = kCslDataRefs[i];
        XPLMDataRef existing;
        XPLMDataRef ref;
        if (!name) continue;
        /* sim/time/... 是 X-Plane 内建，不能也不需要注册 */
        if (strncmp(name, "libxplanemp/", 12) != 0) continue;
        /* 已存在（例如 XPMP2 已注册）就复用，绝对不要重复注册同名 dataref */
        existing = XPLMFindDataRef(name);
        if (existing) continue;
        ref = XPLMRegisterDataAccessor(
                  name, xplmType_Float, 1,
                  NULL, NULL,                       /* int    */
                  csl_dref_get_f, csl_dref_set_f,   /* float  */
                  NULL, NULL,                       /* double */
                  NULL, NULL, NULL, NULL,           /* int/float arrays */
                  NULL, NULL,                       /* binary data */
                  (void*)(intptr_t)i,               /* read refcon  = 槽下标 */
                  (void*)(intptr_t)i);              /* write refcon */
        if (ref) {
            g_csl_owned_refs[i] = ref;
            ++g_csl_owned_count;
        } else {
            xconnect_logf("csl: [!] register dataref FAILED: %s", name);
        }
    }
    xconnect_logf("csl: registered %d/%d libxplanemp datarefs "
                  "(must precede any XPLMLoadObject)",
                  g_csl_owned_count, CSL_DATAREF_COUNT);
}

static void csl_unregister_datarefs(void) {
    int i;
    for (i = 0; i < CSL_DATAREF_COUNT; ++i) {
        if (g_csl_owned_refs[i]) {
            XPLMUnregisterDataAccessor(g_csl_owned_refs[i]);
            g_csl_owned_refs[i] = NULL;
        }
    }
    g_csl_owned_count = 0;
}

static char g_csl_rel[1024] = "";   /* 相对 X-System 根（XPLMLoadObject 用） */
static char g_csl_abs[1024] = "";   /* 绝对路径（存在性检查用） */

static XPLMProbeRef g_probe = NULL; /* 地形探针（地面钳制用，仅主线程） */

int csl_dataref_count(void) {
    return CSL_DATAREF_COUNT;
}

void csl_init(void) {
    char sys[1024];
    const char* sep = XPLMGetDirectorySeparator();
    /* 第一件事：注册 OBJ 动画依赖的自定义 dataref。必须在任何
     * XPLMLoadObject 之前完成，否则实例动画不会绑定（SDK 硬性要求）。 */
    csl_register_datarefs();
    snprintf(g_csl_rel, sizeof(g_csl_rel),
             "Resources%splugins%sXConnect%sCSL", sep, sep, sep);
    XPLMGetSystemPath(sys);   /* SDK 430: void XPLMGetSystemPath(char*) */
    snprintf(g_csl_abs, sizeof(g_csl_abs), "%s%s", sys, g_csl_rel);
}

static void obj_cache_clear(void);   /* 前置声明（定义在下方缓存实现处） */

void csl_shutdown(void) {
    if (g_probe) {
        XPLMDestroyProbe(g_probe);
        g_probe = NULL;
    }
    obj_cache_clear();
    csl_unregister_datarefs();
}

static int path_exists(const char* absPath) {
    FILE* f = fopen(absPath, "rb");
    if (f) {
        fclose(f);
        return 1;
    }
    return 0;
}

/* 归一化 xsb/obj 路径：'\\' '/' ':' 全部转换为系统分隔符（X-CSL 用 `pack:file`
 * 与 `pack\file` 两种写法，语义都是相对 CSL 根或包目录）。 */
static void norm_sep(char* out, size_t cap, const char* in, const char* sep) {
    size_t n = 0;
    for (; *in && n + 2 < cap; ++in) {
        char c = *in;
        if (c == '\\' || c == '/' || c == ':') out[n++] = sep[0];
        else out[n++] = c;
    }
    out[n] = '\0';
}

/* 由「相对 X-System 根」的路径创建实例；文件不存在或创建失败返回 NULL。 */
/* ---------------- OBJ 对象缓存（引用计数） ----------------
 *
 * 为什么必须有：XPLMLoadObject 是同步解析 OBJ + 上传纹理，代价几十~几百毫秒。
 * 旧实现每架玩家每个 OBJ 都重新 LoadObject：
 *   1) 繁忙机场 40 人 × 3 OBJ 全挤在 roster 到达后的循环里 → XP 冻结数秒
 *      （"卡爆"）；
 *   2) 同一涂装的纹理在显存里 ×N 份 → 显存爆 → 持续掉帧；
 *   3) 旧实现释放时只 DestroyInstance、从不 UnloadObject → XPLMObjectRef
 *      泄漏，换涂装/重建越多显存涨得越多。
 * 现在按 OBJ 相对路径缓存 XPLMObjectRef + 引用计数：同变体多架飞机共享
 * 同一个对象，只有实例创建是廉价的；引用归零才真正 UnloadObject。 */
typedef struct {
    char         rel[1200];
    XPLMObjectRef obj;
    int          refs;   /* 0 = 空槽 */
} ObjCacheEnt;

#define OBJ_CACHE_MAX 96
static ObjCacheEnt g_obj_cache[OBJ_CACHE_MAX];

/* 取 OBJ（命中缓存则引用 +1）。成功返回对象并写 *slotOut（>=0 = 缓存槽，
 * -1 = 缓存满时直接加载未入缓存，释放时不卸载）。失败返回 NULL。 */
static XPLMObjectRef obj_cache_acquire(const char* rel, int* slotOut) {
    int i, free_slot = -1;
    *slotOut = -1;
    for (i = 0; i < OBJ_CACHE_MAX; i++) {
        if (g_obj_cache[i].refs > 0) {
            if (strcmp(g_obj_cache[i].rel, rel) == 0) {
                g_obj_cache[i].refs++;
                *slotOut = i;
                return g_obj_cache[i].obj;
            }
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }
    {
        XPLMObjectRef obj = XPLMLoadObject(rel);
        if (!obj) return NULL;
        if (free_slot >= 0) {
            snprintf(g_obj_cache[free_slot].rel,
                     sizeof(g_obj_cache[free_slot].rel), "%s", rel);
            g_obj_cache[free_slot].obj = obj;
            g_obj_cache[free_slot].refs = 1;
            *slotOut = free_slot;
        }
        /* 缓存满：返回对象但 slot=-1，实例销毁时不卸载（不致崩溃，仅少量驻留） */
        return obj;
    }
}

/* 减引用；归零则真正 UnloadObject 释放显存。 */
static void obj_cache_release(int slot) {
    if (slot < 0 || slot >= OBJ_CACHE_MAX) return;
    if (g_obj_cache[slot].refs <= 0) return;
    if (--g_obj_cache[slot].refs == 0) {
        XPLMUnloadObject(g_obj_cache[slot].obj);
        g_obj_cache[slot].obj = NULL;
        g_obj_cache[slot].rel[0] = '\0';
    }
}

/* 插件退出时清空缓存（正常情况下引用已全部归零）。 */
static void obj_cache_clear(void) {
    int i;
    for (i = 0; i < OBJ_CACHE_MAX; i++) {
        if (g_obj_cache[i].obj) {
            XPLMUnloadObject(g_obj_cache[i].obj);
            g_obj_cache[i].obj = NULL;
        }
        g_obj_cache[i].refs = 0;
        g_obj_cache[i].rel[0] = '\0';
    }
}

static XPLMInstanceRef create_from_rel(const char* relPath, int* slotOut) {
    char sys[1024];
    char abs[2048];
    XPLMGetSystemPath(sys);   /* SDK 430: void XPLMGetSystemPath(char*) */
    snprintf(abs, sizeof(abs), "%s%s", sys, relPath);
    if (!path_exists(abs)) return NULL;

#if kXPLM_Version >= 430
    {
        /* SDK 430：XPLMLoadObject（走缓存）+ XPLMCreateInstance（null 结尾 datarefs） */
        int slot = -1;
        XPLMObjectRef obj = obj_cache_acquire(relPath, &slot);
        if (!obj) return NULL;
        {
            const char* drefs[CSL_MAX_DATAREFS + 1];
            XPLMInstanceRef inst;
            int i;
            for (i = 0; i < CSL_DATAREF_COUNT; i++) drefs[i] = kCslDataRefs[i];
            drefs[CSL_DATAREF_COUNT] = NULL;
            inst = XPLMCreateInstance(obj, drefs);
            if (inst) {
                if (slotOut) *slotOut = slot;   /* 实例持有缓存引用，释放时归还 */
            } else {
                obj_cache_release(slot);        /* 创建失败立即归还引用 */
            }
            return inst;
        }
    }
#else
    /* 旧 SDK：XPLMInstanceCreate(path, datarefs, count)——XP 内部自管对象 */
    {
        XPLMInstanceRef inst = XPLMInstanceCreate(relPath, kCslDataRefs,
                                                  CSL_DATAREF_COUNT);
        if (!inst)
            inst = XPLMInstanceCreate(relPath, NULL, 0);
        if (slotOut) *slotOut = -1;
        return inst;
    }
#endif
}

/* ---------------- xsb_aircraft.txt 解析（X-CSL 复合模型 + 涂装） ---------------- */

#define CSL_MAX_VARIANTS 200   /* 单包变体（涂装）上限 */

typedef struct {
    char name[24];
    char code[12];                 /* AIRLINE/LIVERY 变体码（= 航司 ICAO） */
    char note[64];                 /* AIRLINE/LIVERY 行尾附加 token（可能是尾号） */
    char objs[CSL_MAX_OBJS][200];  /* 该变体的 OBJ 路径（xsb 原样） */
    int  n;
} CslVariant;

static CslVariant g_variants[CSL_MAX_VARIANTS];

/* 解析 xsb_aircraft.txt，返回变体数。每个 OBJ8_AIRCRAFT 块 = 一个变体，
 * 其后的 OBJ8 行追加 OBJ，AIRLINE/LIVERY 行给出变体码（第 2 个 token）。 */
static int read_variants(const char* xsbPath, CslVariant* out, int maxOut) {
    FILE* f = fopen(xsbPath, "r");
    char line[512];
    int n = 0, cur = -1;
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        char* s = line;
        char* tok;
        while (*s == ' ' || *s == '\t') s++;
        if (strncmp(s, "OBJ8_AIRCRAFT", 13) == 0) {
            if (n >= maxOut) { cur = -1; continue; }
            cur = n++;
            memset(&out[cur], 0, sizeof(out[cur]));
            {
                char* p = s + 13;
                size_t k = 0;
                while (*p == ' ' || *p == '\t') p++;
                while (p[k] && p[k] != ' ' && p[k] != '\t' &&
                       p[k] != '\r' && p[k] != '\n' && k < sizeof(out[cur].name) - 1) {
                    out[cur].name[k] = p[k];
                    k++;
                }
                out[cur].name[k] = '\0';
            }
        } else if (cur >= 0 && strncmp(s, "OBJ8 ", 5) == 0) {
            char* last = NULL;
            for (tok = strtok(s + 5, " \t\r\n"); tok;
                 tok = strtok(NULL, " \t\r\n")) last = tok;
            if (last && out[cur].n < CSL_MAX_OBJS) {
                strncpy(out[cur].objs[out[cur].n], last,
                        sizeof(out[cur].objs[0]) - 1);
                out[cur].objs[out[cur].n][sizeof(out[cur].objs[0]) - 1] = '\0';
                out[cur].n++;
            }
        } else if (cur >= 0 &&
                   (strncmp(s, "AIRLINE ", 8) == 0 ||
                    strncmp(s, "LIVERY ", 7) == 0)) {
            /* AIRLINE <ICAO> <CODE> / LIVERY <ICAO> <CODE> [variant]
             * 第 3 个及之后的 token 有些包用来写尾号或 "WL/CRG" 标记，
             * 全部收进 note 供注册号匹配（不能匹配则无副作用）。 */
            int idx = 0;
            for (tok = strtok(s + (s[0] == 'A' ? 8 : 7), " \t\r\n"); tok;
                 tok = strtok(NULL, " \t\r\n")) {
                idx++;
                if (idx == 2) {
                    strncpy(out[cur].code, tok, sizeof(out[cur].code) - 1);
                    out[cur].code[sizeof(out[cur].code) - 1] = '\0';
                } else if (idx > 2) {
                    const size_t used = strlen(out[cur].note);
                    if (used + strlen(tok) + 2 < sizeof(out[cur].note))
                        snprintf(out[cur].note + used,
                                 sizeof(out[cur].note) - used,
                                 "%s%s", used ? " " : "", tok);
                }
            }
        }
    }
    fclose(f);
    return n;
}

/* 忽略大小写的前缀比较（s 以 prefix 开头返回 1）。 */
static int starts_with_ci(const char* s, const char* prefix, size_t n) {
    size_t i;
    for (i = 0; i < n; i++)
        if (!s[i] || tolower((unsigned char)s[i]) !=
                          tolower((unsigned char)prefix[i]))
            return 0;
    return 1;
}

/* 选变体（涂装）：优先级
 * 匹配四级（tier 由调用方写日志，便于核对走了哪条规则）：
 *   1) REG    —— 注册号（尾号）一致：同一架飞机的专属涂装/模型
 *                （变体名、OBJ 文件名、AIRLINE/LIVERY 行尾都参与比较）；
 *   2) AIRLINE—— 同航司 + 同机型（pack 已确定机型）：精确码，再"同航司带后缀"
 *                （如 AAL → AALWL/AAL2，后缀≤3 字符）；
 *   3) TYPE   —— 同机型任意涂装：ZZZ 通用白机 → ANY/GEN → 无涂装基础变体；
 *   4) 兜底   —— 返回 -1，由 csl_load 落到同族模型（别名表）或 ZZZZ 白机。
 * 注意不能"无脑取第一个"：那会给不存在的航司挂上别家涂装（如 BAW 拿到 AAR）。 */

/* 归一化：只留字母数字并大写（"B-1234" → "B1234"），用于尾号模糊比较。 */
static void norm_alnum(char* out, size_t cap, const char* s) {
    size_t k = 0;
    for (; s && *s && k + 1 < cap; s++) {
        const unsigned char c = (unsigned char)*s;
        if (isalnum(c)) out[k++] = (char)toupper(c);
    }
    out[k] = '\0';
}

/* 变体里是否含该尾号：变体名 + AIRLINE/LIVERY 行尾注记 + 全部 OBJ 路径拼成
 * 一条串再归一化比较（部分 CSL 包用文件名带尾号，如 B738_CCA_B-1234.obj）。 */
static int variant_has_reg(const CslVariant* v, const char* reg_norm) {
    char hay[1024], flat[1024];
    int i;
    snprintf(hay, sizeof(hay), "%s|%s", v->name, v->note);
    for (i = 0; i < v->n; i++) {
        const size_t used = strlen(hay);
        if (used + 2 >= sizeof(hay)) break;
        strncat(hay, "|", sizeof(hay) - used - 1);
        strncat(hay, v->objs[i], sizeof(hay) - strlen(hay) - 1);
    }
    norm_alnum(flat, sizeof(flat), hay);
    return strstr(flat, reg_norm) != NULL;
}

/* 变体是否"绑定某一架具体飞机"：名字/OBJ 名里出现 <1~2 位国家码>-<2~5 位尾号>
 * 形式的 token（B-1234 / D-AABC / VH-ABC / A6-EUR）。这类变体只对应一个尾号，
 * 在没有拿到对端尾号时应让位给该航司的通用涂装。 */
static int variant_reg_specific(const CslVariant* v) {
    int p;
    for (p = 0; p < 2; p++) {
        const char* s = p == 0 ? v->name : (v->n ? v->objs[0] : NULL);
        while (s && *s) {
            const char* dash = strchr(s, '-');
            const char* b;
            const char* e;
            if (!dash) break;
            b = dash;
            while (b > s && isalnum((unsigned char)b[-1])) b--;
            e = dash + 1;
            while (*e && isalnum((unsigned char)*e)) e++;
            if ((size_t)(dash - b) >= 1 && (size_t)(dash - b) <= 2 &&
                (size_t)(e - dash - 1) >= 2 && (size_t)(e - dash - 1) <= 5)
                return 1;
            s = e;
        }
    }
    return 0;
}

/* 匹配层级：写日志用 */
#define MATCH_NONE     0
#define MATCH_REG      1
#define MATCH_AIRLINE  2
#define MATCH_TYPE     3
#define MATCH_RANDOM   4   /* 无航司映射：同机型随机真实航司涂装 */

static int pick_variant(int n, const char* airline, const char* reg,
                        int* tier) {
    char regn[32];
    int i;
    if (tier) *tier = MATCH_NONE;
    if (n <= 0) return -1;

    /* 1) 注册号：>=4 位才比较，避免 "B1" 这类短串在文件名里误命中 */
    norm_alnum(regn, sizeof(regn), reg);
    if (regn[0] && strlen(regn) >= 4) {
        for (i = 0; i < n; i++)
            if (variant_has_reg(&g_variants[i], regn)) {
                if (tier) *tier = MATCH_REG;
                return i;
            }
    }

    /* 2) 同航司（pack 已按机型选定）：先取通用涂装，实在只有"绑定尾号"的
     *    变体才用其中之一（至少航司与机型是对的） */
    if (airline && *airline) {
        const size_t al = strlen(airline);
        int reg_specific = -1;
        for (i = 0; i < n; i++) {
            if (!g_variants[i].code[0] ||
                csl_stricmp(g_variants[i].code, airline) != 0) continue;
            if (!variant_reg_specific(&g_variants[i])) {
                if (tier) *tier = MATCH_AIRLINE;
                return i;
            }
            if (reg_specific < 0) reg_specific = i;
        }
        if (reg_specific >= 0) {
            if (tier) *tier = MATCH_AIRLINE;
            return reg_specific;
        }
        if (al >= 3) {   /* 航司 ICAO 均为 3 位；2 位码前缀匹配易误伤（CA→CAL） */
            for (i = 0; i < n; i++) {
                const char* c = g_variants[i].code;
                const size_t cl = strlen(c);
                if (cl > al && cl <= al + 3 && starts_with_ci(c, airline, al)) {
                    if (tier) *tier = MATCH_AIRLINE;
                    return i;
                }
            }
        }
    }

    /* 3) 同机型任意涂装：白机优先，避免挂上别家航司 */
    for (i = 0; i < n; i++)
        if (g_variants[i].code[0] &&
            csl_stricmp(g_variants[i].code, "ZZZ") == 0) {
            if (tier) *tier = MATCH_TYPE;
            return i;
        }
    for (i = 0; i < n; i++)
        if (g_variants[i].code[0] &&
            (csl_stricmp(g_variants[i].code, "ANY") == 0 ||
             csl_stricmp(g_variants[i].code, "GEN") == 0)) {
            if (tier) *tier = MATCH_TYPE;
            return i;
        }
    /* 无涂装的基础变体（xsb 中没有 AIRLINE/LIVERY 行）：白机机身，且保留
     * 全部部件（如 E55P 的 fuselage/wings/engine/gear 分件），比裸 {ICAO}.obj 完整 */
    for (i = 0; i < n; i++)
        if (!g_variants[i].code[0]) {
            if (tier) *tier = MATCH_TYPE;
            return i;
        }
    return -1;
}

/* 匹配层级名（日志用） */
static const char* match_tier_name(int t) {
    switch (t) {
    case MATCH_REG:     return "REG";
    case MATCH_AIRLINE: return "AIRLINE";
    case MATCH_TYPE:    return "TYPE";
    case MATCH_RANDOM:  return "RANDOM";
    default:            return "FALLBACK";
    }
}

/* ---------------- 机型别名（CSL 库没有该型号 → 落到同族模型） ----------------
 *
 * X-CSL 库只覆盖常见机型，而 swift / xPilot / FSInn 的 EQUIPMENT 字段是真实
 * ICAO 型号（本工程收到的实例：FA8X、B38M、GLF6、E75L、PC24 …）。直接给
 * ZZZZ 白机箱体虽然"不会错"，但视觉上完全认不出是什么飞机；落到同族/同级别
 * 模型（FA8X→F900、B38M→B738、GLF6→F900、E75L→E175）既不会张冠李戴航空公司，
 * 又能保持尺寸与轮廓合理。
 *
 * 只按"完整 ICAO 型号"匹配（ICAO 型号即等于机型键，无需前缀猜测，前缀匹配
 * 容易误伤：C17→C172、B38M→B738 之外还有 B3XM 之类）。 */
typedef struct {
    const char* type;   /* ICAO 型号（大写，无尾流后缀） */
    const char* pack;   /* 回退使用的 CSL 包名 */
} CslTypeAlias;

static const CslTypeAlias kTypeAlias[] = {
    /* --- 达索猎鹰 / 湾流 / 庞巴迪 / 里尔：远程公务机 → F900 --- */
    { "FA8X", "F900" }, { "FA7X", "F900" }, { "FA50", "F900" },
    { "FA20", "F900" }, { "F2TH", "F900" }, { "F2LX", "F900" },
    { "F900", "F900" }, { "F90",  "F900" },
    { "GLF4", "F900" }, { "GLF5", "F900" }, { "GLF6", "F900" },
    { "G280", "F900" }, { "G450", "F900" }, { "G550", "F900" },
    { "G650", "F900" }, { "GLEX", "F900" }, { "GL7T", "F900" },
    /* --- 中型公务机 / 奖状系列 → C750 --- */
    { "C25A", "C750" }, { "C25B", "C750" }, { "C25C", "C750" },
    { "C25M", "C750" }, { "C501", "C750" }, { "C525", "C750" },
    { "C550", "C750" }, { "C560", "C750" }, { "C56X", "C750" },
    { "C680", "C750" }, { "C68A", "C750" }, { "C700", "C750" },
    { "C750", "C750" }, { "CL30", "C750" }, { "CL35", "C750" },
    { "CL60", "C750" }, { "LJ45", "C750" }, { "LJ60", "C750" },
    { "LJ75", "C750" }, { "PC24", "C750" }, { "HDJT", "C750" },
    { "PRM1", "C750" }, { "BE40", "C750" },
    /* --- 巴航工业 --- */
    { "E50P", "E55P" }, { "E55P", "E55P" }, { "E545", "E55P" },
    { "E550", "E55P" }, { "E35L", "E55P" },
    { "E135", "E145" }, { "E145", "E145" }, { "E45X", "E145" },
    { "E170", "E170" }, { "E75L", "E175" }, { "E75S", "E175" },
    { "E175", "E175" }, { "E190", "E190" }, { "E195", "E195" },
    /* E19L 是 E190LR 在部分 VATSIM/航司数据里的写法；库里只有 E190 包。 */
    { "E19L", "E190" },
    { "E290", "E195" }, { "E295", "E195" },
    /* --- 波音 --- */
    { "B736", "B733" }, { "B737", "B733" }, { "B73G", "B733" },
    { "B73X", "B733" }, { "B733", "B733" }, { "B735", "B735" },
    { "B738", "B738" }, { "B739", "B739" },
    { "B37M", "B738" }, { "B38M", "B738" }, { "B39M", "B738" },
    { "B3XM", "B738" },
    { "B741", "B744" }, { "B742", "B744" }, { "B743", "B744" },
    { "B744", "B744" }, { "B74S", "B744" }, { "B748", "B748" },
    { "B752", "B752" }, { "B753", "B753" }, { "B75M", "B752" },
    { "B762", "B763" }, { "B763", "B763" }, { "B764", "B764" },
    { "B76X", "B763" }, { "B772", "B772" }, { "B773", "B77W" },
    { "B77L", "B77L" }, { "B77W", "B77W" }, { "B778", "B77W" },
    { "B779", "B77W" }, { "B788", "B788" }, { "B789", "B789" },
    { "B78X", "B789" },
    /* --- 空客 --- */
    { "A318", "A318" }, { "A319", "A319" }, { "A320", "A320" },
    { "A321", "A321" }, { "A19N", "A19N" }, { "A20N", "A20N" },
    { "A21N", "A21N" }, { "A32N", "A320" }, { "A32S", "A320" },
    { "A332", "A332" }, { "A333", "A333" }, { "A338", "A333" },
    { "A339", "A333" }, { "A342", "A346" }, { "A343", "A346" },
    { "A345", "A346" }, { "A346", "A346" }, { "A359", "A359" },
    { "A35K", "A359" }, { "A388", "A388" }, { "A380", "A388" },
    { "A400", "C130" },
    /* --- 支线 / 涡桨 --- */
    { "CRJ1", "CRJ2" }, { "CRJ2", "CRJ2" }, { "CRJ7", "CRJ2" },
    { "CRJ9", "CRJ2" }, { "CRJX", "CRJ2" },
    { "AT45", "AT76" }, { "AT46", "AT76" }, { "AT72", "AT72" },
    { "AT75", "AT75" }, { "AT76", "AT76" },
    { "DH8A", "DH8D" }, { "DH8B", "DH8D" }, { "DH8C", "DH8D" },
    { "DH8D", "DH8D" }, { "SF34", "DH8D" }, { "SB20", "DH8D" },
    { "JS41", "DH8D" }, { "J328", "DH8D" },
    /* --- 通航 / 直升机 --- */
    { "C150", "C172" }, { "C152", "C172" }, { "C172", "C172" },
    { "C182", "C182" }, { "C206", "C182" }, { "T206", "C182" },
    { "C210", "C182" }, { "C208", "C208" }, { "SR20", "SR22" },
    { "SR22", "SR22" }, { "PA28", "PA18" }, { "PA34", "PA18" },
    { "PA46", "PA18" }, { "PC12", "PC12" }, { "PC6",  "C208" },
    { "BE20", "BE58" }, { "BE9L", "BE58" }, { "BE36", "BE58" },
    { "DA40", "DR40" }, { "DA42", "DR40" }, { "SF50", "SF50" },
    { "R22",  "B06" },  { "R44",  "B06" },  { "R66",  "B06" },
    { "AS50", "AS50" }, { "EC30", "AS50" }, { "EC35", "AS50" },
    { "H125", "AS50" }, { "EC45", "AS50" }, { "EC75", "EC75" },
    /* --- 老式喷气 / 运输机 --- */
    { "MD82", "MD81" }, { "MD83", "MD81" }, { "MD88", "MD81" },
    { "MD90", "MD81" }, { "DC10", "MD11" }, { "L101", "MD11" },
    { "AN12", "AN24" }, { "AN24", "AN24" }, { "AN26", "AN26" },
    { "IL76", "IL76" }, { "C17",  "IL76" }, { "A124", "IL76" },
    { "C130", "C130" },
};

/* 查机型别名（完整型号匹配）。未收录返回 NULL。 */
static const char* csl_alias_for(const char* type) {
    size_t i;
    if (!type || !*type) return NULL;
    for (i = 0; i < sizeof(kTypeAlias) / sizeof(kTypeAlias[0]); i++)
        if (csl_stricmp(kTypeAlias[i].type, type) == 0)
            return kTypeAlias[i].pack;
    return NULL;
}

/* 加载变体中单个 OBJ：先按「相对 CSL 根」解析（`pack:file`），
 * 再按「相对包目录」解析（裸文件名）。 */
static XPLMInstanceRef load_obj(const char* objpath, const char* pack,
                                int* slotOut) {
    const char* sep = XPLMGetDirectorySeparator();
    char norm[256], rel[2048];
    norm_sep(norm, sizeof(norm), objpath, sep);
    snprintf(rel, sizeof(rel), "%s%s%s", g_csl_rel, sep, norm);
    {
        XPLMInstanceRef inst = create_from_rel(rel, slotOut);
        if (inst) return inst;
    }
    snprintf(rel, sizeof(rel), "%s%s%s%s%s", g_csl_rel, sep, pack, sep, norm);
    return create_from_rel(rel, slotOut);
}

/* 在绝对目录 dir 中查找 "{ICAO}*.obj"（跳过 *fan.obj），
 * 匹配到则把相对路径（X-System 根）写入 outRel。返回 1 找到。 */
static int find_icao_obj_in_dir(const char* dir, const char* icao,
                                char* outRel, size_t cap) {
    const char* sep = XPLMGetDirectorySeparator();
    char ico_u[16];
    size_t i;
    for (i = 0; icao[i] && i < sizeof(ico_u) - 1; i++)
        ico_u[i] = (char)toupper((unsigned char)icao[i]);
    ico_u[i] = '\0';
    icao = ico_u;

#ifdef _WIN32
    {
        char pattern[1200];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        snprintf(pattern, sizeof(pattern), "%s%s%s*.obj", dir, sep, icao);
        h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) return 0;
        do {
            char name[64], name_u[64];
            size_t k;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            for (k = 0; fd.cFileName[k] && k < sizeof(name) - 1; k++)
                name[k] = fd.cFileName[k];
            name[k] = '\0';
            for (k = 0; name[k] && k < sizeof(name_u) - 1; k++)
                name_u[k] = (char)toupper((unsigned char)name[k]);
            name_u[k] = '\0';
            if (strstr(name_u, "FAN.OBJ")) continue;  /* 风扇动画附加 obj */
            snprintf(outRel, cap, "%s%s%s%s%s", g_csl_rel, sep, icao, sep, name);
            FindClose(h);
            return 1;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        return 0;
    }
#else
    {
        DIR* d = opendir(dir);
        struct dirent* e;
        size_t ico_len = strlen(icao);
        if (!d) return 0;
        while ((e = readdir(d)) != NULL) {
            char name[64], name_u[64];
            size_t k;
            for (k = 0; e->d_name[k] && k < sizeof(name) - 1; k++)
                name[k] = e->d_name[k];
            name[k] = '\0';
            for (k = 0; name[k] && k < sizeof(name_u) - 1; k++)
                name_u[k] = (char)toupper((unsigned char)name[k]);
            name_u[k] = '\0';
            if (strncmp(name_u, icao, ico_len) != 0) continue;
            if (strstr(name_u, "FAN.OBJ")) continue;
            {
                /* 长度 < 4 时 name_u + len - 4 会指针下溢（UB），先挡掉 */
                const size_t nl = strlen(name_u);
                if (nl < 4 || strstr(name_u, ".OBJ") != (name_u + nl - 4))
                    continue;
            }
            snprintf(outRel, cap, "%s%s%s%s%s", g_csl_rel, sep, icao, sep, name);
            closedir(d);
            return 1;
        }
        closedir(d);
        return 0;
    }
#endif
}

/* ---------------- 加载 / 定位 / 释放 ---------------- */

/* 按包名（CSL 子目录）加载一架飞机：先走 xsb_aircraft.txt 变体，再退到
 * family/{icao}.obj、{icao}.obj、目录内 {icao}*.obj。成功返回实例数。 */
static int g_last_tier = MATCH_NONE;   /* 本次 load_pack 的匹配层级（日志用） */

/* FNV-1a：同一呼号稳定，不同呼号分散。 */
static unsigned csl_seed_hash(const char* s) {
    unsigned h = 2166136261u;
    while (s && *s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

/* 通用/白机变体码：随机真实航司时排除，避免又退成白机。 */
static int csl_generic_code(const char* code) {
    return csl_stricmp(code, "ZZZ") == 0 ||
           csl_stricmp(code, "ANY") == 0 ||
           csl_stricmp(code, "GEN") == 0;
}

/* 无航司映射时，从同机型里随机挑一个真实航司涂装：
 * 优先非"绑定尾号"变体；没有再用任意真实带码变体（排除 ZZZ/ANY/GEN）。 */
static int pick_random_variant(int n, const char* seed) {
    int cand[CSL_MAX_VARIANTS];
    int count = 0, i;
    for (i = 0; i < n && count < CSL_MAX_VARIANTS; i++) {
        if (!g_variants[i].code[0] || g_variants[i].n <= 0) continue;
        if (csl_generic_code(g_variants[i].code)) continue;
        if (variant_reg_specific(&g_variants[i])) continue;
        cand[count++] = i;
    }
    if (count == 0) {
        for (i = 0; i < n && count < CSL_MAX_VARIANTS; i++) {
            if (!g_variants[i].code[0] || g_variants[i].n <= 0) continue;
            if (csl_generic_code(g_variants[i].code)) continue;
            cand[count++] = i;
        }
    }
    if (count == 0) return -1;
    return cand[csl_seed_hash(seed) % (unsigned)count];
}

static int load_pack(CslNode* node, const char* pack, const char* family,
                     const char* airline, const char* reg,
                     const char* seed) {
    char packAbs[1200], xsb[1400];
    const char* sep = XPLMGetDirectorySeparator();
    int i, n;

    node->inst_count = 0;
    for (i = 0; i < CSL_MAX_OBJS; i++) node->obj_slots[i] = -1;
    node->variant[0] = '\0';
    node->variant_name[0] = '\0';
    g_last_tier = MATCH_NONE;

    snprintf(packAbs, sizeof(packAbs), "%s%s%s", g_csl_abs, sep, pack);
    snprintf(xsb, sizeof(xsb), "%s%s%s", packAbs, sep, "xsb_aircraft.txt");

    /* 1) X-CSL 风格：CSL/{ICAO}/xsb_aircraft.txt（复合模型 + 涂装变体） */
    n = read_variants(xsb, g_variants, CSL_MAX_VARIANTS);
    if (n > 0) {
        int vi = pick_variant(n, airline, reg, &g_last_tier);
        /* 不优先白机：没有航司映射，或只匹配到 ZZZ/ANY/白机基础变体时，
         * 先随机一个同机型真实航司涂装；只有包里没有真实航司才用白机。 */
        if (vi < 0 || g_last_tier == MATCH_TYPE) {
            const int rv = pick_random_variant(n, seed);
            if (rv >= 0) {
                vi = rv;
                g_last_tier = MATCH_RANDOM;
            }
        }
        if (vi >= 0) {
            for (i = 0; i < g_variants[vi].n && node->inst_count < CSL_MAX_OBJS;
                 i++) {
                int slot = -1;
                XPLMInstanceRef inst = load_obj(g_variants[vi].objs[i], pack,
                                                &slot);
                if (inst) {
                    node->obj_slots[node->inst_count] = slot;
                    node->insts[node->inst_count++] = inst;
                }
            }
            if (node->inst_count > 0) {
                strncpy(node->variant, g_variants[vi].code,
                        sizeof(node->variant) - 1);
                node->variant[sizeof(node->variant) - 1] = '\0';
                strncpy(node->variant_name, g_variants[vi].name,
                        sizeof(node->variant_name) - 1);
                node->variant_name[sizeof(node->variant_name) - 1] = '\0';
                return node->inst_count;
            }
        }

        /* 没有真实航司涂装可随机：才退回包内基础 OBJ（{ICAO}.obj 白机）。 */
        {
            char rel[2048];
            int slot = -1;
            snprintf(rel, sizeof(rel), "%s%s%s%s%s.obj",
                     g_csl_rel, sep, pack, sep, pack);
            {
                XPLMInstanceRef inst = create_from_rel(rel, &slot);
                if (inst) {
                    node->obj_slots[node->inst_count] = slot;
                    node->insts[node->inst_count++] = inst;
                    snprintf(node->variant, sizeof(node->variant), "BASE");
                    g_last_tier = MATCH_TYPE;
                    return node->inst_count;
                }
            }
        }

        /* 最后回退：同机型随机一个真实航司涂装（以前固定变体 0，所有
         * 未匹配航司都变成同一个 AAF）。按呼号哈希，稳定且不同机组不同。 */
        {
            int vi = pick_random_variant(n, seed);
            if (vi < 0) vi = 0;
            for (i = 0; i < g_variants[vi].n && node->inst_count < CSL_MAX_OBJS;
                 i++) {
                int slot = -1;
                XPLMInstanceRef inst = load_obj(g_variants[vi].objs[i], pack,
                                                &slot);
                if (inst) {
                    node->obj_slots[node->inst_count] = slot;
                    node->insts[node->inst_count++] = inst;
                }
            }
            if (node->inst_count > 0) {
                strncpy(node->variant, g_variants[vi].code,
                        sizeof(node->variant) - 1);
                node->variant[sizeof(node->variant) - 1] = '\0';
                strncpy(node->variant_name, g_variants[vi].name,
                        sizeof(node->variant_name) - 1);
                node->variant_name[sizeof(node->variant_name) - 1] = '\0';
                g_last_tier = MATCH_RANDOM;
                return node->inst_count;
            }
        }
    }

    /* 2) 回退：CSL/{family}/{icao}.obj → CSL/{icao}.obj → CSL/{icao}/{icao}*.obj */
    {
        char rel[2048], dir[1300];
        int slot;
        if (family && *family) {
            char fm[128];
            size_t k;
            for (k = 0; family[k] && k < sizeof(fm) - 1; k++)
                fm[k] = (char)toupper((unsigned char)family[k]);
            fm[k] = '\0';
            snprintf(rel, sizeof(rel), "%s%s%s%s%s.obj", g_csl_rel, sep, fm, sep, pack);
            slot = -1;
            {
                XPLMInstanceRef inst = create_from_rel(rel, &slot);
                if (inst) {
                    node->obj_slots[node->inst_count] = slot;
                    node->insts[node->inst_count++] = inst;
                    return node->inst_count;
                }
            }
        }
        snprintf(rel, sizeof(rel), "%s%s%s.obj", g_csl_rel, sep, pack);
        slot = -1;
        {
            XPLMInstanceRef inst = create_from_rel(rel, &slot);
            if (inst) {
                node->obj_slots[node->inst_count] = slot;
                node->insts[node->inst_count++] = inst;
                return node->inst_count;
            }
        }
        snprintf(dir, sizeof(dir), "%s%s%s", g_csl_abs, sep, pack);
        {
            char found[1024];
            if (find_icao_obj_in_dir(dir, pack, found, sizeof(found))) {
                slot = -1;
                {
                    XPLMInstanceRef inst = create_from_rel(found, &slot);
                    if (inst) {
                        node->obj_slots[node->inst_count] = slot;
                        node->insts[node->inst_count++] = inst;
                        return node->inst_count;
                    }
                }
            }
        }
    }
    return node->inst_count;
}

int csl_load(CslNode* node, const char* icao, const char* family,
             const char* airline, const char* reg, const char* seed) {
    char pack[16];
    size_t k = 0, j;

    if (node->tried) return node->inst_count;   /* 已尝试过：返回缓存结果 */
    node->tried = 1;
    node->inst_count = 0;
    {
        int si;
        for (si = 0; si < CSL_MAX_OBJS; si++) node->obj_slots[si] = -1;
    }
    node->variant[0] = '\0';
    node->variant_name[0] = '\0';

    /* 机型代码归一化：仅保留字母数字/连字符并大写。
     * PI/飞行计划里的 EQUIPMENT 可能带尾流后缀（"B738/W"）或空白。 */
    for (j = 0; icao && icao[j] && k < sizeof(pack) - 1; j++) {
        const unsigned char c = (unsigned char)icao[j];
        if (c == '/' || c == ' ' || c == '\t') break;
        if (isalnum(c) || c == '-') pack[k++] = (char)toupper(c);
    }
    pack[k] = '\0';

    if (pack[0] && load_pack(node, pack, family, airline, reg, seed) > 0) {
        xconnect_logf("csl: %s -> pack %s variant %s/%s match=%s "
                      "(%d obj, airline=%s, reg=%s)",
                      pack, pack,
                      node->variant[0] ? node->variant : "BASE",
                      node->variant_name[0] ? node->variant_name : "-",
                      match_tier_name(g_last_tier), node->inst_count,
                      (airline && *airline) ? airline : "-",
                      (reg && *reg) ? reg : "-");
        return node->inst_count;
    }

    /* 机型别名：CSL 库没有这个具体型号，但有同族模型（FA8X→F900、
     * B38M→B738、GLF6→F900、E75L→E175 …）。先落到同族模型，比直接给
     * 白机箱体准确得多，也不会张冠李戴航空公司涂装（涂装仍按 airline 选）。 */
    if (pack[0]) {
        const char* alias = csl_alias_for(pack);
        if (alias && csl_stricmp(alias, pack) != 0 &&
            load_pack(node, alias, family, airline, reg, seed) > 0) {
            xconnect_logf("csl: %s has no CSL pack -> alias %s variant %s/%s "
                          "match=%s (%d obj, airline=%s, reg=%s)",
                          pack, alias,
                          node->variant[0] ? node->variant : "BASE",
                          node->variant_name[0] ? node->variant_name : "-",
                          match_tier_name(g_last_tier), node->inst_count,
                          (airline && *airline) ? airline : "-",
                          (reg && *reg) ? reg : "-");
            return node->inst_count;
        }
    }

    /* 机型未知或 CSL 库中没有该机型 → 通用 ZZZZ 模型（白机），
     * 既不会"没有模型"，也不会把所有人错挂成默认机型。 */
    if (strcmp(pack, "ZZZZ") != 0 &&
        load_pack(node, "ZZZZ", NULL, NULL, NULL, seed) > 0) {
        xconnect_logf("csl: no CSL for \"%s\" (airline=%s) -> generic ZZZZ",
                      icao ? icao : "", airline ? airline : "");
        return node->inst_count;
    }
    return node->inst_count;
}

void csl_release_node(CslNode* node) {
    int i;
    for (i = 0; i < node->inst_count; i++) {
        if (node->insts[i]) {
#if kXPLM_Version >= 430
            XPLMDestroyInstance(node->insts[i]);
#else
            XPLMInstanceDestroy(node->insts[i]);
#endif
            node->insts[i] = NULL;
        }
        /* 归还 OBJ 缓存引用；归零才真正 UnloadObject（释放显存）。
         * 旧 SDK（<430）无缓存归属，slot 恒 -1，调用是安全的 no-op。 */
        obj_cache_release(node->obj_slots[i]);
        node->obj_slots[i] = -1;
    }
    node->inst_count = 0;
    node->tried = 0;
    node->variant[0] = '\0';
    node->variant_name[0] = '\0';
}

void csl_set_position(CslNode* node, double lat, double lon,
                      double alt_m, double heading, double pitch, double roll,
                      const float* animValues, int animCount) {
    float data[CSL_MAX_DATAREFS];
    int i;
    if (!node || node->inst_count <= 0) return;

    for (i = 0; i < animCount && i < CSL_MAX_DATAREFS; i++)
        data[i] = animValues[i];
    for (; i < CSL_MAX_DATAREFS; i++)
        data[i] = 0.0f;

#if kXPLM_Version >= 430
    {
        /* 局部坐标 + 姿态结构体（structSize 必须填充） */
        XPLMDrawInfo_t info;
        double x = 0.0, y = 0.0, z = 0.0;
        XPLMWorldToLocal(lat, lon, alt_m, &x, &y, &z);
        memset(&info, 0, sizeof(info));
        info.structSize = (int)sizeof(info);
        info.x = (float)x;
        info.y = (float)y;
        info.z = (float)z;
        info.pitch = (float)pitch;
        info.heading = (float)heading;
        info.roll = (float)roll;
        for (i = 0; i < node->inst_count; i++)
            if (node->insts[i]) XPLMInstanceSetPosition(node->insts[i], &info, data);
    }
#else
    {
        float pos[9];
        int i2;
        pos[0] = (float)lat;
        pos[1] = (float)lon;
        pos[2] = (float)alt_m;
        pos[3] = (float)heading;
        pos[4] = (float)pitch;
        pos[5] = (float)roll;
        pos[6] = 0.0f;
        pos[7] = 0.0f;
        pos[8] = 0.0f;
        for (i2 = 0; i2 < node->inst_count; i2++)
            if (node->insts[i2]) XPLMInstanceSetPosition(node->insts[i2], pos, data);
    }
#endif
}

double csl_terrain_msl(double lat, double lon, double alt_hint_m, int* ok) {
    XPLMProbeInfo_t info;
    double x = 0.0, y = 0.0, z = 0.0;
    double tlat = 0.0, tlon = 0.0, tmsl = 0.0;
    if (ok) *ok = 0;
    if (!g_probe) g_probe = XPLMCreateProbe(xplm_ProbeY);
    if (!g_probe) return 0.0;
    XPLMWorldToLocal(lat, lon, alt_hint_m, &x, &y, &z);
    memset(&info, 0, sizeof(info));
    info.structSize = (int)sizeof(info);
    if (XPLMProbeTerrainXYZ(g_probe, (float)x, (float)(y + 2000.0), (float)z,
                            &info) != xplm_ProbeHitTerrain)
        return 0.0;   /* 未命中地形（海面/星球边缘） */
    XPLMLocalToWorld(x, info.locationY, z, &tlat, &tlon, &tmsl);
    if (ok) *ok = 1;
    return tmsl;
}
