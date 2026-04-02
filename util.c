/*
 * util.c — URL resolution, string helpers, etc.
 */

#include "foxterm.h"

/* ── strdup wrapper ── */
char *str_dup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

bool str_startswith(const char *s, const char *prefix)
{
    if (!s || !prefix) return false;
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

void str_trim(char *s)
{
    if (!s) return;
    /* Leading */
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    /* Trailing */
    int n = (int)strlen(s);
    while (n > 0 && isspace((unsigned char)s[n-1])) s[--n] = '\0';
}

/* ── URL resolution ── */
/*
 * Resolve a relative URL `rel` against base URL `base`.
 * Returns a malloc'd string (caller must free).
 */
char *url_resolve(const char *base, const char *rel)
{
    if (!rel || !*rel) return base ? strdup(base) : NULL;

    /* Already absolute */
    if (strstr(rel, "://")) return strdup(rel);

    /* Protocol-relative */
    if (rel[0] == '/' && rel[1] == '/') {
        if (!base) return strdup(rel);
        /* Grab scheme from base */
        const char *cs = strstr(base, "://");
        if (!cs) return strdup(rel);
        int slen = (int)(cs - base);
        char *result = malloc(slen + strlen(rel) + 2);
        if (!result) return NULL;
        memcpy(result, base, slen);
        strcpy(result + slen, rel);
        return result;
    }

    /* Fragment-only */
    if (rel[0] == '#') {
        if (!base) return strdup(rel);
        /* Strip existing fragment */
        char *b2 = strdup(base);
        char *frag = strchr(b2, '#');
        if (frag) *frag = '\0';
        size_t total = strlen(b2) + strlen(rel) + 2;
        char *result = malloc(total);
        if (!result) { free(b2); return NULL; }
        snprintf(result, total, "%s%s", b2, rel);
        free(b2);
        return result;
    }

    /* Absolute path */
    if (rel[0] == '/') {
        if (!base) return strdup(rel);
        const char *cs = strstr(base, "://");
        if (!cs) return strdup(rel);
        const char *origin_end = strchr(cs + 3, '/');
        size_t origin_len = origin_end ? (size_t)(origin_end - base) : strlen(base);
        size_t total = origin_len + strlen(rel) + 2;
        char *result = malloc(total);
        if (!result) return NULL;
        memcpy(result, base, origin_len);
        strcpy(result + origin_len, rel);
        return result;
    }

    /* Relative path */
    if (!base) return strdup(rel);
    char *b2 = strdup(base);
    /* Remove query/fragment from base */
    char *q = strchr(b2, '?');
    if (q) *q = '\0';
    char *f = strchr(b2, '#');
    if (f) *f = '\0';
    /* Find last slash */
    char *last_slash = strrchr(b2, '/');
    if (last_slash) {
        *(last_slash + 1) = '\0';
    } else {
        strcat(b2, "/");
    }
    size_t total = strlen(b2) + strlen(rel) + 2;
    char *result = malloc(total);
    if (!result) { free(b2); return NULL; }
    snprintf(result, total, "%s%s", b2, rel);
    free(b2);

    /* Resolve .. and . in path */
    /* Find path start after scheme://host */
    char *path_start = result;
    const char *cs = strstr(result, "://");
    if (cs) {
        path_start = strchr(cs + 3, '/');
        if (!path_start) return result;
    }

    /* Collapse /foo/../ → / */
    char *buf = malloc(strlen(result) + 2);
    if (!buf) return result;
    char *out = buf;
    /* Copy scheme://host unchanged */
    memcpy(buf, result, path_start - result);
    out = buf + (path_start - result);

    const char *p = path_start;
    while (*p) {
        if (p[0] == '/' && p[1] == '.' && p[2] == '/' ) { p += 2; continue; }
        if (p[0] == '/' && p[1] == '.' && p[2] == '\0') { p++;    continue; }
        if (p[0] == '/' && p[1] == '.' && p[2] == '.' &&
            (p[3] == '/' || p[3] == '\0')) {
            /* Go up one directory in output */
            while (out > buf + (path_start - result) && *(out-1) != '/') out--;
            if (out > buf + (path_start - result)) out--; /* Remove trailing slash */
            p += 3;
            continue;
        }
        *out++ = *p++;
    }
    *out = '\0';

    free(result);
    return buf;
}

/* ── HTML entity decode (basic) ── */
char *html_decode_entities(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    char *out = malloc(len + 1);
    if (!out) return NULL;

    const char *p = s;
    char *q = out;

    while (*p) {
        if (*p != '&') {
            *q++ = *p++;
            continue;
        }
        const char *semi = strchr(p, ';');
        if (!semi || semi - p > 20) { *q++ = *p++; continue; }

        char ent[24];
        size_t elen = (size_t)(semi - p - 1);
        if (elen >= sizeof(ent)) { *q++ = *p++; continue; }
        memcpy(ent, p + 1, elen);
        ent[elen] = '\0';

        /* Named entities */
        struct { const char *name; const char *val; } entities[] = {
            {"amp",  "&"}, {"lt",  "<"}, {"gt",  ">"},
            {"quot", "\""}, {"apos", "'"}, {"nbsp", " "},
            {"copy", "(c)"}, {"reg", "(r)"}, {"mdash", "--"},
            {"ndash", "-"}, {"hellip", "..."}, {"bull", "*"},
            {"laquo", "<<"}, {"raquo", ">>"}, {"euro", "EUR"},
            {"pound", "GBP"}, {"yen", "JPY"}, {"cent", "c"},
            {NULL, NULL}
        };

        bool found = false;
        for (int i = 0; entities[i].name; i++) {
            if (!strcasecmp(ent, entities[i].name)) {
                const char *v = entities[i].val;
                while (*v) *q++ = *v++;
                found = true;
                break;
            }
        }

        if (!found && ent[0] == '#') {
            /* Numeric entity */
            long code = (ent[1] == 'x' || ent[1] == 'X')
                ? strtol(ent + 2, NULL, 16)
                : strtol(ent + 1, NULL, 10);
            if (code > 0 && code < 128) {
                *q++ = (char)code;
                found = true;
            } else {
                *q++ = '?';
                found = true;
            }
        }

        if (!found) {
            /* Pass through unchanged */
            memcpy(q, p, (size_t)(semi - p + 1));
            q += semi - p + 1;
        }

        p = semi + 1;
    }
    *q = '\0';
    return out;
}

/* ── Simple URL percent-encoding ── */
char *url_encode(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    char *out = malloc(len * 3 + 1);
    if (!out) return NULL;
    char *q = out;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            *q++ = (char)c;
        } else {
            q += sprintf(q, "%%%02X", c);
        }
    }
    *q = '\0';
    return out;
}

/* ── xpath_get_text ── */
char *xpath_get_text(xmlDoc *doc, const char *xpath_expr)
{
    if (!doc || !xpath_expr) return NULL;
    xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
    if (!ctx) return NULL;
    xmlXPathObjectPtr obj = xmlXPathEvalExpression((xmlChar *)xpath_expr, ctx);
    xmlXPathFreeContext(ctx);
    if (!obj) return NULL;

    char *result = NULL;
    if (obj->nodesetval && obj->nodesetval->nodeNr > 0) {
        xmlNode *n = obj->nodesetval->nodeTab[0];
        xmlChar *txt = xmlNodeGetContent(n);
        if (txt) {
            result = strdup((char *)txt);
            xmlFree(txt);
        }
    }
    xmlXPathFreeObject(obj);
    return result;
}