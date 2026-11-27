# XVoice 3.8.17 如何获取 Squawk / 无线电频率（逆向分析）

> 分析对象：`G:\HugoMoveData\User\Administrator\Downloads\3.8.17`（XVoice，Avalonia + .NET）
> 方法：`ilspycmd` 反编译 `XVoice.dll`（21 MB，含 PDB 符号）+ 解析 `simaircrafttable.bytes` 配置表
> 结论用途：对照本项目 XConnect 插件的采集方式，判断哪些是 XFS 平台要求、哪些可以借鉴

---

## 0. 一句话结论

| 项目 | XVoice 的做法 | 本项目是否已一致 |
|---|---|---|
| **无线电频率** | 订阅 `com1/com2_frequency_hz_833`（int，kHz），除以 1000 得到 MHz 字符串 `"121.800"`，变化时向语音服务器发 `C2G_PilotJoinFrequency` | ✅ 已采集（`DR_COM1/2/3`），但我们只上报给 FSD，没有语音服务器 |
| **Squawk** | **完全不采集**。全项目没有任何 Transponder/Squawk 订阅 | ✅ 我们采集了（`transponder_code`），比它更全 |
| **机型识别** | 三键匹配：`AircraftID`（`flight_id`）+ `AircraftICAO`（`acf_ICAO`）+ 配置表 → 得到该机模的专用 dataref 与命令 | ⚠️ 我们只按 `acf_ICAO`，无机型适配表 |

**最重要的发现**：XVoice 有一张**机型适配表**（`simaircrafttable.bytes`），里面**以尾号为键**登记了各机模的音频命令。你正在飞的 Laminar A330 就在表里，登记的尾号正是 `D-AIKN` —— 这解释了为什么尾号必须报对。

---

## 1. 核心代码：`XPlaneTrigger.cs`（329 行，全部采集逻辑都在这里）

XVoice 走的是 **X-Plane 官方 UDP 协议**（`sim/` dataref 订阅），不是第三方插件。与我们的 XConnect 插件（DataRef 直读 + TCP 51001）是两条路。

### 1.1 连接与两阶段订阅

```csharp
public static async Task Connect()
{
    connector = new XPlaneConnector();
    Task.Run(async () => { await SubscribeAircraftData(); });   // 先认机型
    connector.Connect();
}
```

**第一阶段 —— 先认机型，再订阅数据**（关键设计）：

```csharp
private static async Task SubscribeAircraftData()
{
    bool flightIdReceived = false, icaoReceived = false;

    // ① 机模标识（XP 的 aircraft .acf 文件名）
    connector.Subscribe(new StringDataRefElement {
        DataRef = "sim/cockpit2/radios/actuators/flight_id",
        StringLength = 40, Frequency = 1
    }, 1, (el, value) => { flightIdValue = value; flightIdReceived = true; CheckAircraftMatch(...); });

    // ② 机型 ICAO
    connector.Subscribe(DataRefs.AircraftViewAcfICAO, 1, (el, value) => {
        icaoValue = value; icaoReceived = true; CheckAircraftMatch(...);
    });
}

private static void CheckAircraftMatch(bool flightIdReceived, bool icaoReceived,
                                       string flightId, string icao)
{
    if (!flightIdReceived || !icaoReceived) return;        // 两个都到齐才继续

    foreach (SimAircraftConfig data in SimAircraftTable.Instance.DataList)
    {
        if (data.SimType == SIM_TYPE.XPLANE
            && data.AircraftID  == flightId
            && data.AircraftICAO == icao)                  // 双键精确匹配
        {
            Singleton<SimulatorManager>.Instance.SimAircraftConfig = data;
            break;
        }
    }
    SubscribeData();                                       // 第二阶段才订阅频率等
}
```

> **关于 `flight_id` 的真实语义**（容易误解，必须说清）：
> X-Plane 官方定义是 `sim/cockpit2/radios/actuators/flight_id`，类型 `byte[8]`，
> 描述为 *"Flight ID, as transmitted in Mode-S. Up to 7 characters, like N844X or KLM511"*
> —— 它是**应答机 Mode-S 播发的航班标识/呼号**，**不是** `.acf` 文件名。
> XVoice 声明 `StringLength = 40`（超出了 dataref 的 8 字节，属于宽松声明，无害）。
>
> 而配置表的 `AircraftID` 字段存的是 `D-AIKN`（尾号），`AircraftICAO` 是 `A333`。
> 也就是说匹配条件实际是 **`flight_id`（Mode-S ID）== 尾号 且 `acf_ICAO` == 机型码**。
> 这暗示：Laminar A330 在 X-Plane 里把 Mode-S Flight ID 默认填成了机模尾号 `D-AIKN`
> —— 这与 `.acf` 里 `P acf/_tailnum D-AIKN` 是同一个值，两者在默认机模里恰好一致。
>
> 对我们的启示：**不要假设 `flight_id` 等于 `.acf` 名**。若要做同类匹配，用
> `acf_ICAO`（稳）或尾号（`acf_tailnum`，稳）作键，`flight_id` 只作辅助。

### 1.2 频率获取（本题重点）

```csharp
// 通用路径 vs 机型专用路径 —— 二选一（三元表达式）
DataRefElement dataref = !string.IsNullOrEmpty(SimAircraftConfig?.DataVHF1Frequency)
    ? new DataRefElement {
          DataRef   = SimAircraftConfig.DataVHF1Frequency,
          Units     = "hertz",
          Description = "Com radio 1 frequency, hz, supports 8.3 khz spacing",
          Frequency = 5 }
    : DataRefs.Cockpit2RadiosActuatorsCom1FrequencyHz833;

connector.Subscribe(dataref, 25, (DataRefElement element, float value) =>
{
    // ★ 核心换算：dataref 单位是 kHz，除以 1000 → MHz，固定 3 位小数
    string freqId = (value / 1000f).ToString("F3");        // 121800 → "121.800"

    if (topPilotComViewModel.PowerOn                                   // ① COM 电源必须开
        && MyInfo.CurrentVHF1Info.frequencyID != freqId)               // ② 与上次不同
    {
        var msg = C2G_PilotJoinFrequency.Create();
        msg.vhfMode     = SPEAK_MODE.VHF1;
        msg.frequencyID = freqId;                                      // 字符串 "121.800"
        msg.position    = MyInfo.CurrentVHF1Info.position;              // 经纬度+高度
        SendMessage.Send2Server(msg);                                  // 发给语音服务器
    }
});
```

要点：

| 项 | 值 |
|---|---|
| dataref | `sim/cockpit2/radios/actuators/com1_frequency_hz_833`（int，**kHz**） |
| 换算 | `value / 1000f` → MHz，`ToString("F3")` → 三位小数 |
| 订阅周期 | **25 ms**（40 Hz，远高于 FSD 的 5 s） |
| 触发条件 | COM 电源 ON **且** 频率与上次不同（去重，不重复发） |
| 上报格式 | 字符串 `"121.800"`（不是数字），走 **MemoryPack 二进制 RPC**，非文本协议 |

VHF2 完全同理，只是 dataref 换 `com2`、`vhfMode` 换 `VHF2`。

### 1.3 COM 电源判定（频率上报的前置条件）

```csharp
DataRefElement dataref = !string.IsNullOrEmpty(SimAircraftConfig?.DataPower)
    ? new DataRefElement { DataRef = SimAircraftConfig.DataPower, Units = "bool" }
    : DataRefs.CockpitElectricalAvionicsOn;          // sim/cockpit/electrical/avionics_on

connector.Subscribe(dataref, 25, (el, value) => {
    topPilotComViewModel.IsDataLoaded = true;
    topPilotComViewModel.PowerOn = (int)value == 1;
});
```

> 注意：电源关着的时候**不会发** `PilotJoinFrequency`。这跟 FSD 侧"按电源决定 squawk 模式"是同一个思路。

### 1.4 收/发/音量（完整清单）

`XPlaneTrigger.cs` 实际订阅的全部 dataref（共 17 个，无一个与应答机相关）：

| 用途 | 通用 dataref | 订阅周期 |
|---|---|---|
| 机型 ICAO | `sim/aircraft/view/acf_ICAO` | 1 ms |
| 机模 ID | `sim/cockpit2/radios/actuators/flight_id` | 1 ms |
| 纬度 | `sim/flightmodel/position/latitude` | 5 ms |
| 经度 | `sim/flightmodel/position/longitude` | 5 ms |
| 高度 | `sim/flightmodel/position/elevation`（×3.28084 → ft） | 5 ms |
| COM 电源 | `sim/cockpit/electrical/avionics_on` | 25 ms |
| VHF1 频率 | `sim/cockpit2/radios/actuators/com1_frequency_hz_833` | 25 ms |
| VHF2 频率 | `sim/cockpit2/radios/actuators/com2_frequency_hz_833` | 25 ms |
| Com1 收听 VHF1 | `sim/cockpit2/radios/actuators/audio_selection_com1` | 25 ms |
| Com1 收听 VHF2 | `sim/cockpit2/radios/actuators/audio_selection_com2` | 25 ms |
| Com1 VHF1 音量 | `sim/cockpit2/radios/actuators/audio_volume_com1` | 25 ms |
| Com1 VHF2 音量 | `sim/cockpit2/radios/actuators/audio_volume_com2` | 25 ms |
| 发言选择 | `sim/cockpit2/radios/actuators/audio_com_selection`（6=com1, 7=com2） | 25 ms |
| 副驾驶对应项 | 同上 `*_copilot` 变体（5 个） | 25 ms |

---

## 2. Squawk：XVoice 根本不采集

全局搜索结果：

- 全项目 706 个 `.cs` 文件中，**只有** `XPlane.DataRefs/DataRefs.cs` 出现 `squawk` 字样 —— 那是一份**自动生成的 dataref 全表**（37539 行，把 X-Plane 所有 dataref 都列了），不代表被使用。
- `XPlaneTrigger.cs` 里**没有任何** `transponder` / `squawk` 订阅。
- `C2G_PilotJoinFrequency` 消息体只有 `frequencyID` / `vhfMode` / `position` 三个字段，**没有应答机字段**。

结论：**XVoice 是纯语音客户端，不关心应答机。** 应答机信息在 XFS 平台上是走 FSD 协议的，由其他客户端（或服务器侧）负责。

反观本项目：XConnect 采集了 `sim/cockpit2/radios/actuators/transponder_code` 与 `transponder_mode`，并通过 `@` 包（`squawk` + `mode`）上报给 FSD —— **我们这块是完整的，比 XVoice 更全**。

---

## 3. 机型适配表 `simaircrafttable.bytes`（最有价值的部分）

Luban 二进制配置表，15543 字节。字段定义见 `cfg/SimAircraftConfig.cs`：

```csharp
public readonly string AircraftID;        // 机模标识（.acf 名）
public readonly string AircraftICAO;      // 机型 ICAO
public readonly string AircraftImgURL;    // 机型图片
public readonly SIM_TYPE SimType;         // XPLANE / MSFS / ...
public readonly string DataPower;                 // 电源 dataref
public readonly string DataVHF1Frequency;         // VHF1 频率 dataref（可覆盖通用值）
public readonly string DataVHF2Frequency;
public readonly string DataCom1VHF1Hear;          // 收听 dataref
public readonly string DataCom1VHF1Volume;        // 音量 dataref
public readonly string DataCom1VHF1Speak;         // 发言 dataref
public readonly string CommandCom1VHF1Hear;       // 收听「命令」（写操作用）
... 每个字段还配一个 `*Matched` 布尔，表示该机模是否用了自定义 dataref
```

### 3.1 实测：你的 A330 就在表里

从 `simaircrafttable.bytes` 偏移 13334 处提取出的 A333 记录：

```
AircraftID    = D-AIKN          ← 注意！这里存的是尾号，不是 .acf 名
AircraftICAO  = A333
SimType       = XPLANE
CommandCom1VHF1Hear  = laminar/A333/audio/capt/listen_press00
CommandCom1VHF2Hear  = laminar/A333/audio/capt/listen_press01
CommandCom2VHF1Hear  = laminar/A333/audio/fo/listen_press00
CommandCom2VHF2Hear  = laminar/A333/audio/fo/listen_press01
CommandCom1VHF1Volume= laminar/A333/audio/capt/volume_pos_0
CommandCom1VHF2Volume= laminar/A333/audio/capt/volume_pos_1
CommandCom2VHF1Volume= laminar/A333/audio/fo/volume_pos_0
CommandCom2VHF2Volume= laminar/A333/audio/fo/volume_pos_1
CommandCom1VHF1Speak = laminar/A333/audio/capt/mic_push1
CommandCom1VHF2Speak = laminar/A333/audio/capt/mic_push2
CommandCom2VHF1Speak = laminar/A333/audio/fo/mic_push1
CommandCom2VHF2Speak = laminar/A333/audio/fo/mic_push2
```

**`AircraftID` 存的是 `D-AIKN`（尾号）**，不是 `A330`（.acf 名）。配合 `flight_id`
（Mode-S Flight ID）的语义看，这条记录表示：「当 Mode-S ID 为 `D-AIKN` 且机型为
`A333` 时，用 `laminar/A333/*` 这套命令」。不同机模作者的命名习惯差异很大，
用尾号当键反而比用 `.acf` 名更稳定。

> 这直接印证一件事：**尾号必须报对**。XVoice 靠尾号定位机模配置，我们靠尾号做
> CSL 匹配与 PI 上报，两边都依赖它。之前 `acf_tailnumber` 拼错导致尾号恒空，
> 影响面比想象的大。

### 3.2 表里还登记了这些 X-Plane 机模

```
D-AIKN  / A333  / XPLANE   → laminar/A333/*
N816NN  / B738  / XPLANE   → laminar/B738/*
```

X-Plane 部分只有这两条（其余全是 MSFS 条目：Fenix A320/A319/A321、A310、A400M、C919、737 Max 等，用 `L:`/`B:` LVAR 和 `K:` 事件，机制完全不同）。

### 3.3 字节级实证：A333 的频率字段是空的

直接看 `A333` 记录的原始字节（`44 2d 41 49 4b 4e` = `D-AIKN`）：

```
44 2d 41 49 4b 4e   "D-AIKN"    ← AircraftID
04 41 33 33 33      "A333"      ← AircraftICAO
06 58 50 4c 41 4e 45 "XPLANE"   ← SimType
00 00 00 ... (27 个 00)          ← ★ 这一整片是空字符串
26 laminar/A333/audio/capt/listen_press00   ← Command 字段开始有值
```

**意义**：`DataPower` / `DataVHF1Frequency` / `DataVHF2Frequency` / `DataCom*` 等
**dataref 覆盖字段全部为空**，只有 `Command*`（写操作用的命令）有值。所以：

- A333 的**频率读取**走的是**通用 dataref** `sim/cockpit2/radios/actuators/com1_frequency_hz_833`
  —— 也就是三元表达式里的 `else` 分支
- 它自定义的只是 **收听/音量/发言**（用 Laminar 的 `laminar/A333/audio/*` 命令来「写」）

> 这解释了为什么我们项目直接读 `com1_frequency_hz_833` 在 A330 上是好的（实测拿到
> `121800`）。**频率这个 dataref 对 Laminar 全系机模都通用**，不需要机型适配。
> 需要适配的是 ToLiss/FlightFactor 这类把频率存进 `AirbusFBW/` 或自定义 dataref 的机模。

---

## 4. 与本项目 XConnect 的对比

| 维度 | XVoice 3.8.17 | 本项目 XConnect (v14) |
|---|---|---|
| 接入方式 | X-Plane 官方 UDP（`XPlaneConnector` 库） | 自研 C 插件，TCP 127.0.0.1:51001 |
| 频率 dataref | `com1/2_frequency_hz_833` ✅ 相同 | `com1/2/3_frequency_hz_833` ✅（多 VHF3） |
| 频率单位处理 | `/1000f` → MFD，`F3` 格式化 | `freq_to_mhz1000()` 按量级判断 Hz/kHz/MHz → 整数 mHz×1000 |
| 频率用途 | 语音服务器频道（`C2G_PilotJoinFrequency`） | FSD `#TM` 频率消息 + 前端 RMP 显示 |
| 频率采样 | 25 ms（40 Hz） | 5 Hz |
| Squawk | ❌ 不采集 | ✅ `transponder_code` + `transponder_mode`，走 `@` 包 |
| 机型识别 | `flight_id` + `acf_ICAO` 双键 + 配置表 | 仅 `acf_ICAO` |
| 机型适配 | ✅ 有（可覆盖 dataref / 提供命令） | ❌ 无 |
| 尾号用途 | 机型配置键 | CSL 匹配 + PI 上报 |

**一句话**：频率采集两边做的是同一件事（同一个 dataref），我们甚至更全；Squawk 我们比它完整；**唯一值得借鉴的是「机型适配表」** —— 对于 ToLiss/FlightFactor 这类用自定义 dataref 的机模，通用 dataref 读不到值，需要按机型覆盖。

---

## 5. 对本项目的可借鉴点

### 5.1 机型适配表（建议，优先级中）

现在 XConnect 写死了通用 dataref。遇到 ToLiss A321 这类机模：
- 无线电频率存在 `AirbusFBW/...` 而不是 `sim/cockpit2/radios/...`
- 应答机已有部分支持（`DR_XPDR_SYS/PWR/TCAS_MODE` 等 AirbusFBW 系列）

可以照 XVoice 的思路加一张轻量映射表（JSON 即可，不必 Luban）：

```json
{
  "A321": { "icao": "A321", "provider": "toliss",
            "com1": "AirbusFBW/COM1Freq", "xpdr": "AirbusFBW/XPDRSystem" },
  "A333": { "icao": "A333", "provider": "laminar" }
}
```

匹配键建议用 **`acf_ICAO` + 尾号**（对齐 XVoice 的双键思路），未命中则走通用 dataref。

### 5.2 频率采样率（可选）

XVoice 用 25 ms 采样频率。我们的 `flight_data` 是 5 Hz，对 FSD 上报足够了（`@` 包本身 5 s 一次）。但如果将来要做「本地实时频率显示/语音切换」，5 Hz 会有明显延迟感，可考虑把 COM 频率单列一个高频通道。

### 5.3 不需要动的部分

- **Squawk**：我们已经完整，且 FSD 协议要求的就是 `@` 包里的 squawk + mode，没有额外工作。
- **频率换算**：我们的 `freq_to_mhz1000()` 兼容 Hz/kHz/MHz 三种，比 XVoice 的固定 `/1000` 更健壮（不同 X-Plane 版本/机模单位确实不一致，见 `datarefs.c` 注释）。

---

## 6. 复现步骤（下次要再看别的 .NET 客户端时用）

```bash
# 1. 安装反编译器（一次性）
dotnet tool install --global ilspycmd

# 2. 整包反编译（有 PDB 时能还原真实方法名/局部变量名）
"/c/Users/Administrator/.dotnet/tools/ilspycmd.cmd" -p -o /tmp/xv "<程序集>.dll"

# 3. 关键搜索
cd /tmp/xv
grep -rln "Squawk\|squawk" --include=*.cs .
grep -oE "DataRefs\.[A-Za-z0-9]+" XPlaneTrigger.cs | sort -u    # 实际用到的 dataref
```

> 坑：Git Bash 的 `/tmp` 是虚拟映射，Python 打不开。用 `pwd -W` 或 `cygpath -w` 取真实路径
> （本例为 `C:/Users/Administrator/AppData/Local/Temp/xv`）。