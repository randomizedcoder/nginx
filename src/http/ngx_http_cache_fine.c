
/*
 * Millisecond companions for HTTP cache response directives.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


enum {
    MAX_AGE, MAX_AGE_MS, SHARED, SHARED_MS, UPDATE, UPDATE_MS, ERROR_AGE,
    RESTRICTED, MUST_VALIDATE, NVALUES
};

static ngx_str_t  directives[] = {
    ngx_string("max-age"), ngx_string("max-age-ms"),
    ngx_string("s-maxage"), ngx_string("s-maxage-ms"),
    ngx_string("stale-while-revalidate"), ngx_string("stale-while-revalidate-ms"),
    ngx_string("stale-if-error"), ngx_string("no-cache"),
    ngx_string("must-revalidate")
};


int64_t
ngx_http_cache_now_ms(void)
{
    ngx_time_t  *tp;

    tp = ngx_timeofday();
    return (int64_t) tp->sec * 1000 + tp->msec;
}


static ngx_uint_t
ngx_http_fine_name(u_char *p, size_t len, const char *name, size_t n)
{
    return len == n && ngx_strncasecmp(p, (u_char *) name, n) == 0;
}


/* Keep missing (DECLINED) distinct from invalid or repeated (ERROR). */
static ngx_int_t
ngx_http_fine_policy(ngx_table_elt_t *h, int64_t *v)
{
    u_char      *p, *end, *start, *last, *eq, *key;
    ngx_uint_t   i, quoted;

    for (i = 0; i < NVALUES; i++) {
        v[i] = NGX_DECLINED;
    }

    for ( ; h; h = h->next) {
        p = h->value.data;
        end = p + h->value.len;

        while (p < end) {
            while (p < end && (*p == ' ' || *p == '\t' || *p == ',')) {
                p++;
            }

            start = p;
            quoted = 0;
            while (p < end && (quoted || *p != ',')) {
                if (quoted && *p == '\\' && end - p > 1) {
                    p += 2;
                    continue;
                }
                if (*p == '"') {
                    quoted = !quoted;
                }
                p++;
            }

            if (quoted) {
                return NGX_DECLINED;
            }

            last = p;
            while (last > start && (last[-1] == ' ' || last[-1] == '\t')) {
                last--;
            }

            eq = ngx_strlchr(start, last, '=');
            key = eq ? eq : last;

            if (ngx_http_fine_name(start, key - start, "private", 7)
                || ngx_http_fine_name(start, key - start, "no-store", 8))
            {
                return NGX_DECLINED;
            }

            if (ngx_http_fine_name(start, key - start, "proxy-revalidate", 16)) {
                v[MUST_VALIDATE] = 1;
            }

            for (i = 0; i < NVALUES; i++) {
                if (directives[i].len != (size_t) (key - start)
                    || ngx_strncasecmp(start, directives[i].data, key - start))
                {
                    continue;
                }

                if (i == RESTRICTED) {
                    return NGX_DECLINED;
                }

                if (i == MUST_VALIDATE) {
                    v[i] = 1;

                } else if (v[i] != NGX_DECLINED || eq == NULL
                           || last - eq - 1 > 15)
                {
                    v[i] = NGX_ERROR;

                } else {
                    v[i] = ngx_atoof(eq + 1, last - eq - 1);
                }
                break;
            }
        }
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_fine_add(ngx_http_request_t *r, ngx_str_t key, int64_t value)
{
    ngx_table_elt_t  *h;

    h = ngx_list_push(&r->upstream->headers_in.headers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    h->key = key;
    h->lowcase_key = ngx_pnalloc(r->pool, key.len);
    h->value.data = ngx_pnalloc(r->pool, NGX_INT64_LEN);
    if (h->lowcase_key == NULL || h->value.data == NULL) {
        return NGX_ERROR;
    }
    h->hash = ngx_hash_strlow(h->lowcase_key, key.data, key.len);
    h->value.len = ngx_sprintf(h->value.data, "%L", value) - h->value.data;
    h->next = NULL;

    return NGX_OK;
}


static ngx_uint_t
ngx_http_fine_metadata(ngx_str_t *key)
{
    ngx_uint_t        i;
    static ngx_str_t  names[] = {
        ngx_string("Age"), ngx_string("Age-Ms"), ngx_string("Cache-Control"),
        ngx_string("Date"), ngx_string("ETag"), ngx_string("Expires"),
        ngx_string("Vary"), ngx_string("Last-Modified"),
        ngx_string("Content-Location"), ngx_string("Set-Cookie")
    };

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (key->len == names[i].len
            && ngx_strncasecmp(key->data, names[i].data, key->len) == 0)
        {
            return 1;
        }
    }

    return 0;
}


/* Replace cached response metadata supplied by the validation exchange. */
static ngx_int_t
ngx_http_fine_validated(ngx_http_request_t *r)
{
    ngx_uint_t                       i, j;
    ngx_table_elt_t                 *old, *new, *dst;
    ngx_list_part_t                 *p, *v;
    ngx_http_upstream_headers_in_t  *stored, *validation;

    stored = &r->upstream->headers_in;
    validation = r->upstream->fine_validation;

    for (p = &stored->headers.part; p; p = p->next) {
        old = p->elts;
        for (i = 0; i < p->nelts; i++) {
            if (!ngx_http_fine_metadata(&old[i].key)) {
                continue;
            }

            /* Discard the old age sample even when the 304 omits Age. */
            if (ngx_http_fine_name(old[i].key.data, old[i].key.len, "age", 3)
                || ngx_http_fine_name(old[i].key.data, old[i].key.len,
                                      "age-ms", 6))
            {
                old[i].hash = 0;
                continue;
            }

            for (v = &validation->headers.part; v; v = v->next) {
                new = v->elts;
                for (j = 0; j < v->nelts; j++) {
                    if (new[j].hash && new[j].key.len == old[i].key.len
                        && ngx_strncasecmp(new[j].key.data, old[i].key.data,
                                           new[j].key.len) == 0)
                    {
                        old[i].hash = 0;
                    }
                }
            }
        }
    }

    for (v = &validation->headers.part; v; v = v->next) {
        new = v->elts;
        for (j = 0; j < v->nelts; j++) {
            if (!new[j].hash || !ngx_http_fine_metadata(&new[j].key)) {
                continue;
            }
            dst = ngx_list_push(&stored->headers);
            if (dst == NULL) {
                return NGX_ERROR;
            }
            *dst = new[j];
            dst->next = NULL;
        }
    }

    if (validation->cache_control) {
        stored->cache_control = validation->cache_control;
        stored->cache_restricted = validation->cache_restricted;
    }
    if (validation->last_modified) {
        stored->last_modified_time = validation->last_modified_time;
    }

    return NGX_OK;
}


ngx_int_t
ngx_http_cache_fine_headers(ngx_http_request_t *r)
{
    int64_t              now, seconds, age, lifetime, window, v[NVALUES];
    ngx_uint_t           i, base;
    ngx_list_part_t     *part;
    ngx_table_elt_t     *h, *ordinary;
    ngx_http_cache_t    *c;
    ngx_http_upstream_t *u;
    static ngx_str_t     coarse = ngx_string("Age");
    static ngx_str_t     fine = ngx_string("Age-Ms");

    c = r->cache;
    u = r->upstream;
    if (u->fine_validation && ngx_http_fine_validated(r) != NGX_OK) {
        return NGX_ERROR;
    }

    ordinary = NULL;
    seconds = NGX_DECLINED;
    age = NGX_DECLINED;

    for (part = &u->headers_in.headers.part; part; part = part->next) {
        h = part->elts;
        for (i = 0; i < part->nelts; i++) {
            if (!h[i].hash) {
                continue;
            }

            if (ngx_http_fine_name(h[i].key.data, h[i].key.len, "age", 3)) {
                ordinary = &h[i];
                seconds = seconds == NGX_DECLINED
                          ? ngx_atoof(h[i].value.data, h[i].value.len)
                          : NGX_ERROR;
            }

            if (ngx_http_fine_name(h[i].key.data, h[i].key.len, "age-ms", 6)) {
                age = age == NGX_DECLINED && h[i].value.len <= 15
                      ? ngx_atoof(h[i].value.data, h[i].value.len) : NGX_ERROR;
                h[i].hash = 0;
            }
        }
    }

    if (c == NULL || sizeof(time_t) < 8) {
        return NGX_OK;
    }

    now = ngx_http_cache_now_ms();

    if (!r->cached || u->fine_validation) {
        c->fine_deadline = 0;
        if (!u->cacheable || u->headers_in.cache_restricted
            || u->headers_in.status_n != NGX_HTTP_OK
            || (u->conf->ignore_headers & NGX_HTTP_UPSTREAM_IGN_CACHE_CONTROL)
            || (u->headers_in.x_accel_expires
                && !(u->conf->ignore_headers & NGX_HTTP_UPSTREAM_IGN_XA_EXPIRES))
            || ngx_http_fine_policy(u->headers_in.cache_control, v) != NGX_OK)
        {
            return NGX_OK;
        }

        base = v[SHARED] == NGX_DECLINED ? MAX_AGE : SHARED;
        if (v[base] < 0 || (v[base + 1] < 0
                             && (v[UPDATE] < 0 || v[UPDATE_MS] < 0)))
        {
            return NGX_OK;
        }

        if (u->conf->cache_fine_time == NGX_HTTP_CACHE_FINE_ORIGIN
            && seconds == NGX_DECLINED && age == NGX_DECLINED)
        {
            age = 0;

        } else if (u->conf->cache_fine_time != NGX_HTTP_CACHE_FINE_TRUSTED
                   || age < 0 || seconds != age / 1000)
        {
            return NGX_OK;
        }

        lifetime = v[base + 1] >= 0 ? v[base + 1] : v[base] * 1000;
        window = 0;
        if (base != SHARED && v[MUST_VALIDATE] < 0 && v[UPDATE] >= 0) {
            window = v[UPDATE_MS] >= 0 ? v[UPDATE_MS] : v[UPDATE] * 1000;
        }

        age += u->state->header_time;
        c->fine_birth = now - age;
        c->fine_deadline = c->fine_birth + lifetime;
        if (c->fine_deadline <= 0) {
            c->fine_deadline = 0;
            u->cacheable = 0;
            return NGX_OK;
        }

        c->fine_window = window;
        c->valid_sec = ngx_max(1, c->fine_deadline / 1000);
        c->error_sec = base != SHARED && v[MUST_VALIDATE] < 0
                       && v[ERROR_AGE] >= 0 ? v[ERROR_AGE] : 0;
        u->headers_in.no_cache = 0;
        u->headers_in.expired = 0;
    }

    if (c->fine_deadline == 0) {
        return NGX_OK;
    }

    if (ordinary) {
        ordinary->hash = 0;
    }

    age = ngx_max(0, now - c->fine_birth);
    if (ngx_http_fine_add(r, coarse, age / 1000) != NGX_OK) {
        return NGX_ERROR;
    }

    return ngx_http_fine_add(r, fine, age);
}
