# MConnect 外部机型档案（profiles.json）

MConnect 内置了 FBW / Fenix / PMDG / iniBuilds 等档案；若遇到新机模或需要覆盖
内置写法，在 **MConnect.exe 同目录**放一个 `profiles.json` 即可，无需重编译。

- 启动时自动读取；文件修改后 **2 秒内热加载**。
- 外部档案优先于内置表；同一机型匹配第一条。
- 重载不会释放旧档案（当前机型可能仍指向旧条目），内存占用很小。

## 结构

```json
{
  "aircraft": [
    {
      "id": "custom-a320",
      "match": "My A320|A20N",
      "caps": 31,
      "xpdr_code_rpn": "1 (>L:MY_XPDR_CODE_SET)",
      "xpdr_mode_rpn": ["...x6"],
      "xpdr_mode_read": "(L:MY_XPDR_MODE)",
      "xpdr_mode_native": [0, 1, 2, 3, 4, 5],
      "xpdr_code_read": "(A:TRANSPONDER CODE:1, Number)",
      "com_set_rpn": ["...x3"],
      "com_swap_rpn": "%d (>L:MY_COM_SWAP)",
      "com_xmit_rpn": "%d (>L:MY_COM_XMIT)",
      "note": "说明"
    }
  ]
}
```

字段：

| 字段 | 说明 |
|---|---|
| `id` / `match` | **必填**。`match` 用 `|` 分隔多个大小写不敏感子串（标题或 ICAO） |
| `caps` | 能力位；缺省/0 时按模板自动推导（码/模式/COM 设置/交换/发话） |
| `xpdr_code_rpn` | 应答机码 RPN；`%d` = 4 位八进制码 |
| `xpdr_mode_rpn[6]` | 规范模式 0..5 的 RPN 模板；`%d` = 规范枚举（也可直接写原生值） |
| `xpdr_mode_read` | 读取原生模式的计算式（WASM 桥） |
| `xpdr_mode_native[6]` | 规范枚举 → 原生值；-1 = 不支持 |
| `xpdr_code_read` | 读取应答机码的计算式 |
| `com_set_rpn[3]` | COM1/2/3 频率 RPN；`%d` = Hz |
| `com_swap_rpn` | 主备交换 RPN；`%d` = 台号 |
| `com_xmit_rpn` | 发射台选择 RPN；`%d` = 台号 |
| `note` | 日志备注 |

## 诊断

```bash
MConnect.exe --dump-profiles
```

会打印外部 + 内置档案；日志里 `profilejson: loaded N external profile(s)` 表示已加载。
