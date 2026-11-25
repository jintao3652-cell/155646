# Bridge Protocol v1（客户端 ↔ XConnect / MConnect）

XConnect 与 MConnect 对客户端暴露同一套 TCP 协议（换行分隔 JSON）。
v1 在原有报文上增加**握手与能力声明**，旧客户端/旧插件仍可工作。

## 1. 连接与握手

插件在 accept 后立即发送：

```json
{"type":"connected","protocol":1,"plugin":"XConnect",
 "version":"1.0.0","sim":"xplane",
 "capabilities":["csl","parts","controls","diagnostics",
                 "position_vi","position_visual","acc"]}
```

客户端收到后发送（插件也会回 `hello_ack`，字段与 `connected` 相同）：

```json
{"type":"hello","protocol":1,"client":"XFlySim-Client","version":"1.0.0"}
```

兼容规则：

- 旧插件只发 `{"type":"connected","version":1}`：客户端按 legacy 处理，
  `protocol=1`、无 capabilities。
- 未知字段忽略；未知 `type` 忽略。
- 协议主版本不一致时客户端只告警，不主动断开（便于灰度升级）。
- 行长度上限 256 KB（玩家列表可能很大）。

## 2. 消息（插件 → 客户端）

| type | 说明 |
|---|---|
| `connected` / `hello_ack` | 握手与能力 |
| `flight_data` | 本机飞行数据（含 `parts`） |
| `csl_status` | CSL 已加载/缺失列表 |
| `aircraft_caps` | 当前机型可操作项（MConnect 为主） |

## 3. 消息（客户端 → 插件）

| type | 说明 |
|---|---|
| `hello` | 客户端握手信息 |
| `Send:OPD:FSD9` | 在线玩家列表（`data[]`） |
| `Send:ATC` | 管制列表（MConnect 只记录数量） |
| `Send:RADIO` | COM 频率/主备/发话/全收 |
| `Send:XPDR` | 应答机码/模式/IDENT |
| `Send:INPUTEV` | 按名字触发机型 InputEvent |
| `Send:RPN` | 执行 RPN（MConnect + MobiFlight WASM） |
| `Send:SCAN` | 重新扫描 dataref（XConnect 诊断） |

## 4. 能力位

| capability | 含义 |
|---|---|
| `csl` | 能渲染在线飞机 |
| `parts` | 能上报/接收机模部件（ACC） |
| `controls` | 支持 RADIO/XPDR 写入 |
| `wasm` | 支持 RPN/WASM 通道 |
| `diagnostics` | 支持诊断请求（预留） |
| `position_vi` / `position_visual` | FSD 侧发送 VI / `^` 的能力声明 |

## 5. 错误与诊断

- 插件对无效字段**拒收并记日志**，不静默改值。
- 客户端诊断通过 `python -m xfly_client.diagnostics` 导出（日志自动脱敏）。
- `XConnect.log` 8 MB × 3 轮转；`fsd.log` 10 MB × 3 轮转。

## 6. 版本策略

- `protocol` 是桥协议主版本；不兼容变更才递增。
- 插件/客户端版本独立（SemVer），握手只用于提示与诊断。
