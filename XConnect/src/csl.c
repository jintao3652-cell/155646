#include "csl.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "XPLMUtilities.h"

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

static char g_csl_root[1024] = "";

int csl_dataref_count(void) {
    return CSL_DATAREF_COUNT;
}

void csl_init(const char* cslRoot) {
    if (cslRoot) {
        strncpy(g_csl_root, cslRoot, sizeof(g_csl_root) - 1);
        g_csl_root[sizeof(g_csl_root) - 1] = '\0';
    }
}

void csl_shutdown(void) {
    /* 实例在插件级由 XPLM 管理；XPLMUnloadInstance 由持有者调用，
       这里仅做占位以保持 API 对称。 */
}

static int path_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) {
        fclose(f);
        return 1;
    }
    return 0;
}

static void make_root_obj_path(char* out, size_t cap, const char* icao) {
    char ics[16];
    size_t i;
    for (i = 0; icao[i] && i < sizeof(ics) - 1; i++)
        ics[i] = (char)toupper((unsigned char)icao[i]);
    ics[i] = '\0';
    snprintf(out, cap, "%s%s%s.obj", g_csl_root,
             XPLMGetDirectorySeparator(), ics);
}

static void make_pkg_obj_path(char* out, size_t cap, const char* family,
                              const char* icao) {
    char fm[128], ics[16];
    size_t i;
    for (i = 0; family[i] && i < sizeof(fm) - 1; i++)
        fm[i] = (char)toupper((unsigned char)family[i]);
    fm[i] = '\0';
    for (i = 0; icao[i] && i < sizeof(ics) - 1; i++)
        ics[i] = (char)toupper((unsigned char)icao[i]);
    ics[i] = '\0';
    snprintf(out, cap, "%s%s%s%s%s.obj", g_csl_root,
             XPLMGetDirectorySeparator(), fm,
             XPLMGetDirectorySeparator(), ics);
}

static XPLMInstanceRef try_create(const char* objPath) {
    XPLMInstanceRef inst;
    if (!objPath || !*objPath) return NULL;
    if (!path_exists(objPath)) return NULL;
    inst = XPLMInstanceCreate(objPath, kCslDataRefs, CSL_DATAREF_COUNT);
    if (!inst) {
        /* 动画 dataref 版本失败时降级：无动画实例 */
        inst = XPLMInstanceCreate(objPath, NULL, 0);
    }
    return inst;
}

XPLMInstanceRef csl_load(CslNode* node, const char* icao, const char* family) {
    char path[1024];
    if (node->tried) return node->inst;   /* 已尝试过：返回缓存结果 */
    node->tried = 1;

    if (family && *family) {
        make_pkg_obj_path(path, sizeof(path), family, icao);
        node->inst = try_create(path);
        if (node->inst) return node->inst;
    }
    /* 兼容 family 为完整子目录名（含包名时 ICAO 路径可能大写/小写）：
       先用 family 拼，失败则回退单层。 */
    make_root_obj_path(path, sizeof(path), icao);
    node->inst = try_create(path);
    return node->inst;
}

void csl_release_node(CslNode* node) {
    if (node->inst) {
        XPLMInstanceDestroy(node->inst);
        node->inst = NULL;
    }
    node->tried = 0;
}

void csl_set_position(XPLMInstanceRef inst, double lat, double lon,
                      double alt_m, double heading, double pitch, double roll,
                      const float* animValues, int animCount) {
    float pos[9];
    if (!inst) return;
    pos[0] = (float)lat;
    pos[1] = (float)lon;
    pos[2] = (float)alt_m;
    pos[3] = (float)heading;
    pos[4] = (float)pitch;
    pos[5] = (float)roll;
    pos[6] = 0.0f;
    pos[7] = 0.0f;
    pos[8] = 0.0f;
    if (animValues && animCount > 0) {
        XPLMInstanceSetPosition(inst, pos, animValues);
    } else {
        float def[CSL_DATAREF_COUNT];
        memset(def, 0, sizeof(def));
        XPLMInstanceSetPosition(inst, pos, def);
    }
}