/* Drive the production header handler with nginx pools, lists and test time. */
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

static ngx_table_elt_t *
field(ngx_list_t *list, char *key, char *value)
{
    ngx_table_elt_t  *h;

    h = ngx_list_push(list);
    if (h == NULL) {
        exit(2);
    }
    ngx_memzero(h, sizeof(*h));
    h->key.data = (u_char *) key;
    h->key.len = strlen(key);
    h->value.data = (u_char *) value;
    h->value.len = strlen(value);
    h->hash = 1;
    return h;
}


int
main(int argc, char **argv)
{
    ngx_log_t                    log = { 0 };
    ngx_time_t                   clock = { 0 };
    ngx_http_cache_t             cache = { 0 };
    ngx_http_request_t           r = { 0 };
    ngx_http_upstream_t          u = { 0 };
    ngx_http_upstream_conf_t     conf = { 0 };
    ngx_http_upstream_headers_in_t validation = { 0 };
    ngx_http_upstream_state_t    state = { 0 };
    ngx_table_elt_t             *h, *line;
    ngx_list_part_t             *part;
    ngx_uint_t                   i;

    if (argc != 6 && argc != 7) {
        return 2;
    }
    ngx_pagesize = 4096;
    clock.sec = 100;
    ngx_cached_time = &clock;
    r.pool = ngx_create_pool(16384, &log);
    if (r.pool == NULL) {
        return 2;
    }
    r.upstream = &u;
    r.cache = &cache;
    u.conf = &conf;
    u.state = &state;
    u.cacheable = 1;
    u.headers_in.status_n = 200;
    state.header_time = 15;
    conf.cache_fine_time = atoi(argv[1]);
    if (ngx_list_init(&u.headers_in.headers, r.pool, 4,
                      sizeof(ngx_table_elt_t)) != NGX_OK)
    {
        return 2;
    }
    line = field(&u.headers_in.headers, "Cache-Control", argv[2]);
    u.headers_in.cache_control = line;
    if (*argv[3]) {
        line->next = field(&u.headers_in.headers, "Cache-Control", argv[3]);
    }
    if (*argv[4]) {
        field(&u.headers_in.headers, "Age", argv[4]);
    }
    if (*argv[5]) {
        field(&u.headers_in.headers, "Age-Ms", argv[5]);
    }

    if (argc == 7) {
        /* Raw headers came from the stored 200. Supply a separate 304. */
        if (ngx_list_init(&validation.headers, r.pool, 4,
                          sizeof(ngx_table_elt_t)) != NGX_OK)
        {
            return 2;
        }
        if (*argv[6]) {
            validation.cache_control = field(&validation.headers,
                                             "Cache-Control", argv[6]);
        }
        r.cached = 1;
        cache.fine_deadline = 100;
        u.fine_validation = &validation;
    }

    if (ngx_http_cache_fine_headers(&r) != NGX_OK) {
        return 1;
    }
    printf("%lld %lld", (long long) (cache.fine_deadline
                            ? cache.fine_deadline - 100000 : 0),
                            (long long) cache.fine_window);
    for (part = &u.headers_in.headers.part; part; part = part->next) {
        h = part->elts;
        for (i = 0; i < part->nelts; i++) {
            if (h[i].hash && h[i].key.len == 6
                && ngx_strncasecmp(h[i].key.data, (u_char *) "age-ms", 6) == 0)
            {
                printf(" %.*s", (int) h[i].value.len, h[i].value.data);
            }
        }
    }
    putchar('\n');
    ngx_destroy_pool(r.pool);
    return 0;
}
