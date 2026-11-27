
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
ngx_uint_t ngx_http_cache_fine_refused(ngx_http_request_t *r, int64_t now);
int64_t ngx_http_cache_now_ms(void);
void ngx_http_cache_fine_refuse(ngx_http_request_t *r, ngx_uint_t status);
extern ngx_uint_t  ngx_http_cache_fine_fixed_clock;
int64_t ngx_http_cache_fine_offset(void);

/*
 * The wall clock less the monotonic clock is constant unless the wall clock
 * steps or the host is suspended. A fine entry is reusable only while that
 * offset is within this many milliseconds of its value when stored.
 */
#define NGX_HTTP_CACHE_FINE_STEP     10
#define NGX_HTTP_CACHE_FINE_NONE     INT64_MIN

static ngx_inline ngx_uint_t
ngx_http_cache_fine_continuous(int64_t stored, int64_t current)
{
    if (stored == NGX_HTTP_CACHE_FINE_NONE) {
        return 0;
    }

    return stored > current ? stored - current <= NGX_HTTP_CACHE_FINE_STEP
                            : current - stored <= NGX_HTTP_CACHE_FINE_STEP;
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
    int64_t                          fine_offset;
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
    ngx_uint_t                       fine_status;
    ngx_uint_t                       fine_clock;

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

    ngx_event_t                      wait_event;

    unsigned                         lock:1;
    unsigned                         waiting:1;

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
} ngx_http_file_cache_header_t;


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
