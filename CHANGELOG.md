# Changelog

格式参考 Keep a Changelog / SemVer。

## Unreleased

### Added

- Bridge Protocol v1：`connected`/`hello`/`hello_ack` 握手、能力位、版本提示。
- 客户端诊断导出：`python -m xfly_client.diagnostics`（日志自动脱敏）。
- `tests/` 单元测试：日志脱敏/轮转、VI/`^` 报文、桥握手、诊断、回放、mock FSD。
- 诊断 UI：设置页“导出诊断”；后端 `Api.diagnostics/export_diagnostics`。
- MConnect 外部机型档案：`MConnect.exe` 同目录 `profiles.json`，2s 热加载，
  外部档案优先于内置表（`MConnect/PROFILES.md`）。
- CI：Windows XConnect 构建 + CTest；Python unittest + selftest。
  （Linux/macOS 暂时搁置，不纳入 CI/发布。）
- 文档：FAQ、支持矩阵、桥协议、贡献指南、变更日志、MConnect 档案格式。

### Changed

- 远端全量 ACC 轮询：50km 内每 2s（全部请求），50km 外 30s（每轮最多 10 个）。
- `fsd.log` 10MB×3、`xconnect.log` 8MB×3 滚动；`#AP` 密码统一替换为 `***`。
- XConnect 降低高频日志（`send flight_data` 30s、`players updated` 仅变化时）。
- 桥接握手信息写入客户端状态，便于诊断版本/能力不一致。

### Fixed

- 服务器不支持 `^` 视觉位置时返回 `$ER 004 Syntax error` 刷屏：按最近发送的
  `^`/`#SB VI` 自动关闭对应报文（保留 `@`），并节流其它 004 日志。
- ACC 部件规范化：只发送服务器认识的 6 个灯光键（去掉 recognition/cabin/wing），
  并把 gear_down/on_ground/lights/engines.on 统一为 true/false。修复服务端
  `engines=null`、`gearDown=false`、灯光全空的问题。

- 远端呼号标签：改用 `xplm_Phase_Window`，修正 FOV dataref
  （`vertical_field_of_view_deg`）与航向/横滚基向量。
- 滑行卡顿：补 `#SB VI`(1s)、`^`(200ms)、roster 1s、重复位置帧去重。
- 远端 CSL：ACC 灯光/襟翼/起落架/扰流板；引擎风扇使用
  `sim/time/total_running_time_sec`；GEAR 跟随 `gear_down`。
- 涂装回退：不再固定 AAF；按呼号哈希随机同机型真实航司，且不优先白机。
- ACC 增加 `logo_on/recognition_on/cabin_on/wing_on`（X-Plane 无独立 dataref，
  按现有灯光派生；cabin 暂报 false）。

### 2026-11-26

#### Added

- **油门全链路**：ACC 新增 `throttle_ratio`（客户端 5% 量化防令牌桶耗尽）
  → XConnect 写 `libxplanemp/controls/thrust_ratio`、MConnect 写 AI 的
  `GENERAL ENG THROTTLE LEVER POSITION`（仅引擎运转时上报）。
- **音频面板互通**：MConnect 上行 `flight_id`/发射台/收听/音量/
  logo/wing/recognition/cabin 灯（独立 AUX 定义，失败不影响主遥测）；
  双平台新增发射台切换与按台收听反向控制。
- **XConnect 自绘呼号标签**（`labels.c`）：世界坐标投影、距离自适应高度、
  琥珀色无描边。
- **设置界面新增"MSFS 呼号铭牌"开关**（`msfs_toggle_labels`，三语文案，
  写明 2020 事件废弃/铭牌无法贴机身等限制）。
- MConnect 呼号标签：AI 的 ATC ID 写成远端呼号，配合内置交通铭牌显示。

#### Fixed

- **MSFS 2020 兼容三处**：字符串 SimVar units 必须空串（原 `"NULL"` 导致
  NAME_UNRECOGNIZED ×6、主遥测包体缺 352 字节）；`COMn_TRANSMIT_SELECT`
  已废弃 → `PILOT_TRANSMITTER_SET`；`TOGGLE_AIRCRAFT_LABELS` 已废弃
  （2020 无效，保留映射仅 2024 可用）。
- **ACC engines 丢失**：服务器只认 swift 的 `config.aircraft.engines`
  嵌套形状；发送侧统一包装、接收侧提升到顶层（`flapsDown` 别名一并补）。
- **XP12 灯光识别**：改读泛化灯亮度数组（权威状态），logo/wing 从 737 系
  槽位真实读取、turnoff 归并 taxi、landing 按实际亮度判定。
- **XP 部件动画失效**：注册 `libxplanemp/*` 自定义 dataref（必须先于任何
  `XPLMLoadObject`）；`sim/time/total_running_time_sec` 是 float，按实际
  类型读取（原 GetData 恒 0 导致风扇/strobe 不动）。

## 1.0.0

- 初始版本：XFlySim-Client + XConnect（X-Plane）+ MConnect（MSFS）。
