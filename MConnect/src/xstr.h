/* 安全字符串追加辅助。
 *
 * ## 要解决什么
 *
 * 到处都有这种写法：
 *     n += snprintf(buf + n, sizeof(buf) - n, "...");
 *
 * **它是错的**。`snprintf` 返回的是"若缓冲区足够大，本应写入的字符数"，
 * 截断时这个返回值**大于**实际可写空间。于是：
 *
 *   - 第一次写：n = 4090，剩 6 字节，写了 20 字节的内容 → 返回 20
 *   - n 变成 4110 > sizeof(buf)=4096
 *   - 下一次 `sizeof(buf) - n` = -14，转成 size_t 后是 **18446744073709551602**
 *   - snprintf(buf + 4110, 巨大值, ...) → **越界写，破坏堆栈**
 *
 * 这不是理论风险：`xconnect.c::refresh_csl_status()` 用 `char line[4096]`
 * 拼 `PLAYER_MAX(256)` 个 ICAO（每个最多 8 字符，加引号逗号约 12 字节），
 * 256 * 12 = 3072，看着够；但机型 ICAO 可能更长（"B738"→8 字节缓冲，
 * 实际最多 7 字符 + 引号 + 逗号 = 10），叠上 missing 那一半就是 2 倍，
 * **满员时确实会溢出**。MConnect 的 `mconnect.c` 是同一份逻辑。
 *
 * ## 用法
 *
 *     n = xstr_cat(n, snprintf(buf + n, xstr_rem(sizeof(buf), n), "..."),
 *                  (int)sizeof(buf));
 *
 * 或者用宏更省事：
 *
 *     XSTR_APPEND(buf, n, "...");
 *
 * 宏会自己算剩余空间、调用 snprintf、并把位置夹回 [0, cap-1]。
 */
#ifndef XSTR_H
#define XSTR_H

#include <stdio.h>

/* 取写指针时同样要防止 pos 越界（pos >= cap 时指向最后一个字节，
 * 配合 xstr_rem 返回的 1，实际只写 '\0'，安全）。 */
static int xstr_safe_off(int n, int cap) {
    if (n < 0) return 0;
    if (n >= cap) return cap > 0 ? cap - 1 : 0;
    return n;
}

/* 剩余可写空间：保证 >= 1（snprintf 需要至少 1 字节放 '\0'）。
 * n 已越界时返回 1 —— 于是 snprintf 只写 '\0'，不会越界。 */
static int xstr_rem(int cap, int n) {
    if (n < 0) return cap > 0 ? cap : 1;
    if (n >= cap) return 1;
    return cap - n;
}

/* 把 snprintf 的返回值折算成新的安全位置：夹在 [0, cap-1]。
 * 同时修正 snprintf 返回负数（编码错误）的情况。 */
static int xstr_cat(int base, int written, int cap) {
    int p;
    if (cap <= 0) return 0;
    if (written < 0) written = 0;
    p = base + written;
    if (p >= cap) return cap - 1;
    if (p < 0) return 0;
    return p;
}

/* 追加一个格式化片段：自动算剩余空间、自动夹位置。
 * buf 必须是数组（sizeof 才能拿到真实大小）。 */
#define XSTR_APPEND(buf, pos, ...)                                          \
    ((pos) = xstr_cat((pos),                                                 \
                      snprintf((buf) + xstr_safe_off((pos), (int)sizeof(buf)),\
                               xstr_rem((int)sizeof(buf), (pos)),            \
                               __VA_ARGS__),                                 \
                      (int)sizeof(buf)))

#endif /* XSTR_H */