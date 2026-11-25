#include "ini.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 单文件大小上限：aircraft.cfg 通常几 KB，个别带超长 description 的能到
 * 上百 KB；给 1MB 足够且不会因误传大文件耗尽内存。 */
#define INI_MAX_SIZE (1024 * 1024)

typedef struct {
    size_t off;    /* 行首偏移 */
    size_t len;    /* 行长（不含换行符） */
} IniLine;

struct Ini {
    char*     buf;
    size_t    len;
    IniLine*  lines;     /* 行索引 */
    int       nlines;

    int   cur;           /* 当前节在节表中的起始行（-1 = 文件头，无节） */
    int   cur_end;       /* 当前节结束行（不含），nlines 表示到文件尾 */

    /* 键值返回缓冲：ini_get 需要返回去引号/去注释后的**新**字符串，
     * 但不能破坏原缓冲（那样 rewind 就废了）。 */
    char  val[512];
};

static char* read_all(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    long n;
    char* buf;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n < 0 || n > INI_MAX_SIZE) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    buf = (char*)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[n] = '\0';
    *out_len = (size_t)n;
    fclose(f);
    return buf;
}

/* 建行索引。 */
static int build_index(Ini* ini) {
    size_t i = 0;
    int cap = 256;
    IniLine* arr = (IniLine*)malloc(sizeof(IniLine) * (size_t)cap);
    int n = 0;

    if (!arr) return -1;
    while (i < ini->len) {
        size_t start = i;
        size_t end;
        while (i < ini->len && ini->buf[i] != '\n') i++;
        end = i;
        /* 去行尾 \r */
        if (end > start && ini->buf[end - 1] == '\r') end--;
        if (n >= cap) {
            IniLine* bigger;
            cap *= 2;
            bigger = (IniLine*)realloc(arr, sizeof(IniLine) * (size_t)cap);
            if (!bigger) { free(arr); return -1; }
            arr = bigger;
        }
        arr[n].off = start;
        arr[n].len = end - start;
        n++;
        if (i < ini->len && ini->buf[i] == '\n') i++;
    }
    ini->lines = arr;
    ini->nlines = n;
    return 0;
}

Ini* ini_open(const char* path) {
    Ini* ini;
    size_t len = 0;
    char* buf;

    if (!path || !path[0]) return NULL;
    buf = read_all(path, &len);
    if (!buf) return NULL;

    /* 去掉 UTF-8 BOM：带 BOM 时第一行 "[VERSION]" 会变成
     * "\xEF\xBB\xBF[VERSION]"，节名匹配全部失效、机型目录变空。 */
    if (len >= 3 && (unsigned char)buf[0] == 0xEF &&
        (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) {
        memmove(buf, buf + 3, len - 2);
        len -= 3;
    }

    ini = (Ini*)calloc(1, sizeof(Ini));
    if (!ini) { free(buf); return NULL; }
    ini->buf = buf;
    ini->len = len;
    ini->cur = -1;
    ini->cur_end = 0;

    if (build_index(ini) != 0) {
        free(ini->buf);
        free(ini);
        return NULL;
    }
    return ini;
}

void ini_close(Ini* ini) {
    if (!ini) return;
    free(ini->lines);
    free(ini->buf);
    free(ini);
}

/* 取第 i 行的去空白起止（不修改缓冲，返回长度）。 */
static void line_trim(Ini* ini, int i, const char** pstart, size_t* plen) {
    const char* s = ini->buf + ini->lines[i].off;
    size_t n = ini->lines[i].len;
    while (n > 0 && (*s == ' ' || *s == '\t')) { s++; n--; }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    *pstart = s;
    *plen = n;
}

/* 取第 i 行的节名（若该行是 [NAME]）。成功返回 1，名字写入 out。 */
static int line_section(Ini* ini, int i, char* out, size_t cap) {
    const char* s;
    size_t n;
    size_t k;

    line_trim(ini, i, &s, &n);
    if (n < 2 || s[0] != '[') return 0;
    if (s[n - 1] != ']') return 0;
    n -= 2;
    s++;
    while (n > 0 && (*s == ' ' || *s == '\t')) { s++; n--; }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    k = n < cap - 1 ? n : cap - 1;
    memcpy(out, s, k);
    out[k] = '\0';
    return 1;
}

void ini_rewind(Ini* ini) {
    if (!ini) return;
    ini->cur = -1;
    ini->cur_end = ini->nlines;
}

const char* ini_next_section(Ini* ini) {
    static char name[64];
    int i;
    if (!ini) return NULL;

    /* 从当前节末尾继续找（首次调用时 cur=-1，cur_end=nlines） */
    i = (ini->cur < 0) ? 0 : ini->cur_end;
    for (; i < ini->nlines; i++) {
        if (line_section(ini, i, name, sizeof(name))) {
            int j = i + 1;
            /* 本节到下一个节头之前结束 */
            while (j < ini->nlines) {
                char tmp[64];
                if (line_section(ini, j, tmp, sizeof(tmp))) break;
                j++;
            }
            ini->cur = i;
            ini->cur_end = j;
            return name;
        }
    }
    ini->cur = -2;   /* 已到结尾 */
    ini->cur_end = ini->nlines;
    return NULL;
}

int ini_get(Ini* ini, const char* key, char* out, int cap) {
    int i;
    size_t klen;

    if (!ini || !key || !out || cap < 1) return 0;
    out[0] = '\0';
    klen = strlen(key);

    /* 无节状态（文件头，cur=-1）：扫描 0..第一个节头；
     * 有节状态：扫描 cur+1 .. cur_end。 */
    {
        int start = (ini->cur < 0) ? 0 : ini->cur + 1;
        int end = ini->cur_end;

        for (i = start; i < end; i++) {
            const char* s;
            size_t n;
            const char* eq;
            size_t k;

            line_trim(ini, i, &s, &n);
            if (n == 0 || s[0] == ';') continue;
            if (n >= 2 && s[0] == '/' && s[1] == '/') continue;

            /* 找 '='（不越过行尾）。
             * 注意 MSFS 的 UserCfg.opt 用的是 `Key "value"`（空格分隔、无等号），
             * 而 aircraft.cfg 用 `key = value`。两种都要支持：
             *   - 有 '='  → 键名 = '=' 之前的部分
             *   - 无 '='  → 键名 = 第一个引号/空白之前的部分，值从该处开始 */
            eq = (const char*)memchr(s, '=', n);
            {
                const char* vstart;
                size_t kend;

                if (eq) {
                    vstart = eq + 1;
                    kend = (size_t)(eq - s);
                } else {
                    /* 无等号形式：键名取到第一个空白或引号为止 */
                    size_t m = 0;
                    while (m < n && s[m] != ' ' && s[m] != '\t' && s[m] != '"')
                        m++;
                    if (m == 0 || m >= n) continue;   /* 该行没有值 */
                    vstart = s + m;
                    kend = m;
                }

                /* 比较键名（去尾空白、忽略大小写） */
                {
                    size_t kn = kend;
                    while (kn > 0 && (s[kn - 1] == ' ' || s[kn - 1] == '\t')) kn--;
                    if (kn != klen) continue;
                    for (k = 0; k < kn; k++) {
                        if (toupper((unsigned char)s[k]) !=
                            toupper((unsigned char)key[k])) break;
                    }
                    if (k != kn) continue;
                }

                /* 取值：去引号、去行尾注释 */
                {
                    const char* v = vstart;
                    size_t vn = n - (size_t)(vstart - s);
                    int in_quote = 0;
                    size_t m;
                    size_t take;

                    while (vn > 0 && (*v == ' ' || *v == '\t')) { v++; vn--; }

                    /* 去行尾注释（引号内的 ; 与 // 不算） */
                    for (m = 0; m < vn; m++) {
                        if (v[m] == '"') in_quote = !in_quote;
                        else if (!in_quote &&
                                 (v[m] == ';' ||
                                  (v[m] == '/' && m + 1 < vn && v[m + 1] == '/'))) {
                            vn = m;
                            break;
                        }
                    }
                    while (vn > 0 && (v[vn - 1] == ' ' || v[vn - 1] == '\t')) vn--;

                    /* 去外层引号 */
                    if (vn >= 2 && v[0] == '"' && v[vn - 1] == '"') { v++; vn -= 2; }

                    take = vn < (size_t)(cap - 1) ? vn : (size_t)(cap - 1);
                    memcpy(out, v, take);
                    out[take] = '\0';
                    return 1;
                }
            }
        }
    }
    return 0;
}

int ini_goto(Ini* ini, const char* section) {
    int saved_cur, saved_end;

    if (!ini || !section) return 0;

    /* 先试当前节（连续 ini_get 时性能更好） */
    if (ini->cur >= 0) {
        char cur_name[64];
        if (line_section(ini, ini->cur, cur_name, sizeof(cur_name)) &&
            _stricmp(cur_name, section) == 0) {
            return 1;
        }
    }

    saved_cur = ini->cur;
    saved_end = ini->cur_end;
    ini_rewind(ini);
    {
        /* ini_next_section 返回指针指向它自己的静态缓冲，必须用返回值比较，
         * 不能用本地数组（早期版本用未初始化的本地 name 比较，永远不匹配，
         * 导致 aircraft.cfg 的 [GENERAL] 段读不到 ICAO，机型目录全空）。 */
        const char* got;
        while ((got = ini_next_section(ini)) != NULL) {
            if (_stricmp(got, section) == 0) return 1;
        }
    }
    /* 找不到：游标复位，调用方后续 ini_get 仍在原节 */
    ini->cur = saved_cur;
    ini->cur_end = saved_end;
    return 0;
}

int ini_read_global(const char* path, const char* key, char* out, int cap) {
    Ini* ini = ini_open(path);
    int ok = 0;

    if (!ini) return 0;
    if (cap > 0) out[0] = '\0';

    /* UserCfg.opt 是顶层键（无节）+ 后面有 [Video] 等节。
     * 把 cur 置为 -1、cur_end 收窄到第一个节头，正好覆盖"文件头"区域。 */
    ini_rewind(ini);
    {
        char name[64];
        int i, first = ini->nlines;
        for (i = 0; i < ini->nlines; i++) {
            if (line_section(ini, i, name, sizeof(name))) { first = i; break; }
        }
        ini->cur = -1;
        ini->cur_end = first;
    }
    ok = ini_get(ini, key, out, cap);
    ini_close(ini);
    return ok;
}