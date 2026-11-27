(function () {
    'use strict';

    /* xflysim-labels —— MSFS 自绘呼号标签（世界锚定 overlay）
     *
     * 背景：MSFS 20/24 对 SimConnect 注入的 AI 飞机永远不渲染原生 Traffic
     * Nameplates（渲染层硬限制；TOGGLE_AIRCRAFT_LABELS 事件已被官方废弃）。
     * 呼号显示只能自绘 —— 与 X-Plane 侧 XConnect 的 labels.c 自绘标签对齐。
     *
     * 本脚本由覆盖版 InGameMarkers.html 注入飞行标记层（全屏透明 frame，
     * 与原生铭牌同一渲染层级），职责：
     *   1. 每 500ms 从 MConnect（127.0.0.1:51002-4，同端口 HTTP 嗅探）拉取
     *      roster 快照（注入 AI 的位置 + 呼号 + labels_on 开关）；
     *   2. 驾驶舱视角共形投影：读用户机位置/姿态 SimVar，把 AI 经纬度投影
     *      到屏幕坐标，绘制琥珀色呼号（与 XP 侧样式一致：无描边）。
     *
     * v1 限制（真机校准后迭代）：
     *   - 仅驾驶舱视角（CAMERA STATE=2）绘制，外部视角隐藏；
     *   - 假设相机位于用户机基准点（忽略视点偏移，远距离误差可忽略）；
     *   - 驾驶舱自由视点（鼠标环视）不补偿，环视时标签会漂移；
     *   - FOV 用 COCKPIT_CAMERA_ZOOM 百分比近似换算，基准值可调。
     */

    if (window.__XFLY_LABELS_LOADED__) return;
    window.__XFLY_LABELS_LOADED__ = true;

    /* ---------------- 可调参数（真机校准用） ---------------- */

    const POLL_MS       = 500;    // roster 轮询间隔
    const MAX_DIST_KM   = 30;     // 距离剔除
    const MAX_LABELS    = 24;     // 同屏标签上限（取最近的）
    const STALE_MS      = 10000;  // 快照超时：MConnect 断开后标签保留时长
    const FOV_BASE_DEG  = 60;     // COCKPIT_CAMERA_ZOOM=ZOOM_REF 时假设的垂直 FOV
    const ZOOM_REF      = 50;     // zoom 百分比基准（MSFS 默认 50）
    const COLOR         = '#ffb000';
    const FONT          = '13px/16px Consolas, "Courier New", monospace';

    /* ---------------- 状态 ---------------- */

    let roster = null;            // 最近一次快照
    let rosterTs = 0;             // 快照到达时间
    let portIdx = 0;
    const PORTS = [51002, 51003, 51004];
    const labels = new Map();     // callsign -> {el, lastSeen}
    let simReady = false;

    function log(m) { try { console.log('[XFlyLabels] ' + m); } catch (e) { } }

    /* ---------------- DOM ---------------- */

    const host = document.createElement('div');
    host.id = 'xfly-labels-host';
    host.style.cssText =
        'position:fixed;left:0;top:0;width:100vw;height:100vh;' +
        'pointer-events:none;overflow:hidden;z-index:50;' +
        'font:' + FONT + ';';
    document.body.appendChild(host);

    function ensureLabel(cs) {
        let l = labels.get(cs);
        if (l) return l;
        const el = document.createElement('div');
        el.className = 'xfly-cs';
        el.style.cssText =
            'position:absolute;left:0;top:0;color:' + COLOR + ';' +
            'white-space:nowrap;will-change:transform;display:none;';
        host.appendChild(el);
        l = { el: el, lastSeen: 0 };
        labels.set(cs, l);
        return l;
    }

    function hideAll() {
        labels.forEach(function (l) { l.el.style.display = 'none'; });
    }

    /* ---------------- roster 轮询 ---------------- */

    async function poll() {
        for (let k = 0; k < PORTS.length; k++) {
            const port = PORTS[(portIdx + k) % PORTS.length];
            try {
                const r = await fetch('http://127.0.0.1:' + port + '/roster',
                                      { cache: 'no-store' });
                if (r.ok) {
                    portIdx = (portIdx + k) % PORTS.length;
                    roster = await r.json();
                    rosterTs = Date.now();
                    return;
                }
            } catch (e) { /* 换下一个端口 */ }
        }
    }
    setInterval(poll, POLL_MS);
    poll();

    /* ---------------- 投影（驾驶舱共形） ----------------
     * 输入度/米；返回屏幕像素或 null（不可见）。符号约定已用数值算例验证：
     * 北半球向北目标在正前 → 屏中；上仰 → 目标下移；右转 → 西侧目标左移。 */

    function project(lat1, lon1, alt1, pitch, bank, hdgTrue,
                     lat2, lon2, alt2, fovVDeg, w, h) {
        const R = 6371000, d2r = Math.PI / 180;
        const e = (lon2 - lon1) * Math.cos(lat1 * d2r) * R * d2r;
        const n = (lat2 - lat1) * R * d2r;
        const dn = -(alt2 - alt1);
        const th = pitch * d2r, ph = bank * d2r, ps = hdgTrue * d2r;
        // body FRD（前-右-下）基向量在 NED 中的展开
        const f0 = Math.cos(th) * Math.cos(ps),  f1 = Math.cos(th) * Math.sin(ps),  f2 = -Math.sin(th);
        const r0 = Math.sin(ph) * Math.sin(th) * Math.cos(ps) - Math.cos(ph) * Math.sin(ps);
        const r1 = Math.sin(ph) * Math.sin(th) * Math.sin(ps) + Math.cos(ph) * Math.cos(ps);
        const r2 = Math.sin(ph) * Math.cos(th);
        const d0 = Math.cos(ph) * Math.sin(th) * Math.cos(ps) + Math.sin(ph) * Math.sin(ps);
        const d1 = Math.cos(ph) * Math.sin(th) * Math.sin(ps) - Math.sin(ph) * Math.cos(ps);
        const d2 = Math.cos(ph) * Math.cos(th);
        const depth = f0 * n + f1 * e + f2 * dn;
        if (depth <= 1) return null;           // 身后/重合
        const xCam = r0 * n + r1 * e + r2 * dn;
        const yCam = -(d0 * n + d1 * e + d2 * dn);   // 屏幕上为正
        const f = 1 / Math.tan(fovVDeg * d2r / 2);
        const aspect = w / h;
        const ndx = (f / aspect) * xCam / depth;
        const ndy = f * yCam / depth;
        return {
            x: (0.5 + 0.5 * ndx) * w,
            y: (0.5 - 0.5 * ndy) * h,
            dist: depth
        };
    }

    function cockpitFov() {
        let zoom = ZOOM_REF;
        try { zoom = SimVar.GetSimVarValue('COCKPIT CAMERA ZOOM', 'percent'); } catch (e) { }
        if (!zoom || zoom < 1) zoom = ZOOM_REF;
        return 2 * Math.atan(Math.tan(FOV_BASE_DEG * Math.PI / 360) * ZOOM_REF / zoom)
               * 180 / Math.PI;
    }

    /* ---------------- 渲染循环 ---------------- */

    function render() {
        const now = Date.now();
        let state = 2;
        try { state = SimVar.GetSimVarValue('CAMERA STATE', 'number'); } catch (e) { }

        const fresh = roster && (now - rosterTs) < STALE_MS;
        const show = !!(fresh && roster.labels_on && state === 2);
        if (!show) { hideAll(); return; }

        let lat, lon, alt, pitch, bank, hdg, w = window.innerWidth, h = window.innerHeight;
        try {
            lat   = SimVar.GetSimVarValue('PLANE LATITUDE', 'degree latitude');
            lon   = SimVar.GetSimVarValue('PLANE LONGITUDE', 'degree longitude');
            alt   = SimVar.GetSimVarValue('PLANE ALTITUDE', 'meters');
            pitch = SimVar.GetSimVarValue('PLANE PITCH DEGREES', 'degrees');
            bank  = SimVar.GetSimVarValue('PLANE BANK DEGREES', 'degrees');
            hdg   = SimVar.GetSimVarValue('PLANE HEADING DEGREES TRUE', 'degrees');
        } catch (e) { hideAll(); return; }
        if (!isFinite(lat) || !isFinite(lon)) { hideAll(); return; }

        const fov = cockpitFov();
        const maxDist = MAX_DIST_KM * 1000;
        const players = roster.players || [];
        const hits = [];

        for (let i = 0; i < players.length && hits.length <= MAX_LABELS; i++) {
            const p = players[i];
            if (!p || !p.callsign || p.lat === undefined) continue;
            const pt = project(lat, lon, alt, pitch, bank, hdg,
                               p.lat, p.lon, p.alt, fov, w, h);
            if (!pt || pt.dist > maxDist) continue;
            hits.push({ cs: p.callsign, pt: pt });
        }
        if (hits.length > MAX_LABELS) {
            hits.sort(function (a, b) { return a.pt.dist - b.pt.dist; });
            hits.length = MAX_LABELS;
        }

        const wanted = {};
        for (let i = 0; i < hits.length; i++) {
            const hit = hits[i];
            wanted[hit.cs] = true;
            const l = ensureLabel(hit.cs);
            l.lastSeen = now;
            l.el.style.display = 'block';
            l.el.style.transform =
                'translate(' + (hit.pt.x | 0) + 'px,' + ((hit.pt.y - 16) | 0) + 'px)';
            l.el.textContent = hit.cs;
        }
        labels.forEach(function (l, cs) {
            if (!wanted[cs]) l.el.style.display = 'none';
            if (now - l.lastSeen > 60000 && l.el.parentNode) {
                l.el.parentNode.removeChild(l.el);
                labels.delete(cs);
            }
        });
    }

    /* ---------------- 启动 ---------------- */

    function start() {
        if (simReady) return;
        simReady = true;
        log('overlay started (poll ' + POLL_MS + 'ms, ports ' + PORTS.join('/') + ')');
        (function loop() { requestAnimationFrame(loop); try { render(); } catch (e) { } })();
    }

    if (typeof Include !== 'undefined' && Include.addScript) {
        Include.addScript('/JS/simvar.js', start);
    } else {
        // common.js 未就绪时兜底：DOM 完成后再试一次
        window.document.addEventListener('DOMContentLoaded', function () {
            if (typeof Include !== 'undefined' && Include.addScript) {
                Include.addScript('/JS/simvar.js', start);
            } else {
                log('WARN: Include 不可用，SimVar 姿态数据缺失（标签将不可见）');
            }
        });
    }
})();
