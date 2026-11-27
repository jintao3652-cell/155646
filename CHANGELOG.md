# Changelog

格式参考 Keep a Changelog / SemVer。

## Unreleased

### Added

- Bridge Protocol v1：`connected`/`hello`/`hello_ack` 握手、能力位、版本提示。
- 客户端诊断导出：`python -m xfly_client.diagnostics`（日志自动脱敏）。
- `tests/` 单元测试：日志脱敏/轮转、VI/`^` 报文、桥握手、诊断、回放、mock FSD。
- 诊断 UI：设置页“导出诊断”；后端 `Api.diagnostics/export_diagnostics`。
- MConnect 外部机型档案：`MConnect.exe` 同目录 `profiles.json`，2s 热加载，
  外部档案优先于内置表（`docs/MConnect-PROFILES.md`）。
- CI：Windows + Linux XConnect 构建 + CTest；Python unittest + selftest。
  （macOS 暂时搁置，不纳入 CI/发布。）
- 文档：FAQ、支持矩阵、桥协议、贡献指南、变更日志、MConnect 档案格式。
- **MSFS 自绘呼号标签**：研究确认原生铭牌对 SimConnect 注入 AI 永不渲染
  （模拟器硬限制，`TOGGLE_AIRCRAFT_LABELS` 已被官方废弃）。新增：
  - MConnect 同端口 HTTP roster 快照端点（`GET /roster`，CORS，协议嗅探）；
  - Community 覆盖包 `xflysim-labels`：JS overlay 注入飞行标记层，驾驶舱
    视角把注入 AI 投影为世界锚定呼号标签（与 X-Plane 自绘样式一致）；
  - `msfs_toggle_labels` / `Send:LABELS` 语义改为 overlay 显隐开关，运行时
    生效（不再发废弃的铭牌事件）。
- **MSFS AI 模型库选择**：设置新增 `msfs_traffic_lib`（自动 / FSLTL / AIG）。
  扫描时按包名给每条目录条目标记来源（`fsltl-*` / `aig-*`），匹配时优先
  使用所选库，缺失机型或涂装才回退另一套；`Send:TRAFFICLIB` 运行时切换，
  下一帧重建他机模型。`--traffic-lib <auto|fsltl|aig>` 供启动与诊断使用。
  `catalog_dump_stats()` 按来源计数，`--scan-only` 可直接确认两库都读到。
  X-Plane 侧固定使用 X-CSL，不受该项影响。
- **他机资源层**：CSL / FSLTL / AIG 从主程序更新通道拆出，按「机型包 / 航司包」
  增量更新。启动只查目录存在性与小清单版本（零文件遍历、零哈希）；完整校验
  需用户主动触发，支持按包过滤、限速、取消。详见 `docs/资源层更新设计.md`；
  `xfly_client/resources.py` 提供 `quick_check` / `verify`，资源摘要进入诊断导出。
- **XUpdate.exe 独立更新器**：客户端 / XConnect / MConnect 的版本检查与更新
  统一到 `xupdate.py` + `xfly_client/updater.py`。默认 `--check` 只读，
  `--apply` 才写盘；按 sha256 增量下载、下载后校验、替换前备份、失败整体
  回滚，检测 exe/xpl 被客户端或模拟器占用（退出码 3）。日志独立写入
  `logs/xupdate-YYYY-MM-DD.log`；`build_exe.ps1` 同步产出 XUpdate.exe。
  CSL / FSLTL / AIG 仍走独立资源层，不在此更新器内。
- **客户端在线更新（整包自更新）**：启动自动检查更新接口（只比版本号），
  有新版本时在 LCD 主屏右侧提示，点进更新小屏。流程按"不会自覆盖"设计：
  下载到 `%TEMP%\XFlySimUpdate\<version>\*.part`（HTTP Range 断点续传、边下边算
  MD5）→ 校验 → **先拉起独立助手 XUpdate.exe 再退出主程序** → 助手等进程退出
  后备份 `.bak`、同卷原子替换、失败回滚、写回版本号并重启。接口字段
  `softwareName/version/logs/link/md5/createdTime`，地址在
  `xfly_client/update_service.py::UPDATE_API_URL`（留空即关闭）。插件仍走清单
  通道；详见 `docs/客户端在线更新.md`。

### Security

- 诊断导出强化脱敏：`#AP` 登录报文的 CID 与密码、`#DP` 的 CID、通用
  `key=value` 凭据字段均遮蔽；`extra` 递归脱敏并隐藏用户目录。
  `.gitignore` 新增 `*diagnostics*.json`，已有诊断包从索引移除。

### Changed

- **文档补全与纠错**：新增 `docs/MC-XC字段手册.md`，逐字段说明两桥握手、
  `flight_data` / `parts` / `csl_status` / `aircraft_caps`、全部 `Send:*` 命令、
  配置/CLI 与关键 C 结构体，并标注单位、取值、来源和两桥差异；修正三份协议文档中
  `capabilities`、`hello_ack`、`engines.rpm_pct`、XPDR 状态枚举、`elevation`/IAS
  单位、`--port` 无效等多处与代码不符的描述；README 增加文档索引。
- **文档目录收敛**：除根目录 `README.md` / `CHANGELOG.md` 外，所有项目文档
  统一放入 `docs/`（贡献指南、打包说明、MC/XC 协议与 dataref、机型档案、
  FSD 协议参考等）；同名协议文档加桥名前缀区分，全仓引用同步更新。
- **日志改为独立目录 + 按北京时间分日**：`logs/xflysim-YYYY-MM-DD.log`，
  每天一个新文件（按 UTC+8 日切），启动与日切时自动删除早于 7 天窗口的
  `<prefix>-YYYY-MM-DD.log`（保留今天及前 6 天），其它文件不动。
  日志行时间戳同为北京时间；诊断导出优先读今天/最近的分日日志，旧 `fsd.log`
  仅作回退且不再写入。支持 `XFLYSIM_LOG_DIR` 覆盖目录；打包后日志落在 exe
  同级 `logs/`（不写 `sys._MEIPASS` 临时目录）。
- **可观测性统一**：客户端日志统一 `DEBUG/INFO/WARN/ERROR` 四级标签，
  前端按标签着色（DEBUG 淡显），桥日志转发时也带标签；诊断导出升级为
  `xflysim-diagnostics` schema v1，附 `redaction` 标记与 `extra.log`
  结构化日志（级别计数 + 最近 200 条脱敏记录），便于用户反馈时直接附上。
- **本机字段读取对齐 swift（VATSIM 客户端）的 MSFS 2020/2024 实践**：
  - 引擎开关判定改为 `GENERAL ENG COMBUSTION`（|| N1>5 兜底）——N1 对
    活塞机恒 0，只看 N1 会把活塞机全部上报熄火；
  - ICAO 机型识别链新增 `ATC TYPE` 来源（标准 ICAO 代码，swift 的机型
    匹配首选；排在 TITLE 反查之后、ATC MODEL 之前）；
  - 起落架新增第三来源 `GEAR HANDLE POSITION`（swift 的本机起落架上
    报来源；TOTAL PCT ∪ 动画位置 ∪ 手柄取最大值）；
  - COM 可用性判定新增 `COM STATUS:n`（0=OK 1=不存在 2=断电 3=故障，
    swift 同款：transmit && status==0）。
- 远端全量 ACC 轮询：50km 内每 2s（全部请求），50km 外 30s（每轮最多 10 个）。
- `fsd.log` 10MB×3、`xconnect.log` 8MB×3 滚动；`#AP` 密码统一替换为 `***`。
- XConnect 降低高频日志（`send flight_data` 30s、`players updated` 仅变化时）。
- 桥接握手信息写入客户端状态，便于诊断版本/能力不一致。
- **MSFS 铭牌开关改为运行时生效**：新增 `Send:LABELS` 桥命令，设置里改开关
  并保存时立即下发切换，不再要求重启 MConnect（见 Added：语义已改为自绘
  overlay 显隐，废弃的铭牌事件不再发送）。
- **软件中心发布包解析器**：XUpdate 新增 `--software-api` / `--software-uuid`
  （或对应环境变量）模式，按 `/softwareVersion/latest|logs/{uuid}` 取版本与
  更新记录，下载后校验服务端 md5；**非 zip 直接放根目录，zip 解压后按包内
  相对路径放根目录**，带 zip slip 防护、备份与失败回滚。原清单模式不变。

### Fixed

- **MSFS 把呼号当尾号上报（注册号 / LIVERY 双重污染）**：`ATC ID` 在不少机模/
  涂装里被写成呼号（`SJX110`）或留空，MConnect 直接当 registration 上报 →
  站点显示呼号、PI 变成 `LIVERY=SWIFT_A672-SJX110`（对端按这个变体码匹配
  涂装必然失败）。现在 MConnect 走"ATC ID（形状像尾号才算）→ TITLE 里的尾号
  → 空"的来源链（`src=atc_id/title`），客户端再加一道"模拟器值 == 本机呼号
  一律丢弃"的护栏。形状判定保守到能挡住 `B738`/`A320NEO`/`Lufthansa`。
- **深度机模（PMDG/Fenix/iFly/A2A）灯光全灭**：这些机模不维护逐个
  `LIGHT LANDING/BEACON/…` 的 Bool，只驱动模拟器自己的灯光总线。MConnect
  AUX 包新增 `LIGHT ON STATES`（Mask）并按位与逐灯读数**取并集**
  （NAV/BEACON/LANDING/TAXI/STROBE/RECOGNITION/WING/LOGO/CABIN），
  掩码恒 0 的机模行为不变。掩码放 AUX 而非主包：主包有字节数硬校验。
- **冷舱多发动机机型被报成"1 台"**：两侧桥的"台数未知"兜底是
  `en = 0 → 1`，于是 `NUMBER OF ENGINES`/`acf_num_engines` 缺失且全部熄火时，
  B744 之类被上报成 1 发（站点/对端据此误判机型）。改为"本次会话见过的最大
  台数 → 实测在转的最高编号 → 0（未知，整键不发）"，由客户端用 swift 机型库
  兜底；XConnect 的 `engine_count` 与 `parts.engines` 条目数同步一致。
- 站点 `aircraft.engines` 恒 null（ACC 只发广播地址，从未进服务器）：
  站点那四个字段分属两条通道——`equipment/airline/livery` 走 PI
  （`#SB…:PI:GEN`），`engines` **只能**走 ACC（`$CQ…:ACC`；swift 的
  `PlaneInformation::toTokens()` 只发三对键，PI 里根本没有 engines）。
  而 PI 我们发给 `SERVER`、ACC 只发 `@94836` 广播地址 —— 正好解释"前三个
  有值、engines 恒 null"。修复：
  - 全量 ACC 现在同时发给 `@94836` **和 `SERVER`**（增量仍只发广播）；
    若服务器回 004，自动退回仅广播并打日志（自愈，不刷屏）。
  - `_acfg_swift_shape()` 在桥当前帧未给 `engines` 时按台数补"全关"掩码
    （站点缺键就是 null，下一帧真实值覆盖）。
  - 新增 CAPS 应答：声明 `ACCONFIG=1`（swift/vPilot 只给声明了该能力的
    客户端发 ACC；不声明服务器也可能不采纳我们发的 ACC）。
  - `tools/verify_site_aircraft.py`：把"日志里实际发出的 PI"与"站点返回的
    aircraft 对象"并排成一张表，逐字段判定是本机没发还是服务器没采纳。
- **`equipment`/`airline`/`livery`/`engines` 由四处各自维护 → 收敛为一个解析点**：
  新增 `xfly_client/report_identity.py`（值对象 + 纯函数）。固定兜底顺序：
  equipment（手填 → 桥值规范化 → 机模名反查）、airline（配置 → 呼号前缀）、
  livery（swift 机型库 `A<id>` + 尾号）、engines（桥实测 → 机型库台数）。
  过去桥报 `engine_count=0`（机型未加载完）时 `if eng > 0` 分支直接跳过，
  PI/ACC 双双缺 engines —— 现在按机型库兜底，首包就有值。
  PI 线级再加一道防线：`airline_icao` 为空时按呼号补 `AIRLINE=`。
- **断线幽灵帧判据太窄，实测那帧仍被放过**：MConnect 卡死时的坐标不是
  精确 `0,0`，而是 `(0.0004150, 0.0139740)`；`|lon| < 0.01` 刚好放过它，
  于是 `PI:GEN:AIRLINE=SJX` 照发（这正是"以为修好、其实还在覆盖"的原因）。
  阈值放到 0.05° 并要求机型/机模名/尾号三者全空才判废；同时不再让"机型
  识别为空"的帧清空 `aircraft_icao`（清空即产出残缺 PI）。
- 站点在线用户 `equipment`/`livery`/`engines` 变 null（断线幽灵帧覆盖）：
  SimConnect 断线重连期间插件推来一帧全默认 flight_data（位置 0,0、机型空、
  尾号空），客户端据此触发 PI 重发，发出残缺报文 `PI:GEN:AIRLINE=SJX`；
  服务器按位置解析后把站点已正确的 aircraft 段整块覆盖成 null（fsd.log
  18:17:29 会话实证）。三层防御：① `model.py` 帧门控——机型未知且位置 0,0
  的幽灵帧直接丢弃，不更新 FSD 状态/PI/ACC；② `packet.plane_info()` 在
  EQUIPMENT 与 LIVERY 都为空时返回空串；③ `_send()` 跳过空报文。补 8 条
  回归测试（幽灵帧不触碰 FSD、只提示一次、正常帧不误杀、残缺 PI 不生成）。
  注：`controller`/`isVirtual` 对站点所有用户都是 null，属服务端管制员字段，
  客户端无上报路径，非本 bug。
- **启动后误报「MConnect 未编译，跳过自动启动」**：`webapp.py` 位于仓库根目录，
  却对 `__file__` 做了两层 `dirname`（那是 `xfly_client/` 子目录模块的写法），
  把启动器根目录指到了仓库上一级，永远找不到 `MConnect/build/MConnect.exe`。
  改用 `app_base_dir()`（源码=仓库根，打包=exe 同目录），并给启动器加候选
  路径搜索（含子目录/父目录，兼容 build-vs2026）。实测能定位到已编译的 exe。
- **MSFS 重启后在线地图不显示自己 / 游戏内看不到其他飞机（SimConnect 断线不重连）**：
  模拟器退出再开时旧 SimConnect 管道被服务端关闭，但本进程收不到 QUIT，
  陷入"静默死亡"：`CallDispatch` 不再投递数据，位置停在默认值
  `0.00042:0.01397`（约 0°,0°）被上报到服务器，`AICreateNonATCAircraft`
  持续返回 `0xc000014b`（STATUS_PIPE_BROKEN）导致所有他机 AI 创建失败。
  MConnect 现在用 flight_data 新鲜度检测断线（超过 8s 无新帧），自动
  `close → load → open` 重建会话、重注册全部数据定义/事件映射/能力枚举，
  并有 20s 冷却防模拟器未就绪时反复重试。实测重启后新进程读到正常坐标。
- **MAIN 屏右侧 MSG 键点了没反应**：该键原先错误地跳到日志屏（`/log`），
  而消息抽屉 `MsgDrawer` 由 `store.overlays.msg` 控制、没有任何入口打开它。
  现在 MSG 键直接打开消息抽屉并清除 NEW MSG 提示。
- **MSFS（MC）下误报 X-Plane CSL 缺失模型**：MConnect 借用 `csl_status`
  通道上报 AI 模型注入状态（报文中带 `sim:"msfs"`），前端原先不区分来源，
  把 MSFS 的 AI 失败显示成「CSL 缺失模型（放入 plugins/XConnect/CSL/）」。
  前端现在按 `sim` 分流文案与状态标签（MC 显示 `AI x/y`，XC 才显示 CSL）；
  MC 侧 `missing` 只统计 `catalog_match` 彻底未命中的机型，AI 创建失败、
  对象被回收等暂态问题会退避重试，不再误报。实测 A343 本就有
  `A343 → A333` 别名，之前的提示是误报，已补回归断言。
- **设置保存后同一条日志打印两次**：后端 `set_msfs_traffic_lib()` /
  `toggle_msfs_labels()` 已通过 `_push("log")` 推送并落盘，前端保存流程又
  用 `message()` 打印同一句话。去掉前端重复提示，并补齐后端日志的
  en / zh-TW 翻译，保证只有一条且按界面语言显示。
- **X-Plane CSL 缺失 E19L**：`E19L` 是 E190LR 在部分 VATSIM/航司数据里的
  写法，别名表里只有 `E190`。XConnect 补 `E19L -> E190`，MConnect 同步补
  `E19L -> E195`（与现有 E190 映射一致）；两侧各加一条回归测试。
  注意这条日志来自 X-Plane 侧 CSL，与 MSFS 的模型库偏好（FSLTL/AIG）无关。
- **MSFS 侧本机机型无法自动识别（Fenix A320 实测）**：两处独立 bug 叠加。
  1. `DEF_FLIGHT` 主数据定义里混入 7 个 IniBuilds A350 灯光 LVar
     `(L:INI_LIGHTS_*, number)`，MSFS 2020 的 SimConnect 不认 calculator
     变量名，抛 `NAME_UNRECOGNIZED` ×7 并从发包中剔除这些字段（56 字节），
     主包按 780 字节硬校验 → 每帧整包丢弃 → `TITLE`/`ATC TYPE` 永远读不到。
     拆成独立可选定义 `DEF_INI_LIGHTS`/`REQ_INI_LIGHTS`，主包降到 724 字节，
     LVar 读不到只影响 A350 灯光覆盖。
  2. `catalog_icao_from_title_keywords()` 把 title 转小写后，却拿**大写**
     关键词表去做 `strstr`，大小写不敏感匹配永远失败 —— Fenix 连写涂装
     标题（无精确目录条目、ATC TYPE 又是 `ATCCOM` 占位符）因此认不出机型。
     关键词同样转小写后再比较。
  实测：`local aircraft title="FenixA320..." -> ICAO A320`，不再出现
  `SIMOBJECT_DATA too short`；新增 3 条目录回归测试，CTest 8/8。
- **设置里手填机型保存失败，连线仍报「机型 ICAO 缺失」**：`openSettings()`
  用旧字段名 `host/port/xphost/xpport` 初始化设置对象，而模板与
  `saveSettings()` 用的是 `server_host/server_port/xconnect_host/xconnect_port`，
  点保存时 `.trim()` 抛异常，手填机型等设置全部没写入配置。统一字段名，
  并对旧内存状态做兼容；插件地址/端口缺省回填 `127.0.0.1` / `51001`，
  避免配置缺字段时输入框留空；手填机型增加 2~4 位字母数字前端校验，
  非法值直接提示，不再静默回退自动识别。
- **XConnect Linux CI 编译失败**：`labels.c` 的呼号字号放大依赖
  `glPushMatrix/glScalef`，GitHub runner 缺 `GL/gl.h` 与链接用的
  `libGL.so`。CI 在 Ubuntu 构建前安装 `libgl1-mesa-dev`；Apple 平台改用
  `<OpenGL/gl.h>`。顺带修复地形探针首次命中时 `terrain` 未初始化的真实
  bug（AGL/贴地判断此前会读到随机值），并消除 `csl.c` 的嵌套注释告警。
- **站点 JSON 里 `livery` 恒为 null（IniBuilds A350 实测）**：机型推断
  原来只在 ICAO 为空（或 `ATC*` 占位符）时触发，而 IniBuilds A350 上报的
  ICAO 是非标准码 `A350-900`（标准码 A359）——非空、非占位，直接穿透护栏
  进入 PI；`swift_livery("A350-900")` 在 swift 机型库查不到 id →
  `LIVERY` 键不发 → 站点 livery 恒 null。修复：新增
  `aircraft_db.normalize_icao()`（库内码原样保留；库外码按机模名称/码本身
  重推，`A350-900` → A359；推不出保留原值保底 equipment），
  `model.py`/`webapp.py` 两条上报路径统一走规范化
  （回归测试 `tests/test_normalize_icao.py`）。
- **站点 JSON 里 `engines` 恒为 null（第二条实测路径）**：机型 ICAO 反查
  不到 swift id（如 MSFS 老插件的 `ATCCOM` 占位符）时 PI 缺 `LIVERY` 键，
  `ENGINES=` 顶到 LIVERY 的槽位，服务器按位置取第 4 键拿到 `REG=` →
  engines null（PI 里明明有 ENGINES=2）。修复：
  - `packet.plane_info`：LIVERY 缺失时连带省略 ENGINES（保证槽位）；
  - `model.py`/`webapp.py`：`ATC*` 占位符当**未知机型**走名称推断
    （Fenix 连写 title → A320），equipment/livery/engines 三链一起恢复。
- MConnect 起落架识别：Fenix 等 WASM 起落架机模只写 `GEAR ANIMATION POSITION`
  而不维护 `GEAR TOTAL PCT EXTENDED`（停在地面上仍报 0），对端 CSL 起落架
  显示收起。现按两个来源取最大值上报。
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
