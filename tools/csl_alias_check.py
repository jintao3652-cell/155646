"""校验 XConnect 机型别名表（csl.c::kTypeAlias）与本地 CSL 库是否一致。

用法:
    python tools/csl_alias_check.py [CSL 目录]

默认 CSL 目录为 X-Plane 12 插件下的 CSL/。检查项：
  1. 别名目标包里是否真有 xsb_aircraft.txt（打错字/包被删都会报出来）；
  2. 源型号是否本身就有 CSL 包（有的话别名是多余的，仅提示）；
  3. 表内是否有重复源型号（C 侧按顺序匹配，重复等于后一条失效）。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

DEFAULT_CSL = Path(
    r"D:\steam\steamapps\common\X-Plane 12\Resources\plugins\XConnect\CSL"
)
CSL_C = Path(__file__).resolve().parent.parent / "XConnect" / "src" / "csl.c"

PAIR = re.compile(r'\{\s*"([A-Z0-9\-]{2,6})"\s*,\s*"([A-Z0-9\-]{2,6})"\s*\}')


def parse_aliases(src: Path) -> list[tuple[str, str]]:
    text = src.read_text(encoding="utf-8", errors="replace")
    start = text.find("kTypeAlias[]")
    if start < 0:
        raise SystemExit("csl.c 中找不到 kTypeAlias 表")
    end = text.find("};", start)
    return PAIR.findall(text[start:end])


def main() -> int:
    csl = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_CSL
    packs = {
        p.name.upper()
        for p in csl.iterdir()
        if p.is_dir() and (p / "xsb_aircraft.txt").is_file()
    } if csl.is_dir() else set()

    aliases = parse_aliases(CSL_C)
    print(f"别名条目: {len(aliases)}  CSL 库: {csl}（{len(packs)} 个包）")

    seen: dict[str, str] = {}
    bad_target, redundant, dup = [], [], []
    for src, dst in aliases:
        if src in seen:
            dup.append(f"{src}（{seen[src]} / {dst}）")
        seen[src] = dst
        if packs and dst not in packs:
            bad_target.append(f"{src} -> {dst}")
        if packs and src in packs and src != dst:
            redundant.append(f"{src} -> {dst}（源型号已有包）")

    for title, rows in (("目标包不存在", bad_target),
                        ("源型号已有 CSL 包（别名多余）", redundant),
                        ("重复源型号", dup)):
        if rows:
            print(f"\n[{title}] {len(rows)}")
            for r in rows:
                print("  -", r)

    ok = not (bad_target or dup)
    print("\n结果:", "OK" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
