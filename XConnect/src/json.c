#include "json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 内部解析器 ---------------- */

typedef struct {
    const char* p;
    int fail;
} ParseCtx;

static void skip_ws(ParseCtx* c) {
    while (*c->p && isspace((unsigned char)*c->p)) c->p++;
}

static Json* node(JsonType t) {
    Json* j = (Json*)calloc(1, sizeof(Json));
    if (j) j->type = t;
    return j;
}

static Json* parse_value(ParseCtx* c);

/* "..." 字符串：返回新分配的字符串 */
static char* parse_string(ParseCtx* c) {
    const char* start;
    size_t cap = 64, len = 0;
    char* out = (char*)malloc(cap);
    if (!out) { c->fail = 1; return NULL; }
    c->p++; /* 跳过 " */
    start = c->p;
    while (*c->p && *c->p != '"') {
        if (*c->p == '\\') {
            /* 简单处理 \\ \" \/ \n \t \r \u 不做 unicode 转义（原样保留），
               调用方可接受 UTF-8 直接字节。 */
            c->p++;
            if (!*c->p) break;
        }
        if (len + 1 >= cap) {
            cap *= 2;
            char* n = (char*)realloc(out, cap);
            if (!n) { free(out); c->fail = 1; return NULL; }
            out = n;
        }
        out[len++] = *c->p;
        c->p++;
    }
    if (*c->p != '"') { free(out); c->fail = 1; return NULL; }
    out[len] = '\0';
    (void)start;
    c->p++;
    return out;
}

static Json* parse_array(ParseCtx* c) {
    Json* arr = node(JSON_ARR);
    Json** tail = &arr->child;
    if (!arr) { c->fail = 1; return NULL; }
    c->p++; /* [ */
    skip_ws(c);
    if (*c->p == ']') { c->p++; return arr; }
    while (1) {
        Json* el = parse_value(c);
        if (!el || c->fail) { json_free(arr); c->fail = 1; return NULL; }
        *tail = el;
        tail = &el->next;
        skip_ws(c);
        if (*c->p == ',') { c->p++; skip_ws(c); continue; }
        if (*c->p == ']') { c->p++; return arr; }
        json_free(arr);
        c->fail = 1;
        return NULL;
    }
}

static Json* parse_object(ParseCtx* c) {
    Json* obj = node(JSON_OBJ);
    Json** tail = &obj->child;
    if (!obj) { c->fail = 1; return NULL; }
    c->p++; /* { */
    skip_ws(c);
    if (*c->p == '}') { c->p++; return obj; }
    while (1) {
        char* key;
        Json* val;
        skip_ws(c);
        if (*c->p != '"') { json_free(obj); c->fail = 1; return NULL; }
        key = parse_string(c);
        if (!key) { json_free(obj); c->fail = 1; return NULL; }
        skip_ws(c);
        if (*c->p != ':') { free(key); json_free(obj); c->fail = 1; return NULL; }
        c->p++;
        skip_ws(c);
        val = parse_value(c);
        if (!val || c->fail) { free(key); json_free(obj); c->fail = 1; return NULL; }
        val->key = key;
        *tail = val;
        tail = &val->next;
        skip_ws(c);
        if (*c->p == ',') { c->p++; continue; }
        if (*c->p == '}') { c->p++; return obj; }
        json_free(obj);
        c->fail = 1;
        return NULL;
    }
}

static Json* parse_value(ParseCtx* c) {
    skip_ws(c);
    if (!*c->p) { c->fail = 1; return NULL; }
    if (*c->p == '{') return parse_object(c);
    if (*c->p == '[') return parse_array(c);
    if (*c->p == '"') {
        Json* j = node(JSON_STR);
        if (!j) { c->fail = 1; return NULL; }
        j->sval = parse_string(c);
        if (!j->sval) { free(j); c->fail = 1; return NULL; }
        return j;
    }
    if (strncmp(c->p, "true", 4) == 0) {
        c->p += 4;
        Json* j = node(JSON_BOOL);
        if (!j) { c->fail = 1; return NULL; }
        j->bval = 1;
        return j;
    }
    if (strncmp(c->p, "false", 5) == 0) {
        c->p += 5;
        Json* j = node(JSON_BOOL);
        if (!j) { c->fail = 1; return NULL; }
        j->bval = 0;
        return j;
    }
    if (strncmp(c->p, "null", 4) == 0) {
        c->p += 4;
        return node(JSON_NULL);
    }
    /* 数字 */
    {
        char* end;
        double d = strtod(c->p, &end);
        if (end == c->p) { c->fail = 1; return NULL; }
        c->p = end;
        Json* j = node(JSON_NUM);
        if (!j) { c->fail = 1; return NULL; }
        j->num = d;
        return j;
    }
}

/* ---------------- 公开 API ---------------- */

Json* json_parse(const char* text) {
    ParseCtx c;
    Json* j;
    c.p = text;
    c.fail = 0;
    j = parse_value(&c);
    if (!j || c.fail) {
        json_free(j);
        return NULL;
    }
    return j;
}

void json_free(Json* j) {
    if (!j) return;
    json_free(j->child);
    json_free(j->next);
    free(j->key);
    free(j->sval);
    free(j);
}

const Json* json_get(const Json* obj, const char* key) {
    const Json* it;
    if (!obj || obj->type != JSON_OBJ) return NULL;
    for (it = obj->child; it; it = it->next)
        if (it->key && strcmp(it->key, key) == 0) return it;
    return NULL;
}

const Json* json_at(const Json* arr, int i) {
    const Json* it;
    int n = 0;
    if (!arr || arr->type != JSON_ARR) return NULL;
    for (it = arr->child; it; it = it->next) {
        if (n == i) return it;
        n++;
    }
    return NULL;
}

int json_len(const Json* j) {
    int n = 0;
    const Json* it;
    if (!j || (j->type != JSON_ARR && j->type != JSON_OBJ)) return 0;
    for (it = j->child; it; it = it->next) n++;
    return n;
}
