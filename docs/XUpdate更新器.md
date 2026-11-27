# XUpdate 更新器（客户端 / XConnect / MConnect）

`XUpdate.exe` 是**独立**的更新器：主程序、XConnect、MConnect 插件的
版本检查与更新全部走它。CSL / FSLTL / AIG 属于独立资源层，
**不在本更新器内下载或校验**（见 `资源层更新设计.md`）。

源码：`xupdate.py`（入口） + `xfly_client/updater.py`（核心）；
打包：`build_exe.ps1` 会同时产出 `dist/XFlySimClient.exe` 与 `dist/XUpdate.exe`。

## 1. 为什么独立成一个 exe

- 客户端自己不能覆盖正在运行的 `XFlySimClient.exe`；独立进程可以在客户端
  退出后完成替换。
- 插件 `XConnect.xpl` / `MConnect.exe` 可能被模拟器占用，需要单独检测并提示
  "先关闭模拟器再更新"，而不是让客户端更新流程半途失败。
- 更新失败必须能整体回滚；把下载/校验/替换/回滚放在一个短生命周期进程里，
  成功率与可诊断性都更好。

## 2. 命令行

源码运行把 `XUpdate.exe` 换成 `python xupdate.py`。**默认只检查，不写磁盘**。

```bat
XUpdate.exe --check
XUpdate.exe --apply
XUpdate.exe --check --json
XUpdate.exe --check --manifest https://your.host/xflysim/update/manifest.json
XUpdate.exe --apply --component client --xplane-dir "D:\steam\steamapps\common\X-Plane 12"
XUpdate.exe --sample-manifest
```

| 参数 | 含义 |
|---|---|
| `--check` | 只输出差异（默认），不改动任何文件 |
| `--apply` | 下载校验后备份+原子替换，失败整体回滚 |
| `--manifest <路径或 URL>` | 清单来源；默认环境变量 `XFLYSIM_UPDATE_MANIFEST`，再默认 `<exe目录>/update/manifest.json` |
| `--component <列表>` | `client,xconnect,mconnect` 或 `all`（默认 all） |
| `--base-dir <目录>` | 客户端根目录（默认 exe 所在目录；源码运行为仓库根） |
| `--xplane-dir <目录>` | X-Plane 根目录，也可设环境变量 `XPLANE_ROOT` |
| `--mconnect-dir <目录>` | MConnect 根目录，默认客户端目录（清单内路径相对它） |
| `--force` | 忽略本地哈希，强制重新下载全部文件 |
| `--timeout <秒>` | 单文件下载超时（默认 30） |
| `--json` | 输出机器可解析报告（日志走 stdout，错误也是 JSON） |
| `--sample-manifest` | 打印清单模板后退出 |
| `--apply-package <JOB.json>` | 客户端自更新专用：按作业文件等客户端退出后替换 exe 并重启（由客户端拉起，不需要手工调用） |
| `--wait-timeout <秒>` | `--apply-package` 等客户端退出的上限（默认 60） |
| `--quiet` | 不输出到标准输出（`--apply-package` 日志仍写入文件） |
| `--software-api <URL>` | 飞行员中心 API 根地址（或 `XFLYSIM_SOFTWARE_API`）；指定即走软件中心模式 |
| `--software-uuid <uuid>` | 软件标识（或 `XFLYSIM_SOFTWARE_UUID`） |
| `--software-legacy` | 用 `/software/latest` 旧路径代替 `/softwareVersion/latest` |
| `--software-history` | 只打印版本记录，不检查/更新 |

退出码：`0` = 无需更新/应用成功；`1` = 检查到有更新（仅 `--check`）；
`2` = 清单/下载/写入错误；`3` = 前置条件不满足（根目录未指定、文件被占用）。

## 3. 清单格式

顶层：

```json
{
  "schema": "xflysim-update-manifest",
  "schema_version": 1,
  "base_url": "https://your.host/xflysim/releases/v1/",
  "components": { "...": "..." }
}
```

每个组件：

| 字段 | 含义 |
|---|---|
| `version` | 目标版本字符串（`v30` / `1.2.3` 均可）；与已安装版本比较 |
| `root` | 说明性字段：`xplane` / `mconnect` / 空（client），实际路径由命令行/环境变量决定 |
| `files[]` | 文件列表，每项 `path` + `sha256` + `bytes`；可选 `url` 覆盖 `base_url` |

`path` 相对组件根目录，统一用 `/`。**禁止**绝对路径、盘符、`..`；
解析后还会用 realpath 确认没有通过符号链接逃出根目录。

完整模板：`XUpdate.exe --sample-manifest`。

### 目标根目录

| 组件 | 根目录 | 缺少时 |
|---|---|---|
| `client` | `--base-dir` / exe 所在目录 | 直接报错 |
| `xconnect` | `--xplane-dir` 或 `XPLANE_ROOT` | 退出码 3，提示指定 |
| `mconnect` | `--mconnect-dir` / `XFLYSIM_MCONNECT_DIR` / 客户端目录 | 回退客户端目录 |

## 4. 更新流程与安全边界

1. **版本比较**：先读 `<base>/xupdate-installed.json`；源码树里 client 还可回退
   `xfly_client/version.py::CLIENT_VERSION`。版本未知时不假设已更新，
   改为逐文件比对内容。
2. **增量判定**：每个文件检查 `bytes` 与 `sha256`，一致就标 `skip`，不下载。
   这是显式更新流程，允许哈希计算；启动客户端时绝不做这件事。
3. **下载**：URL = `base_url` + URL 编码后的 `path`（或文件自带 `url`）；
   支持 `http` / `https` / `file://`；下载后再次核对长度与 sha256。
4. **替换**：先备份为 `<目标>.xupdate-bak`，再写同目录临时文件并 `os.replace`
   原子替换。任何一步失败都按逆序恢复备份，并删除临时文件。
5. **占用检测**：Windows 上被客户端/模拟器占用的 `.exe` / `.xpl` 会在备份阶段
   抛 `PermissionError`，转成退出码 3 并提示关闭程序后重试。
6. **版本落盘**：全部成功后原子写 `xupdate-installed.json`。
7. **日志**：`logs/xupdate-YYYY-MM-DD.log`（北京时间分日、保留 7 天），与客户端
   `xflysim-*.log` 分开；URL 等敏感信息先经过 `redact()`。

`unmanaged`（用户自行安装的插件）边界与资源层一致：清单里没列的文件一律不碰；
备份文件也只保留一份 `.xupdate-bak`，便于用户手动回滚。

## 5. 软件中心模式（飞行员中心）

软件中心发布的是**一个包**，不是逐文件清单。指定 `--software-api` 后
本更新器改走下面四个接口，`--manifest` 被忽略：

| 接口 | 用途 |
|---|---|
| `GET /softwareVersion/latest/{uuid}` | 最新版本：`softwareName/version/logs/link/md5/createdTime` |
| `GET /softwareVersion/logs/{uuid}` | 历史记录：`records[]` |
| `GET /software/latest/{uuid}` / `GET /software/logs/{uuid}` | 旧路径别名（`--software-legacy`） |

`link` 是绝对 URL 时直接下载；相对路径按 API 根地址解析。下载后**先按
服务端 md5 校验**，再按文件内容（不看扩展名）决定落盘方式：

1. **非 zip**：直接放进根目录，文件名取下载链接末段；
2. **zip**：先解压，按包内相对路径放进根目录，不额外套一层以压缩包
   命名的子目录。

与清单模式共用安全边界：单包 ≤ 2 GB、拒绝绝对路径/盘符/`..` 的 zip
成员（zip slip）与符号链接逃逸、落盘前备份、失败整体回滚。版本号写入
`xupdate-installed.json` 的 `client` 字段，下次 `--check` 用 `version`
比较。

```bat
XUpdate.exe --software-api https://host/api --software-uuid <uuid> --check
XUpdate.exe --software-api https://host/api --software-uuid <uuid> --apply
XUpdate.exe --software-api https://host/api --software-uuid <uuid> --software-history --json
```

## 6. 服务端接入（清单模式）

服务端只需托管一个静态 `manifest.json` 和清单里列出的文件。建议：

- 客户端与插件分别打小包（`XFlySimClient.exe`、`webui/dist` 变更文件、
  `XConnect.xpl`、`MConnect.exe`），不要整包重发；
- 清单按版本目录发布（`.../releases/v1/`），新版本换目录、不改旧文件；
- `XUpdate.exe --check` 可由客户端在用户点"检查更新"时拉起；
  `--apply` 则在客户端退出后调用，避免自覆盖失败。

目前客户端 UI 尚未接入"检查更新"按钮；更新器本身已可独立使用。

> 客户端**自身**的整包更新走另一条更短的通道（服务端一个 JSON 接口 + 一个 exe
> 直链）：客户端下载校验后写作业文件、拉起 `XUpdate.exe --apply-package`，
> 由助手在客户端退出后替换并重启。见 `客户端在线更新.md`。

## 7. 测试

```powershell
python -m unittest tests.test_updater tests.test_software_update -v
```

覆盖：清单结构/路径逃逸/符号链接逃逸、版本比较、增量跳过、备份与原子替换、
下载失败整体回滚、X-Plane 根目录前置条件、CLI JSON 报告；软件中心
latest/logs 解析、md5 校验、zip/非 zip 落盘、zip slip 拒绝。
