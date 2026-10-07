/* XConnect — 远程玩家呼叫号标签（2D 顶层绘制）。
 *
 * 原理：把每个玩家当前渲染位置从局部坐标投影到屏幕像素，在
 * xplm_Phase_LastCockpit（最后 2D 机会，位于 3D 场景之上）用
 * XPLMDrawString 绘制带黑色描边的白色呼号文本。
 * 无需自定义 OpenGL 字体/矩阵，跨客户端可靠。
 */
#include "xconnect.h"
#include "players.h"

#include "XPLMCamera.h"
#include "XPLMDisplay.h"
#include "XPLMGraphics.h"

#define LABEL_MAX_DIST_M      40000.0  /* 超过此距离不画标签 */
#define LABEL_ALT_OFFSET_M    14.0     /* 文本略高于模型，避免遮挡机模 */

static XPLMDataRef s_fov_ref = NULL;

/* 局部坐标 3D 点 → 屏幕像素（屏幕坐标原点左上角，Y 向下）。
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

    /* 相机基向量 */
    const double ch = cos(h), sh = sin(h), cp = cos(p), sp = sin(p);
    const double cr = cos(r), sr = sin(r);
    fx = -sh * cp; fy = sp; fz = -ch * cp;       /* forward */
    rx = ch;      ry = 0;  rz = -sh;             /* right（偏航后） */
    ux = ry * fz - rz * fy;                      /* up = right × forward */
    uy = rz * fx - rx * fz;
    uz = rx * fy - ry * fx;
    /* 横滚：right/up 绕 forward 旋转 */
    r2x = rx * cr + ux * sr;  r2y = ry * cr + uy * sr;  r2z = rz * cr + uz * sr;
    u2x = -rx * sr + ux * cr; u2y = -ry * sr + uy * cr; u2z = -rz * sr + uz * cr;

    xc = dx * r2x + dy * r2y + dz * r2z;
    yc = dx * u2x + dy * u2y + dz * u2z;
    zc = dx * fx + dy * fy + dz * fz;            /* 前方为正 */
    if (zc < 1.0) return 0;

    nx = (xc / zc) / half_tan / aspect;
    ny = (yc / zc) / half_tan;
    XPLMGetScreenSize(&sw2, &sh2);
    if (sw2 < 16 || sh2 < 16) return 0;
    *out_x = (int)(sw2 * 0.5 * (1.0 + nx));
    *out_y = (int)(sh2 * 0.5 * (1.0 - ny));
    return 1;
}

/* 带描边的文本：四向 ±1px 黑色 + 本体白色 */
static void draw_label(int x, int y, const char* label) {
    float black[4] = { 0.0f, 0.0f, 0.0f, 0.9f };
    float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    int ox, oy;
    for (ox = -1; ox <= 1; ox++)
        for (oy = -1; oy <= 1; oy++) {
            if (!ox && !oy) continue;
            XPLMDrawString(black, x + ox, y + oy, label, NULL, xplmFont_Basic);
        }
    XPLMDrawString(white, x, y, label, NULL, xplmFont_Basic);
}

static int draw_callsign_labels(XPLMDrawingPhase phase, int isBefore,
                                void* refcon) {
    XPLMCameraPosition_t cam;
    double half_tan, aspect;
    int sw = 0, sh = 0;
    int cw = 0, ch = 0;
    int i;
    (void)phase; (void)isBefore; (void)refcon;

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
    }

    XPLMSetGraphicsState(0, 1, 0, 0, 1, 0, 0);
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

        snprintf(label, sizeof(label), "%s %dft",
                 p->callsign, (int)(p->cur_alt * 3.28084));
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
                                    xplm_Phase_LastCockpit, 0, NULL);
}

void labels_shutdown(void) {
    XPLMUnregisterDrawCallback(draw_callsign_labels,
                               xplm_Phase_LastCockpit, 0, NULL);
}