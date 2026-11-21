#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

int
main(void)
{
    size_t  i;
    struct {
        int64_t    now, deadline, window;
        ngx_int_t  expected;
    } cases[] = {
        { 0, 0, 0, NGX_HTTP_CACHE_EXPIRED },
        { 199, 200, 200, NGX_HTTP_CACHE_HIT },
        { 200, 200, 200, NGX_HTTP_CACHE_UPDATING },
        { 201, 200, 200, NGX_HTTP_CACHE_UPDATING },
        { 399, 200, 200, NGX_HTTP_CACHE_UPDATING },
        { 400, 200, 200, NGX_HTTP_CACHE_EXPIRED },
        { 401, 200, 200, NGX_HTTP_CACHE_EXPIRED },
        { 200, 200, 0, NGX_HTTP_CACHE_EXPIRED },
        { 999, 1250, 0, NGX_HTTP_CACHE_HIT },
        { 1000, 1250, 0, NGX_HTTP_CACHE_HIT },
        { 1250, 1250, 0, NGX_HTTP_CACHE_EXPIRED }
    };

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (ngx_http_cache_fine_status(cases[i].now, cases[i].deadline,
                                       cases[i].window) != cases[i].expected)
        {
            fprintf(stderr, "deadline case %zu failed\n", i);
            return 1;
        }
    }

    return 0;
}
