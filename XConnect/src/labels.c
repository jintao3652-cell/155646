/* XConnect — 远程玩家呼叫号标签（2D 顶层绘制）。
 *
 * 原理：把每个玩家当前渲染位置从局部坐标投影到屏幕像素，在
 * xplm_Phase_Window（2D 阶段，外部/座舱视角都会调用）用
 * XPLMDrawString 绘制带黑色描边的琥珀色呼号文本。
 *
 * 注意：不要用 xplm_Phase_LastCockpit——它只在 3D 座舱渲染时触发，
 * 外部视角下回调根本不会被调用（日志里完全没有 draw 记录）。
 */
#include "xconnect.h"
#include "players.h"

#include "XPLMCamera.h"
#include "XPLMDisplay.h"
#include "XPLMGraphics.h"

#define LABEL_MAX_DIST_M      40000.0  /* 超过此距离不画标签 */
#define LABEL_ALT_OFFSET_M    18.0     /* 文本略高于模型，避免遮挡机模 */
/* 2D 阶段：外部视角和座舱视角都会调用（LastCockpit 只在 3D 座舱时调用）。
 * inBefore=1 → 画在 X-Plane 浮动窗口之前、3D 场景之上。 */
#define LABEL_PHASE           xplm_Phase_Window
#define LABEL_BEFORE          1

static XPLMDataRef s_fov_ref = NULL;

/* 诊断：确认绘制回调在外部视角也会被调用，以及第一个标签的屏幕位置。
 * 每次插件启动只在日志里各记一次。 */
static int s_logged_call = 0;
static int s_logged_label = 0;

/* 局部坐标 3D 点 → 屏幕像素。
 * 注意：XPLMDrawString 的屏幕坐标原点是**左下角**、Y 向上（OpenGL 约定），
 * 不是左上角。之前按左上角算 `1 - ny`，标签会被上下镜像画到别处。
 * 相机基向量按 XPLM 约定：-Z 为 forward（0 航向朝北），顺时针偏航、
 * 抬头为正俯仰、绕视轴顺时针横滚；zoom 放大对应半视场角缩小。
 * 返回 1 表示点在相机前方。 */
static int project_local_to_screen(double px, double py, double pz,
                                   const XPLMCameraPosition_t* cam,
                                   double half_tan, double aspect,
                                   int* out_x, int* out_y) {
    const double d2r = 3.14159265358979323846 / 180.0;
    const double h = cam->heading * d2r;
    const double p = cam->pitch * d2r;
    const double r = cam->roll * d2r;
    const double dx = px - cam->x, dy = py - cam->y, dz = pz - cam->z;
    double fx, fy, fz, rx, ry, rz, ux, uy, uz;
    double r2x, r2y, r2z, u2x, u2y, u2z;
    double xc, yc, zc, nx, ny;
    int sw2, sh2;

    /* 相机基向量。X-Plane 本地坐标：+X 东、+Y 上、+Z 南（北 = -Z）。
     * 航向 0 = 北、90 = 东、顺时针增大；之前的 -sin(h) 把东西镜像了。 */
    const double ch = cos(h), sh = sin(h), cp = cos(p), sp = sin(p);
    const double cr = cos(r), sr = sin(r);
    fx = sh * cp;  fy = sp;  fz = -ch * cp;     /* forward */
    rx = ch;       ry = 0;   rz = sh;           /* right（偏航后） */
    ux = ry * fz - rz * fy;                     /* up = right × forward */
    uy = rz * fx - rx * fz;
    uz = rx * fy - ry * fx;
    /* 横滚：正横滚 = 右翼下沉（right 向量向下、up 向量向右） */
    r2x = rx * cr - ux * sr;  r2y = ry * cr - uy * sr;  r2z = rz * cr - uz * sr;
    u2x = rx * sr + ux * cr;  u2y = ry * sr + uy * cr;  u2z = rz * sr + uz * cr;

    xc = dx * r2x + dy * r2y + dz * r2z;
    yc = dx * u2x + dy * u2y + dz * u2z;
    zc = dx * fx + dy * fy + dz * fz;            /* 前方为正 */
    if (zc < 1.0) return 0;

    nx = (xc / zc) / half_tan / aspect;
    ny = (yc / zc) / half_tan;
    XPLMGetScreenSize(&sw2, &sh2);
    if (sw2 < 16 || sh2 < 16) return 0;
    *out_x = (int)(sw2 * 0.5 * (1.0 + nx));
    /* 左下角原点、Y 向上：ny>0（目标在相机上方）时 y 应更大 */
    *out_y = (int)(sh2 * 0.5 * (1.0 + ny));
    return 1;
}

/* 带描边的文本：周围 ±1px 黑色 + 本体琥珀色（与 swift 的呼号标签一致）。
 * 注意 XPLMDrawString 的颜色是 3 个 float（RGB），不是 RGBA。 */
static void draw_label(int x, int y, const char* label) {
    float black[3] = { 0.0f, 0.0f, 0.0f };
    float amber[3] = { 1.0f, 0.62f, 0.10f };
    int ox, oy;
    for (ox = -1; ox <= 1; ox++)
        for (oy = -1; oy <= 1; oy++) {
            if (!ox && !oy) continue;
            XPLMDrawString(black, x + ox, y + oy, label, NULL, xplmFont_Basic);
        }
    XPLMDrawString(amber, x, y, label, NULL, xplmFont_Basic);
}

static int draw_callsign_labels(XPLMDrawingPhase phase, int isBefore,
                                void* refcon) {
    XPLMCameraPosition_t cam;
    double half_tan, aspect;
    int sw = 0, sh = 0;
    int cw = 0, ch = 0;
    int i;
    (void)refcon;

    if (!s_fov_ref)
        s_fov_ref = XPLMFindDataRef("sim/graphics/view/field_of_view_vertical_deg");
    XPLMReadCameraPosition(&cam);
    XPLMGetScreenSize(&sw, &sh);
    if (sw < 16 || sh < 16) return 1;
    aspect = (double)sw / (double)sh;
    {
        const double fov_deg = s_fov_ref ? (double)XPLMGetDataf(s_fov_ref) : 70.0;
        const double zoom = cam.zoom > 0.0001 ? (double)cam.zoom : 1.0;
        half_tan = tan(fov_deg * 0.5 * 3.14159265358979323846 / 180.0) / zoom;
        if (!s_logged_call) {
            s_logged_call = 1;
            xconnect_logf("labels: draw called phase=%d before=%d screen=%dx%d "
                          "fov=%.1f zoom=%.2f",
                          (int)phase, isBefore, sw, sh, fov_deg, zoom);
        }
    }

    /* 画 2D 文字：关闭 2D 纹理 / 光照 / 深度测试，保留 alpha 混合。
     * （XPLMDrawString 会自己管理字体纹理；开纹理单元反而可能让文字不可见） */
    XPLMSetGraphicsState(0, 0, 0, 0, 1, 0, 0);
    XPLMGetFontDimensions(xplmFont_Basic, &cw, &ch, NULL);

    LOCK_ENTER(g_players_lock);
    for (i = 0; i < PLAYER_MAX; i++) {
        Player* p = &g_players[i];
        double wx, wy, wz, dx, dy, dz, dist2;
        char label[48];
        int sx, sy, len, w2;
        if (!p->active || p->fix_count == 0) continue;

        XPLMWorldToLocal(p->cur_lat, p->cur_lon,
                         p->cur_alt + LABEL_ALT_OFFSET_M, &wx, &wy, &wz);
        dx = wx - cam.x; dy = wy - cam.y; dz = wz - cam.z;
        dist2 = dx * dx + dy * dy + dz * dz;
        if (dist2 > LABEL_MAX_DIST_M * LABEL_MAX_DIST_M) continue;

        if (!project_local_to_screen(wx, wy, wz, &cam, half_tan, aspect,
                                     &sx, &sy))
            continue;
        if (sx < -300 || sx > sw + 300 || sy < -100 || sy > sh + 100) continue;

        /* 只显示呼号（与 swift 的标签一致）；高度不再挤在同一行 */
        snprintf(label, sizeof(label), "%s", p->callsign);
        if (!s_logged_label) {
            s_logged_label = 1;
            xconnect_logf("labels: first label %s at (%d,%d) dist=%.0fm",
                          p->callsign, sx, sy, sqrt(dist2));
        }
        len = (int)strlen(label);
        w2 = len * cw;
        if (w2 < 8) w2 = 8;
        draw_label(sx - w2 / 2, sy - ch / 2, label);
    }
    LOCK_LEAVE(g_players_lock);
    return 1;
}

int labels_init(void) {
    return XPLMRegisterDrawCallback(draw_callsign_labels,
                                    LABEL_PHASE, LABEL_BEFORE, NULL);
}

void labels_shutdown(void) {
    XPLMUnregisterDrawCallback(draw_callsign_labels,
                               LABEL_PHASE, LABEL_BEFORE, NULL);
}