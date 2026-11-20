#include "json.h"
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 64

typedef struct {
    const char *p;
    int depth;
} parser_t;

static void skip_ws(parser_t *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')
        ps->p++;
}

static json_t *node_new(json_type_t t)
{
    json_t *j = calloc(1, sizeof(*j));
    if (j)
        j->type = t;
    return j;
}

static int hex4(const char *p, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

static size_t utf8_put(char *o, unsigned cp)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12));
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18));
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Parses a string starting at the opening quote. Output is never longer than input. */
static char *parse_string(parser_t *ps)
{
    const char *s = ps->p + 1;
    const char *e = s;
    while (*e && *e != '"') {
        if (*e == '\\' && e[1])
            e++;
        e++;
    }
    if (*e != '"')
        return NULL;
    char *out = malloc((size_t)(e - s) + 1);
    if (!out)
        return NULL;
    size_t n = 0;
    const char *p = s;
    while (p < e) {
        if (*p != '\\') {
            out[n++] = *p++;
            continue;
        }
        p++;
        switch (*p) {
        case 'n': out[n++] = '\n'; p++; break;
        case 't': out[n++] = '\t'; p++; break;
        case 'r': out[n++] = '\r'; p++; break;
        case 'b': out[n++] = '\b'; p++; break;
        case 'f': out[n++] = '\f'; p++; break;
        case 'u': {
            unsigned cp;
            if (e - p < 5 || hex4(p + 1, &cp) < 0) { p++; break; }
            p += 5;
            if (cp >= 0xD800 && cp < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                unsigned lo;
                if (hex4(p + 2, &lo) == 0 && lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
            }
            /* \uXXXX is 6 input bytes, at most 4 output bytes (surrogate pair: 12 -> 4) */
            n += utf8_put(out + n, cp);
            break;
        }
        default: out[n++] = *p++; break; /* \" \\ \/ */
        }
    }
    out[n] = 0;
    ps->p = e + 1;
    return out;
}

static json_t *parse_value(parser_t *ps);

static json_t *parse_container(parser_t *ps, int is_obj)
{
    json_t *j = node_new(is_obj ? JSON_OBJECT : JSON_ARRAY);
    json_t **tail;
    if (!j)
        return NULL;
    tail = &j->child;
    if (++ps->depth > MAX_DEPTH)
        goto fail;
    ps->p++;
    skip_ws(ps);
    if (*ps->p == (is_obj ? '}' : ']')) {
        ps->p++;
        ps->depth--;
        return j;
    }
    for (;;) {
        char *key = NULL;
        skip_ws(ps);
        if (is_obj) {
            if (*ps->p != '"' || !(key = parse_string(ps)))
                goto fail;
            skip_ws(ps);
            if (*ps->p != ':') { free(key); goto fail; }
            ps->p++;
        }
        json_t *v = parse_value(ps);
        if (!v) { free(key); goto fail; }
        v->key = key;
        *tail = v;
        tail = &v->next;
        j->count++;
        skip_ws(ps);
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == (is_obj ? '}' : ']')) { ps->p++; break; }
        goto fail;
    }
    ps->depth--;
    return j;
fail:
    json_free(j);
    return NULL;
}

static json_t *parse_value(parser_t *ps)
{
    skip_ws(ps);
    char c = *ps->p;
    if (c == '{' || c == '[')
        return parse_container(ps, c == '{');
    if (c == '"') {
        json_t *j = node_new(JSON_STRING);
        if (j && !(j->str = parse_string(ps))) { free(j); return NULL; }
        return j;
    }
    if (!strncmp(ps->p, "true", 4) || !strncmp(ps->p, "false", 5)) {
        json_t *j = node_new(JSON_BOOL);
        if (j) j->num = c == 't';
        ps->p += c == 't' ? 4 : 5;
        return j;
    }
    if (!strncmp(ps->p, "null", 4)) {
        ps->p += 4;
        return node_new(JSON_NULL);
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char *end;
        double d = strtod(ps->p, &end);
        if (end == ps->p)
            return NULL;
        ps->p = end;
        json_t *j = node_new(JSON_NUMBER);
        if (j) j->num = d;
        return j;
    }
    return NULL;
}

json_t *json_parse(const char *text)
{
    if (!text)
        return NULL;
    parser_t ps = { text, 0 };
    return parse_value(&ps);
}

void json_free(json_t *j)
{
    while (j) {
        json_t *n = j->next;
        json_free(j->child);
        free(j->key);
        free(j->str);
        free(j);
        j = n;
    }
}

json_t *json_get(const json_t *j, const char *key)
{
    if (!j || j->type != JSON_OBJECT)
        return NULL;
    for (json_t *c = j->child; c; c = c->next)
        if (c->key && !strcmp(c->key, key))
            return c;
    return NULL;
}

json_t *json_at(const json_t *j, int index)
{
    if (!j || (j->type != JSON_ARRAY && j->type != JSON_OBJECT))
        return NULL;
    json_t *c = j->child;
    while (c && index-- > 0)
        c = c->next;
    return c;
}

json_t *json_path(const json_t *j, const char *path)
{
    char part[64];
    if (!path)
        return (json_t *)j;
    while (j && *path) {
        size_t n = 0;
        while (*path && *path != '.' && n + 1 < sizeof(part))
            part[n++] = *path++;
        part[n] = 0;
        if (*path == '.')
            path++;
        if (j->type == JSON_ARRAY && part[0] >= '0' && part[0] <= '9')
            j = json_at(j, atoi(part));
        else
            j = json_get(j, part);
    }
    return (json_t *)j;
}

const char *json_str(const json_t *j, const char *path, const char *def)
{
    j = json_path(j, path);
    return (j && j->type == JSON_STRING) ? j->str : def;
}

double json_num(const json_t *j, const char *path, double def)
{
    j = json_path(j, path);
    return (j && (j->type == JSON_NUMBER || j->type == JSON_BOOL)) ? j->num : def;
}
