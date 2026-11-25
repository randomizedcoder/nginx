#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

int
main(void)
{
    size_t  i, failed;
    struct {
        const char  *description;
        int64_t      now, deadline, window;
        ngx_int_t    expected;
    } status[] = {
        { "no policy", 0, 0, 0, NGX_HTTP_CACHE_EXPIRED },
        { "last fresh millisecond", 199, 200, 200, NGX_HTTP_CACHE_HIT },
        { "stale at the deadline", 200, 200, 200, NGX_HTTP_CACHE_UPDATING },
        { "inside the window", 201, 200, 200, NGX_HTTP_CACHE_UPDATING },
        { "last window millisecond", 399, 200, 200, NGX_HTTP_CACHE_UPDATING },
        { "window end", 400, 200, 200, NGX_HTTP_CACHE_EXPIRED },
        { "after the window", 401, 200, 200, NGX_HTTP_CACHE_EXPIRED },
        { "no window", 200, 200, 0, NGX_HTTP_CACHE_EXPIRED },
        { "before a second boundary", 999, 1250, 0, NGX_HTTP_CACHE_HIT },
        { "on a second boundary", 1000, 1250, 0, NGX_HTTP_CACHE_HIT },
        { "past a second boundary", 1250, 1250, 0, NGX_HTTP_CACHE_EXPIRED }
    };
    struct {
        const char  *description;
        int64_t      age, remaining, max_age, min_fresh;
        ngx_uint_t   expected;
    } request[] = {
        { "no request directives", 1600, 400, -1, -1, 1 },
        { "draft example, max-age-ms=1750", 1600, 400, 1750, -1, 1 },
        { "draft example, min-fresh-ms=500", 1600, 400, -1, 500, 0 },
        { "age equals max-age", 1750, 250, 1750, -1, 1 },
        { "age one past max-age", 1751, 249, 1750, -1, 0 },
        { "max-age of zero at age zero", 0, 2000, 0, -1, 1 },
        { "max-age of zero at age one", 1, 1999, 0, -1, 0 },
        { "remaining equals min-fresh", 1500, 500, -1, 500, 1 },
        { "remaining one short of min-fresh", 1501, 499, -1, 500, 0 },
        { "min-fresh of zero on the deadline", 2000, 0, -1, 0, 1 },
        { "min-fresh of zero when stale", 2001, -1, -1, 0, 0 },
        { "both satisfied", 100, 1900, 200, 1000, 1 },
        { "max-age fails, min-fresh passes", 300, 1700, 200, 1000, 0 },
        { "max-age passes, min-fresh fails", 100, 900, 200, 1000, 0 },
        { "clock stepped back", -5, 2005, 0, -1, 1 }
    };

    failed = 0;

    for (i = 0; i < sizeof(status) / sizeof(status[0]); i++) {
        if (ngx_http_cache_fine_status(status[i].now, status[i].deadline,
                                       status[i].window) != status[i].expected)
        {
            fprintf(stderr, "status: %s failed\n", status[i].description);
            failed++;
        }
    }

    for (i = 0; i < sizeof(request) / sizeof(request[0]); i++) {
        if (ngx_http_cache_fine_acceptable(request[i].age,
                                           request[i].remaining,
                                           request[i].max_age,
                                           request[i].min_fresh)
            != request[i].expected)
        {
            fprintf(stderr, "request: %s failed\n", request[i].description);
            failed++;
        }
    }

    return failed != 0;
}
