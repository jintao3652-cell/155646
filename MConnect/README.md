# MConnect — MSFS 数据桥接与 AI 交通渲染

`MConnect` 是 `XConnect` 的 **Microsoft Flight Simulator 2020 / 2024** 版本：
同样在模拟器侧起 TCP 服务端（默认 51002），向 XFlySim-Client 推送本机飞行
数据，并把在线玩家列表注入成模拟器里的真实 AI 飞机；同时支持**反向控制**
（客户端设置应答机与 COM 频率）。

**客户端零改动**：报文格式与 XConnect 完全一致（`flight_data` /
`csl_status` / `Send:OPD:FSD9`），XFlySim-Client 换一个 `xconnect_port`
指过来即可。

## 与 XConnect 的对照

| 维度 | XConnect（X-Plane 11/12） | MConnect（MSFS 2020/2024） |
|---|---|---|
| 形态 | 进程内插件 `.xpl` | 外部 exe（SimConnect 官方推荐 out-of-process） |
| 连接 | TCP 服务端 :51001 | TCP 服务端 :51002（避开 X-Plane） |
| 本机数据 | XPLM dataref | SimConnect `AddToDataDefinition` + `RequestDataOnSimObject` |
| 显示他机 | `XPLMInstance` 渲染 CSL 模型（无 AI 逻辑） | `AICreateNonATCAircraft` 注入真实 AI 机 |
| 模型来源 | CSL 包 + `xsb_aircraft.txt` | `aircraft.cfg` / `livery.cfg` 里的容器标题 + 涂装名 |
| 位置驱动 | 写实例变换矩阵 | `SetDataOnSimObject` 写位置+速度 |
| 反向控制 | 直写 dataref | SimConnect **事件**（只读 SimVar 无法直接写） |
| 版本差异处理 | 编译期 `#if kXPLM_Version` | **运行时**探测 `SimConnect.dll` 导出 |

## 2020 / 2024 双版本兼容

不靠编译期宏区分，靠**运行时能力探测**，因此同一个 exe 两边都能跑：

`SimConnect.dll` 用 `LoadLibrary` 动态装载，函数用 `GetProcAddress` 取。
2024 SU1 起新增的两个导出决定了走哪条路径：

| 导出 | 2020 | 2024 SU1+ | 用途 |
|---|---|---|---|
| `SimConnect_AICreateNonATCAircraft_EX1` | 无 | 有 | 多一个 `szLivery` 参数，能创建**模块化机型**（2024 官方机全是模块化） |
| `SimConnect_EnumerateSimObjectsAndLiveries` | 无 | 有 | 运行时枚举「容器标题 + 涂装名」组合 |

创建 AI 飞机的降级链（`players.c::ai_create`）：

```
_EX1(title, livery)  →  _EX1(title, "")  →  AICreateNonATCAircraft(title)
   ↑ 2024 模块化+涂装      ↑ 涂装名不被接受      ↑ 2020 legacy / 兜底
```

机型匹配时，模块化条目在 2020 环境下会被**打 -100 分**直接排除
（2020 创建不出来），所以同一条目表在两边都给出可创建的结果。

## 目录结构

```
MConnect/
  src/
    mconnect.c   入口、SimConnect 派发循环、网络线程、客户端协议解析
    sc.h/sc.c    SimConnect ABI 声明 + 动态装载 + 版本/能力探测
    datadefs.c   本机飞行数据定义、读取、flight_data 构建、AI 姿态换算
    catalog.c    机型/涂装目录扫描（aircraft.cfg/livery.cfg）+ 四级匹配
    players.c    玩家表、时间插值、AI 对象注入与位置驱动
    control.c    反向控制：应答机/COM 频率（事件映射 + 命令队列）
    inputev.c    Input Events：深度机模自定义输入事件（按名字触发）
    wasmbridge.c WASM 桥：ClientData + 任意 RPN（MobiFlight 协议）
    pmdgbridge.c PMDG SDK 桥：官方二进制 ClientData（应答机状态唯一来源）
    aircraftcaps.c 机型能力探测：按实际机型给出可选项
    aircraftdb.c 机型档案：各机模族的实测写法（FBW/Fenix/PMDG/IniBuilds…）
    ini.c        aircraft.cfg / livery.cfg / UserCfg.opt 轻量 INI 解析
    net.c        TCP（WinSock2）
    json.c       JSON 解析
  test/
    catalog_test.c    机型匹配单元测试（不需要模拟器）
    control_test.c    反向控制单元测试（事件调用用桩替换）
    inputev_test.c    Input Events 单元测试（结构布局 + 分页 + 查找）
    radio_test.c      无线电报文测试（频率换算 + 发射台推导 + JSON 字段）
    wasmbridge_test.c WASM 桥测试（命令格式 + 队列 + 状态机 + LVar）
    aircraftcaps_test.c 机型能力测试（语义分类 + 优先级 + 报文 + 变更检测）
    aircraftdb_test.c   机型档案测试（识别 + 模板展开 + 各机模实测写法）
  CMakeLists.txt
```

## 编译

无外部依赖（**不需要安装 MSFS SDK**——`SimConnect.h` 的 ABI 已在 `sc.h`
里按官方定义复刻，运行时才去找 `SimConnect.dll`）。

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
# 产物: build/MConnect.exe
```

静态 CRT（`/MT`），单个 exe 拷到目标机即可运行，无需 VC++ 运行库。

### CI

`.github/workflows/build-mconnect.yml`：Windows 上编译 + 跑单元测试 + 上传 artifact。

## 使用

1. 先启动 MSFS 2020/2024，进入主菜单或载入飞行
2. **XFlySim-Client 会自动拉起 `MConnect.exe`**（无窗口模式，默认开启）
   —— 不需要手动运行。X-Plane 用户可在配置里关掉 `auto_start_mconnect`
3. 把 `xconnect_port` 指向 MConnect 实际监听的端口（51002/51003/51004，
   看 `mconnect.log` 首行）

### 两个可执行文件

| 文件 | 子系统 | 用途 |
|---|---|---|
| `MConnect.exe` | **GUI（无控制台窗口）** | 日常运行，随客户端自动启动；日志写入同目录 `mconnect.log` |
| `MConnectConsole.exe` | Console（有窗口） | 诊断专用：`--help` / `--scan-only` / `--dump-*` / `--parts-probe` 需看到 stdout 时用 |

无窗口版本带 `--help` 等诊断参数时会在 `mconnect.log` 里提示改用 Console 版。

```
MConnectConsole.exe [选项]

  --port <n>         **当前无效**：解析后即丢弃；端口固定 51002，占用退 51003/51004
  --labels           启动时把呼号 overlay 开关设为 1（toggle 语义，非内置铭牌）
  --scan <dir>       只用指定目录作为机型目录
  --scan-only        只扫描机型目录并打印统计，不连模拟器
  --dump-input       把当前机模的 Input Events 表写入日志（调试深度机模）
  --dump-wasm        检查 WASM 桥（MobiFlight 模块）是否可用
  --dump-profiles    打印机型档案表（各机模实测写法与已知限制）
  --parts-probe      真机探针：注入临时 AI 飞机，检测部件 SimVar/灯光事件是否可写
  --traffic-lib <s>  AI 模型库偏好：auto（默认）/ fsltl / aig
  --simconnect <p>   指定 SimConnect.dll 路径或所在目录
  --verbose, -v      详细日志
```

`--scan-only` 是最常用的排查手段：装机后直接跑一次，能看到抓了多少机型、
常见机型分别匹配到什么容器标题与涂装。样例输出：

```
机型目录条目数: 7824（内置兜底表 41 条）

抽样匹配（机型 / 航司 / 尾号 → 容器标题 + 涂装）：
  B738   -> level=2  "AIGAIM_Air China Boeing 737-800"  livery="CCA-Air China"
  A20N   -> level=2  "AIGAIM_Air China Airbus A320-200neo - CFM"  livery="CCA-Air China_CFM"
  E190   -> level=3  "FSLTL_E190_ZZZZ"  livery=""
  XXXX   -> level=4  "Generic Airliner Twin Engines Asobo 00"  livery=""
```

日志还会按来源分别统计，一眼确认 FSLTL/AIG 两套库是否都扫到了：

```
catalog: AI 库条目 FSLTL=<n> AIG=<n> 其它=<n>（当前偏好=AUTO）
```

## 机型目录

三处来源合并去重。每条条目会按 Community 包名标记来源
（`fsltl-*` → FSLTL，`aig-*` / `AIG_AIManager` → AIG，其它 → OTHER），
供设置里的「MSFS 他机模型库」优先匹配使用：所选库先跑完四级匹配，
没有合适机型/涂装时才回退到另一套库与内置表。两套库可以同时安装。

三处来源合并去重：

1. **包扫描**：`Community` 与 `Official` 下所有 `aircraft.cfg`
   （路径从 `UserCfg.opt` 的 `InstalledPackagesPath` 得到，覆盖 Store/Steam 两版）
   - `[GENERAL] icao_type_designator` → ICAO 机型码（最准）
   - `[FLTSIM.N] title` → 容器标题（`AICreate` 的第 1 个参数）
   - `[FLTSIM.N] icao_airline` / `atc_airline` → 航司码
   - `[FLTSIM.N] atc_id` → 绑定尾号
   - `[FLTSIM.N] texture` → 涂装标识（legacy 包区分多涂装的唯一手段）
2. **2024 运行时枚举**：`EnumerateSimObjectsAndLiveries` 拿「标题 + 涂装名」
   （模块化机型的涂装只有模拟器自己知道，文件系统里信息不全）
3. **内置兜底表**：41 条常见 Asobo 机型，未装第三方机型包时也能显示他机

### 四级匹配（与 XConnect 规则一致，对象换成「容器标题 + 涂装」）

| 级别 | 条件 | 说明 |
|---|---|---|
| 1 `REG` | 尾号一致 | `atc_id` 精确相同，或涂装名/标题含同一尾号（忽略大小写与 `-`） |
| 2 `AIRLINE` | 同航司同机型 | 航司码精确，再「同航司带后缀」；多套涂装时优先**不绑尾号**的那套 |
| 3 `TYPE` | 同机型任意涂装 | 白机（`ZZZZ`/`ZZZ`）→ `ANY`/`GEN` → 无涂装 → 首个；**不给未知航司挂别家涂装** |
| 4 `FALLBACK` | 同族机型 | 186 条别名表（`B735`→`B738`、`A359`→`A339`、`E75L`→`E175` …）→ 通用双发客机 |

## 反向控制（客户端 → 模拟器）

客户端可以设置应答机与 COM 频率。这与 XConnect 的能力对齐，但底层机制完全不同：

**MSFS 的 `COM ACTIVE FREQUENCY`、`TRANSPONDER CODE` 都是只读 SimVar**，直接
`SetDataOnSimObject` 无效，必须用 SimConnect **事件**：

| 目标 | 事件 | 参数 |
|---|---|---|
| COM1 主频 | `COM_RADIO_SET_HZ` | Hz |
| COM1 备频 | `COM_STBY_RADIO_SET_HZ` | Hz |
| COM2 / COM3 | `COM{2,3}_RADIO_SET_HZ` / `_STBY_` | Hz |
| 主备互换 | `COM{1,2,3}_RADIO_SWAP` | — |
| 应答机码 | `XPNDR_SET` | **BCD**（`1234` → `0x1234`） |
| 应答机模式 | 可写 SimVar `TRANSPONDER STATE` | Enum 0..5 |
| IDENT | `XPNDR_IDENT_SET` | 1/0 |

协议（与 XConnect 客户端接口一致）：

```json
{"type":"Send:RADIO","com":1,"active":true,"freq":122800}   // 122.800 MHz
{"type":"Send:RADIO","com":1,"swap":true}
{"type":"Send:XPDR","code":"1234"}
{"type":"Send:XPDR","state":4}                              // 4 = ALT
{"type":"Send:XPDR","ident":true}
```

三个设计要点：

1. **命令队列**：网络线程只入队（64 条），主线程出队执行。原因同 AI 驱动
   —— SimConnect 非线程安全，`TransmitClientEvent` 不能在网络线程调
2. **`_HZ` 而非 BCD**：`COM_RADIO_SET`（不带 `_HZ`）收 BCD16，8.33kHz 间隔的
   第三位小数在 BCD16 里表达不全；统一用 `_HZ` 收十进制 Hz。应答机码例外，
   `XPNDR_SET` 只收 BCD，需自己按位编码
3. **不保证生效**：机模可能自己覆盖（CDU 逻辑），也可能不实现该事件。
   客户端必须以回读的 `flight_data` 为准刷新界面，不能假定写入成功

`Send:RADIO` 的 `freq` 单位是 **MHz×1000**，与 `flight_data.com1_freq` 完全
同单位，客户端可以直接回填，不需要换算。

读侧同时补全了发射选择（对齐 XVoice 的 `COM ACTIVE FREQUENCY` /
`COM TRANSMIT` / `PILOT TRANSMITTER TYPE` 组合）：

| 字段 | 含义 |
|---|---|
| `com_transmit` | `[com1,com2,com3]` 各台是否处于发射状态 |
| `pilot_xmit` | 飞行员选定发射台 `0=COM1 1=COM2 2=COM3`，`-1`=未选中/未知 |
| `copilot_xmit` | 同上（副驾驶） |
| `com_receive_all` | 全收（所有 COM 同时接收） |
| `audio_panel_on` | 音频面板供电（断电时无线电实际不可用） |

**`pilot_xmit` 必须交叉验证**：`PILOT TRANSMITTER TYPE` 在部分机型上没实现
（恒为 0），直接信它会把"实际在 COM3 讲话"误报成 COM1——语音通讯里这是严重
错误（管制听到的频率和客户端显示的对不上）。因此同时看 `COM TRANSMIT:n`：

| COM TRANSMIT | 枚举 | 结果 |
|---|---|---|
| 只有一台为真 | 任意 | **以在发的那台为准**（最可信） |
| 多台为真 | 有效 | 取枚举指定的那台 |
| 都没在发 | 有效 | 报枚举值（面板选了台但没按 PTT） |
| 都没在发 | 无效 | -1 |

两平台字段语义统一：`pilot_xmit` 都是 `0=COM1 1=COM2 2=COM3`、`-1` 未选中/未知。

### 音频面板反向控制（对齐 XConnect 能力）

| 协议字段 | MSFS 2020/2024 写法 | 说明 |
|---|---|---|
| `tx` / `xmit`（发射台） | `PILOT_TRANSMITTER_SET`，参数 `com-1` | **MSFS 2020 已废弃 `COMn_TRANSMIT_SELECT` 系列**（发送后 EXCEPTION NAME_UNRECOGNIZED），统一改用本事件；0=COM1 1=COM2 2=COM3 |
| `flip`（主备互换） | `COM{n}_RADIO_SWAP` | 同 swap 别名 |
| `monitor`（按台收听） | `COM{n}_RECEIVE_SELECT` toggle | 按 `COM RECEIVE:n` 当前读数精准 toggle 不符的台 |
| `recv_all`（全收） | 显式忽略 + 日志 | MSFS 无"全收"事件（读侧有 `COM RECEIVE ALL`，无对应写入） |
| `power`（单台电源） | 显式忽略 + 日志 | MSFS 无单台 COM 电源 SimVar |

### 呼号标签（自绘 overlay）

MConnect 在每个 AI 创建成功后把 `ATC ID` 写成远端呼号（DEF_AI_ATCID，
String32）——内置 ATC 语音会用它称呼远端航班。

**为什么原生铭牌不可行（2026-11 研究结论，模拟器硬限制）**：

- MSFS 20/24 只对**模拟器自建**的 AI/多人飞机渲染 Traffic Nameplates；
  SimConnect 注入的 AI（`AICreateNonATCAircraft` / Parked / Enroute 均同）
  一律没有铭牌（官方 DevSupport 确认 + FSTraffic/AIG/FSLTL/vPilot 全线一致）
- `TOGGLE_AIRCRAFT_LABELS` 事件已被官方废弃，任何版本均无效果、无替代
  （2024 的 ALT+2 快捷键无对应 SimConnect 事件）

**现行方案：xflysim-labels 覆盖包（自绘，与 X-Plane 侧对齐）**：

- Community 包 `xflysim-labels`（随发布包附带，解压进 Community 即可）：
  JS overlay 注入飞行标记层（覆盖版 `InGameMarkers.html`），每 500ms 轮询
  MConnect 的 **HTTP roster 快照**（同端口 51002-4，协议嗅探，不影响桥接），
  读用户机位置/姿态 SimVar 把注入 AI 投影为世界锚定的琥珀色呼号标签
- 显隐控制：`--labels`（启动默认）或客户端设置开关（保存时发 `Send:LABELS`
  运行时翻转快照里的 `labels_on`，overlay 立即生效，无需重启）
- v1 限制：仅驾驶舱视角可见；驾驶舱自由环视不补偿（标签会漂移）；
  FOV 由 `COCKPIT_CAMERA_ZOOM` 近似换算（`xfly_labels.js` 顶部常量可调）；
  MSFS 大版本更新后如原生 `InGameMarkers.html` 变更需重新合并覆盖文件

### 深度机模：三级写入通道

上面这些**标准事件只能驱动默认机**。Fenix / PMDG / Inibuilds 这类深度机模把
座舱逻辑做成了自定义 InputEvent / LVar，标准事件对它们完全无效（`HRESULT`
仍是 `S_OK`，但机模直接忽略）。

MConnect 因此做了**三级回退**，越往下能力越强、依赖越多：

| 级 | 通道 | 能力 | 依赖 |
|---|---|---|---|
| ⓿ | **机型档案** | 各机模族的**实测精准写法**（FBW 写 LVar、PMDG 转旋钮） | 无（表内置） |
| ① | 标准 SimEvent | 默认机全功能 | 无 |
| ② | Input Events | 按名字触发机模预定义按钮 + 按机型给选项 | 无（纯 SimConnect） |
| ③ | **WASM 桥**（ClientData） | **执行任意 RPN**：LVar 读写、B 事件、多参数 K 事件 | 需装 WASM 模块 |

#### ⓿ 机型档案（aircraftdb.c）— 这层才是"全都要适配"的关键

从 MobiFlight HubHop 的 **27382 条社区预置**里核对出来的事实：**各机模族用的
机制完全不同，根本不存在通用写法**。

| 机模 | 应答机模式 | COM 频率 |
|---|---|---|
| FBW A32NX/A380X | `2 (>L:A32NX_TRANSPONDER_MODE)` | `122800000 (>A:COM ACTIVE FREQUENCY:1, Hz)` **可写** |
| Fenix A320 | `2 (>L:S_XPDR_OPERATION)` + `2 (>L:S_XPDR_MODE)`（两套） | 标准事件 |
| PMDG 737/777 | `80008 (>K:ROTOR_BRAKE)` **魔法数字** | `72607 (>K:ROTOR_BRAKE)` **只能逐档** |
| iniBuilds A310 | `1 (>L:A310_tcas_mode_pedestal)` | `(L:INI_CPT_VHF_TRANSFER_SWITCH)` 翻转 |
| iniBuilds A300 | `1 (>L:INI_tcas_mode_pedestal)` | 同上 |
| iniBuilds A220 | `0 (>L:A22X Transponder Mode)` + 标准 `XPNDR_SET` | 标准 `COM*_RADIO_SET_HZ` |
| iniBuilds A350 | `1 (>L:INI_tcas_mode_pedestal)` 0=STBY 1=XPNDR 2=TA/RA 3=TA ONLY | `(B:AIRLINER_RMP_*)` B 事件 |
| iniBuilds A340 | 同 A350（同族类推） | 同 A350 |
| TFDi MD-11 | `69855 (>L:CEVENT)` | `69632 (>L:CEVENT)` 逐档 |
| Leonardo Maddog | — | `8192 11 + (>L:radio_event)` 位运算 |
| iFly 737 MAX | — | `(L:VC_Navigation_trigger_VAL)` 数字编码 |
| **FSL A32X** | `EVENT_ID+动作 (>L:CD_InfKnobID)` → `(>K:ROTOR_BRAKE)` | 标准事件 |
| **C919** | 未开放（只有 `HW_PUB.*` 具名事件，无应答机） | 标准事件 |
| Just Flight | `1 (>L:PED_AFT_XPDR_MODE)` | 标准事件 |
| Aerosoft CRJ | `1 (>L:ASCRJ_XPDR_ATC_SEL)` 按钮 | 标准事件 |
| Asobo 默认机 | 标准事件 | 标准事件 |

**覆盖 19 个档案**：`--dump-profiles` 可查看全部（含每条的已知限制）。

靠"猜名字"对 PMDG 必然失败——它的按钮叫 `switch_800_73X`。所以按供应商/机型
识别后直接给出**已核实的写法**：

```c
{
    "fbw-a32nx", "A32NX|A380X|flybywire|fly by wire",
    NULL,                                  /* 码：走标准 XPNDR_SET */
    { NULL,                                /* OFF：FBW 无此档 */
      "0 (>L:A32NX_TRANSPONDER_MODE, Number)",   /* STBY */
      "2 (>L:A32NX_TRANSPONDER_MODE, Number)",   /* ON  */
      "1 (>L:A32NX_TRANSPONDER_MODE, Number)",   /* ALT → AUTO */
      NULL, NULL },
    { "%ld (>A:COM ACTIVE FREQUENCY:1, Hz)", ... },
    "1 (>K:COM%d_RADIO_SWAP)",
    NULL,
    CAP_XPDR_MODE | CAP_COM_SET | CAP_COM_SWAP,
    "应答机只有 STBY/AUTO/ON；ALT 在 FBW 语境即 AUTO"
}
```

**已知的硬限制**（档案里如实标注，不假装支持）：

| 机模 | 限制 |
|---|---|
| **PMDG 频率** | 频率仍只能用旋钮 `ROTOR_BRAKE`（72607/72608 逐档）。应答机模式/IDENT 已通过 SDK ClientData 解决 |
| **Fenix** | 应答机有两套变量（OPERATION 开关 / MODE 的 TA-TARA），没有独立 TEST |
| **TFDi MD-11** | `(L:CEVENT)` 单一通道传事件号，频率只能 MHz/kHz 逐档增减 |
| **Leonardo Maddog** | 位运算 `8192+N`/`16384+N`，只能逐档转 |
| **iFly 737 MAX** | 数字编码码表只确认了 IDENT=411，应答机码位需实机实测 |
| **IniBuilds A340** | 与 A350 同族类推，本机未装 A340 实机未复核 |
| **FSL A32X** | 应答机用 `EVENT_ID+动作` 送 `ROTOR_BRAKE`。实测 MODE 旋钮 `EVENT_ID=78347`（+1 逆时针 / +2 顺时针），数字键 78292~78332。**但 MODE 档位数未知**（ANIM_LENGTH=20 不代表档数），无法算"到目标要转几格"，故只做**相对转动**（按当前值决定方向，每次一格） |
| **C919** | 用具名客户端事件 `HW_PUB.*`（编号从 72000 起），但公开清单里**没有应答机/无线电事件** —— 面板未对外开放 |

数据来源保持可核对：档案里每条写法都注明来自哪条 HubHop 预置的 label
或哪份官方文档，可直接复核。

#### 四种控制机制（MSFS 机模没有统一接口）

| 机制 | 机模 | 写法 |
|---|---|---|
| 标准 SimEvent | Asobo 默认机、多数兼容机模 | `(>K:XPNDR_SET)` |
| 自定义 LVar | FBW、Fenix、IniBuilds 全系 | `2 (>L:A32NX_TRANSPONDER_MODE)` |
| **ROTOR_BRAKE 魔法数字** | PMDG 737/777、**FSL A32X** | `80008 (>K:ROTOR_BRAKE)` / `ID*4+动作 (>L:ATCXPDRBtnID)` |
| **数字编码通道** | TFDi MD-11、iFly 737 MAX | `69855 (>L:CEVENT)` / `411 (>L:VC_Navigation_trigger_VAL)` |
| **具名客户端事件** | C919 | `"HW_PUB.FMCP_AT_BTN"`（编号 72000 起） |
| **官方 SDK 二进制** | PMDG 737/777 | ClientData（见下） |

A220 的来源是官方变量表（iniBuilds / SynapticSim）：
`docs.synapticsim.com/pilots/simvars` 与 `/pilots/inputs`。

**变数据从机型文件直接挖，比 HubHop 权威**：

```bash
cd "L:\MSFS\Community\<机型目录>"
# IniBuilds 全系
grep -rhoE "\(L:(INI_|A3[0-9]+_)[A-Za-z0-9_]+" ModelBehaviorDefs | sort -u
# FSL / 其他厂商看 ROTOR_BRAKE 与自己的按钮ID变量
grep -rn "ROTOR_BRAKE" ModelBehaviorDefs | head
grep -rn "KNOB_VAR\|NUM_STATES" ModelBehaviorDefs | head
```

#### 应答机状态：读和写必须走同一条路

这是最容易出隐蔽 bug 的地方。**标准 SimVar `TRANSPONDER STATE:1` 只有默认机会维护**：

| 机模 | 状态实际存在哪 |
|---|---|
| Asobo 默认机 | 模拟器维护的 `TRANSPONDER STATE` SimVar ✅ 直接读 |
| FBW A32NX | `(L:A32NX_TRANSPONDER_MODE)` ❌ SimVar 恒为 0 |
| Fenix A320 | `(L:S_XPDR_OPERATION)` ❌ |
| iniBuilds A310 | `(L:A310_tcas_mode_pedestal)` ❌ |
| iniBuilds A220 | `(L:A22X Transponder Mode)` ❌ |
| iniBuilds A350 | `(L:INI_tcas_mode_pedestal)` ❌ |
| **PMDG 737/777** | **官方 SDK 二进制 ClientData（唯一途径）** ⚠️ |

如果只读 SimVar 会怎样：客户端发"设 ON"**其实生效了**（面板灯亮），但读回来
仍是 `0 = Off`，界面显示与实际相反。这比"写不进去"更糟——用户会以为失败而
反复点。

因此档案里每个机型带三样东西，读写共用同一张映射表：

```c
/* 写：规范枚举下标 → 该机模自己的 RPN 模板 */
const char* xpdr_mode_rpn[6];
/* 读：calculator 表达式，经 WASM 桥周期读取 */
const char* xpdr_mode_read;
/* 映射：规范枚举 ↔ 该机模原生档位值（-1 = 该档不支持） */
int xpdr_mode_native[6];
```

举例（A220，官方文档值）：

```c
xpdr_mode_rpn[XPDR_CANON_STBY] = "1 (>L:A22X Transponder Mode, Number)";
xpdr_mode_read                 = "(L:A22X Transponder Mode, Number)";
xpdr_mode_native               = { 5, 1, 0, 4, -1, -1 };
/*                                 OFF STBY ON ALT TEST GND
 *       原生含义：0=Auto 1=Stby 2=TA/RA 4=AltOn 5=AltOff */
```

读写必须**用同一张 `xpdr_mode_native`**，否则会出现"写 ON 有效、读回来是
别的档"。测试里专门有 13 组往返断言盯着这一点
（`[13] 应答机模式读写一致性`）。

未装 WASM 模块时无法读 LVar → 退回标准 SimVar（对默认机正确，对深度机模
会显示过期值，属已知限制）。

#### PMDG：走官方 SDK ClientData（pmdgbridge.c）

PMDG 是唯一"**既没有可读 LVar、又不维护标准 SimVar**"的机模，HubHop 里只能写
（`80007/80008 (>K:ROTOR_BRAKE)`）读不到。唯一正解是官方 SDK 的二进制通道。

**737 和 777 的协议完全不同**（别以为只改通道名就行）：

| | 737 NG3 | 777X |
|---|---|---|
| 数据通道 | `PMDG_NG3_Data` | `PMDG_777X_Data` |
| 数据 ID / DEF | `0x4E473331` / `...32` | `0x504D4447` / `...48` |
| 控制通道 | `PMDG_NG3_Control` | `PMDG_777X_Control` |
| 控制 ID / DEF | `0x4E473333` / `...34` | `0x504D4449` / `...4A` |
| `sizeof(Data)` | **916** | **684** |
| 模式字段偏移 | 612 | **449** |
| `EVT_TCAS_MODE` | 69632+**800** = 70432 | 69632+**749** = 70381 |
| `EVT_TCAS_IDENT` | 69632+806 = 70438 | 69632+746 = 70378 |

来源（本机官方头文件，逐项核实）：
- 737 `L:\MSFS\Community\pmdg-aircraft-737\Documentation\SDK\PMDG_NG3_SDK.h`
- 777 `L:\MSFS\Community\pmdg-aircraft-77l\Documentation\SDK\PMDG_777X_SDK.h`

**模式枚举也不同**：

| 值 | 737 | 777 |
|---|---|---|
| 0 | STBY | STBY |
| 1 | ALT RPTG OFF | ALT RPTG OFF |
| 2 | **ON** | **XPNDR** |
| 3 | **ALT** | **TA ONLY** |
| 4 | TA/RA | TA/RA |

（映射到规范枚举时 2/3 号档恰好都对应 ON/ALT，所以读侧不用分支）

**模式是旋钮，不是设值**：`EVT_TCAS_MODE` 的 `Parameter` 是**转动量**
（+1 顺时针 / -1 逆时针），不是目标档位。因此设具体模式要算差值：

```
STBY(0) → ON(2)      转 2 格顺时针
STBY(0) → TA/RA(4)   转 1 格逆时针（5 档循环，取最短路径）
```

**必须由用户开启**（否则通道建立成功但收不到任何广播）：

```ini
; 737: <MSFS>\Packages\pmdg-aircraft-737\work\737NG3_Options.ini
; 777: <MSFS>\Packages\pmdg-aircraft-777\work\777_Options.ini
[SDK]
EnableDataBroadcast=1
```

未开启时 5 秒超时后判 `failed`，日志会明确指出改哪个 ini。

#### ② Input Events

```
SimConnect_EnumerateInputEvents      → 拿到当前机模全部输入事件
                                       （名字 + hash + 类型 + 节点，分页返回）
SimConnect_EnumerateInputEventParams → 该事件的参数签名
SimConnect_SetInputEvent(hash, 大小, 值) → 触发
```

- **按机模枚举**：InputEvent 表是每个机模独有的，换机模后必须重新枚举。
  MConnect 在检测到 `TITLE` 变化时自动重发（主循环 2d 步）
- **hash 不自己算**：官方文档说是 CRC，实测是 FNV-1a 变体，不同版本有差异
  ——一律用枚举返回值
- **参数要自己拼**：`SetInputEvent` 只有一个 `PVOID + size`，多参数需按
  `pack(1)` 拼接。多数按钮是 1 个 float（Push=1 / Release=0）

#### 按实际机型给选项（aircraft_caps）

这是最关键的一环：**每个机模给按钮起的名字都不一样**，写死候选表必然漏机型。

| 机模 | 应答机模式 | COM 频率交换 |
|---|---|---|
| Fenix A320 | `XPDR_STBY_Push` | `COM1_SWAP_Push` |
| PMDG 737 | `EVT_XPNDR_MODE` | `EVT_COM1_TRANSFER` |
| Asobo 默认机 | `TRANSPONDER MODE SELECT` | `COM1 STANDBY SWAP` |

做法：从**当前机模实际枚举到的事件**里按语义归类（`inputev_classify`），
把**真实名字 + hash** 报给客户端：

```json
{"type":"aircraft_caps","icao":"A20N","title":"Fenix A320",
 "inputev":{"total":10,"xpndr_code":2,"xpndr_mode":1,"xpndr_ident":1},
 "wasm":{"state":"ready","version":"0.6.0"},
 "events":[{"cat":"xpndr_code","name":"XPDR_CODE_SET","hash":3735928559}]}
```

客户端只需：
1. 收到报文 → 渲染这架飞机**真能操作**的选项
2. 用户选中 → 原样发 `{"type":"Send:INPUTEV","name":"XPDR_CODE_SET","value":7000}`
3. 换机型 → MConnect 自动重推，界面跟着刷新

分类优先级容易错的地方：`XPDR_IDENT` 必须先判 IDENT，否则会被 `XPDR`
泛匹配吃成 MODE；`XPDR_MODE` 必须先判 MODE，否则会被当成 CODE。

#### ③ WASM 桥（MobiFlight 协议）

```
MobiFlight.Command    客户端 → WASM（写字符串命令）
MobiFlight.Response   WASM → 客户端（字符串响应）
MobiFlight.LVars      WASM → 客户端（float 数组，按 Add 顺序）

命令（纯文本，不是 JSON）：
  MF.SimVars.Set.5 (>L:MyVar)              执行任意 RPN ★核心能力
  MF.SimVars.Set.1 (>B:FOO_Push)           触发 B 事件
  MF.SimVars.Add.(A:GROUND ALTITUDE,Meters) 注册变量读取
  MF.Clients.Add.<名字>                     注册自己的独立通道
```

**为什么不直接用默认通道**：`MobiFlight.*` 是给 MobiFlight Connector 自己用的。
如果 MConnect 也写它，就会和用户打开的 Connector **抢通道**——两边的
`SimVars.Add` 偏移互相踩，读到的值全是错的。所以先
`MF.Clients.Add.mconnect`，拿到 `mconnect.Command` / `.Response` / `.LVars`
三条独立通道后再用。

**首个命令可能丢失**：MobiFlight 文档明确提到"新客户端启动后第一条命令有时被
忽略"，因此注册时先发 `MF.Ping` 探活。

**与 XVoice 的对应关系**：XVoice 用的 WASimCommander 跟这是一回事——都是
"模拟器内 WASM + ClientData 共享内存 + execute_calculator_code"。区别在协议
格式：WASimCommander 是二进制结构体，MobiFlight 是文本命令。MConnect 选了
MobiFlight，因为协议简单、且用户装机率更高（MobiFlight Connector 用户群大）。

调试：

```bash
MConnect.exe --dump-input    # 列出当前机模全部 InputEvent（查按钮名）
MConnect.exe --dump-wasm      # 检查 WASM 桥是否可用 + 模块版本
MConnect.exe --parts-probe    # 真机验证他机起落架/襟翼/油门/灯光能否被驱动
```

`--parts-probe` 需先进入飞行，约 12 秒后打印 `parts probe: ai_objects=1
parts_setdata=OK light_events=OK`。任一项 `FAILED` 表示该写入通道在当前
MSFS 版本不可用，插件会自动禁用它，位置渲染与其他功能不受影响。

## 渲染与平滑

复用 XConnect 的时间插值（这块逻辑与平台无关，解决的是**网络帧到达
不均匀**）：

- 位置帧按接收时刻进入 6 帧环形缓冲
- 渲染时刻 = `now - 自适应滞后`（常规 6s / 快速 1.5s，滞后 ≥ 帧到达间隔）
- 落在两帧之间线性插值，航向取最短弧；**纯插值、永不外推、永不回跳**
- 数据停滞即停住；>60km 跳变（重连/重生）直接吸附

与 XConnect 的一处关键差异：MSFS 的 AI 飞机由模拟器持有控制权，长时间不写
位置会被 AI 逻辑接管（自己飞/自己滑行），因此**每帧（50Hz）都要写**位置 +
速度分量。只写位置不写速度会被当成瞬移而抖动/回弹。

对端 ACC 部件消费：起落架/襟翼/扰流板/**油门**（`throttle_ratio`，仅引擎
运转时上报；<0 时退 0.8 兜底）经 `DEF_AI_PARTS` 写入 AI 的
`GENERAL ENG THROTTLE LEVER POSITION:1..4`——对端推油门，本端能听到
AI 引擎的转速/推力变化。

地面吸附：`onGround` 为真时用 `高度 - AGL` 把飞机压到地形上（AGL 在
±400ft 内才采信），消除"整机悬空"。

## 线程模型

SimConnect 客户端**不是线程安全的**（官方文档明确说明），因此：

| 线程 | 职责 | 是否碰 SimConnect |
|---|---|---|
| 主线程 | `CallDispatch` → 建/驱动 AI → 5Hz 发 flight_data | **是**（独占） |
| 网络线程 | accept/recv、解析 JSON 写入玩家表 | 否（持锁） |

与 XConnect 的分工一致（XConnect 网络线程也不碰 XPLM），只是把"渲染"换成
"驱动 AI 对象"。

## 已知限制

- **仅 Windows**：SimConnect 只有 Windows 实现
- **管制目标不渲染**：`Send:ATC` 里的塔台/管制没有可注入的对应模型，仅记录数量
- **AI 飞机可能被模拟器回收**：撞地、超距、AI 交通密度设置过低时模拟器会
  自己删除对象；`SetDataOnSimObject` 失败会自动重建
- **他机动画受模拟器能力限制**：MConnect 已消费对端 `parts`（灯/起落架/襟翼/
  扰流板/引擎），起落架、襟翼、扰流板、油门通过 `DEF_AI_PARTS` 写入，
  灯光按 ObjectID 发 SimConnect 事件。两类写入都会探测能力：若当前 MSFS
  版本/机型不接受，会打一条日志并全局禁用该通道，位置渲染不受影响。
  因此"灯/动画不跟随"时先看日志里 `parts_writable` / `lights_ok`。
- **反向控制受机模实现限制**：少数第三方机模不响应标准 COM/XPNDR 事件
  （需要走 LVAR/自定义事件）。MConnect 会退到 Input Events 通道，但**设置
  具体频率值**没有对应 InputEvent（面板按钮只能"转一档"），只有主备互换
  这类按钮操作可回退。这也是为什么协议要求客户端以回读值为准
- **LVar 读不到（无 WASM 模块时）**：Input Events 只覆盖"写"（触发按钮）。
  读机模内部 LVar 需要 WASM 桥（③级），未装 MobiFlight WASM 模块时该通道
  停在 `failed`，不影响其余功能
- 机型目录全量扫描需要 1~3 秒（取决于包数量）；扫描完成前用内置兜底表，
  AI 创建会等待目录就绪（上限 90s 防扫描失败永不渲染）
- **WASM 桥注册可能超时**：MobiFlight WASM 模块只响应已知客户端名时，
  `"mconnect"` 注册 5s 超时 → 判定通道不可用（回退机型档案/Input Events，
  不影响其余功能）；是否可用看日志 `wasm: 注册超时`
- **字符串 SimVar 的 units 必须传空串**：TITLE/ATC ID/AIRLINE 等字符串
  定义传 `"NULL"` 会 NAME_UNRECOGNIZED（MSFS 2020 真机实测），包体也会
  缺 352 字节。这是 SimConnect 的坑，自定义字段时注意

## 单元测试

两个测试都不需要模拟器：

```bash
build\catalog_test.exe       # 机型四级匹配 / 别名回退（30 断言）
build\control_test.exe       # 频率 / BCD / 报文 / 队列 / 发射台 / INPUTEV（66 断言）
build\inputev_test.exe       # InputEvent 布局 / 分页 / 查找 / 截断（24 断言）
build\radio_test.exe         # 无线电报文 / 发射台推导 / JSON 字段（41 断言）
build\wasmbridge_test.exe    # WASM 命令 / 队列 / 状态机 / LVar（42 断言）
build\aircraftcaps_test.exe  # 语义分类 / 优先级 / 能力报文 / 变更检测（57 断言）
build\aircraftdb_test.exe    # 机型档案识别 / 模板展开 / **应答机读写一致性**（115 断言）
build\pmdgbridge_test.exe    # PMDG 737/777 双协议 / 重映射 / 旋钮差值（67 断言）
```

共 442 个断言。`ctest --test-dir build` 也可跑全部。

`control_test.exe` 用桩替换 `TransmitClientEvent` 与 `SetDataOnSimObject`，
断言"我们发出的事件 ID 与参数是对的"（事件是否真的被模拟器接受需要实机）。
`inputev_test.exe` 用桩替换 `SetInputEvent`，并验证结构体必须是 `pack(1)`
（布局差一个字节，读出来的事件名就全是乱码）。
`ctest --test-dir build` 也可跑全部。

## 版权

GPL-3.0（与 XFlySim-Client / XConnect 一致）