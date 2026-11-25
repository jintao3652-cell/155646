# FAQ / 常见问题

## CSL 不显示 / 呼号标签不显示

1. 确认插件已加载：`Resources/plugins/XConnect/xconnect.log` 有
   `XConnect v... entry`、`step: labels ok`。
2. 标签需要外部视角也能触发：使用 `xplm_Phase_Window`；若日志没有
   `labels: draw called phase=50`，说明还是旧插件。
3. FOV 必须有效：日志 `vfov` 不能为 0（旧版误用了
   `field_of_view_vertical_deg`，那是多显示器俯仰偏移）。
4. CSL 路径：`Resources/plugins/XConnect/CSL/<ICAO>/xsb_aircraft.txt`；
   日志 `csl: <ICAO> -> pack ...` 能看出匹配层级。

## 滑行时其他飞机一卡一卡

- 客户端会发 `@`(5s) + `#SB VI`(1s) + `^`(200ms)；roster 1s 推给插件。
- 插件对“位置/姿态未变”的帧去重，避免“零速→跳变→零速”。
- 如果服务器不支持 VI/`^`，插件仍会按 `@` 插值；可在日志/配置里看能力。
- 仍卡时导出诊断：`python -m xfly_client.diagnostics --out diag.json`，
  附上在线机组呼号、时间、平台。

## 涂装不对 / 回退到白机或随机航司

- 先看日志 `csl: ... match=REG/AIRLINE/TYPE/RANDOM`。
- `RANDOM` = 本地 CSL 包没有该航司：按呼号哈希随机同机型真实航司；
  不同机组可能不同，同一呼号稳定。
- 如有白机基础变体且没有真实航司，才用白机。
- 要精确涂装：在 `CSL/<ICAO>` 增加对应航司贴图 + `xsb_aircraft.txt` 条目。

## 机型识别不到 / 机型变成白机

- 需要对方客户端响应 PIR/PI 或提供 ACC；服务器只从采集器 PIR 采纳站点记录。
- 本地有别名表（如 B38M→B738、FA8X→F900）；日志会写
  `has no CSL pack -> alias`。
- MSFS 2024 的模块化机型 TITLE 可能变化：MConnect 以实际 TITLE/ICAO
  重新匹配，必要时更新机型档案。

## 插件版本/协议不一致

- 日志 `[xconnect] XConnect v... 协议 v1`。
- 若提示版本不一致，升级客户端和插件到同一发布版本；协议主版本不同才可能不兼容。

## 端口 / 防火墙

- XConnect 默认 51001，MConnect 默认 51002（本机回环）。
- 被防火墙拦截时客户端一直“未发现模拟机”；允许对应 exe 的本机回环即可。

## 日志与隐私

- FSD 登录密码在日志中已自动替换为 `***`；`fsd.log` 10MB×3、
  `xconnect.log` 8MB×3 轮转。
- 反馈问题时请附诊断 JSON，不要直接发完整原始日志。

## Linux / macOS

- 当前版本**只支持 Windows**；Linux/macOS 暂时搁置，不提供安装包/CI 验证。
- 构建脚本与 CMake 分支保留，后续恢复时再验证 WebView/插件签名与 CSL 路径大小写。
