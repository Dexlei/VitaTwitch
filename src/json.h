#ifndef VT_JSON_H
#define VT_JSON_H

#include <stddef.h>

/* Small DOM JSON parser (enough for Twitch GQL / OAuth / 7TV responses). */

typedef enum { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT } json_type_t;

typedef struct json json_t;
struct json {
    json_type_t type;
    char *key;          /* member name when inside an object */
    char *str;          /* JSON_STRING (UTF-8, unescaped) */
    double num;         /* JSON_NUMBER / JSON_BOOL */
    json_t *child;      /* first element / member */
    json_t *next;       /* next sibling */
    int count;          /* number of children */
};

json_t *json_parse(const char *text);
void json_free(json_t *j);

/* Object member lookup (NULL if j is not an object or key is missing). */
json_t *json_get(const json_t *j, const char *key);
/* Dotted path, e.g. "data.currentUser.followedLiveUsers.edges". */
json_t *json_path(const json_t *j, const char *path);
json_t *json_at(const json_t *j, int index);

/* Typed accessors with defaults (path may be NULL to read j itself). */
const char *json_str(const json_t *j, const char *path, const char *def);
double json_num(const json_t *j, const char *path, double def);

#endif
