# Millisecond proxy cache experiment

This branch compares HTTP cache policy at a shield and an edge. For example, an origin can send:

```http
Cache-Control: max-age=0, max-age-ms=200
Cache-Control: stale-while-revalidate=0, stale-while-revalidate-ms=200
```

Set `proxy_cache_fine_time origin;` at the cache connected directly to the origin. Set `proxy_cache_fine_time trusted;` at an edge only if the entire upstream cache path maintains fine age. A shield might send `Age: 0` and `Age-Ms: 175`. The edge has at most 25 ms of freshness left, including its request delay. At 200 ms the response is stale. Background update needs the usual nginx proxy cache configuration.

The default is `off`. To use the legacy policy at a boundary that might replay stale fine age, configure `proxy_cache_fine_time off;` and `proxy_hide_header Age-Ms;`. A trusted receiver falls back on a missing, invalid or inconsistent age pair. Presence of a plausible pair cannot identify an old cache on the path.

### Other companions

| Companion | Prototype behaviour |
|---|---|
| `stale-if-error-ms` (response) | Paired with `stale-if-error`. Sets the error window in milliseconds. A valid pair alone makes the entry fine. |
| `max-age-ms`, `min-fresh-ms` (request) | Applied to entries with fine age when `proxy_cache_fine_request on;` is set. A refused entry is fetched again and is not served stale. |
| Cache-Status `ttl-ms` | `$upstream_cache_ttl` and `$upstream_cache_ttl_ms` give the remaining freshness of an entry with fine age. Both are empty for whole-second entries and negative once stale. |
| `max-stale-ms`, request `stale-if-error-ms` | Not implemented. |
| `Retry-After-Ms`, Prefer `wait-ms` | Passed through. nginx neither generates nor consumes these. |

`proxy_cache_fine_request` defaults to `off`, because stock nginx ignores request Cache-Control and a client could otherwise force origin fetches. A missing or invalid base directive leaves the request unconstrained. An invalid companion yields to its base.

nginx honours stale-if-error only for failures that reach `ngx_http_upstream_next`, such as connection errors and timeouts. That is unchanged.

A Cache-Status field can be composed from the variables:

```nginx
map $upstream_cache_ttl_ms $fine_ttl {
    ""      "";
    default "; ttl=$upstream_cache_ttl; ttl-ms=$upstream_cache_ttl_ms";
}
add_header Cache-Status "edge$fine_ttl";
```

### Build and test on Linux

From the nginx repository root, using the build shell for the working tree:

```sh
nix develop ./contrib/subsecond-cache#build -c sh -c '
  auto/configure && make -j"$(nproc)" &&
  sh contrib/subsecond-cache/tests/native.sh &&
  export TEST_NGINX_BINARY=$PWD/objs/nginx &&
  cd "$NGINX_TESTS" && prove proxy_cache*.t'
```

### Build and test on Apple Silicon

Run these from this directory in the Git checkout:

```sh
nix build
nix run .#demo
nix run .#bench -- --output "$PWD/results"
nix develop -c sh -c 'cd "$NGINX_TESTS" && prove proxy_cache*.t'
```

The integration tests are `proxy_cache_fine*.t` on the `http-cache-millisecond-freshness` branch of [randomizedcoder/nginx-tests](https://github.com/randomizedcoder/nginx-tests/tree/http-cache-millisecond-freshness), a fork of the nginx test suite. The flake pins that branch. The tests start local nginx processes and are skipped on a binary without the new directives. The upgrade test needs `TEST_NGINX_BASELINE`, which the default shell sets. For the native comparison and header tests, run these commands at the nginx repository root on Darwin with the system compiler:

```sh
auto/configure --without-http_rewrite_module --without-http_gzip_module
make -j8
contrib/subsecond-cache/tests/native.sh
```

These tests use fixed timestamps and the compiled production code. The Nix demo and integration-test builds include the rewrite module. Building with `--without-http-cache` also checks that non-cache builds compile.

### Layered simulation

`nix run .#layers` shows how quickly an origin update reaches a client through a chain of caches. It takes about 15 seconds. The chain follows a live stream from an origin in New York to viewers in Melbourne.

| Address | Role | `--wan` delay each way |
|---|---|---:|
| `127.0.0.10` | Origin, New York | |
| `127.0.0.11` | New York cache, `proxy_cache_fine_time origin;` | 1 ms |
| `127.0.0.12` | Chicago cache, `trusted` | 10 ms |
| `127.0.0.13` | Los Angeles cache, `trusted` | 25 ms |
| `127.0.0.14` | Sydney cache, `trusted` | 75 ms |
| `127.0.0.15` | Melbourne cache, `trusted` | 6 ms |

- **Origin.** Returns its own clock in milliseconds as the body and in `X-Origin-Time`. Two paths differ only in Cache-Control: `/second` sends `max-age=1` and `/fine` sends `max-age=0, max-age-ms=100`.
- **Caches.** Each proxies to the address before it.
- **Client.** wrk pulls both paths through Melbourne at the same time.
- **Measurement.** Melbourne logs its own time and the `X-Origin-Time` it sent. The difference is the staleness of that response.
- **Delay.** Loopback has none. `--wan` puts a small TCP relay on each hop that holds traffic for the delay shown, 117 ms one way and 234 ms for the round trip. The figures are illustrative, not measured routes.

Three runs on x86_64 Linux, ten seconds, four connections per path. Staleness is in milliseconds:

| Run | Path | Responses | Mean | p50 | p99 | Max | Origin req/s |
|---|---|---:|---:|---:|---:|---:|---:|
| Loopback | `/second` | 225988 | 999.2 | 996.0 | 1974.0 | 1999.0 | 1.8 |
| Loopback | `/fine`, 100 ms | 219498 | 48.2 | 48.0 | 94.0 | 111.0 | 35.9 |
| `--wan` | `/second` | 451257 | 1001.3 | 1008.0 | 1861.0 | 1880.0 | 2.0 |
| `--wan` | `/fine`, 100 ms | 164 | 124.8 | 123.0 | 144.0 | 144.0 | 12.3 |
| `--wan --fine-ms 500` | `/second` | 386656 | 1010.7 | 1016.0 | 1862.0 | 1881.0 | 2.0 |
| `--wan --fine-ms 500` | `/fine`, 500 ms | 62713 | 207.9 | 210.0 | 276.0 | 470.0 | 6.4 |

Reading the result:

- **The fine lifetime holds across five caches.** Each cache deducts the age it received, so the lifetimes do not add up.
- **The lifetime has to exceed the round trip to the origin.** With `--wan`, a 100 ms response is already stale when it reaches Melbourne. Nothing is reused there and every request crosses the Pacific: 164 responses in ten seconds.
- **At 500 ms the path works.** Melbourne reuses each response for about 266 ms. Requests that arrive during the 234 ms refresh wait for it, which is why the fine path served fewer responses. A stale-while-revalidate window would cover that gap. This run does not use one.
- **nginx keeps a `max-age=1` entry for one to two seconds**, because it stores whole-second deadlines.
- **The whole-second lifetimes do not add up here either.** All five caches share one clock and expire at the same second boundary, so they refresh together. Caches with independent clocks and demand would not. This simulation does not show that case.
- **The cost is origin requests.** One fetch per lifetime would be 10 a second at 100 ms. The rest are concurrent misses, because the caches run without `proxy_cache_lock`.
- **`proxy_cache_lock` is off on purpose.** A request waiting on the lock checks again only every 500 ms.
- This is one host with constant load. It measures cache behaviour, not players.

Options, after `--`:

| Option | Default | Meaning |
|---|---|---|
| `--layers` | 5 | Caches in the chain. Site names apply to five |
| `--wan` | off | Add the delays in the table above |
| `--delays` | none | One-way delay of each hop in ms, origin side first, comma separated |
| `--base` | `127.0.0.10` | Origin address. Caches take the following addresses |
| `--port` | 8080 | Port on every address. Relays use the next port |
| `--seconds` | 10 | Measured duration, after a two-second warm-up |
| `--connections` | 4 | wrk connections per path |
| `--fine-ms` | 100 | `max-age-ms` on the fine path |
| `--output` | temporary directory | Keeps configurations, access logs, wrk reports and `summary.json` |

Linux answers on every `127.0.0.0/8` address. On macOS add each address first, for example `sudo ifconfig lo0 alias 127.0.0.10`.

### Read the experiment

The demo runs four loopback nginx processes and several short scenarios. The full benchmark takes about an hour on an idle machine. Two origins share immutable media and an atomically published six-segment playlist. Each run selects a policy and routes players through the edge and shield to the corresponding origin. The publisher advances the sequence every second. The two mixed-version smoke runs use a stock shield, with fallback lifetimes of zero and one second.

Twenty staggered players track sequence numbers, reload at HLS intervals, download complete segments, and start simulated playback with three seconds buffered. The paced profile spends roughly 750 ms downloading each one-second segment. The player accounts for elapsed time at each completion. Gaps reset playback buffering. Reported buffer samples are immediately before crediting completed downloads. Stalls are accumulated at completions, so a stall extending beyond the final sample is censored.

Each benchmark repetition uses the same player phases across policies. Compare zero, one second and 200 ms at a zero stale window first. The background-update case adds 200 ms of stale serving. Read unloaded and wrk-loaded runs separately. Each summary records discovery and completion lag, p05 buffer, stalled player-seconds, late downloads, missing observations and origin playlist GETs per publication. wrk's achieved rate and errors describe the offered workload. Percentiles are over player-segment observations within each run.

The top-level comparison.csv gives the median of the per-run metrics for each scenario. Keep the per-run summary, events.csv, access logs and wrk.txt. The summary's sample poll ID joins player discovery and download times to the request on each tier that processed it. An edge hit needs no shield request. Use monotonic timestamps for player elapsed times and nginx wall timestamps for tier logs. The output directory holds temporary segment files and cache data as well as measurements. Use a fresh output path for each invocation.

### Implementation limits for reviewers

- This prototype implements integer milliseconds in ordinary Cache-Control on builds with 64-bit time_t. Existing v5 disk entries retain their layout. Only entries with fine policy use v6. Version 6 uses the existing date, expiry, update-window and error-window slots in milliseconds. Older binaries reject those entries and refill them.
- The cache measures upstream request delay using nginx's elapsed timer. Its stored deadlines and residence estimates use nginx's cached wall clock. Wall-clock steps and timer_resolution affect precision.
- On conditional validation of a fine entry the cache retires its existing disk header, merges the validation response's caching metadata, and serves the validated body. The next request refills the entry. This avoids reviving the old policy at the cost of an extra full fetch. The benchmark uses full manifest refreshes; the Perl integration test exercises the 304 path separately.
- This is a reference implementation for the HTTP WG discussion. The benchmark provides measured outcomes only after a run on the host.
