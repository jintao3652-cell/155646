/* CSL 变体匹配（四级规则）与机型别名的独立单元测试 — 不需要 X-Plane。
 *
 * 用桩替换 XPLM 的对象加载/实例 API，在临时目录造出 X-CSL 结构的包，然后
 * 断言 csl_load 选中的变体名（CslNode::variant_name）：
 *
 *   1) REG     —— 注册号（尾号）一致 → 同一架飞机的专属涂装/模型
 *   2) AIRLINE —— 同航司 + 同机型（优先不绑定尾号的通用涂装）
 *   3) TYPE    —— 同机型任意涂装（ZZZ/ANY/基础变体）
 *   4) FALLBACK—— 机型别名同族模型 → ZZZZ 白机
 *
 * 编译（与插件同一套开关，见 XConnect/README.md「单元测试」）：
 *   cl /nologo /MT /utf-8 /DXPLM200 ... /DXPLM430 /I<SDK> /Isrc \
 *      test/csl_match_test.c src/csl.c /Fe:build/csl_match_test.exe
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#endif

#include "XPLMGraphics.h"
#include "XPLMInstance.h"
#include "XPLMPlanes.h"
#include "XPLMScenery.h"
#include "XPLMUtilities.h"

#include "csl.h"
#include "XPLMDataAccess.h"   /* dataref 注册桩的类型（XPLMDataRef/回调 typedef） */

/* ---------------- XPLM / 日志桩 ---------------- */

static char g_sys[1024] = "";
static int g_match_pass = 0;
static int g_match_fail = 0;

void XPLMGetSystemPath(char* outSystemPath) {
    strcpy(outSystemPath, g_sys);
}
const char* XPLMGetDirectorySeparator(void) { return "/"; }

/* libxplanemp dataref 注册桩（csl_init 在 LoadObject 前注册，见 csl.c）：
 * 测试里返回一个假句柄并接受注册/注销，不校验回调。 */
XPLMDataRef XPLMFindDataRef(const char* inDatarefName) {
    (void)inDatarefName;
    return NULL;   /* 测试环境无真实 dataref，全部走"自行注册"分支 */
}
XPLMDataRef XPLMRegisterDataAccessor(
    const char* inDatarefName, XPLMDataTypeID inDataType, int inIsWritable,
    XPLMGetDatai_f inReadInt, XPLMSetDatai_f inWriteInt,
    XPLMGetDataf_f inReadFloat, XPLMSetDataf_f inWriteFloat,
    XPLMGetDatad_f inReadDouble, XPLMSetDatad_f inWriteDouble,
    XPLMGetDatavi_f inReadIntArray, XPLMSetDatavi_f inWriteIntArray,
    XPLMGetDatavf_f inReadFloatArray, XPLMSetDatavf_f inWriteFloatArray,
    void* inReadRefcon, void* inWriteRefcon,
    XPLMGetDatab_f inReadData, XPLMSetDatab_f inWriteData) {
    (void)inDataType; (void)inIsWritable;
    (void)inReadInt; (void)inWriteInt; (void)inReadFloat; (void)inWriteFloat;
    (void)inReadDouble; (void)inWriteDouble; (void)inReadIntArray;
    (void)inWriteIntArray; (void)inReadFloatArray; (void)inWriteFloatArray;
    (void)inReadRefcon; (void)inWriteRefcon; (void)inReadData; (void)inWriteData;
    return (XPLMDataRef)(size_t)(0x1000 +
           (unsigned)strlen(inDatarefName ? inDatarefName : ""));
}
void XPLMUnregisterDataAccessor(XPLMDataRef inDataref) { (void)inDataref; }

XPLMObjectRef XPLMLoadObject(const char* inPath) {
    (void)inPath;
    return (XPLMObjectRef)1;              /* 非 NULL 即“加载成功” */
}
void XPLMUnloadObject(XPLMObjectRef inObject) { (void)inObject; }

XPLMInstanceRef XPLMCreateInstance(XPLMObjectRef inObject,
                                   const char** inDataRefs) {
    (void)inObject;
    (void)inDataRefs;
    return (XPLMInstanceRef)1;
}
void XPLMDestroyInstance(XPLMInstanceRef inInstance) { (void)inInstance; }
void XPLMInstanceSetPosition(XPLMInstanceRef inInstance,
                             const XPLMDrawInfo_t* inPosition,
                             const float* inData) {
    (void)inInstance; (void)inPosition; (void)inData;
}

void XPLMWorldToLocal(double inLatitude, double inLongitude, double inAltitude,
                      double* outX, double* outY, double* outZ) {
    (void)inLatitude; (void)inLongitude;
    *outX = 0.0; *outY = inAltitude; *outZ = 0.0;
}
void XPLMLocalToWorld(double inX, double inY, double inZ,
                      double* outLatitude, double* outLongitude,
                      double* outAltitude) {
    (void)inX; (void)inZ;
    *outLatitude = 0.0; *outLongitude = 0.0; *outAltitude = inY;
}

XPLMProbeRef XPLMCreateProbe(XPLMProbeType inTerrainProbeType) {
    (void)inTerrainProbeType;
    return NULL;
}
void XPLMDestroyProbe(XPLMProbeRef inProbe) { (void)inProbe; }
XPLMProbeResult XPLMProbeTerrainXYZ(XPLMProbeRef inProbe, float inX,
                                    float inY, float inZ,
                                    XPLMProbeInfo_t* outInfo) {
    (void)inProbe; (void)inX; (void)inY; (void)inZ; (void)outInfo;
    return xplm_ProbeError;
}

void xconnect_logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/* ---------------- 造测试 CSL 树 ---------------- */

static void write_text(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    if (!f) {
        printf("FATAL: 无法写入 %s\n", path);
        return;
    }
    fputs(text, f);
    fclose(f);
}

static void touch(const char* path) { write_text(path, ""); }

static void make_dir(const char* path) {
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0777);
#endif
}

static void make_tree(const char* root) {
    char p[1024];
    make_dir(root);
    snprintf(g_sys, sizeof(g_sys), "%s/", root);

    snprintf(p, sizeof(p), "%s/Resources", root);        make_dir(p);
    snprintf(p, sizeof(p), "%s/Resources/plugins", root); make_dir(p);
    snprintf(p, sizeof(p), "%s/Resources/plugins/XConnect", root); make_dir(p);
    snprintf(p, sizeof(p), "%s/Resources/plugins/XConnect/CSL", root); make_dir(p);

    /* B738：一条“绑定尾号”的变体 + 通用 CCA 涂装 + ZZZ 白机 */
    snprintf(p, sizeof(p), "%s/Resources/plugins/XConnect/CSL/B738", root); make_dir(p);
    snprintf(p, sizeof(p), "%s/xsb_aircraft.txt", p);
    write_text(p,
        "OBJ8_AIRCRAFT B738_CCA_B1234\n"
        "OBJ8 SOLID YES B738\\B738_CCA_B-1234.obj\n"
        "VERT_OFFSET 0.0\n"
        "LIVERY B738 CCA B-1234\n"
        "OBJ8_AIRCRAFT B738_CCA\n"
        "OBJ8 SOLID YES B738\\B738_CCA.obj\n"
        "VERT_OFFSET 0.0\n"
        "AIRLINE B738 CCA\n"
        "OBJ8_AIRCRAFT B738_ZZZ\n"
        "OBJ8 SOLID YES B738\\B738_ZZZ.obj\n"
        "VERT_OFFSET 0.0\n"
        "AIRLINE B738 ZZZ\n");
    snprintf(p, sizeof(p), "%s/Resources/plugins/XConnect/CSL/B738/B738_CCA_B-1234.obj", root); touch(p);
    snprintf(p, sizeof(p), "%s/Resources/plugins/XConnect/CSL/B738/B738_CCA.obj", root); touch(p);
    snprintf(p, sizeof(p), "%s/Resources/plugins/XConnect/CSL/B738/B738_ZZZ.obj", root); touch(p);

    /* F900：FA8X 的别名目标（只有 ZZZ 白机，模拟“库里没有 DLH”的情况） */
    snprintf(p, sizeof(p), "%s/Resources/plugins/XConnect/CSL/F900", root); make_dir(p);
    snprintf(p, sizeof(p), "%s/xsb_aircraft.txt", p);
    write_text(p,
        "OBJ8_AIRCRAFT F900_ZZZ\n"
        "OBJ8 SOLID YES F900\\F900W_ZZZ.obj\n"
        "VERT_OFFSET 0.0\n"
        "AIRLINE F900 ZZZ\n");
    snprintf(p, sizeof(p), "%s/Resources/plugins/XConnect/CSL/F900/F900W_ZZZ.obj", root); touch(p);

    /* E190：E19L（E190LR）的别名目标，同样只放 ZZZ 白机 */
    snprintf(p, sizeof(p), "%s/Resources/plugins/XConnect/CSL/E190", root); make_dir(p);
    snprintf(p, sizeof(p), "%s/xsb_aircraft.txt", p);
    write_text(p,
        "OBJ8_AIRCRAFT E190_ZZZ\n"
        "OBJ8 SOLID YES E190\\E190_ZZZ.obj\n"
        "VERT_OFFSET 0.0\n"
        "AIRLINE E190 ZZZ\n");
    snprintf(p, sizeof(p), "%s/Resources/plugins/XConnect/CSL/E190/E190_ZZZ.obj", root); touch(p);

    /* ZZZZ：兜底通用白机 */
    snprintf(p, sizeof(p), "%s/Resources/plugins/XConnect/CSL/ZZZZ", root); make_dir(p);
    snprintf(p, sizeof(p), "%s/xsb_aircraft.txt", p);
    write_text(p,
        "OBJ8_AIRCRAFT ZZZZ_ZZZZ\n"
        "OBJ8 SOLID YES ZZZZ:ZZZZ.obj\n"
        "VERT_OFFSET 0.0\n");
    snprintf(p, sizeof(p), "%s/Resources/plugins/XConnect/CSL/ZZZZ/ZZZZ.obj", root); touch(p);
}

/* ---------------- 断言 ---------------- */

static void expect(const char* label, const char* icao, const char* airline,
                   const char* reg, const char* want) {
    CslNode node;
    memset(&node, 0, sizeof(node));
    csl_load(&node, icao, "", airline, reg, "TEST");
    {
        const int ok = node.variant_name[0] &&
                       strcmp(node.variant_name, want) == 0;
        if (ok) g_match_pass++; else g_match_fail++;
        printf("%s %-28s %-4s airline=%-4s reg=%-7s -> %s (want %s)\n",
               ok ? "PASS" : "FAIL", label, icao,
               (airline && *airline) ? airline : "-",
               (reg && *reg) ? reg : "-",
               node.variant_name[0] ? node.variant_name : "<none>", want);
    }
    csl_release_node(&node);
}

int main(int argc, char** argv) {
    const char* root = (argc > 1) ? argv[1] : "build/csltest";

    make_tree(root);
    csl_init();

    /* 1) 注册号一致 → 专属尾号模型 */
    expect("1 reg hit", "B738", "CCA", "B-1234", "B738_CCA_B1234");
    /* 1b) 尾号大小写/分隔符不同也应命中（B1234 / b-1234） */
    expect("1b reg normalized hit", "B738", "CCA", "b1234", "B738_CCA_B1234");
    /* 2) 无尾号 → 同航司通用涂装（不占用特定尾号的模型） */
    expect("2 airline generic", "B738", "CCA", "", "B738_CCA");
    /* 2b) 尾号不在库里 → 退回同航司 */
    expect("2b unknown reg -> airline", "B738", "CCA", "B-9999", "B738_CCA");
    /* 3) 航司不在库里 → 同机型随机真实航司（不优先白机） */
    expect("3 unknown airline -> random", "B738", "BAW", "", "B738_CCA");
    /* 4) 库里没有 FA8X → 别名 F900（白机） */
    expect("4 alias FA8X -> F900", "FA8X", "DLH", "", "F900_ZZZ");
    /* 4b) 别名 + 尾号也不命中 → 仍是 F900 */
    expect("4b alias + stray reg", "FA8X", "DLH", "N829PL", "F900_ZZZ");
    /* 4c) E19L（E190LR 写法）→ E190 包 */
    expect("4c alias E19L -> E190", "E19L", "DLH", "", "E190_ZZZ");
    /* 5) 完全未知机型 → ZZZZ */
    expect("5 unknown -> ZZZZ", "XXXX", "", "", "ZZZZ_ZZZZ");
    /* 5b) 空机型 → ZZZZ */
    expect("5b empty -> ZZZZ", "", "", "", "ZZZZ_ZZZZ");

    printf("\n%d passed, %d failed\n", g_match_pass, g_match_fail);
    return g_match_fail == 0 ? 0 : 1;
}
