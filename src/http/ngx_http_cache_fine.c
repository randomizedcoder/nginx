
/*
 * Millisecond companions for HTTP cache response directives.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


enum {
    MAX_AGE, MAX_AGE_MS, SHARED, SHARED_MS, UPDATE, UPDATE_MS, ERROR_AGE,
    ERROR_MS, RESTRICTED, MUST_VALIDATE, NVALUES
};

static ngx_str_t  directives[] = {
    ngx_string("max-age"), ngx_string("max-age-ms"),
    ngx_string("s-maxage"), ngx_string("s-maxage-ms"),
    ngx_string("stale-while-revalidate"), ngx_string("stale-while-revalidate-ms"),
    ngx_string("stale-if-error"), ngx_string("stale-if-error-ms"),
    ngx_string("no-cache"), ngx_string("must-revalidate")
};

enum { REQ_MAX_AGE, REQ_MAX_AGE_MS, REQ_MIN_FRESH, REQ_MIN_FRESH_MS, REQ_NVALUES };

static ngx_str_t  request_directives[] = {
    ngx_string("max-age"), ngx_string("max-age-ms"),
    ngx_string("min-fresh"), ngx_string("min-fresh-ms")
};


/* Unit tests set this to supply a fixed clock. */
ngx_uint_t  ngx_http_cache_fine_fixed_clock;


/*
 * The cached time is updated once per event loop iteration, or once per
 * timer_resolution interval.  Fine decisions and reported ages read the
 * clock, so that a millisecond value is not derived from a coarser one.
 * This also refreshes ngx_current_msec.
 */
int64_t
ngx_http_cache_now_ms(void)
{
    ngx_time_t  *tp;

    if (!ngx_http_cache_fine_fixed_clock) {
        ngx_time_update();
    }

    tp = ngx_timeofday();
    return (int64_t) tp->sec * 1000 + tp->msec;
}


int64_t
ngx_http_cache_fine_offset(void)
{
    return ngx_http_cache_now_ms() - (int64_t) ngx_current_msec;
}


/* Record why fine age is not used, and say so once in the log. */
void
ngx_http_cache_fine_refuse(ngx_http_request_t *r, ngx_uint_t status)
{
    static char  *reasons[] = {
        "", "", "",
        "response policy excludes it",
        "companion directive is unpaired or invalid",
        "age fields are missing, invalid or inconsistent",
        "age is out of range",
        "elapsed time is uncertain"
    };
    ngx_int_t          skipped;
    static time_t      last[NGX_HTTP_CACHE_FINE_CLOCK + 1];
    static ngx_uint_t  left_out[NGX_HTTP_CACHE_FINE_CLOCK + 1];

    if (status == NGX_HTTP_CACHE_FINE_CLOCK) {
        r->cache->fine_clock = 1;

    } else {
        r->cache->fine_status = status;
    }

    skipped = ngx_http_cache_fine_log_due(ngx_time(), &last[status],
                                          &left_out[status]);
    if (skipped < 0) {
        return;
    }

    ngx_log_error(NGX_LOG_INFO, r->pool->log, 0,
                  "fine cache time not used: %s, "
                  "%i similar messages not logged", reasons[status], skipped);
}


static ngx_uint_t
ngx_http_fine_name(u_char *p, size_t len, const char *name, size_t n)
{
    return len == n && ngx_strncasecmp(p, (u_char *) name, n) == 0;
}


/*
 * Take the next directive from a field line. DONE reports the end of the
 * line and DECLINED an unterminated quoted string.
 */
static ngx_int_t
ngx_http_fine_token(u_char **pos, u_char *end, ngx_str_t *name, ngx_str_t *arg)
{
    u_char      *p, *start, *last, *eq;
    ngx_uint_t   quoted;

    p = *pos;
    while (p < end && (*p == ' ' || *p == '\t' || *p == ',')) {
        p++;
    }

    if (p == end) {
        return NGX_DONE;
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

    *pos = p;

    last = p;
    while (last > start && (last[-1] == ' ' || last[-1] == '\t')) {
        last--;
    }

    eq = ngx_strlchr(start, last, '=');
    name->data = start;
    name->len = (eq ? eq : last) - start;
    arg->data = eq ? eq + 1 : NULL;
    arg->len = eq ? last - eq - 1 : 0;

    return NGX_OK;
}


/* Keep missing (DECLINED) distinct from invalid or repeated (ERROR). */
static ngx_uint_t
ngx_http_fine_value(ngx_str_t *names, ngx_uint_t n, ngx_str_t *name,
    ngx_str_t *arg, int64_t *v)
{
    ngx_uint_t  i;

    for (i = 0; i < n; i++) {
        if (names[i].len != name->len
            || ngx_strncasecmp(name->data, names[i].data, name->len))
        {
            continue;
        }

        if (v[i] != NGX_DECLINED || arg->data == NULL || arg->len > 15) {
            v[i] = NGX_ERROR;

        } else {
            v[i] = ngx_atoof(arg->data, arg->len);
        }

        return 1;
    }

    return 0;
}


static ngx_int_t
ngx_http_fine_policy(ngx_table_elt_t *h, int64_t *v)
{
    u_char      *p, *end;
    ngx_int_t    rc;
    ngx_str_t    name, arg;
    ngx_uint_t   i;

    for (i = 0; i < NVALUES; i++) {
        v[i] = NGX_DECLINED;
    }

    for ( ; h; h = h->next) {
        p = h->value.data;
        end = p + h->value.len;

        for ( ;; ) {
            rc = ngx_http_fine_token(&p, end, &name, &arg);
            if (rc == NGX_DONE) {
                break;
            }

            if (rc != NGX_OK
                || ngx_http_fine_name(name.data, name.len, "private", 7)
                || ngx_http_fine_name(name.data, name.len, "no-store", 8)
                || ngx_http_fine_name(name.data, name.len, "no-cache", 8))
            {
                return NGX_DECLINED;
            }

            if (ngx_http_fine_name(name.data, name.len, "proxy-revalidate", 16)
                || ngx_http_fine_name(name.data, name.len,
                                      "must-revalidate", 15))
            {
                v[MUST_VALIDATE] = 1;
                continue;
            }

            (void) ngx_http_fine_value(directives, RESTRICTED, &name, &arg, v);
        }
    }

    return NGX_OK;
}


/*
 * Report whether the request's max-age and min-fresh directives, with
 * their millisecond companions, refuse a stored response with fine age.
 */
ngx_uint_t
ngx_http_cache_fine_refused(ngx_http_request_t *r, int64_t now)
{
    u_char            *p, *end;
    int64_t            v[REQ_NVALUES], limit[2];
    ngx_int_t          rc;
    ngx_str_t          name, arg;
    ngx_uint_t         i;
    ngx_list_part_t   *part;
    ngx_table_elt_t   *h;
    ngx_http_cache_t  *c;

    c = r->cache;

    for (i = 0; i < REQ_NVALUES; i++) {
        v[i] = NGX_DECLINED;
    }

    for (part = &r->headers_in.headers.part; part; part = part->next) {
        h = part->elts;
        for (i = 0; i < part->nelts; i++) {
            if (!ngx_http_fine_name(h[i].key.data, h[i].key.len,
                                    "cache-control", 13))
            {
                continue;
            }

            p = h[i].value.data;
            end = p + h[i].value.len;

            for ( ;; ) {
                rc = ngx_http_fine_token(&p, end, &name, &arg);
                if (rc == NGX_DONE) {
                    break;
                }

                if (rc != NGX_OK) {
                    return 0;
                }

                (void) ngx_http_fine_value(request_directives, REQ_NVALUES,
                                           &name, &arg, v);
            }
        }
    }

    /* A companion needs a valid base. An invalid companion yields to it. */
    for (i = 0; i < 2; i++) {
        limit[i] = v[2 * i] < 0 ? -1
                   : v[2 * i + 1] >= 0 ? v[2 * i + 1] : v[2 * i] * 1000;
    }

    return !ngx_http_cache_fine_acceptable(now - c->fine_birth,
                                           c->fine_deadline - now,
                                           limit[0], limit[1]);
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


/*
 * Find the value of a field in the stored header block.  Returns the
 * number of field lines with that name; *value is the last one found.
 */
static ngx_uint_t
ngx_http_fine_stored(ngx_http_cache_t *c, ngx_str_t *name, ngx_str_t *value)
{
    u_char      *p, *end, *eol, *colon;
    ngx_uint_t   n;

    n = 0;
    p = c->buf->pos + c->header_start;
    end = c->buf->pos + c->body_start;

    if (end > c->buf->last) {
        return 0;
    }

    /* The first line is the status line. */

    while (p < end) {
        eol = ngx_strlchr(p, end, LF);
        if (eol == NULL) {
            break;
        }

        colon = ngx_strlchr(p, eol, ':');

        if (colon && (size_t) (colon - p) == name->len
            && ngx_strncasecmp(p, name->data, name->len) == 0)
        {
            colon++;
            while (colon < eol && *colon == ' ') {
                colon++;
            }

            value->data = colon;
            while (eol > colon && (eol[-1] == CR || eol[-1] == ' ')) {
                eol--;
            }
            value->len = eol - colon;

            eol = ngx_strlchr(colon, end, LF);
            n++;
        }

        p = eol + 1;
    }

    return n;
}


/*
 * Decide whether a 304 leaves the stored header block the same length, and
 * if so apply it to the block in memory.  That holds when each metadata
 * field of the 304 equals the stored one, or is a Date or Cache-Control of
 * the same length.  The stored Age fields are not used for a fine entry.
 */
static void
ngx_http_fine_inplace(ngx_http_request_t *r)
{
    ngx_str_t                        value, replacement;
    ngx_uint_t                       j, pass;
    ngx_list_part_t                 *v;
    ngx_table_elt_t                 *new;
    ngx_http_cache_t                *c;
    ngx_http_upstream_headers_in_t  *validation;

    c = r->cache;
    validation = r->upstream->fine_validation;

    c->fine_inplace = 0;

    if (c->buf == NULL) {
        return;
    }

    /* Check every field first, then change the block. */

    for (pass = 0; pass < 2; pass++) {
        for (v = &validation->headers.part; v; v = v->next) {
            new = v->elts;
            for (j = 0; j < v->nelts; j++) {
                if (!new[j].hash || !ngx_http_fine_metadata(&new[j].key)
                    || ngx_http_fine_name(new[j].key.data, new[j].key.len,
                                          "age", 3)
                    || ngx_http_fine_name(new[j].key.data, new[j].key.len,
                                          "age-ms", 6))
                {
                    continue;
                }

                replacement = new[j].value;

                if (replacement.len == 0
                    && ngx_http_fine_name(new[j].key.data, new[j].key.len,
                                          "date", 4))
                {
                    /* The 304 had no Date, and this server's is sent. */
                    replacement = ngx_cached_http_time;
                }

                if (ngx_http_fine_stored(c, &new[j].key, &value) != 1
                    || value.len != replacement.len)
                {
                    return;
                }

                if (ngx_memcmp(value.data, replacement.data, value.len) == 0)
                {
                    continue;
                }

                if (!ngx_http_fine_name(new[j].key.data, new[j].key.len,
                                        "date", 4)
                    && !ngx_http_fine_name(new[j].key.data, new[j].key.len,
                                           "cache-control", 13))
                {
                    return;
                }

                if (pass) {
                    ngx_memcpy(value.data, replacement.data, value.len);
                }
            }
        }
    }

    c->fine_inplace = 1;
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


/*
 * A whole-second entry served from a location with fine time: add the
 * time it has been stored to the age it arrived with.  The difference of
 * two whole-second readings is never less than the whole seconds elapsed.
 */
static ngx_int_t
ngx_http_fine_coarse_age(ngx_http_request_t *r, ngx_table_elt_t *ordinary,
    int64_t seconds)
{
    time_t            residence;
    static ngx_str_t  coarse = ngx_string("Age");

    residence = ngx_time() - r->cache->date;

    if (seconds == NGX_ERROR || residence < 0) {
        return NGX_OK;
    }

    if (ordinary) {
        ordinary->hash = 0;

    } else {
        seconds = 0;
    }

    return ngx_http_fine_add(r, coarse, seconds + residence);
}


/* After a 304: update the stored entry in place, or retire it. */
void
ngx_http_cache_fine_update(ngx_http_request_t *r, ngx_int_t rc)
{
    if (r->upstream->fine_validation == NULL || r->cache == NULL) {
        return;
    }

    if (rc == NGX_OK) {
        ngx_http_fine_inplace(r);
    }

    ngx_http_file_cache_update_header(r);
}


ngx_int_t
ngx_http_cache_fine_headers(ngx_http_request_t *r)
{
    int64_t              now, seconds, age, lifetime, window, v[NVALUES];
    ngx_uint_t           stale;
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
        c->fine_status = NGX_HTTP_CACHE_FINE_COARSE;

        if (!u->cacheable || u->headers_in.cache_restricted
            || ngx_http_fine_policy(u->headers_in.cache_control, v) != NGX_OK
            || (v[MAX_AGE_MS] == NGX_DECLINED && v[SHARED_MS] == NGX_DECLINED
                && v[UPDATE_MS] == NGX_DECLINED
                && v[ERROR_MS] == NGX_DECLINED))
        {
            /* Nothing fine was asked for, or nothing can be stored. */
            return NGX_OK;
        }

        if (u->headers_in.status_n != NGX_HTTP_OK
            || (u->conf->ignore_headers & NGX_HTTP_UPSTREAM_IGN_CACHE_CONTROL)
            || (u->headers_in.x_accel_expires
                && !(u->conf->ignore_headers
                     & NGX_HTTP_UPSTREAM_IGN_XA_EXPIRES)))
        {
            ngx_http_cache_fine_refuse(r, NGX_HTTP_CACHE_FINE_POLICY);
            return NGX_OK;
        }

        base = v[SHARED] == NGX_DECLINED ? MAX_AGE : SHARED;
        stale = base != SHARED && v[MUST_VALIDATE] < 0;

        /* Fine time needs a valid lifetime and at least one valid pair. */
        if (v[base] < 0
            || (v[base + 1] < 0
                && !(stale && v[UPDATE] >= 0 && v[UPDATE_MS] >= 0)
                && !(stale && v[ERROR_AGE] >= 0 && v[ERROR_MS] >= 0)))
        {
            ngx_http_cache_fine_refuse(r, NGX_HTTP_CACHE_FINE_COMPANION);
            return NGX_OK;
        }

        if (u->conf->cache_fine_time == NGX_HTTP_CACHE_FINE_ORIGIN
            && seconds == NGX_DECLINED && age == NGX_DECLINED)
        {
            age = 0;

        } else if (u->conf->cache_fine_time != NGX_HTTP_CACHE_FINE_TRUSTED
                   || age < 0 || seconds != age / 1000)
        {
            ngx_http_cache_fine_refuse(r, NGX_HTTP_CACHE_FINE_AGE);
            return NGX_OK;
        }

        lifetime = v[base + 1] >= 0 ? v[base + 1] : v[base] * 1000;
        window = 0;
        if (stale && v[UPDATE] >= 0) {
            window = v[UPDATE_MS] >= 0 ? v[UPDATE_MS] : v[UPDATE] * 1000;
        }

        /*
         * The request delay runs from the start of this upstream attempt
         * to the clock reading above.  The start is a cached reading and
         * can only be early, which overstates the delay.
         */
        age += (int64_t) (ngx_msec_int_t) (ngx_current_msec - u->start_time);
        c->fine_birth = now - age;
        c->fine_offset = ngx_http_cache_fine_offset();
        c->fine_deadline = c->fine_birth + lifetime;
        if (c->fine_deadline <= 0) {
            /* An age of decades.  Use the whole-second policy instead. */
            c->fine_deadline = 0;
            ngx_http_cache_fine_refuse(r, NGX_HTTP_CACHE_FINE_RANGE);
            return NGX_OK;
        }

        c->fine_window = window;
        c->valid_sec = ngx_max(1, c->fine_deadline / 1000);
        c->fine_error = 0;
        if (stale && v[ERROR_AGE] >= 0) {
            c->fine_error = v[ERROR_MS] >= 0 ? v[ERROR_MS]
                                             : v[ERROR_AGE] * 1000;
        }
        c->error_sec = c->fine_error / 1000;
        u->headers_in.no_cache = 0;
        u->headers_in.expired = 0;
    }

    if (c->fine_deadline == 0) {
        if (!r->cached || u->fine_validation) {
            return NGX_OK;
        }

        c->fine_status = NGX_HTTP_CACHE_FINE_COARSE;

        return ngx_http_fine_coarse_age(r, ordinary, seconds);
    }

    if (ordinary) {
        ordinary->hash = 0;
    }

    age = now - c->fine_birth;

    if (age < 0) {
        /* The clock stepped back.  Report no age instead of a wrong one. */
        ngx_http_cache_fine_refuse(r, NGX_HTTP_CACHE_FINE_CLOCK);
        return NGX_OK;
    }

    c->fine_status = NGX_HTTP_CACHE_FINE_USED;

    /* Age and Age-Ms come from one estimate, and Age is its floor. */
    if (ngx_http_fine_add(r, coarse, age / 1000) != NGX_OK) {
        return NGX_ERROR;
    }

    if (!u->conf->cache_fine_age_header) {
        return NGX_OK;
    }

    return ngx_http_fine_add(r, fine, age);
}
