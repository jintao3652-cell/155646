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

- `fsd.log` 10MB×3、`xconnect.log` 8MB×3 滚动；`#AP` 密码统一替换为 `***`。
- XConnect 降低高频日志（`send flight_data` 30s、`players updated` 仅变化时）。
- 桥接握手信息写入客户端状态，便于诊断版本/能力不一致。

### Fixed

- 远端呼号标签：改用 `xplm_Phase_Window`，修正 FOV dataref
  （`vertical_field_of_view_deg`）与航向/横滚基向量。
- 滑行卡顿：补 `#SB VI`(1s)、`^`(200ms)、roster 1s、重复位置帧去重。
- 远端 CSL：ACC 灯光/襟翼/起落架/扰流板；引擎风扇使用
  `sim/time/total_running_time_sec`；GEAR 跟随 `gear_down`。
- 涂装回退：不再固定 AAF；按呼号哈希随机同机型真实航司，且不优先白机。
- ACC 增加 `logo_on/recognition_on/cabin_on/wing_on`（X-Plane 无独立 dataref，
  按现有灯光派生；cabin 暂报 false）。

## 1.0.0

- 初始版本：XFlySim-Client + XConnect（X-Plane）+ MConnect（MSFS）。
