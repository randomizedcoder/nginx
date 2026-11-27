/*
 * Pitfall 1: a fine decision reads the clock.  It does not reuse the value
 * that nginx stores once per event loop pass or timer_resolution interval.
 */
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

static int64_t
wall(void)
{
    struct timeval  tv;

    ngx_gettimeofday(&tv);
    return (int64_t) tv.tv_sec * 1000 + tv.tv_usec / 1000;
}


int
main(void)
{
    size_t      i, failed;
    int64_t     before, after, stored, got;
    ngx_time_t *clock;

    /*
     * lag is how far the stored time is behind the real clock, in ms; a
     * negative lag is a stored time ahead of it.  fixed selects the test
     * seam.  expected is 1 when the reading must come from the real clock,
     * and 0 when it must be the stored value.  counter rows check the
     * monotonic millisecond counter instead, which starts at zero.
     */
    struct {
        const char  *description;
        const char  *tags;
        int64_t      lag;
        ngx_uint_t   fixed;
        ngx_uint_t   counter;
        ngx_uint_t   expected;
    } cases[] = {
        { "stored time one timer_resolution interval behind", "1:positive",
          100, 0, 0, 1 },
        { "stored time ten seconds behind", "1:positive", 10000, 0, 0, 1 },
        { "stored time one millisecond behind", "1:boundary", 1, 0, 0, 1 },
        { "stored time equal to the clock", "1:boundary", 0, 0, 0, 1 },
        { "stored time five seconds ahead", "1:corner", -5000, 0, 0, 1 },
        { "stored time a day behind", "1:corner", 86400000, 0, 0, 1 },
        { "monotonic counter is refreshed with the reading", "1:corner",
          100, 0, 1, 1 },
        { "test seam returns the stored value", "1:negative",
          5000, 1, 0, 0 },
        { "test seam leaves the monotonic counter alone", "1:negative",
          5000, 1, 1, 0 }
    };

    failed = 0;

    /* Point ngx_cached_time at nginx's own storage, as in a server. */
    ngx_time_update();

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {

        /* Overwrite the stored time, as if it had been read earlier. */
        clock = (ngx_time_t *) ngx_cached_time;

        before = wall();
        stored = before - cases[i].lag;

        clock->sec = stored / 1000;
        clock->msec = stored % 1000;
        ngx_current_msec = 0;
        ngx_http_cache_fine_fixed_clock = cases[i].fixed;

        got = ngx_http_cache_now_ms();
        after = wall();

        if (cases[i].counter) {
            if ((ngx_current_msec != 0) == cases[i].expected) {
                continue;
            }

        } else if (cases[i].expected) {
            if (got >= before && got <= after) {
                continue;
            }

        } else if (got == stored) {
            continue;
        }

        fprintf(stderr, "clock reading: %s failed\n", cases[i].description);
        failed++;
    }

    printf("clock: %d rows, %d failures\n",
           (int) (sizeof(cases) / sizeof(cases[0])), (int) failed);

    return failed != 0;
}
