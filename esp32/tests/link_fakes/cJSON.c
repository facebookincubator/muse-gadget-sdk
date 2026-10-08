/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "cJSON.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *cur;
    const char *end;
} parser_t;

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} out_t;

static char *xstrdup_len(const char *src, size_t len) {
    char *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, src, len);
    out[len] = '\0';
    return out;
}

static char *xstrdup(const char *src) {
    return src ? xstrdup_len(src, strlen(src)) : NULL;
}

static cJSON *new_item(int type) {
    cJSON *item = calloc(1, sizeof(*item));
    if (item) item->type = type;
    return item;
}

static void append_child(cJSON *parent, cJSON *child) {
    if (!parent->child) {
        parent->child = child;
        return;
    }
    cJSON *tail = parent->child;
    while (tail->next) tail = tail->next;
    tail->next = child;
    child->prev = tail;
}

static void skip_ws(parser_t *p) {
    while (p->cur < p->end && isspace((unsigned char)*p->cur)) p->cur++;
}

static bool consume(parser_t *p, char ch) {
    skip_ws(p);
    if (p->cur >= p->end || *p->cur != ch) return false;
    p->cur++;
    return true;
}

static bool append_char(out_t *out, char ch) {
    if (out->len + 1 >= out->cap) {
        size_t new_cap = out->cap ? out->cap * 2 : 32;
        char *nb = realloc(out->buf, new_cap);
        if (!nb) return false;
        out->buf = nb;
        out->cap = new_cap;
    }
    out->buf[out->len++] = ch;
    out->buf[out->len] = '\0';
    return true;
}

static bool append_str(out_t *out, const char *s) {
    while (s && *s) {
        if (!append_char(out, *s++)) return false;
    }
    return true;
}

static bool append_escaped_string(out_t *out, const char *s) {
    if (!append_char(out, '"')) return false;
    for (; s && *s; s++) {
        switch (*s) {
            case '"':
                if (!append_str(out, "\\\"")) return false;
                break;
            case '\\':
                if (!append_str(out, "\\\\")) return false;
                break;
            case '\b':
                if (!append_str(out, "\\b")) return false;
                break;
            case '\f':
                if (!append_str(out, "\\f")) return false;
                break;
            case '\n':
                if (!append_str(out, "\\n")) return false;
                break;
            case '\r':
                if (!append_str(out, "\\r")) return false;
                break;
            case '\t':
                if (!append_str(out, "\\t")) return false;
                break;
            default:
                if ((unsigned char)*s < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)*s);
                    if (!append_str(out, buf)) return false;
                } else if (!append_char(out, *s)) {
                    return false;
                }
                break;
        }
    }
    return append_char(out, '"');
}

static char *parse_string_raw(parser_t *p) {
    skip_ws(p);
    if (p->cur >= p->end || *p->cur != '"') return NULL;
    p->cur++;

    out_t out = {0};
    while (p->cur < p->end) {
        char ch = *p->cur++;
        if (ch == '"') {
            if (!out.buf) return xstrdup("");
            return out.buf;
        }
        if (ch == '\\') {
            if (p->cur >= p->end) goto fail;
            ch = *p->cur++;
            switch (ch) {
                case '"':
                case '\\':
                case '/':
                    if (!append_char(&out, ch)) goto fail;
                    break;
                case 'b':
                    if (!append_char(&out, '\b')) goto fail;
                    break;
                case 'f':
                    if (!append_char(&out, '\f')) goto fail;
                    break;
                case 'n':
                    if (!append_char(&out, '\n')) goto fail;
                    break;
                case 'r':
                    if (!append_char(&out, '\r')) goto fail;
                    break;
                case 't':
                    if (!append_char(&out, '\t')) goto fail;
                    break;
                case 'u':
                    if (p->end - p->cur < 4) goto fail;
                    p->cur += 4;
                    if (!append_char(&out, '?')) goto fail;
                    break;
                default:
                    goto fail;
            }
        } else {
            if ((unsigned char)ch < 0x20 || !append_char(&out, ch)) goto fail;
        }
    }

fail:
    free(out.buf);
    return NULL;
}

static cJSON *parse_value(parser_t *p);

static cJSON *parse_object(parser_t *p) {
    if (!consume(p, '{')) return NULL;
    cJSON *object = new_item(cJSON_Object);
    if (!object) return NULL;

    skip_ws(p);
    if (p->cur < p->end && *p->cur == '}') {
        p->cur++;
        return object;
    }

    while (p->cur < p->end) {
        char *key = parse_string_raw(p);
        if (!key || !consume(p, ':')) {
            free(key);
            goto fail;
        }
        cJSON *value = parse_value(p);
        if (!value) {
            free(key);
            goto fail;
        }
        value->string = key;
        append_child(object, value);

        skip_ws(p);
        if (p->cur < p->end && *p->cur == ',') {
            p->cur++;
            continue;
        }
        if (p->cur < p->end && *p->cur == '}') {
            p->cur++;
            return object;
        }
        goto fail;
    }

fail:
    cJSON_Delete(object);
    return NULL;
}

static cJSON *parse_array(parser_t *p) {
    if (!consume(p, '[')) return NULL;
    cJSON *array = new_item(cJSON_Array);
    if (!array) return NULL;

    skip_ws(p);
    if (p->cur < p->end && *p->cur == ']') {
        p->cur++;
        return array;
    }

    while (p->cur < p->end) {
        cJSON *value = parse_value(p);
        if (!value) goto fail;
        append_child(array, value);

        skip_ws(p);
        if (p->cur < p->end && *p->cur == ',') {
            p->cur++;
            continue;
        }
        if (p->cur < p->end && *p->cur == ']') {
            p->cur++;
            return array;
        }
        goto fail;
    }

fail:
    cJSON_Delete(array);
    return NULL;
}

static cJSON *parse_literal(parser_t *p, const char *literal, int type) {
    size_t len = strlen(literal);
    skip_ws(p);
    if ((size_t)(p->end - p->cur) < len || memcmp(p->cur, literal, len) != 0) {
        return NULL;
    }
    p->cur += len;
    return new_item(type);
}

static cJSON *parse_value(parser_t *p) {
    skip_ws(p);
    if (p->cur >= p->end) return NULL;
    if (*p->cur == '{') return parse_object(p);
    if (*p->cur == '[') return parse_array(p);
    if (*p->cur == '"') {
        char *value = parse_string_raw(p);
        if (!value) return NULL;
        cJSON *item = new_item(cJSON_String);
        if (!item) {
            free(value);
            return NULL;
        }
        item->valuestring = value;
        return item;
    }
    if (*p->cur == 't') return parse_literal(p, "true", cJSON_True);
    if (*p->cur == 'f') return parse_literal(p, "false", cJSON_False);
    if (*p->cur == 'n') return parse_literal(p, "null", cJSON_NULL);
    return NULL;
}

cJSON *cJSON_ParseWithLength(const char *value, size_t buffer_length) {
    if (!value) return NULL;
    parser_t p = {.cur = value, .end = value + buffer_length};
    cJSON *root = parse_value(&p);
    skip_ws(&p);
    if (!root || p.cur != p.end) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

void cJSON_Delete(cJSON *item) {
    while (item) {
        cJSON *next = item->next;
        cJSON_Delete(item->child);
        free(item->valuestring);
        free(item->string);
        free(item);
        item = next;
    }
}

cJSON *cJSON_CreateObject(void) {
    return new_item(cJSON_Object);
}

cJSON *cJSON_CreateBool(int boolean) {
    return new_item(boolean ? cJSON_True : cJSON_False);
}

cJSON *cJSON_CreateNumber(double number) {
    cJSON *item = new_item(cJSON_Number);
    if (item) {
        item->valuedouble = number;
        item->valueint = (int)number;
    }
    return item;
}

cJSON *cJSON_AddItemToObject(cJSON *object, const char *name, cJSON *item) {
    if (!cJSON_IsObject(object) || !name || !item || item->string) return NULL;
    item->string = xstrdup(name);
    if (!item->string) return NULL;
    append_child(object, item);
    return item;
}

cJSON *cJSON_AddStringToObject(cJSON *object, const char *name, const char *string) {
    if (!cJSON_IsObject(object) || !name || !string) return NULL;
    cJSON *item = new_item(cJSON_String);
    if (!item) return NULL;
    item->string = xstrdup(name);
    item->valuestring = xstrdup(string);
    if (!item->string || !item->valuestring) {
        cJSON_Delete(item);
        return NULL;
    }
    append_child(object, item);
    return item;
}

cJSON *cJSON_AddBoolToObject(cJSON *object, const char *name, int boolean) {
    cJSON *item = cJSON_CreateBool(boolean);
    if (!item || !cJSON_AddItemToObject(object, name, item)) {
        cJSON_Delete(item);
        return NULL;
    }
    return item;
}

cJSON *cJSON_AddNumberToObject(cJSON *object, const char *name, double number) {
    cJSON *item = cJSON_CreateNumber(number);
    if (!item || !cJSON_AddItemToObject(object, name, item)) {
        cJSON_Delete(item);
        return NULL;
    }
    return item;
}

static bool print_value(out_t *out, const cJSON *item) {
    if (cJSON_IsString(item)) {
        return append_escaped_string(out, item->valuestring);
    }
    if (cJSON_IsTrue(item)) return append_str(out, "true");
    if (item && item->type == cJSON_False) return append_str(out, "false");
    if (item && item->type == cJSON_NULL) return append_str(out, "null");
    if (item && item->type == cJSON_Number) {
        char number[64];
        int n = snprintf(number, sizeof(number), "%.17g", item->valuedouble);
        return n > 0 && (size_t)n < sizeof(number) && append_str(out, number);
    }
    if (cJSON_IsObject(item)) {
        if (!append_char(out, '{')) return false;
        for (const cJSON *child = item->child; child; child = child->next) {
            if (child != item->child && !append_char(out, ',')) return false;
            if (!append_escaped_string(out, child->string)
                || !append_char(out, ':') || !print_value(out, child)) {
                return false;
            }
        }
        return append_char(out, '}');
    }
    return false;
}

char *cJSON_PrintUnformatted(const cJSON *item) {
    out_t out = {0};
    if (!print_value(&out, item)) goto fail;
    return out.buf;

fail:
    free(out.buf);
    return NULL;
}

int cJSON_PrintPreallocated(cJSON *item, char *buffer, const int length, const int format) {
    (void)format;
    char *json = cJSON_PrintUnformatted(item);
    if (!json) return 0;
    size_t len = strlen(json);
    int ok = length > 0 && len < (size_t)length;
    if (ok) memcpy(buffer, json, len + 1);
    free(json);
    return ok;
}

void cJSON_free(void *object) {
    free(object);
}

int cJSON_IsArray(const cJSON *item) {
    return item && item->type == cJSON_Array;
}

int cJSON_IsFalse(const cJSON *item) {
    return item && item->type == cJSON_False;
}

int cJSON_IsNull(const cJSON *item) {
    return item && item->type == cJSON_NULL;
}

int cJSON_IsObject(const cJSON *item) {
    return item && item->type == cJSON_Object;
}

int cJSON_IsString(const cJSON *item) {
    return item && item->type == cJSON_String;
}

int cJSON_IsTrue(const cJSON *item) {
    return item && item->type == cJSON_True;
}

cJSON *cJSON_GetObjectItem(const cJSON *object, const char *string) {
    if (!cJSON_IsObject(object) || !string) return NULL;
    for (cJSON *child = object->child; child; child = child->next) {
        if (child->string && strcmp(child->string, string) == 0) {
            return child;
        }
    }
    return NULL;
}
