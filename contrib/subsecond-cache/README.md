# Millisecond proxy cache experiment

This branch compares HTTP cache policy at a shield and an edge. For example, an origin can send:

```http
Cache-Control: max-age=0, max-age-ms=200
Cache-Control: stale-while-revalidate=0, stale-while-revalidate-ms=200
```

Set `proxy_cache_fine_time origin;` at the cache connected directly to the origin. Set `proxy_cache_fine_time trusted;` at an edge only if the entire upstream cache path maintains fine age. A shield might send `Age: 0` and `Age-Ms: 175`. The edge has at most 25 ms of freshness left, including its request delay. At 200 ms the response is stale. Background update needs the usual nginx proxy cache configuration.

The default is `off`. To use the legacy policy at a boundary that might replay stale fine age, configure `proxy_cache_fine_time off;` and `proxy_hide_header Age-Ms;`. A trusted receiver falls back on a missing, invalid or inconsistent age pair. Presence of a plausible pair cannot identify an old cache on the path.

### Build and test on Apple Silicon

Run these from this directory in the Git checkout:

```sh
nix build
nix run .#demo
nix run .#bench -- --output "$PWD/results"
nix develop -c prove -v tests/proxy_cache_fine.t tests/cache_upgrade.t
nix develop -c sh -c 'cd "$NGINX_TESTS" && prove -Ilib proxy_cache*.t'
```

The pinned Test::Nginx suite supplies the harness. Its integration tests start local nginx processes. For the native comparison and header tests, run these commands at the nginx repository root on Darwin with the system compiler:

```sh
auto/configure --without-http_rewrite_module --without-http_gzip_module
make -j8
contrib/subsecond-cache/tests/native.sh
```

These tests use fixed timestamps and the compiled production code. The Nix demo and integration-test builds include the rewrite module. Building with `--without-http-cache` also checks that non-cache builds compile.

### Read the experiment

The demo runs four loopback nginx processes and several short scenarios. The full benchmark takes about an hour on an idle machine. Two origins share immutable media and an atomically published six-segment playlist. Each run selects a policy and routes players through the edge and shield to the corresponding origin. The publisher advances the sequence every second. The two mixed-version smoke runs use a stock shield, with fallback lifetimes of zero and one second.

Twenty staggered players track sequence numbers, reload at HLS intervals, download complete segments, and start simulated playback with three seconds buffered. The paced profile spends roughly 750 ms downloading each one-second segment. The player accounts for elapsed time at each completion. Gaps reset playback buffering. Reported buffer samples are immediately before crediting completed downloads. Stalls are accumulated at completions, so a stall extending beyond the final sample is censored.

Each benchmark repetition uses the same player phases across policies. Compare zero, one second and 200 ms at a zero stale window first. The background-update case adds 200 ms of stale serving. Read unloaded and wrk-loaded runs separately. Each summary records discovery and completion lag, p05 buffer, stalled player-seconds, late downloads, missing observations and origin playlist GETs per publication. wrk's achieved rate and errors describe the offered workload. Percentiles are over player-segment observations within each run.

The top-level comparison.csv gives the median of the per-run metrics for each scenario. Keep the per-run summary, events.csv, access logs and wrk.txt. The summary's sample poll ID joins player discovery and download times to the request on each tier that processed it. An edge hit needs no shield request. Use monotonic timestamps for player elapsed times and nginx wall timestamps for tier logs. The output directory holds temporary segment files and cache data as well as measurements. Use a fresh output path for each invocation.

### Implementation limits for reviewers

- This prototype implements integer milliseconds in ordinary Cache-Control on builds with 64-bit time_t. Existing v5 disk entries retain their layout. Only entries with fine policy use v6. Version 6 uses the existing date, expiry and update-window slots in milliseconds. Older binaries reject those entries and refill them.
- The cache measures upstream request delay using nginx's elapsed timer. Its stored deadlines and residence estimates use nginx's cached wall clock. Wall-clock steps and timer_resolution affect precision.
- On conditional validation of a fine entry the cache retires its existing disk header, merges the validation response's caching metadata, and serves the validated body. The next request refills the entry. This avoids reviving the old policy at the cost of an extra full fetch. The benchmark uses full manifest refreshes; the Perl integration test exercises the 304 path separately.
- This is a reference implementation for the HTTP WG discussion. The benchmark provides measured outcomes only after a run on the host.
