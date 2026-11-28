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

    struct {
        const char  *description;
        uint32_t     stored;
        int64_t      current;
        ngx_uint_t   expected;
    } clock[] = {
        { "unchanged offset", 5000, 5000, 1 },
        { "jitter forwards within the limit", 5000, 5010, 1 },
        { "jitter backwards within the limit", 5000, 4990, 1 },
        { "step forwards past the limit", 5000, 5011, 0 },
        { "step backwards past the limit", 5000, 4989, 0 },
        { "one second back", 5000, 4000, 0 },
        { "suspend for a minute", 5000, 65000, 0 },
        { "16:negative file written before the offset was stored", 0,
          1700000000000, 0 },
        { "zero offset without a monotonic clock", 0, 0, 1 },
        { "16:positive offset wider than 32 bits, unchanged",
          (uint32_t) 1700000000000, 1700000000000, 1 },
        { "16:negative offset wider than 32 bits, 11 ms on",
          (uint32_t) 1700000000000, 1700000000011, 0 },
        { "16:boundary offset wider than 32 bits, 10 ms on",
          (uint32_t) 1700000000000, 1700000000010, 1 },
        { "16:boundary jitter across the 32-bit wrap, forwards", 0xfffffffb,
          0x100000005, 1 },
        { "16:boundary jitter across the 32-bit wrap, backwards", 5,
          0xfffffffc, 1 },
        { "16:boundary step across the 32-bit wrap", 0xfffffffb,
          0x100000006, 0 },
        { "16:corner negative offset, unchanged", (uint32_t) -5000, -5000,
          1 },
        { "16:corner negative offset, jitter", (uint32_t) -5000, -5004, 1 },
        { "16:corner a step of exactly 2^32 ms is not seen", 5000,
          0x100000000 + 5000, 1 }
    };

    /*
     * Pitfalls 4 and 5: fine age is refused where elapsed time cannot be
     * checked or a millisecond deadline does not fit.  expected is the
     * reason for refusing, or NULL when fine age may be enabled.
     */
    struct {
        const char  *description;
        const char  *tags;
        ngx_uint_t   monotonic;
        size_t       time_size;
        const char  *expected;
    } support[] = {
        { "monotonic clock and 64-bit time_t", "4:positive 5:positive", 1, 8,
          NULL },
        { "no monotonic clock", "4:negative", 0, 8, "a monotonic clock" },
        { "32-bit time_t", "5:negative", 1, 4, "a 64-bit time_t" },
        { "time_t one byte short of 64 bits", "5:boundary", 1, 7,
          "a 64-bit time_t" },
        { "time_t wider than 64 bits", "5:boundary", 1, 16, NULL },
        { "neither: the clock is reported first", "4:corner 5:corner", 0, 4,
          "a monotonic clock" },
        { "no monotonic clock with a wide time_t", "4:corner", 0, 16,
          "a monotonic clock" },
        { "zero-width time_t", "5:corner", 1, 0, "a 64-bit time_t" }
    };

    /*
     * Problem 15: after a 304 the times of a stored entry are set in the
     * header that is written back, and nothing else in it changes.
     * expected is the offset kept in the file, the low 32 bits.
     */
    struct {
        const char  *description;
        int64_t      birth, deadline, window, error, offset;
        uint32_t     expected;
    } stamp[] = {
        { "15:positive times and offset are set", 1700000000123,
          1700000000323, 500, 2000, 5000, 5000 },
        { "15:negative zero windows are written as zero", 1700000000123,
          1700000000323, 0, 0, 5000, 5000 },
        { "15:boundary offset of 2^32 - 1 is kept whole", 1, 2, 3, 4,
          0xffffffff, 0xffffffff },
        { "15:boundary offset of 2^32 keeps its low bits", 1, 2, 3, 4,
          0x100000000, 0 },
        { "15:corner negative offset keeps its low bits", 1, 2, 3, 4, -1,
          0xffffffff }
    };

    /*
     * Problem 10: a request waiting for the cache lock looks again after
     * an interval that starts short with fine time and doubles.  Each row
     * gives the time left to wait and the interval so far; expected is
     * the timer set and the interval for the next look.
     */
    struct {
        const char  *description;
        const char  *tags;
        ngx_msec_t   remaining, wait;
        ngx_msec_t   expected, expected_next;
    } lock[] = {
        { "fine time, first look", "10:positive", 5000, 5, 5, 10 },
        { "fine time, second look", "10:positive", 5000, 10, 10, 20 },
        { "fine time, eighth look reaches the stock interval",
          "10:boundary", 5000, 320, 320, 500 },
        { "doubling stops at the stock interval", "10:boundary", 5000, 250,
          250, 500 },
        { "one below half the stock interval still doubles", "10:boundary",
          5000, 249, 249, 498 },
        { "stock location keeps 500 ms", "10:negative", 5000, 500, 500, 500 },
        { "time left is shorter than the interval", "10:corner", 3, 5, 3,
          10 },
        { "no time left", "10:corner", 0, 5, 0, 10 },
        { "interval never set is the stock one", "10:corner", 5000, 0, 500,
          500 }
    };

    /*
     * Problem 12: one log line per reason per second.  Each row is one
     * fallback at time now, after a line at time last with some left out
     * since; expected is the count reported, or -1 for no line.
     */
    struct {
        const char  *description;
        const char  *tags;
        time_t       now, last;
        ngx_uint_t   skipped;
        ngx_int_t    expected;
        ngx_uint_t   expected_skipped;
    } logged[] = {
        { "first fallback is logged", "12:positive", 100, 0, 0, 0, 0 },
        { "second fallback in the same second is not", "12:negative", 100,
          100, 0, -1, 1 },
        { "twentieth in the same second is not", "12:negative", 100, 100, 18,
          -1, 19 },
        { "first in the next second reports those left out", "12:boundary",
          101, 100, 19, 19, 0 },
        { "next second with none left out", "12:boundary", 101, 100, 0, 0,
          0 },
        { "clock stepped back is logged", "12:corner", 99, 100, 4, 4, 0 },
        { "long quiet period", "12:corner", 100000, 100, 1, 1, 0 }
    };

    /*
     * Problem 13: a zone used with and without fine time.  Each row is a
     * location that uses a zone whose recorded setting is given (0 none,
     * 1 whole seconds, 2 fine); expected is whether to warn.
     */
    struct {
        const char  *description;
        const char  *tags;
        ngx_uint_t   recorded, fine;
        ngx_uint_t   expected, expected_recorded;
    } zone[] = {
        { "first location, whole seconds", "13:positive", 0, 0, 0, 1 },
        { "first location, fine", "13:positive", 0, 1, 0, 2 },
        { "second fine location", "13:positive", 2, 1, 0, 2 },
        { "second whole-second location", "13:positive", 1, 0, 0, 1 },
        { "fine location on a whole-second zone", "13:negative", 1, 1, 1, 1 },
        { "whole-second location on a fine zone", "13:negative", 2, 0, 1, 2 },
        { "trusted and origin are both fine", "13:boundary", 2, 2, 0, 2 },
        { "a third location still warns", "13:corner", 1, 2, 1, 1 }
    };

    failed = 0;

    /*
     * Problem 17: a location without fine time uses a millisecond deadline
     * in whole seconds.  nginx serves an entry to the end of the second
     * valid_sec, so the row holds when (expected + 1) * 1000 <= deadline.
     */
    struct {
        const char  *description;
        int64_t      deadline;
        time_t       expected;
    } whole[] = {
        { "17:positive deadline in the middle of a second", 60500, 59 },
        { "17:negative one second earlier would lose a usable second",
          61500, 60 },
        { "17:boundary deadline on a second", 60000, 59 },
        { "17:boundary one millisecond before a second", 59999, 58 },
        { "17:boundary one millisecond after a second", 60001, 59 },
        { "17:corner deadline within the first second", 200, -1 }
    };

    /*
     * Problem 18: proxy_cache_fine_age_header may be a variable.  expected
     * is whether its value for a request sends Age-Ms.
     */
    struct {
        const char  *description;
        const char  *value;
        ngx_uint_t   expected;
    } enabled[] = {
        { "18:positive 1", "1", 1 },
        { "18:positive on", "on", 1 },
        { "18:negative empty", "", 0 },
        { "18:negative 0", "0", 0 },
        { "18:negative off", "off", 0 },
        { "18:boundary OFF in capitals", "OFF", 0 },
        { "18:boundary 00 is not 0", "00", 1 },
        { "18:boundary of is not off", "of", 1 },
        { "18:corner a space", " ", 1 },
        { "18:corner offline is not off", "offline", 1 }
    };

    for (i = 0; i < sizeof(enabled) / sizeof(enabled[0]); i++) {
        if (ngx_http_cache_fine_enabled((u_char *) enabled[i].value,
                                        strlen(enabled[i].value))
            != enabled[i].expected)
        {
            fprintf(stderr, "enabled: %s failed\n", enabled[i].description);
            failed++;
        }
    }

    for (i = 0; i < sizeof(whole) / sizeof(whole[0]); i++) {
        time_t  valid;

        valid = ngx_http_cache_fine_whole(whole[i].deadline);

        if (valid != whole[i].expected
            || ((int64_t) valid + 1) * 1000 > whole[i].deadline)
        {
            fprintf(stderr, "whole: %s failed\n", whole[i].description);
            failed++;
        }
    }

    for (i = 0; i < sizeof(stamp) / sizeof(stamp[0]); i++) {
        ngx_http_file_cache_header_t  h, before;

        memset(&h, 0x5a, sizeof(h));
        before = h;

        ngx_http_cache_fine_stamp(&h, stamp[i].birth, stamp[i].deadline,
                                  stamp[i].window, stamp[i].error,
                                  stamp[i].offset);

        if (h.date != stamp[i].birth || h.valid_sec != stamp[i].deadline
            || h.updating_sec != stamp[i].window
            || h.error_sec != stamp[i].error
            || h.fine_offset != stamp[i].expected
            || h.version != before.version
            || h.last_modified != before.last_modified
            || h.crc32 != before.crc32
            || h.header_start != before.header_start
            || h.body_start != before.body_start
            || memcmp(h.etag, before.etag, sizeof(h.etag)) != 0
            || memcmp(h.vary, before.vary, sizeof(h.vary)) != 0
            || memcmp(h.variant, before.variant, sizeof(h.variant)) != 0)
        {
            fprintf(stderr, "stamp: %s failed\n", stamp[i].description);
            failed++;
        }
    }

    for (i = 0; i < sizeof(lock) / sizeof(lock[0]); i++) {
        ngx_msec_t  wait, timer;

        wait = lock[i].wait;
        timer = ngx_http_cache_lock_wait(lock[i].remaining, &wait);

        if (timer != lock[i].expected || wait != lock[i].expected_next) {
            fprintf(stderr, "lock: %s failed\n", lock[i].description);
            failed++;
        }
    }

    for (i = 0; i < sizeof(logged) / sizeof(logged[0]); i++) {
        time_t      last;
        ngx_uint_t  skipped;

        last = logged[i].last;
        skipped = logged[i].skipped;

        if (ngx_http_cache_fine_log_due(logged[i].now, &last, &skipped)
            != logged[i].expected
            || skipped != logged[i].expected_skipped)
        {
            fprintf(stderr, "logged: %s failed\n", logged[i].description);
            failed++;
        }
    }

    for (i = 0; i < sizeof(zone) / sizeof(zone[0]); i++) {
        ngx_uint_t  recorded;

        recorded = zone[i].recorded;

        if (ngx_http_cache_fine_zone_mixed(&recorded, zone[i].fine)
            != zone[i].expected
            || recorded != zone[i].expected_recorded)
        {
            fprintf(stderr, "zone: %s failed\n", zone[i].description);
            failed++;
        }
    }

    for (i = 0; i < sizeof(support) / sizeof(support[0]); i++) {
        const char  *reason;

        reason = ngx_http_cache_fine_unsupported(support[i].monotonic,
                                                 support[i].time_size);

        if ((reason == NULL) != (support[i].expected == NULL)
            || (reason && strcmp(reason, support[i].expected) != 0))
        {
            fprintf(stderr, "support: %s failed\n", support[i].description);
            failed++;
        }
    }

    for (i = 0; i < sizeof(clock) / sizeof(clock[0]); i++) {
        if (ngx_http_cache_fine_continuous(clock[i].stored, clock[i].current)
            != clock[i].expected)
        {
            fprintf(stderr, "clock: %s failed\n", clock[i].description);
            failed++;
        }
    }

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

    printf("deadlines: %d rows, %d failures\n",
           (int) (sizeof(support) / sizeof(support[0])
                  + sizeof(lock) / sizeof(lock[0])
                  + sizeof(stamp) / sizeof(stamp[0])
                  + sizeof(whole) / sizeof(whole[0])
                  + sizeof(enabled) / sizeof(enabled[0])
                  + sizeof(logged) / sizeof(logged[0])
                  + sizeof(zone) / sizeof(zone[0])
                  + sizeof(clock) / sizeof(clock[0])
                  + sizeof(status) / sizeof(status[0])
                  + sizeof(request) / sizeof(request[0])),
           (int) failed);

    return failed != 0;
}
