#include "csl.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "XPLMUtilities.h"
#if kXPLM_Version >= 430
#include "XPLMGraphics.h"   /* XPLMWorldToLocal（XPLMDrawInfo_t 在 XPLMScenery.h，经 XPLMInstance.h 引入） */
#endif

/* XPMP2 / X-CSL 约定的动画 dataref（顺序与 X-CSL obj 一致） */
static const char* kCslDataRefs[CSL_MAX_DATAREFS] = {
    "sim/cockpit2/controls/flap_ratio",
    "sim/flightmodel/controls/gear_deploy_ratio",
    "sim/cockpit2/engine/actuator_throttle_ratio_all",
    "sim/cockpit2/electrical/battery_on[0]",
    "sim/flightmodel/engine/ENGN_beacon_lite[0]",
    "sim/flightmodel/engine/ENGN_nav_lite[0]",
    "sim/cockpit2/controls/left_brake_ratio",
    "sim/cockpit2/controls/right_brake_ratio",
    "sim/cockpit2/radios/actuators/transponder_mode_available",
    "sim/cockpit2/radios/actuators/transponder_code",
    "sim/cockpit2/radios/actuators/transponder_mode",
};
#define CSL_DATAREF_COUNT ((int)(sizeof(kCslDataRefs) / sizeof(kCslDataRefs[0])))

static char g_csl_rel[1024] = "";   /* 相对 X-System 根（XPLMLoadObject 用） */
static char g_csl_abs[1024] = "";   /* 绝对路径（存在性检查用） */

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
    /* 实例由持有者逐个释放；对象引用在实例销毁后由 XPLM 管理 */
}

static int path_exists(const char* absPath) {
    FILE* f = fopen(absPath, "rb");
    if (f) {
        fclose(f);
        return 1;
    }
    return 0;
}

static void make_obj_rel(char* out, size_t cap, const char* family,
                         const char* icao) {
    char fm[128], ics[16];
    size_t i;
    for (i = 0; icao[i] && i < sizeof(ics) - 1; i++)
        ics[i] = (char)toupper((unsigned char)icao[i]);
    ics[i] = '\0';
    if (family && *family) {
        for (i = 0; family[i] && i < sizeof(fm) - 1; i++)
            fm[i] = (char)toupper((unsigned char)family[i]);
        fm[i] = '\0';
        snprintf(out, cap, "%s%s%s%s%s.obj", g_csl_rel,
                 XPLMGetDirectorySeparator(), fm,
                 XPLMGetDirectorySeparator(), ics);
    } else {
        snprintf(out, cap, "%s%s%s.obj", g_csl_rel,
                 XPLMGetDirectorySeparator(), ics);
    }
}

static XPLMInstanceRef try_create(const char* relPath) {
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
            int i;
            for (i = 0; i < CSL_DATAREF_COUNT; i++) drefs[i] = kCslDataRefs[i];
            drefs[CSL_DATAREF_COUNT] = NULL;
            return XPLMCreateInstance(obj, drefs);
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

XPLMInstanceRef csl_load(CslNode* node, const char* icao, const char* family) {
    char rel[2048];
    if (node->tried) return node->inst;   /* 已尝试过：返回缓存结果 */
    node->tried = 1;

    if (family && *family) {
        make_obj_rel(rel, sizeof(rel), family, icao);
        node->inst = try_create(rel);
        if (node->inst) return node->inst;
    }
    make_obj_rel(rel, sizeof(rel), NULL, icao);
    node->inst = try_create(rel);
    return node->inst;
}

void csl_release_node(CslNode* node) {
    if (node->inst) {
#if kXPLM_Version >= 430
        XPLMDestroyInstance(node->inst);
#else
        XPLMInstanceDestroy(node->inst);
#endif
        node->inst = NULL;
    }
    node->tried = 0;
}

void csl_set_position(XPLMInstanceRef inst, double lat, double lon,
                      double alt_m, double heading, double pitch, double roll,
                      const float* animValues, int animCount) {
    float data[CSL_MAX_DATAREFS];
    int i;
    if (!inst) return;

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
        XPLMInstanceSetPosition(inst, &info, data);
    }
#else
    {
        float pos[9];
        pos[0] = (float)lat;
        pos[1] = (float)lon;
        pos[2] = (float)alt_m;
        pos[3] = (float)heading;
        pos[4] = (float)pitch;
        pos[5] = (float)roll;
        pos[6] = 0.0f;
        pos[7] = 0.0f;
        pos[8] = 0.0f;
        XPLMInstanceSetPosition(inst, pos, data);
    }
#endif
}