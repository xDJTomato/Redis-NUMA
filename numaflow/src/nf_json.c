/* nf_json.c - minimal JSON parser/writer (pure C11). */
#include "nf_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <inttypes.h>

typedef struct { const char *s; size_t pos; size_t len; const char *err; } jp_t;

static void jp_err(jp_t *p, const char *m) { if (!p->err) p->err = m; }

static void skip_ws(jp_t *p) {
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->pos++;
        else break;
    }
}

static nf_json_t *parse_value(jp_t *p);

static char *parse_string_raw(jp_t *p) {
    if (p->pos >= p->len || p->s[p->pos] != '"') { jp_err(p, "expected string"); return NULL; }
    p->pos++;
    size_t cap = 32, len = 0;
    char *out = (char *)malloc(cap);
    if (!out) { jp_err(p, "oom"); return NULL; }
    while (p->pos < p->len) {
        char c = p->s[p->pos++];
        if (c == '"') { out[len] = '\0'; return out; }
        if (c == '\\') {
            if (p->pos >= p->len) break;
            char e = p->s[p->pos++];
            switch (e) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    if (p->pos + 4 > p->len) { jp_err(p, "bad unicode escape"); free(out); return NULL; }
                    unsigned cp = 0;
                    for (int i = 0; i < 4; i++) {
                        char h = p->s[p->pos++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                        else { jp_err(p, "bad unicode hex"); free(out); return NULL; }
                    }
                    /* encode as UTF-8 (single byte fallback for BMP) */
                    if (cp < 0x80) { c = (char)cp; }
                    else if (cp < 0x800) {
                        if (len + 2 >= cap) { cap *= 2; out = (char *)realloc(out, cap); }
                        out[len++] = (char)(0xC0 | (cp >> 6));
                        c = (char)(0x80 | (cp & 0x3F));
                    } else {
                        if (len + 3 >= cap) { cap *= 2; out = (char *)realloc(out, cap); }
                        out[len++] = (char)(0xE0 | (cp >> 12));
                        out[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        c = (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: jp_err(p, "bad escape"); free(out); return NULL;
            }
        }
        if (len + 1 >= cap) { cap *= 2; char *t = (char *)realloc(out, cap); if (!t) { free(out); jp_err(p, "oom"); return NULL; } out = t; }
        out[len++] = c;
    }
    jp_err(p, "unterminated string");
    free(out);
    return NULL;
}

static double parse_number_raw(jp_t *p) {
    size_t start = p->pos;
    if (p->pos < p->len && (p->s[p->pos] == '-' || p->s[p->pos] == '+')) p->pos++;
    while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos])) p->pos++;
    if (p->pos < p->len && p->s[p->pos] == '.') {
        p->pos++;
        while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos])) p->pos++;
    }
    if (p->pos < p->len && (p->s[p->pos] == 'e' || p->s[p->pos] == 'E')) {
        p->pos++;
        if (p->pos < p->len && (p->s[p->pos] == '-' || p->s[p->pos] == '+')) p->pos++;
        while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos])) p->pos++;
    }
    return strtod(p->s + start, NULL);
}

static nf_json_t *parse_array(jp_t *p) {
    nf_json_t *arr = nf_json_new_arr();
    if (!arr) { jp_err(p, "oom"); return NULL; }
    p->pos++; /* consume [ */
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return arr; }
    while (p->pos < p->len) {
        skip_ws(p);
        nf_json_t *v = parse_value(p);
        if (!v) { nf_json_free(arr); return NULL; }
        if (nf_json_arr_push(arr, v) != 0) { nf_json_free(v); nf_json_free(arr); jp_err(p, "oom"); return NULL; }
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return arr; }
        jp_err(p, "expected , or ]"); nf_json_free(arr); return NULL;
    }
    jp_err(p, "unterminated array"); nf_json_free(arr); return NULL;
}

static nf_json_t *parse_object(jp_t *p) {
    nf_json_t *obj = nf_json_new_obj();
    if (!obj) { jp_err(p, "oom"); return NULL; }
    p->pos++; /* consume { */
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; return obj; }
    while (p->pos < p->len) {
        skip_ws(p);
        char *key = parse_string_raw(p);
        if (!key) { nf_json_free(obj); return NULL; }
        skip_ws(p);
        if (p->pos >= p->len || p->s[p->pos] != ':') { free(key); jp_err(p, "expected :"); nf_json_free(obj); return NULL; }
        p->pos++;
        skip_ws(p);
        nf_json_t *v = parse_value(p);
        if (!v) { free(key); nf_json_free(obj); return NULL; }
        if (nf_json_obj_set(obj, key, v) != 0) { free(key); nf_json_free(v); nf_json_free(obj); jp_err(p, "oom"); return NULL; }
        free(key);
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; return obj; }
        jp_err(p, "expected , or }"); nf_json_free(obj); return NULL;
    }
    jp_err(p, "unterminated object"); nf_json_free(obj); return NULL;
}

static nf_json_t *parse_value(jp_t *p) {
    skip_ws(p);
    if (p->pos >= p->len) { jp_err(p, "unexpected end"); return NULL; }
    char c = p->s[p->pos];
    if (c == '{') return parse_object(p);
    if (c == '[') return parse_array(p);
    if (c == '"') { char *s = parse_string_raw(p); if (!s) return NULL; nf_json_t *v = nf_json_new_str(s); free(s); return v; }
    if (c == 't' && strncmp(p->s + p->pos, "true", 4) == 0) { p->pos += 4; return nf_json_new_bool(true); }
    if (c == 'f' && strncmp(p->s + p->pos, "false", 5) == 0) { p->pos += 5; return nf_json_new_bool(false); }
    if (c == 'n' && strncmp(p->s + p->pos, "null", 4) == 0) { p->pos += 4; return nf_json_new_null(); }
    if (c == '-' || c == '+' || isdigit((unsigned char)c)) { return nf_json_new_num(parse_number_raw(p)); }
    jp_err(p, "unexpected character");
    return NULL;
}

nf_json_t *nf_json_parse(const char *text, const char **err) {
    if (err) *err = NULL;
    if (!text) { if (err) *err = "null input"; return NULL; }
    jp_t p; p.s = text; p.pos = 0; p.len = strlen(text); p.err = NULL;
    nf_json_t *v = parse_value(&p);
    if (!v) { if (err) *err = p.err ? p.err : "parse error"; return NULL; }
    skip_ws(&p);
    if (p.pos != p.len) { nf_json_free(v); if (err) *err = "trailing content"; return NULL; }
    return v;
}

/* ---- constructors ---- */
static nf_json_t *nf_json_alloc(nf_json_type_t t) {
    nf_json_t *v = (nf_json_t *)calloc(1, sizeof(nf_json_t));
    if (!v) return NULL;
    v->type = t;
    return v;
}
nf_json_t *nf_json_new_null(void) { return nf_json_alloc(NF_JSON_NULL); }
nf_json_t *nf_json_new_bool(bool b) { nf_json_t *v = nf_json_alloc(NF_JSON_BOOL); if (v) v->boolean = b; return v; }
nf_json_t *nf_json_new_num(double d) { nf_json_t *v = nf_json_alloc(NF_JSON_NUM); if (v) v->number = d; return v; }
nf_json_t *nf_json_new_str(const char *s) {
    nf_json_t *v = nf_json_alloc(NF_JSON_STR);
    if (!v) return NULL;
    if (s) { v->str = strdup(s); if (!v->str) { free(v); return NULL; } }
    else v->str = strdup("");
    return v;
}
nf_json_t *nf_json_new_arr(void) { return nf_json_alloc(NF_JSON_ARR); }
nf_json_t *nf_json_new_obj(void) { return nf_json_alloc(NF_JSON_OBJ); }

static int children_reserve(nf_json_t *v, size_t n) {
    if (n <= v->child_cap) return 0;
    size_t ncap = v->child_cap ? v->child_cap * 2 : 8;
    while (ncap < n) ncap *= 2;
    nf_json_t **nc = (nf_json_t **)realloc(v->children, ncap * sizeof(nf_json_t *));
    if (!nc) return -1;
    v->children = nc;
    if (v->type == NF_JSON_OBJ) {
        char **nk = (char **)realloc(v->keys, ncap * sizeof(char *));
        if (!nk) return -1;
        v->keys = nk;
    }
    v->child_cap = ncap;
    return 0;
}

int nf_json_obj_set(nf_json_t *obj, const char *key, nf_json_t *value) {
    if (!obj || obj->type != NF_JSON_OBJ || !key) return -1;
    for (size_t i = 0; i < obj->child_count; i++) {
        if (strcmp(obj->keys[i], key) == 0) {
            nf_json_free(obj->children[i]);
            obj->children[i] = value;
            return 0;
        }
    }
    if (children_reserve(obj, obj->child_count + 1) != 0) return -1;
    obj->keys[obj->child_count] = strdup(key);
    obj->children[obj->child_count] = value;
    obj->child_count++;
    return 0;
}

nf_json_t *nf_json_obj_get(const nf_json_t *obj, const char *key) {
    if (!obj || obj->type != NF_JSON_OBJ || !key) return NULL;
    for (size_t i = 0; i < obj->child_count; i++) {
        if (strcmp(obj->keys[i], key) == 0) return obj->children[i];
    }
    return NULL;
}

const char *nf_json_obj_get_str(const nf_json_t *obj, const char *key) {
    nf_json_t *v = nf_json_obj_get(obj, key);
    return v ? nf_json_str(v) : NULL;
}
double nf_json_obj_get_num(const nf_json_t *obj, const char *key, double dflt) {
    nf_json_t *v = nf_json_obj_get(obj, key);
    return (v && v->type == NF_JSON_NUM) ? v->number : dflt;
}
bool nf_json_obj_get_bool(const nf_json_t *obj, const char *key, bool dflt) {
    nf_json_t *v = nf_json_obj_get(obj, key);
    return (v && v->type == NF_JSON_BOOL) ? v->boolean : dflt;
}

int nf_json_arr_push(nf_json_t *arr, nf_json_t *value) {
    if (!arr || arr->type != NF_JSON_ARR || !value) return -1;
    if (children_reserve(arr, arr->child_count + 1) != 0) return -1;
    arr->children[arr->child_count++] = value;
    return 0;
}
nf_json_t *nf_json_arr_get(const nf_json_t *arr, size_t i) {
    if (!arr || arr->type != NF_JSON_ARR || i >= arr->child_count) return NULL;
    return arr->children[i];
}
size_t nf_json_arr_len(const nf_json_t *arr) {
    return (arr && arr->type == NF_JSON_ARR) ? arr->child_count : 0;
}

nf_json_type_t nf_json_type(const nf_json_t *v) { return v ? v->type : NF_JSON_NULL; }
const char *nf_json_str(const nf_json_t *v) { return (v && v->type == NF_JSON_STR) ? v->str : NULL; }
double nf_json_num(const nf_json_t *v) { return (v && v->type == NF_JSON_NUM) ? v->number : 0.0; }
bool nf_json_bool(const nf_json_t *v) { return (v && v->type == NF_JSON_BOOL) ? v->boolean : false; }

void nf_json_free(nf_json_t *v) {
    if (!v) return;
    free(v->str);
    for (size_t i = 0; i < v->child_count; i++) {
        if (v->type == NF_JSON_OBJ) free(v->keys[i]);
        nf_json_free(v->children[i]);
    }
    free(v->keys);
    free(v->children);
    free(v);
}

/* ---- serializer ---- */
typedef struct { char *buf; size_t len, cap; } sb_t;
static int sb_put(sb_t *sb, const char *s) {
    size_t n = strlen(s);
    if (sb->len + n + 1 > sb->cap) {
        size_t ncap = sb->cap ? sb->cap : 64;
        while (ncap < sb->len + n + 1) ncap *= 2;
        char *t = (char *)realloc(sb->buf, ncap);
        if (!t) return -1;
        sb->buf = t; sb->cap = ncap;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
    return 0;
}
static int sb_putc(sb_t *sb, char c) {
    if (sb->len + 2 > sb->cap) {
        size_t ncap = sb->cap ? sb->cap : 64;
        while (ncap < sb->len + 2) ncap *= 2;
        char *t = (char *)realloc(sb->buf, ncap);
        if (!t) return -1;
        sb->buf = t; sb->cap = ncap;
    }
    sb->buf[sb->len++] = c;
    sb->buf[sb->len] = '\0';
    return 0;
}

static int ser_string(sb_t *sb, const char *s) {
    if (!s) s = "";
    sb_putc(sb, '"');
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"': sb_put(sb, "\\\""); break;
            case '\\': sb_put(sb, "\\\\"); break;
            case '\n': sb_put(sb, "\\n"); break;
            case '\r': sb_put(sb, "\\r"); break;
            case '\t': sb_put(sb, "\\t"); break;
            case '\b': sb_put(sb, "\\b"); break;
            case '\f': sb_put(sb, "\\f"); break;
            default:
                if (c < 0x20) { char tmp[8]; snprintf(tmp, sizeof(tmp), "\\u%04x", c); sb_put(sb, tmp); }
                else sb_putc(sb, (char)c);
        }
    }
    sb_putc(sb, '"');
    return 0;
}

static int ser_value(sb_t *sb, const nf_json_t *v, int indent, int depth);

static void ser_indent(sb_t *sb, int indent, int depth) {
    if (indent <= 0) return;
    sb_putc(sb, '\n');
    for (int i = 0; i < depth * indent; i++) sb_putc(sb, ' ');
}

static int ser_value(sb_t *sb, const nf_json_t *v, int indent, int depth) {
    if (!v) { sb_put(sb, "null"); return 0; }
    switch (v->type) {
        case NF_JSON_NULL: return sb_put(sb, "null");
        case NF_JSON_BOOL: return sb_put(sb, v->boolean ? "true" : "false");
        case NF_JSON_NUM: {
            char tmp[64];
            if (v->number == (long long)v->number && fabs(v->number) < 1e15)
                snprintf(tmp, sizeof(tmp), "%" PRId64, (int64_t)v->number);
            else snprintf(tmp, sizeof(tmp), "%.17g", v->number);
            return sb_put(sb, tmp);
        }
        case NF_JSON_STR: return ser_string(sb, v->str);
        case NF_JSON_ARR: {
            sb_putc(sb, '[');
            for (size_t i = 0; i < v->child_count; i++) {
                if (i) sb_putc(sb, ',');
                ser_indent(sb, indent, depth + 1);
                ser_value(sb, v->children[i], indent, depth + 1);
            }
            if (v->child_count) ser_indent(sb, indent, depth);
            sb_putc(sb, ']');
            return 0;
        }
        case NF_JSON_OBJ: {
            sb_putc(sb, '{');
            for (size_t i = 0; i < v->child_count; i++) {
                if (i) sb_putc(sb, ',');
                ser_indent(sb, indent, depth + 1);
                ser_string(sb, v->keys[i]);
                sb_put(sb, indent > 0 ? ": " : ":");
                ser_value(sb, v->children[i], indent, depth + 1);
            }
            if (v->child_count) ser_indent(sb, indent, depth);
            sb_putc(sb, '}');
            return 0;
        }
    }
    return sb_put(sb, "null");
}

static char *nf_json_serialize_indent(const nf_json_t *v, int indent) {
    sb_t sb; sb.buf = NULL; sb.len = 0; sb.cap = 0;
    ser_value(&sb, v, indent, 0);
    return sb.buf ? sb.buf : strdup("");
}

char *nf_json_serialize(const nf_json_t *v) { return nf_json_serialize_indent(v, 2); }
