/* MConnect — 轻量 INI 读取：解析 MSFS 的 aircraft.cfg / livery.cfg / UserCfg.opt。
 *
 * 只需要「按节读键」这一种能力，且这些文件里大量出现：
 *   - 注释：`;` 与 `//` 都可能出现（cfg 用 `;`，opt 用 `//`）
 *   - 引号：`title = "Boeing 747-8i Asobo" ; desc`
 *   - 同名键在多个 [FLTSIM.N] 节里重复
 * 因此不能用「一次性解析成字典」的思路，而是「按节范围 + 逐键扫描」。
 *
 * 实现要点：打开时建立行索引（偏移+长度），**不修改原缓冲**。
 * 这点很关键——早期版本用「把 \n 改写成 \0」的方式切行，导致 rewind 之后
 * 再次读取时第一个字符就是 \0，整个文件被当成一个空行，机型目录永远是 0 条。
 */
#ifndef MCONNECT_INI_H
#define MCONNECT_INI_H

typedef struct Ini Ini;

Ini* ini_open(const char* path);
void ini_close(Ini* ini);

/* 重置遍历游标到文件开头（section 置为"无节"）。 */
void ini_rewind(Ini* ini);

/* 前进到下一个节，返回节名（如 "GENERAL"、"FLTSIM.0"）；文件结束返回 NULL。
 * 返回的指针指向 Ini 内部缓冲，下次调用即失效。 */
const char* ini_next_section(Ini* ini);

/* 在**当前节**内查找键，把值写入 out（已去引号/注释/首尾空白）。
 * 找不到返回 0，找到返回 1。
 *
 * 为什么要求调用方给缓冲：早期版本返回指向 Ini 内部共享缓冲的指针，
 * 连续调用两次（如先读 title 再读 texture）第二个值会覆盖第一个，
 * 结果 title 变量里实际存的是 texture 的值——这个 bug 曾让整张机型目录
 * 的标题全变成涂装名。 */
int ini_get(Ini* ini, const char* key, char* out, int cap);

/* 定位到指定节名的第一个实例（如 "GENERAL"）。成功返回 1。 */
int ini_goto(Ini* ini, const char* section);

/* 便捷：读文件中某个键的值（用于 UserCfg.opt 的
 * `InstalledPackagesPath "L:\MSFS"`，该键在文件头、无节）。成功返回 1。 */
int ini_read_global(const char* path, const char* key, char* out, int cap);

#endif