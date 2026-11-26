# 贡献指南

## 环境

- Python 3.12/3.13（不建议 3.14，打包依赖兼容性未验证）
- Node.js 20+（仅 webui）
- Windows：VS 2022/2026（MSVC）、CMake
- X-Plane SDK（`XConnect/SDK` 已随仓库）
- MSFS SimConnect SDK（仅 MConnect）
- 支持矩阵见 [docs/SUPPORT_MATRIX.md](docs/SUPPORT_MATRIX.md)

## 一键开发环境

```powershell
# Windows
.\scripts\dev.ps1
```

```sh
# Linux / macOS
./scripts/dev.sh
```

脚本会创建 `.venv`、安装依赖、跑单元测试与 selftest。

## 常用命令

```bash
# Python 单元测试（stdlib unittest，无需 pytest）
python -m unittest discover -s tests -t . -v

# 客户端自测（68 项）
python main.py --selftest

# 诊断导出（自动脱敏）
python -m xfly_client.diagnostics --out diag.json

# XConnect（Windows）
cmake -S XConnect -B build/xconnect -A x64
cmake --build build/xconnect --config Release

# MConnect（Windows）
cmake -S MConnect -B build/mconnect -A x64
cmake --build build/mconnect --config Release

# Web UI
cd webui
npm ci
npm run build
```

## 代码约定

- C：C99；插件导出保持 `/MT` 静态 CRT；不要跨线程调用 XPLM/SimConnect。
- Python：标准库优先；新增协议字段必须**向后兼容**（未知字段忽略）。
- 日志：默认 info；原始 FSD/每帧数据只在 debug 下；任何日志先 `redact()`。
- 桥协议改动：更新 [docs/BRIDGE_PROTOCOL.md](docs/BRIDGE_PROTOCOL.md)，
  并在 XConnect/MConnect 两侧同时实现。
- 不提交 `fsd.log`、`xconnect.log`、配置里的密码、构建产物。

## PR 检查清单

- [ ] `python -m unittest discover -s tests -t . -v` 通过
- [ ] `python main.py --selftest` 通过
- [ ] XConnect/MConnect 编译通过（涉及 C 时）
- [ ] 新日志不含密码/敏感信息
- [ ] 协议/配置改动有文档或迁移路径
- [ ] CSL 匹配/位置插值等核心逻辑有测试或回放用例

## 反馈问题

请附：
- 平台/模拟器版本、客户端/插件版本（握手日志）
- `xfly_client.diagnostics` 导出的 JSON
- 复现步骤；不要直接贴含密码的原始日志
