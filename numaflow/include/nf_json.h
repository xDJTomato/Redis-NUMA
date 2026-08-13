/* =============================================================================
 * nf_json.h - minimal JSON parser/writer (pure C11, no dependencies).
 * ========================================================================== */
#ifndef NF_JSON_H
#define NF_JSON_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NF_JSON_NULL = 0,
    NF_JSON_BOOL,
    NF_JSON_NUM,
    NF_JSON_STR,
    NF_JSON_ARR,
    NF_JSON_OBJ
} nf_json_type_t;

typedef struct nf_json {
    nf_json_type_t type;
    bool     boolean;
    double   number;
    char    *str;
    char   **keys;
    struct nf_json **children;
    size_t   child_count;
    size_t   child_cap;
} nf_json_t;

nf_json_t *nf_json_parse(const char *text, const char **err);
void       nf_json_free(nf_json_t *v);

nf_json_t *nf_json_new_null(void);
nf_json_t *nf_json_new_bool(bool b);
nf_json_t *nf_json_new_num(double d);
nf_json_t *nf_json_new_str(const char *s);
nf_json_t *nf_json_new_arr(void);
nf_json_t *nf_json_new_obj(void);

int  nf_json_obj_set(nf_json_t *obj, const char *key, nf_json_t *value);
nf_json_t *nf_json_obj_get(const nf_json_t *obj, const char *key);
const char *nf_json_obj_get_str(const nf_json_t *obj, const char *key);
double nf_json_obj_get_num(const nf_json_t *obj, const char *key, double dflt);
bool   nf_json_obj_get_bool(const nf_json_t *obj, const char *key, bool dflt);

int  nf_json_arr_push(nf_json_t *arr, nf_json_t *value);
nf_json_t *nf_json_arr_get(const nf_json_t *arr, size_t i);
size_t nf_json_arr_len(const nf_json_t *arr);

nf_json_type_t nf_json_type(const nf_json_t *v);
const char *nf_json_str(const nf_json_t *v);
double      nf_json_num(const nf_json_t *v);
bool        nf_json_bool(const nf_json_t *v);

char *nf_json_serialize(const nf_json_t *v);

#ifdef __cplusplus
}
#endif

#endif /* NF_JSON_H */
