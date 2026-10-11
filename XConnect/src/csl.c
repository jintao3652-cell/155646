#include "csl.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>   /* FindFirstFileA 目录枚举 */
#define csl_stricmp _stricmp
#else
#include <dirent.h>
#include <strings.h>
#define csl_stricmp strcasecmp
#endif

#include "XPLMUtilities.h"
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
};
#define CSL_DATAREF_COUNT ((int)(sizeof(kCslDataRefs) / sizeof(kCslDataRefs[0])))

static char g_csl_rel[1024] = "";   /* 相对 X-System 根（XPLMLoadObject 用） */
static char g_csl_abs[1024] = "";   /* 绝对路径（存在性检查用） */

static XPLMProbeRef g_probe = NULL; /* 地形探针（地面钳制用，仅主线程） */

int csl_dataref_count(void) {
    return CSL_DATAREF_COUNT;
}

void csl_init(void) {
    char sys[1024];
    const char* sep = XPLMGetDirectorySeparator();
    snprintf(g_csl_rel, sizeof(g_csl_rel),
             "Resources%splugins%sXConnect%sCSL", sep, sep, sep);
    XPLMGetSystemPath(sys);   /* SDK 430: void XPLMGetSystemPath(char*) */
    snprintf(g_csl_abs, sizeof(g_csl_abs), "%s%s", sys, g_csl_rel);
}

void csl_shutdown(void) {
    if (g_probe) {
        XPLMDestroyProbe(g_probe);
        g_probe = NULL;
    }
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
static XPLMInstanceRef create_from_rel(const char* relPath) {
    char sys[1024];
    char abs[2048];
    XPLMGetSystemPath(sys);   /* SDK 430: void XPLMGetSystemPath(char*) */
    snprintf(abs, sizeof(abs), "%s%s", sys, relPath);
    if (!path_exists(abs)) return NULL;

#if kXPLM_Version >= 430
    {
        /* SDK 430：XPLMLoadObject + XPLMCreateInstance（null 结尾 datarefs） */
        XPLMObjectRef obj = XPLMLoadObject(relPath);
        if (!obj) return NULL;
        {
            const char* drefs[CSL_MAX_DATAREFS + 1];
            XPLMInstanceRef inst;
            int i;
            for (i = 0; i < CSL_DATAREF_COUNT; i++) drefs[i] = kCslDataRefs[i];
            drefs[CSL_DATAREF_COUNT] = NULL;
            inst = XPLMCreateInstance(obj, drefs);
            if (!inst) XPLMUnloadObject(obj);   /* 创建失败需卸载对象引用 */
            return inst;
        }
    }
#else
    /* 旧 SDK：XPLMInstanceCreate(path, datarefs, count) */
    {
        XPLMInstanceRef inst = XPLMInstanceCreate(relPath, kCslDataRefs,
                                                  CSL_DATAREF_COUNT);
        if (!inst)
            inst = XPLMInstanceCreate(relPath, NULL, 0);
        return inst;
    }
#endif
}

/* ---------------- xsb_aircraft.txt 解析（X-CSL 复合模型 + 涂装） ---------------- */

#define CSL_MAX_VARIANTS 200   /* 单包变体（涂装）上限 */

typedef struct {
    char name[24];
    char code[12];                 /* AIRLINE/LIVERY 变体码（= 航司 ICAO） */
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
            /* AIRLINE <ICAO> <CODE> / LIVERY <ICAO> <CODE> [variant] */
            int idx = 0;
            for (tok = strtok(s + (s[0] == 'A' ? 8 : 7), " \t\r\n"); tok;
                 tok = strtok(NULL, " \t\r\n")) {
                idx++;
                if (idx == 2) {
                    strncpy(out[cur].code, tok, sizeof(out[cur].code) - 1);
                    out[cur].code[sizeof(out[cur].code) - 1] = '\0';
                    break;
                }
            }
        }
    }
    fclose(f);
    return n;
}

/* 选变体：优先 airline 精确匹配；否则 "ANY"；再否则第一个。 */
static int pick_variant(int n, const char* airline) {
    int i, any = -1;
    if (n <= 0) return -1;
    if (airline && *airline) {
        for (i = 0; i < n; i++)
            if (g_variants[i].code[0] &&
                csl_stricmp(g_variants[i].code, airline) == 0) return i;
    }
    for (i = 0; i < n; i++)
        if (g_variants[i].code[0] &&
            csl_stricmp(g_variants[i].code, "ANY") == 0) { any = i; break; }
    if (any >= 0) return any;
    return 0;
}

/* 加载变体中单个 OBJ：先按「相对 CSL 根」解析（`pack:file`），
 * 再按「相对包目录」解析（裸文件名）。 */
static XPLMInstanceRef load_obj(const char* objpath, const char* pack) {
    const char* sep = XPLMGetDirectorySeparator();
    char norm[256], rel[2048];
    norm_sep(norm, sizeof(norm), objpath, sep);
    snprintf(rel, sizeof(rel), "%s%s%s", g_csl_rel, sep, norm);
    {
        XPLMInstanceRef inst = create_from_rel(rel);
        if (inst) return inst;
    }
    snprintf(rel, sizeof(rel), "%s%s%s%s%s", g_csl_rel, sep, pack, sep, norm);
    return create_from_rel(rel);
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
            if (strstr(name_u, ".OBJ") != (name_u + strlen(name_u) - 4))
                continue;
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

int csl_load(CslNode* node, const char* icao, const char* family,
             const char* airline) {
    char pack[16], packAbs[1200], xsb[1400];
    const char* sep = XPLMGetDirectorySeparator();
    int i, n;
    if (node->tried) return node->inst_count;   /* 已尝试过：返回缓存结果 */
    node->tried = 1;
    node->inst_count = 0;
    node->variant[0] = '\0';

    {
        size_t k;
        for (k = 0; icao[k] && k < sizeof(pack) - 1; k++)
            pack[k] = (char)toupper((unsigned char)icao[k]);
        pack[k] = '\0';
    }
    snprintf(packAbs, sizeof(packAbs), "%s%s%s", g_csl_abs, sep, pack);
    snprintf(xsb, sizeof(xsb), "%s%s%s", packAbs, sep, "xsb_aircraft.txt");

    /* 1) X-CSL 风格：CSL/{ICAO}/xsb_aircraft.txt（复合模型 + 涂装变体） */
    n = read_variants(xsb, g_variants, CSL_MAX_VARIANTS);
    if (n > 0) {
        int vi = pick_variant(n, airline);
        if (vi >= 0) {
            for (i = 0; i < g_variants[vi].n && node->inst_count < CSL_MAX_OBJS;
                 i++) {
                XPLMInstanceRef inst = load_obj(g_variants[vi].objs[i], pack);
                if (inst) node->insts[node->inst_count++] = inst;
            }
            if (node->inst_count > 0) {
                strncpy(node->variant, g_variants[vi].code,
                        sizeof(node->variant) - 1);
                node->variant[sizeof(node->variant) - 1] = '\0';
                return node->inst_count;
            }
        }
    }

    /* 2) 回退：CSL/{family}/{icao}.obj → CSL/{icao}.obj → CSL/{icao}/{icao}*.obj */
    {
        char rel[2048], dir[1300];
        if (family && *family) {
            char fm[128];
            size_t k;
            for (k = 0; family[k] && k < sizeof(fm) - 1; k++)
                fm[k] = (char)toupper((unsigned char)family[k]);
            fm[k] = '\0';
            snprintf(rel, sizeof(rel), "%s%s%s%s%s.obj", g_csl_rel, sep, fm, sep, pack);
            {
                XPLMInstanceRef inst = create_from_rel(rel);
                if (inst) { node->insts[node->inst_count++] = inst; return node->inst_count; }
            }
        }
        snprintf(rel, sizeof(rel), "%s%s%s.obj", g_csl_rel, sep, pack);
        {
            XPLMInstanceRef inst = create_from_rel(rel);
            if (inst) { node->insts[node->inst_count++] = inst; return node->inst_count; }
        }
        snprintf(dir, sizeof(dir), "%s%s%s", g_csl_abs, sep, pack);
        {
            char found[1024];
            if (find_icao_obj_in_dir(dir, pack, found, sizeof(found))) {
                XPLMInstanceRef inst = create_from_rel(found);
                if (inst) { node->insts[node->inst_count++] = inst; return node->inst_count; }
            }
        }
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
    }
    node->inst_count = 0;
    node->tried = 0;
    node->variant[0] = '\0';
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
