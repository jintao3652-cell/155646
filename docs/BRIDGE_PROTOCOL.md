# Bridge Protocol v1（客户端 ↔ XConnect / MConnect）

XConnect 与 MConnect 对客户端暴露同一套 TCP 协议（换行分隔 JSON）。
v1 在原有报文上增加**握手与能力声明**，旧客户端/旧插件仍可工作。

## 1. 连接与握手

插件在 accept 后立即发送。**两桥 capabilities 不同，互不 superset**：

```json
{"type":"connected","protocol":1,"plugin":"XConnect","version":"1.0.0",
 "sim":"xplane",
 "capabilities":["csl","parts","controls","diagnostics",
                 "position_vi","position_visual","acc"]}

{"type":"connected","protocol":1,"plugin":"MConnect","version":"1.0.0",
 "sim":"msfs",
 "capabilities":["csl","parts","controls","wasm","diagnostics"]}
```

客户端收到后发送：

```json
{"type":"hello","protocol":1,"client":"XFlySim-Client","version":"1.0.0"}
```

插件回 `hello_ack`：字段与 `connected` 同名同值，**但没有 `sim`**。
插件只按 `type` 分发 `hello`，不读取 `client` / `version` / `protocol`。

兼容规则：

- 旧插件只发 `{"type":"connected","version":1}`：客户端按 legacy 处理，
  `protocol=1`、无 capabilities。
- 未知字段忽略；未知 `type` 忽略。
- 协议主版本不一致时客户端只告警，不主动断开（便于灰度升级）。
- 插件侧单行上限 256 KB（玩家列表可能很大）；客户端读循环按 `\n` 无限累积，
  没有实际硬限制（`RX_LINE_LIMIT` 常量未使用）。

## 2. 消息（插件 → 客户端）

| type | 说明 |
|---|---|
| `connected` / `hello_ack` | 握手与能力 |
| `flight_data` | 本机飞行数据（含 `parts`） |
| `csl_status` | CSL 已加载/缺失列表（MC 另有 `sim`/`active`/`pending`） |
| `aircraft_caps` | 当前机型可操作项（**仅 MConnect 发送**，XC 不发） |

另有 HTTP 端点（非 TCP 桥）：MConnect `GET /roster` 返回
`roster_snapshot`，供 MSFS 呼号 overlay 轮询，字段见 `MConnect-PROTOCOL.md`。

## 3. 消息（客户端 → 插件）

| type | 说明 |
|---|---|
| `hello` | 客户端握手信息 |
| `Send:OPD:FSD9` | 在线玩家列表（`data[]`） |
| `Send:ATC` | 管制列表（MConnect 只记录数量） |
| `Send:RADIO` | COM 频率/主备/发话/全收 |
| `Send:XPDR` | 应答机码/模式/IDENT |
| `Send:TRAFFICLIB` | MSFS AI 模型库偏好 `auto`/`fsltl`/`aig`（仅 MConnect） |
| `Send:LABELS` | 翻转 MSFS 呼号 overlay 显隐（仅 MConnect，无字段） |
| `Send:INPUTEV` | 按名字触发机型 InputEvent |
| `Send:RPN` | 执行 RPN（MConnect + MobiFlight WASM） |
| `Send:SCAN` | 重新扫描 dataref（XConnect 诊断） |

## 4. 能力位

| capability | 含义 |
|---|---|
| `csl` | 能渲染在线飞机 |
| `parts` | 能上报/接收机模部件（ACC） |
| `controls` | 支持 RADIO/XPDR 写入 |
| `wasm` | 支持 RPN/WASM 通道（**仅 MC**） |
| `diagnostics` | 支持诊断请求 |
| `acc` | 支持 ACC 机模配置（**仅 XC**） |
| `position_vi` / `position_visual` | FSD 侧发送 VI / `^` 的能力声明（**仅 XC**） |

## 5. 错误与诊断

- 插件对无效字段**拒收并记日志**，不静默改值。
- 客户端诊断通过 `python -m xfly_client.diagnostics` 导出（日志自动脱敏）。
- 客户端日志写在独立 `logs/xflysim-YYYY-MM-DD.log`（北京时间日切，保留 7 天）；
  插件日志仍由插件自身轮转（`xconnect.log` / `mconnect.log`），不在客户端日志目录。
  全字段与单位差异见 `docs/MC-XC字段手册.md`。

## 6. 版本策略

- `protocol` 是桥协议主版本；不兼容变更才递增。
- 插件/客户端版本独立（SemVer），握手只用于提示与诊断。
