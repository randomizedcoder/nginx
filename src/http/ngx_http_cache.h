
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_HTTP_CACHE_H_INCLUDED_
#define _NGX_HTTP_CACHE_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


#define NGX_HTTP_CACHE_MISS          1
#define NGX_HTTP_CACHE_BYPASS        2
#define NGX_HTTP_CACHE_EXPIRED       3
#define NGX_HTTP_CACHE_STALE         4
#define NGX_HTTP_CACHE_UPDATING      5
#define NGX_HTTP_CACHE_REVALIDATED   6
#define NGX_HTTP_CACHE_HIT           7
#define NGX_HTTP_CACHE_SCARCE        8

ngx_int_t ngx_http_cache_fine_headers(ngx_http_request_t *r);
void ngx_http_cache_fine_update(ngx_http_request_t *r, ngx_int_t rc);
ngx_uint_t ngx_http_cache_fine_refused(ngx_http_request_t *r, int64_t now);
int64_t ngx_http_cache_now_ms(void);
void ngx_http_cache_fine_refuse(ngx_http_request_t *r, ngx_uint_t status);
#if (NGX_HTTP_CACHE_FINE_TEST)
extern ngx_uint_t  ngx_http_cache_fine_fixed_clock;
#endif
int64_t ngx_http_cache_fine_offset(void);

/*
 * The wall clock less the monotonic clock is constant unless the wall clock
 * steps or the host is suspended or rebooted.  It is the same after this
 * server is restarted.  A fine entry is reusable only while that offset is
 * within this many milliseconds of its value when stored.
 *
 * The cache file keeps the low 32 bits of the offset, which fit in the
 * padding at the end of its header, and the comparison is modulo 2^32
 * milliseconds, 49.7 days.
 */
#define NGX_HTTP_CACHE_FINE_STEP     10

static ngx_inline ngx_uint_t
ngx_http_cache_fine_continuous(uint32_t stored, int64_t current)
{
    uint32_t  ahead, behind;

    ahead = (uint32_t) current - stored;
    behind = stored - (uint32_t) current;

    return ahead <= NGX_HTTP_CACHE_FINE_STEP
           || behind <= NGX_HTTP_CACHE_FINE_STEP;
}


/*
 * proxy_cache_fine_age_header may be a variable, so that Age-Ms goes only
 * to chosen recipients.  An empty value, "0" and "off" mean no.
 */
static ngx_inline ngx_uint_t
ngx_http_cache_fine_enabled(u_char *value, size_t len)
{
    if (len == 0 || (len == 1 && value[0] == '0')) {
        return 0;
    }

    if (len == 3 && (value[0] == 'o' || value[0] == 'O')
        && (value[1] == 'f' || value[1] == 'F')
        && (value[2] == 'f' || value[2] == 'F'))
    {
        return 0;
    }

    return 1;
}


/*
 * The whole-second expiry of an entry with a millisecond deadline, for a
 * location without fine time.  nginx treats an entry as fresh to the end
 * of the second valid_sec.
 */
static ngx_inline time_t
ngx_http_cache_fine_whole(int64_t deadline)
{
    return (time_t) (deadline / 1000) - 1;
}


static ngx_inline ngx_int_t
ngx_http_cache_fine_status(int64_t now, int64_t deadline, int64_t window)
{
    if (now < deadline) {
        return NGX_HTTP_CACHE_HIT;
    }

    if (now - deadline < window) {
        return NGX_HTTP_CACHE_UPDATING;
    }

    return NGX_HTTP_CACHE_EXPIRED;
}


/*
 * A fallback is logged at most once a second for each reason.  Returns the
 * number of lines left out since the last one written, or -1 when this
 * one is to be left out.
 */
static ngx_inline ngx_int_t
ngx_http_cache_fine_log_due(time_t now, time_t *last, ngx_uint_t *skipped)
{
    ngx_int_t  n;

    if (now == *last) {
        (*skipped)++;
        return -1;
    }

    n = (ngx_int_t) *skipped;
    *last = now;
    *skipped = 0;

    return n;
}


/*
 * A request waiting for the cache lock looks again after this interval.
 * Stock nginx uses 500 ms, which is longer than a sub-second lifetime: the
 * response the waiter was waiting for is stale again when it looks.  With
 * fine time the first interval is short and each later one is doubled, up
 * to the stock value.
 */
#define NGX_HTTP_CACHE_LOCK_WAIT       500
#define NGX_HTTP_CACHE_FINE_LOCK_WAIT  5

static ngx_inline ngx_msec_t
ngx_http_cache_lock_wait(ngx_msec_t remaining, ngx_msec_t *wait)
{
    ngx_msec_t  timer;

    if (*wait == 0) {
        *wait = NGX_HTTP_CACHE_LOCK_WAIT;
    }

    timer = (remaining > *wait) ? *wait : remaining;

    *wait = (*wait >= NGX_HTTP_CACHE_LOCK_WAIT / 2) ? NGX_HTTP_CACHE_LOCK_WAIT
                                                    : *wait * 2;

    return timer;
}


/*
 * A cache zone holds one file version for each key.  Locations that share
 * a zone and differ in fine time replace each other's files.  Records the
 * first setting seen for the zone and reports a later one that differs.
 */
static ngx_inline ngx_uint_t
ngx_http_cache_fine_zone_mixed(ngx_uint_t *recorded, ngx_uint_t fine)
{
    fine = fine ? 2 : 1;

    if (*recorded == 0) {
        *recorded = fine;
        return 0;
    }

    return *recorded != fine;
}


/*
 * Fine age needs a monotonic clock, to detect steps of the wall clock, and
 * a time_t wide enough for millisecond deadlines.  Returns what is missing,
 * or NULL when fine age can be enabled.
 */
static ngx_inline const char *
ngx_http_cache_fine_unsupported(ngx_uint_t monotonic, size_t time_size)
{
    if (!monotonic) {
        return "a monotonic clock";
    }

    if (time_size < 8) {
        return "a 64-bit time_t";
    }

    return NULL;
}


/* A negative limit means the request did not send that directive. */
static ngx_inline ngx_uint_t
ngx_http_cache_fine_acceptable(int64_t age, int64_t remaining,
    int64_t max_age, int64_t min_fresh)
{
    return (max_age < 0 || age <= max_age)
           && (min_fresh < 0 || remaining >= min_fresh);
}


#define NGX_HTTP_CACHE_KEY_LEN       16
#define NGX_HTTP_CACHE_ETAG_LEN      128
#define NGX_HTTP_CACHE_VARY_LEN      128

#define NGX_HTTP_CACHE_VERSION       5
#define NGX_HTTP_CACHE_FINE_VERSION  6

#define NGX_HTTP_CACHE_FINE_ORIGIN   1
#define NGX_HTTP_CACHE_FINE_TRUSTED  2

/* Why a response did or did not get fine age, for logs and the variable. */
#define NGX_HTTP_CACHE_FINE_USED       1
#define NGX_HTTP_CACHE_FINE_COARSE     2
#define NGX_HTTP_CACHE_FINE_POLICY     3
#define NGX_HTTP_CACHE_FINE_COMPANION  4
#define NGX_HTTP_CACHE_FINE_AGE        5
#define NGX_HTTP_CACHE_FINE_RANGE      6
#define NGX_HTTP_CACHE_FINE_CLOCK      7


typedef struct {
    ngx_uint_t                       status;
    time_t                           valid;
} ngx_http_cache_valid_t;


typedef struct {
    ngx_rbtree_node_t                node;
    ngx_queue_t                      queue;

    u_char                           key[NGX_HTTP_CACHE_KEY_LEN
                                         - sizeof(ngx_rbtree_key_t)];

    unsigned                         count:20;
    unsigned                         uses:10;
    unsigned                         valid_msec:10;
    unsigned                         error:10;
    unsigned                         exists:1;
    unsigned                         updating:1;
    unsigned                         deleting:1;
    unsigned                         purged:1;
                                     /* 10 unused bits */

    ngx_file_uniq_t                  uniq;
    time_t                           expire;
    time_t                           valid_sec;
    size_t                           body_start;
    off_t                            fs_size;
    ngx_msec_t                       lock_time;
} ngx_http_file_cache_node_t;


struct ngx_http_cache_s {
    ngx_file_t                       file;
    ngx_array_t                      keys;
    uint32_t                         crc32;
    u_char                           key[NGX_HTTP_CACHE_KEY_LEN];
    u_char                           main[NGX_HTTP_CACHE_KEY_LEN];

    ngx_file_uniq_t                  uniq;
    time_t                           valid_sec;
    time_t                           updating_sec;
    time_t                           error_sec;
    time_t                           last_modified;
    time_t                           date;

    int64_t                          fine_birth;
    int64_t                          fine_deadline;
    int64_t                          fine_window;
    int64_t                          fine_error;
    int64_t                          fine_offset;
    uint32_t                         fine_stored;
    ngx_uint_t                       fine_status;
    ngx_uint_t                       fine_clock;
    ngx_uint_t                       fine_inplace;

    ngx_str_t                        etag;
    ngx_str_t                        vary;
    u_char                           variant[NGX_HTTP_CACHE_KEY_LEN];

    size_t                           buffer_size;
    size_t                           header_start;
    size_t                           body_start;
    off_t                            length;
    off_t                            fs_size;

    ngx_uint_t                       min_uses;
    ngx_uint_t                       error;
    ngx_uint_t                       valid_msec;
    ngx_uint_t                       vary_tag;

    ngx_buf_t                       *buf;

    ngx_http_file_cache_t           *file_cache;
    ngx_http_file_cache_node_t      *node;

#if (NGX_THREADS || NGX_COMPAT)
    ngx_thread_task_t               *thread_task;
#endif

    ngx_msec_t                       lock_timeout;
    ngx_msec_t                       lock_age;
    ngx_msec_t                       lock_time;
    ngx_msec_t                       wait_time;
    ngx_msec_t                       lock_wait;

    ngx_event_t                      wait_event;
    ngx_queue_t                      wait_queue;

    unsigned                         lock:1;
    unsigned                         waiting:1;
    unsigned                         wait_queued:1;
    unsigned                         woken:1;

    unsigned                         updated:1;
    unsigned                         updating:1;
    unsigned                         exists:1;
    unsigned                         temp_file:1;
    unsigned                         purged:1;
    unsigned                         reading:1;
    unsigned                         secondary:1;
    unsigned                         update_variant:1;
    unsigned                         background:1;

    unsigned                         stale_updating:1;
    unsigned                         stale_error:1;
};


typedef struct {
    ngx_uint_t                       version;
    time_t                           valid_sec;
    time_t                           updating_sec;
    time_t                           error_sec;
    time_t                           last_modified;
    time_t                           date;
    uint32_t                         crc32;
    u_short                          valid_msec;
    u_short                          header_start;
    u_short                          body_start;
    u_char                           etag_len;
    u_char                           etag[NGX_HTTP_CACHE_ETAG_LEN];
    u_char                           vary_len;
    u_char                           vary[NGX_HTTP_CACHE_VARY_LEN];
    u_char                           variant[NGX_HTTP_CACHE_KEY_LEN];
    uint32_t                         fine_offset;
} ngx_http_file_cache_header_t;


/*
 * fine_offset takes the place of padding.  The header has to keep its
 * size, because a version 5 file has the same layout.
 */
typedef char  ngx_http_file_cache_header_size_check
    [sizeof(time_t) < 8 || sizeof(ngx_http_file_cache_header_t) == 336
     ? 1 : -1];


/* The times of a version 6 header, as written after a 304. */
static ngx_inline void
ngx_http_cache_fine_stamp(ngx_http_file_cache_header_t *h, int64_t birth,
    int64_t deadline, int64_t window, int64_t error, int64_t offset)
{
    h->date = (time_t) birth;
    h->valid_sec = (time_t) deadline;
    h->updating_sec = (time_t) window;
    h->error_sec = (time_t) error;
    h->fine_offset = (uint32_t) offset;
}


typedef struct {
    ngx_rbtree_t                     rbtree;
    ngx_rbtree_node_t                sentinel;
    ngx_queue_t                      queue;
    ngx_atomic_t                     cold;
    ngx_atomic_t                     loading;
    off_t                            size;
    ngx_uint_t                       count;
    ngx_uint_t                       watermark;
} ngx_http_file_cache_sh_t;


struct ngx_http_file_cache_s {
    ngx_http_file_cache_sh_t        *sh;
    ngx_slab_pool_t                 *shpool;

    ngx_path_t                      *path;

    off_t                            min_free;
    off_t                            max_size;
    size_t                           bsize;

    time_t                           inactive;

    time_t                           fail_time;

    ngx_uint_t                       files;
    ngx_uint_t                       loader_files;
    ngx_msec_t                       last;
    ngx_msec_t                       loader_sleep;
    ngx_msec_t                       loader_threshold;

    ngx_uint_t                       manager_files;
    ngx_msec_t                       manager_sleep;
    ngx_msec_t                       manager_threshold;

    ngx_shm_zone_t                  *shm_zone;

    ngx_uint_t                       use_temp_path;
                                     /* unsigned use_temp_path:1 */

    ngx_uint_t                       fine;
                                     /* 0 unknown, 1 whole seconds, 2 fine */
};


ngx_int_t ngx_http_file_cache_new(ngx_http_request_t *r);
ngx_int_t ngx_http_file_cache_create(ngx_http_request_t *r);
void ngx_http_file_cache_create_key(ngx_http_request_t *r);
ngx_int_t ngx_http_file_cache_open(ngx_http_request_t *r);
ngx_int_t ngx_http_file_cache_set_header(ngx_http_request_t *r, u_char *buf);
void ngx_http_file_cache_update(ngx_http_request_t *r, ngx_temp_file_t *tf);
void ngx_http_file_cache_update_header(ngx_http_request_t *r);
ngx_int_t ngx_http_cache_send(ngx_http_request_t *);
void ngx_http_file_cache_free(ngx_http_cache_t *c, ngx_temp_file_t *tf);
time_t ngx_http_file_cache_valid(ngx_array_t *cache_valid, ngx_uint_t status);

char *ngx_http_file_cache_set_slot(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
char *ngx_http_file_cache_valid_set_slot(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);


extern ngx_str_t  ngx_http_cache_status[];


#endif /* _NGX_HTTP_CACHE_H_INCLUDED_ */
