# XConnect — X-Plane 数据桥接与 CSL 渲染插件

`XConnect` 是一个原生 X-Plane 11/12 插件（XPLM C API），功能：

1. **桥接数据**：在 X-Plane 内读取本机飞行数据，通过 TCP 实时发送给外部客户端
   （Python/XFlySim-Client 等）
2. **回传渲染**：接收客户端推送的在线玩家/管制列表，用 XPLMInstance 将 CSL
   模型渲染到 X-Plane 场景中（其他玩家的飞机实时显示、跟随移动）。
   位置平滑采用**时间插值**（参考 Swift pilotclient 的插件层设计）：网络位置
   帧按接收时间戳进入环形缓冲，渲染滞后**自适应 ≥ 更新间隔**（常规 6s、
   快速更新 1.5s），渲染时刻始终落在已知两帧之间做线性插值（航向取最短弧）
   ——纯插值、永不外推、永不回跳；数据停滞即停住、遥传跳变（>60km）直接
   吸附。全程基于时间轴，与帧率无关。
3. **落地吸附**：遥测 `onGround`（PBH bit1 / `#SB VI`）为真且速度 <100kt 时，
   忽略对端高度字段的偏差（气压/指示高度基准、重心 vs 轮子接地点），直接把
   模型落到地形探针高度；否则近地（AGL ≤8m）同样贴地。消除"整机飘在空中/陷地"。
4. **涂装与机型匹配（四级规则）**：日志里 `match=` 会标出实际命中哪一级
   1. `REG` 注册号（尾号）一致 —— 变体名 / OBJ 文件名（`B738_CCA_B-1234.obj`）
      / `AIRLINE`·`LIVERY` 行尾注记（`LIVERY B738 CCA B-1234`）含同一尾号
      （忽略大小写与 `-`）→ 同一架飞机的专属涂装；
   2. `AIRLINE` 同航司 + 同机型 —— 变体码精确，再同航司带后缀（`AAL`→`AALWL`）；
      同航司多套涂装时优先**不绑定具体尾号**的那套；
   3. `TYPE` 同机型任意涂装 —— `ZZZ` 白机 → `ANY`/`GEN` → 无涂装基础变体 →
      包内基础 OBJ；
   4. `FALLBACK` 兜底 —— 型号别名表落到同族模型（`FA8X`/`GLF6`→`F900`、
      `B38M`→`B738`、`E75L`→`E175`，共 173 条）→ `CSL/ZZZZ` 白机。
   别名表在 `src/csl.c::kTypeAlias`，可用 `python tools/csl_alias_check.py`
   校验目标包是否真实存在。
   尾号来源：本机 `acf_tailnum`（缺失时取 `acf_livery_path` 涂装目录名）→
   客户端按「手动填写 → 模拟器 → 同机型上次值 → 空」定值 → 随 `#SB PI:GEN` 的
   `LIVERY` 字段（另附 `REG=`）广播给对端；swift / FSInn 原生不发尾号，此时从
   第 2 级开始匹配。
5. **反向控制**：客户端可设置应答机（码/模式）与 COM1-3 频率（主频/备频/互换）
   —— `Send:RADIO` / `Send:XPDR`，直写 dataref。报文与 MSFS 版 `MConnect`
   完全一致（那边走 SimConnect 事件，因为频率是只读 SimVar）。
   命令由网络线程入队、主线程执行（XPLM 非线程安全）。

## 目录结构

```
XConnect/
  src/
    xconnect.c       插件入口：flight loop、数据读取/发送、玩家管理
    json.c / json.h  轻量 JSON 解析/序列化助手
    net.c / net.h    TCP 跨平台封装（WinSock / POSIX）
    csl.c / csl.h    CSL 模型加载与 XPLMInstance 实例渲染
  CMakeLists.txt     跨平台构建
  Makefile           Linux/macOS 简单构建
  PROTOCOL.md        TCP JSON 协议文档
  README.md          本文件
  CSL/               存放 CSL 模型包（如 X-CSL），见下文
```

## 编译要求

- X-Plane SDK **已内置**于 `XConnect/SDK/`（XPSDK 43.0，含头文件与
  `XPLM_64.lib`），本地与 CI 均无需额外下载
- Windows：Visual Studio / Build Tools；Linux：gcc；macOS：clang

### CMake 构建（推荐，SDK 自动指向仓库内置目录）

```bash
cmake -B build
cmake --build build --config Release
# 产物: build/XConnect.xpl
```

如需使用其他 SDK：`cmake -B build -DXPlaneSDK=<路径>`

### 直接用 MSVC 编译（CMake 找不到 VS 实例时）

某些 VS 预览版不向 vswhere 注册，`cmake -G "Visual Studio ..."` 会报
"could not find any instance"。此时先用 `VsDevCmd.bat` 进环境再直接调 `cl`：

```bat
call "D:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat" -arch=x64
cl /nologo /LD /O2 /W3 /utf-8 /MT /DXPLM200 /DXPLM210 /DXPLM300 /DXPLM400 ^
   /DXPLM410 /DXPLM420 /DXPLM430 /D_CRT_SECURE_NO_WARNINGS /DIBM ^
   /I SDK\CHeaders\XPLM /I src src\xconnect.c src\datarefs.c src\players.c ^
   src\labels.c src\json.c src\net.c src\csl.c ^
   /link /LIBPATH:SDK\Libraries\Win XPLM_64.lib ws2_32.lib /OUT:build\XConnect.xpl
```

### GitHub Action（一键编译 + 上传）

仓库已内置 `.github/workflows/build-xconnect.yml`：

1. 把项目推送到 GitHub 仓库（需 `git init` + 首次 push）
2. Action 在 `ubuntu-latest`（gcc）与 `windows-latest`（MSVC）自动编译
3. 编译产物 `XConnect.xpl` 作为 artifact 上传，可直接下载

SDK 已随仓库提交，无需配置任何 secrets。

### Linux 简单构建

```bash
make SDK=SDK
# 产物: XConnect.xpl
```

> 注：`Makefile` 基于旧 SDK 布局编写；用内置 SDK（430）请改用 CMake 构建。

## SDK 版本适配

- 已适配 **XPSDK 43.0**（`XPLMCreateInstance` + `XPLMDrawInfo_t` 新实例化 API、
  `XPLM_64.lib` 命名、头文件拆分无 `XPLM.h`）
- 源码含双版本条件编译（`kXPLM_Version >= 430`），旧 SDK（300+）亦可编译

## 安装

1. 将编译产物 `XConnect.xpl` 拷贝到
   `X-Plane 目录/Resources/plugins/XConnect/`
   （Windows 上目录名任意，`.xpl` 为 64 位插件文件）
2. 将 CSL 模型包放入 `Resources/plugins/XConnect/CSL/`，目录结构（X-CSL 风格）：
   - `CSL/{ICAO}/xsb_aircraft.txt` + `CSL/{ICAO}/*.obj`
     （一架飞机为**复合模型**，一个 `OBJ8_AIRCRAFT` 变体由多个 OBJ 组成，
     如 A346 = `A346fan.obj`（发动机/风扇）+ `A346_{涂装}.obj`（机身）；
     涂装由 `AIRLINE`/`LIVERY` 行标识，按 airline 变体码匹配；
     有些机队包把尾号写进模型名/变体名，也能按尾号精确匹配）
   - 回退：`CSL/{family}/{ICAO}.obj` 或 `CSL/{ICAO}.obj` 单文件
3. 启动 X-Plane，插件自动监听 TCP 端口 **51001**
4. 启动客户端并连接

> CSL 模型包可从 fly.vatsim.net / X-CSL 分发获取（自由使用协议因包而异）。

## 客户端配合

插件启动后等待客户端连接。客户端需要：

- 连接 `127.0.0.1:51001`
- 接收 `connected` 与 `flight_data` 消息（本机飞行数据，5Hz）
- 发送 `Send:OPD:FSD9` 推送在线玩家（含位置/姿态/机型 ICAO）→ 插件渲染 CSL
- 发送 `Send:ATC` 推送管制列表（可附带，用于标注）
- 断开连接时插件清空已渲染模型

完整报文见 `PROTOCOL.md`。仓库根目录现有 Python 客户端可参考
`tools/xconnect_bridge.py`（将 XFlySim-Client 的 FSD 数据转发给插件）。

## 查机模私有 dataref（填 radio.json 用）

第三方机模（ToLiss / FlightFactor / Fenix）把无线电频率放在私有 dataref 里，
通用 `sim/cockpit2/radios/*` 读不到。要找出它们的真实名字：

```bash
# 1. 启动 X-Plane 并加载目标机模（必须进飞行场景，否则私有 dataref 未注册）
# 2. 跑工具（会自动发 Send:SCAN 让插件实时枚举）
python tools/dref_find.py
python tools/dref_find.py AirbusFBW        # 只看某前缀
python tools/dref_find.py com VHF audio    # 多关键字（名字或值命中）
```

它打印 `名字 + 当前值 + 是否可写`。原始数据在
`Resources/plugins/XConnect/xconnect.log` 的 `dref:` 行。

> ⚠️ **不要用 `Resources/plugins/DataRefs.txt`**。实测那只是 Laminar 原生条目的
> 静态快照（`AirbusFBW` / `laminar/B738` 匹配数均为 0）。私有 dataref 在加载
> `.acf` 时才注册到 XPLM，只有运行时枚举才可见。
>
> 挑选技巧：同型机常有通用与私有两套 dataref，**值和你座舱显示一致的那个**
> 才是机模真正在用的 —— 别只看名字像就填进 `radio.json`。

## 单元测试（不需要 X-Plane）

`test/csl_match_test.c` 用桩替换 XPLM 加载/实例 API，在临时目录造 X-CSL 包，
断言四级匹配与机型别名真的按预期选中变体（9 个用例）：

```
cl /nologo /MT /utf-8 /DXPLM200 ... /DXPLM430 /I<SDK>/CHeaders/XPLM /Isrc \
   -Fobuild/ -Febuild/csl_match_test.exe test/csl_match_test.c src/csl.c
build\csl_match_test.exe build\csltest
```

输出形如 `csl: B738 -> pack B738 variant CCA/B738_CCA_B1234 match=REG`。

## 备注

- 渲染使用 `XPLMInstance`（实例化模型），性能开销低，支持大量玩家
- 玩家模型动画（襟翼/起落架/灯光/应答机）由 CSL 模型声明的 dataref 驱动，
  遵循 XPMP2 / X-CSL 约定
- 插件版权：GPL-3.0（与 XFlySim-Client 一致）