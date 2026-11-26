/* 轻量 JSON 助手：递归下降解析器 + 查询 API。
 * 仅实现 XConnect 所需子集（对象/数组/字符串/数字/布尔/null）。
 * 解析失败返回 NULL；所有字符串 UTF-8。
 */
#ifndef XCONNECT_JSON_H
#define XCONNECT_JSON_H

typedef enum {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUM,
    JSON_STR,
    JSON_ARR,
    JSON_OBJ
} JsonType;

typedef struct Json {
    struct Json* next;   /* 对象成员 / 数组元素 链 */
    char* key;           /* 对象成员名；数组元素为 NULL */
    JsonType type;
    double num;
    int bval;
    char* sval;
    struct Json* child;  /* 对象：首个成员；数组：首个元素 */
} Json;

/* 解析一段 JSON 文本（text 可含尾随空白）。失败返回 NULL。 */
Json* json_parse(const char* text);

/* 释放整棵树 */
void json_free(Json* j);

/* 对象成员查找（线性），无则返回 NULL */
const Json* json_get(const Json* obj, const char* key);

/* 数组第 i 个元素，越界返回 NULL */
const Json* json_at(const Json* arr, int i);

/* 元素个数（数组），对象返回 0 */
int json_len(const Json* j);

#endif
