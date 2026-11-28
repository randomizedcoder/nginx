# Millisecond proxy cache experiment

This branch compares HTTP cache policy at a shield and an edge. For example, an origin can send:

```http
Cache-Control: max-age=0, max-age-ms=200
Cache-Control: stale-while-revalidate=0, stale-while-revalidate-ms=200
```

Set `proxy_cache_fine_time origin;` at the cache connected directly to the origin. Set `proxy_cache_fine_time trusted;` at an edge only if the entire upstream cache path maintains fine age. A shield might send `Age: 0` and `Age-Ms: 175`. The edge has at most 25 ms of freshness left, including its request delay. At 200 ms the response is stale. Background update needs the usual nginx proxy cache configuration.

| Directive | Default | Meaning |
|---|---|---|
| `proxy_cache_fine_time off \| origin \| trusted` | `off` | Whether this location keeps fine age, and whether its upstream is an origin or a cache trusted to send `Age-Ms` |
| `proxy_cache_fine_age_header on \| off` | `off` | Send `Age-Ms` with responses that have fine age. Turn it on only where the recipient is a cache configured with `trusted` |
| `proxy_cache_fine_request on \| off` | `off` | Apply request `max-age-ms` and `min-fresh-ms` |

- **`Age-Ms` is withheld by default.** A cache with fine age still sends `Age`, in whole seconds, from the same estimate. Fine timing reaches a recipient only where `proxy_cache_fine_age_header on;` is set. A shield sets it for its edge; an edge serving clients leaves it off.
- **Fine time is off by default.** To use the legacy policy at a boundary that might replay stale fine age, configure `proxy_cache_fine_time off;` and `proxy_hide_header Age-Ms;`. A trusted receiver falls back on a missing, invalid or inconsistent age pair. Presence of a plausible pair cannot identify an old cache on the path.
- **The clock is read, not cached.** nginx updates its cached time once per event loop pass, or once per `timer_resolution` interval. Fine decisions and reported ages read the clock instead, so a millisecond value is not derived from a coarser one. The request delay is measured from a cached start time, which can only be early and so overstates the delay.
- **A clock fault is not hidden.** An entry whose age would be negative, or whose wall clock has stepped against the monotonic clock, is not reused and no age is reported for it. `proxy_cache_fine_time` is rejected at configuration time on a build without a monotonic clock or with a 32-bit `time_t`.
- **A whole-second entry reports its age.** In a location with fine time, a stored entry without fine age is served with `Age` equal to the age it arrived with plus the whole seconds it has been stored. Stock nginx replays the stored `Age` unchanged.
- **Fallback is visible.** `$upstream_cache_fine_status` and an `info` log line say whether fine age was used:

| `$upstream_cache_fine_status` | Meaning |
|---|---|
| empty | Fine time is off for the location, or the request did not use the cache |
| `used` | The response has fine age |
| `coarse` | The response asked for nothing fine, or cannot be stored |
| `fallback:policy` | A companion was present, but the status is not 200, `X-Accel-Expires` applies or Cache-Control is ignored |
| `fallback:companion` | A companion was unpaired, repeated or malformed |
| `fallback:age` | `Age` and `Age-Ms` were missing, invalid or inconsistent, or an origin sent either |
| `fallback:range` | The age received was too large to give a deadline |
| `clock` | A stored entry was refused after a restart, a clock step or a suspend, and fetched again |

### Other companions

| Companion | Prototype behaviour |
|---|---|
| `stale-if-error-ms` (response) | Paired with `stale-if-error`. Sets the error window in milliseconds. A valid pair alone makes the entry fine. |
| `max-age-ms`, `min-fresh-ms` (request) | Applied to entries with fine age when `proxy_cache_fine_request on;` is set. A refused entry is fetched again and is not served stale. |
| Cache-Status `ttl-ms` | `$upstream_cache_ttl` and `$upstream_cache_ttl_ms` give the remaining freshness of an entry with fine age. Both are empty for whole-second entries and negative once stale. |
| `max-stale-ms`, request `stale-if-error-ms` | Not implemented. |
| `Retry-After-Ms`, Prefer `wait-ms` | Passed through. nginx neither generates nor consumes these. |

`proxy_cache_fine_request` defaults to `off`, because stock nginx ignores request Cache-Control and a client could otherwise force origin fetches. With it on, any client can send `Cache-Control: max-age=0` and turn each of its requests into an origin request. Enable it only where clients are trusted, or behind `limit_req`. A missing or invalid base directive leaves the request unconstrained. An invalid companion yields to its base.

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

These tests use fixed timestamps and the production source. The fixed clock is compiled only into the test programs. The Nix demo and integration-test builds include the rewrite module. Building with `--without-http-cache` also checks that non-cache builds compile.

### Tests that catch the prototype's own mistakes

An earlier version of this prototype made nine mistakes, listed in the draft under "Implementation Pitfalls", and later review found ten more problems. Each now has table-driven test rows, every row with a `description` and an `expected` outcome, tagged by pitfall and by kind: positive, negative, boundary or corner.

The first nine fixes were written before this harness, so theirs is not a test-first history. The evidence is the reverse check: each patch in `tests/mutants/` puts one mistake back into a scratch copy of the source, and the suites must then fail.

```sh
nix develop ./contrib/subsecond-cache#build -c \
    sh contrib/subsecond-cache/tests/mutants.sh
```

It needs the `nginx-tests` checkout beside this tree, or `NGINX_TESTS_DIR`. It never patches the working tree, and exits non-zero if the unpatched source fails or any mistake is not caught. Result on 27 November 2026:

| Pitfall | Mistake put back | Test tables | Rows by kind (positive, negative, boundary, corner) | Rows that failed | Result |
|---|---|---|---|---|---|
| 1 | Use the time nginx stored earlier | `clock.c`; `proxy_cache_fine_resolution.t` | 3, 3, 4, 5 | 16 (6 unit, 10 integration) | Caught |
| 2 | Request delay from the stored header time | `headers.t` | 4, 2, 3, 2 | 44 unit | Caught |
| 3 | Negative age clamped to zero | `headers.t` | 1, 1, 2, 2 | 4 unit | Caught |
| 4 | No check for a monotonic clock | `deadlines.c`; `monotonic.t` | 3, 3, 2, 4 | 7 unit | Caught |
| 5 | No check of the width of `time_t` | `deadlines.c` | 1, 1, 2, 2 | 3 unit | Caught |
| 6 | `Age-Ms` sent whenever there is a fine age | `headers.t`; `proxy_cache_fine_exposure.t` | 6, 5, 2, 5 | 13 (5 unit, 8 integration) | Caught |
| 7 | Out-of-range age makes the response uncacheable | `headers.t` | 1, 1, 2, 2 | 4 unit | Caught |
| 8 | Stored whole-second entry served with the `Age` it arrived with | `headers.t`; `proxy_cache_fine_exposure.t` | 3, 1, 3, 2 | 6 (5 unit, 1 integration) | Caught |
| 9 | Nothing recorded when fine age is not used | `headers.t`; `proxy_cache_fine_exposure.t` | 2, 9, 3, 4 | 46 (33 unit, 13 integration) | Caught |
| 10 | Lock waiter checks every 500 ms | `deadlines.c`; `proxy_cache_fine_lock.t`; `proxy_cache_fine_lock_workers.t` | 5, 3, 6, 7 | 2 integration | Caught |
| 11 | Every fine entry retired after a 304 | `proxy_cache_fine_validate.t` | 2, 2, 2, 4 | 18 integration | Caught |
| 12 | A log line for every fallback | `deadlines.c`; `proxy_cache_fine_exposure.t` | 2, 3, 3, 3 | 2 integration | Caught |
| 13 | No warning for a zone used both ways | `deadlines.c`; `zones.t` | 6, 4, 3, 5 | 3 unit | Caught |
| 14 | Fixed clock of the tests compiled into nginx | `symbols.t` | 1, 1, 1, 1 | 2 unit | Caught |
| 15 | Only the times written back after a 304 | `deadlines.c`; `proxy_cache_fine_validate.t` | 1, 1, 2, 1 | 5 integration | Caught |
| 16 | Clock offset not kept in the cache file | `deadlines.c`; `proxy_cache_fine_exposure.t`; `proxy_cache_fine_validate.t` | 3, 5, 6, 6 | 112 (4 unit, 108 integration) | Caught |
| 17 | Fine entry unreadable without fine time | `deadlines.c`; `proxy_cache_fine_off.t` | 2, 2, 4, 4 | 7 integration | Caught |
| 18 | `Age-Ms` to every recipient of a location | `deadlines.c`; `agevalue.t`; `proxy_cache_fine_exposure.t` | 6, 8, 6, 7 | 3 integration | Caught |
| 19 | Waiting requests not woken | `proxy_cache_fine_lock.t` | 2, 1, 1, 2 | 6 integration | Caught |

Rows 10 to 19 are the later problems under "Known operational problems".

On the unpatched source the same run passed 97 `deadlines` rows, 9 `clock` rows, 94 `headers.t` rows, 8 `monotonic.t` rows, 10 `zones.t` rows, 10 `agevalue.t` rows, 4 `symbols.t` rows and 434 integration tests.

Limits of this evidence:

- **Pitfall 4** is checked end to end by rebuilding with `NGX_HAVE_CLOCK_MONOTONIC` turned off in `objs/ngx_auto_config.h` and running `nginx -t`. That is this host's build with the macro changed, not a platform that lacks the clock.
- **Pitfall 5** is checked only through the function that makes the decision, with the width passed in. No 32-bit build was made.
- **Pitfall 3** has two parts. The rows catch the clamp where the age is written. The refusal of a future entry at lookup is in the same patch and has no row of its own, because it needs a clock that steps back under a running server.
- **Pitfall 2** is caught by unit rows only. The integration test with `timer_resolution 100ms` did not fail with that mistake put back.
- **Integration counts vary between runs** for pitfalls 1 to 3, where rows depend on timing. In this run pitfall 3 was caught by unit rows only.
- **Problems 10, 11, 12, 15, 17, 18 and 19 are caught by integration rows only.** Their unit rows test a helper, and the patch changes its caller.
- **Problem 10** is now caught only by `proxy_cache_fine_lock_workers.t`, which runs four worker processes: within one worker the wake-up of problem 19 hides the timer.
- **Problems 10 and 19 are timing rows.** The limits leave room for a loaded machine, and the run above was on one with a load average near 30 on 24 cores.
- **Problem 16's mutant** stores a zero offset, which refuses every fine entry and not only those read after a restart. That is why 108 integration rows fail.

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

- **Fine age is carried across five caches.** Each cache deducts the age it received, so five caches did not multiply the lifetime. This is not a strict bound on delivery age.
- **The lifetime has to exceed the round trip to the origin.** With `--wan`, a 100 ms response is already stale when it reaches Melbourne. Nothing is reused there and every request crosses the Pacific: 164 responses in ten seconds.
- **At 500 ms the path works.** Melbourne reuses each response for about 266 ms. Requests that arrive during the 234 ms refresh wait for it, which is why the fine path served fewer responses. A stale-while-revalidate window would cover that gap. This run does not use one.
- **nginx keeps a `max-age=1` entry for one to two seconds**, because it stores whole-second deadlines.
- **The whole-second path here is nginx's behaviour, not RFC 9111's.** Stock nginx forwards an Age field it receives, but neither interprets nor generates one, so each cache applies the full `max-age` from the moment it stores a response. In this run all five caches share one clock and expire at the same second boundary, so their lifetimes did not add up. The runs change the lifetime and the age accounting together and do not isolate either.
- **The cost is origin requests.** One fetch per lifetime would be 10 a second at 100 ms. The rest are concurrent misses, because the caches run without `proxy_cache_lock`.
- **`proxy_cache_lock` was off in these runs.**

Repeated on 27 November with the later fixes, on a busier host, with and without `--lock`:

| Run | Path | Lock | Responses | Mean | p50 | p99 | Max | Origin req/s |
|---|---|---|---:|---:|---:|---:|---:|---:|
| Loopback | `/fine`, 100 ms | Off | 197003 | 48.8 | 49.0 | 96.0 | 181.0 | 22.9 |
| Loopback | `/fine`, 100 ms | On | 87235 | 47.4 | 47.0 | 95.0 | 237.0 | 24.7 |
| `--wan` | `/fine`, 100 ms | Off | 156 | 133.8 | 129.0 | 228.0 | 228.0 | 10.8 |
| `--wan` | `/fine`, 100 ms | On | 156 | 132.5 | 130.0 | 164.0 | 165.0 | 10.4 |
| `--wan --fine-ms 500` | `/fine`, 500 ms | Off | 22468 | 203.7 | 202.0 | 273.0 | 475.0 | 5.3 |
| `--wan --fine-ms 500` | `/fine`, 500 ms | On | 34234 | 210.3 | 208.0 | 313.0 | 577.0 | 4.9 |

- **The means repeat; the maxima are larger.** The cause of the larger maxima was not isolated.
- **The lock did not reduce origin requests.** `proxy_cache_lock` makes requests wait only for an entry that does not exist yet. For an entry that has expired, one request is marked as updating and the others go upstream. A stale window, `stale-while-revalidate-ms` or `proxy_cache_use_stale updating`, is what collapses refreshes.
- The `/second` rows and the summaries are in the [proposal repository](https://github.com/randomizedcoder/draft-seddon-httpbis-subsecond-freshness/tree/main/results/layers-2026-11-27).
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
| `--path` | the two paths above | `NAME=POLICY`, repeatable. Replaces the default paths, for example `--path "paired=max-age=1, max-age-ms=1000"` |
| `--modes` | `origin`, then `trusted` | `proxy_cache_fine_time` of each cache, origin side first, comma separated. For misconfiguration runs, for example `origin,trusted,off,trusted,trusted` |
| `--lock` | off | `proxy_cache_lock on` in every cache |
| `--output` | temporary directory | Keeps configurations, access logs, wrk reports and `summary.json` |

Linux answers on every `127.0.0.0/8` address. On macOS add each address first, for example `sudo ifconfig lo0 alias 127.0.0.10`.

### Read the experiment

The demo runs four loopback nginx processes and several short scenarios. The full benchmark takes about an hour on an idle machine. Two origins share immutable media and an atomically published six-segment playlist. Each run selects a policy and routes players through the edge and shield to the corresponding origin. The publisher advances the sequence every second. The two mixed-version smoke runs use a stock shield, with fallback lifetimes of zero and one second.

Twenty staggered players track sequence numbers, reload at HLS intervals, download complete segments, and start simulated playback with three seconds buffered. The paced profile spends roughly 750 ms downloading each one-second segment. The player accounts for elapsed time at each completion. Gaps reset playback buffering. Reported buffer samples are immediately before crediting completed downloads. Stalls are accumulated at completions, so a stall extending beyond the final sample is censored.

Each benchmark repetition uses the same player phases across policies. Compare zero, one second and 200 ms at a zero stale window first. The background-update case adds 200 ms of stale serving. Read unloaded and wrk-loaded runs separately. Each summary records discovery and completion lag, p05 buffer, stalled player-seconds, late downloads, missing observations and origin playlist GETs per publication. wrk's achieved rate and errors describe the offered workload. Percentiles are over player-segment observations within each run.

The top-level comparison.csv gives the median of the per-run metrics for each scenario. Keep the per-run summary, events.csv, access logs and wrk.txt. The summary's sample poll ID joins player discovery and download times to the request on each tier that processed it. An edge hit needs no shield request. Use monotonic timestamps for player elapsed times and nginx wall timestamps for tier logs. The output directory holds temporary segment files and cache data as well as measurements. Use a fresh output path for each invocation.

### Implementation status for reviewers

The detailed [prototype conformance and evidence table](https://github.com/randomizedcoder/draft-seddon-httpbis-subsecond-freshness/blob/main/explainer.md#prototype-conformance)
is maintained with the proposal. It separates the draft's requirements from
implemented behavior and reported checks, including the elapsed-time,
Age-Ms parameter, targeted-field and Expires-only gaps. Prototype source
revision: `9198f9a28f58b8afc8c06817f51175b4fbeea786`.

Later commits on this branch read the clock for fine decisions,
withhold `Age-Ms` unless `proxy_cache_fine_age_header on;` is set, report
fallbacks in `$upstream_cache_fine_status`, and fall back to whole seconds
on an out-of-range age instead of declining to store. Measurements published
before this change used the cached clock and sent `Age-Ms` to every client.
Still not implemented: targeted fields, `max-stale-ms`, request
`stale-if-error-ms`, and fine stale windows for Expires-only responses.
A fine entry is kept across a restart of nginx, and refused after a clock
step, a suspend or a reboot.

### Known operational problems

Found in review after the nine fixes above. Each correction has table rows and a patch in `tests/mutants/` that puts the problem back. Problems 10 to 13 were corrected in revision `694bc608c`, and 14 to 19 in revision `092c1979b`.

| # | Problem | Correction | Limit |
|---|---|---|---|
| 10 | A request waiting on `proxy_cache_lock` checked again only every 500 ms, so collapsing failed at shorter lifetimes | On a location with `proxy_cache_fine_time` the first check is after 5 ms and the interval doubles to 500 ms. Other locations are unchanged | This timer is now the path for a fetch in another worker process; see 19 |
| 11 | A 304 retired the fine entry, so the next request fetched it in full | The entry is updated in place when the stored header block keeps its length: identical fields, or a `Date` or `Cache-Control` of the same length | Any other change still retires the entry |
| 12 | Each fallback wrote one `info` line | One line per reason per second per worker, with the number left out. `$upstream_cache_fine_status` still reports every fallback | The count is per worker |
| 13 | Two locations share a cache zone, one with `proxy_cache_fine_time` and one without; each overwrites the other's file version for a shared key | `nginx -t` warns | A warning, not an error, because the keys may not overlap. A zone named by a variable is not checked |
| 14 | The unit tests fixed the clock through a variable compiled into nginx | The variable exists only in the test build, under `NGX_HTTP_CACHE_FINE_TEST`. `symbols.t` checks the nginx binary with `nm` | The unit tests compile their own copy of `ngx_http_cache_fine.c` with the macro set |
| 15 | The in-place update after a 304 was two writes, header block then times | One write from the start of the file. No length changes, so a reader that sees part of it still finds a well-formed entry | The rows show that the header block and the times are both written. They cannot tell one write from two |
| 16 | A restart refused every fine entry, because the clock offset was kept only in shared memory | The low 32 bits of the offset are kept in the cache file, in what was padding, so the header keeps its size. An entry is reused after a restart of nginx and its age carries on | A clock step, suspend or reboot still refuses the entry, as it should: the elapsed time is unknown. The comparison is modulo 2^32 ms, 49.7 days. A v6 file written before this change is refused once |
| 17 | Turning `proxy_cache_fine_time` off made every v6 entry unreadable | A location without fine time reads a v6 entry in whole seconds and never past its millisecond deadline. A 304 there rewrites it as v5 | The entry can expire up to a second early there. An nginx without this patch still rejects v6 files |
| 18 | `proxy_cache_fine_age_header on` sent `Age-Ms` to every client of the location | The directive also takes a variable, evaluated for each request. An empty value, `0` and `off` withhold the field, so a `geo` or `map` can name the downstream caches | The choice of recipients is the operator's. `on` still means everyone |
| 19 | A waiting request found out that the fetch had ended only by its timer | A fetch that ends wakes the requests waiting for it in the same worker process, and they look at the cache again together | Requests in other worker processes still use the timer of problem 10 |

Still as before, by design or because the limit is in nginx:

| Behaviour | Effect | What to do |
|---|---|---|
| A clock step, suspend or reboot refuses every fine entry in the zone at once | Each is treated as expired. With `proxy_cache_revalidate on` it is validated, and a 304 keeps it; otherwise it is fetched in full. Stale windows are not used for a refused entry | `proxy_cache_lock on` limits this to one request per key. Size the origin for one request for every hot key at once |
| A response whose lifetime is shorter than its fetch is stale when it arrives | Requests that waited for it cannot use it, and each fetches in turn. RFC 9111 Section 4 allows a response to serve collapsed requests only where the cache may reuse it for them | Give the response a lifetime longer than the fetch, or a stale window: with `stale-while-revalidate-ms` and `proxy_cache_background_update on` the waiting requests are all served it while one refreshes it |
| A waiting request in another worker process is not woken | It sees the response up to one timer interval late: 5 ms at first, then doubling to 500 ms | None in this prototype. nginx has no wake-up between worker processes for the cache lock |
| A downstream cache is set to `trusted` before its upstream sends it `Age-Ms` | The downstream cache gets no `Age-Ms` and applies the base: a longer lifetime, or with a base of zero an origin request per request | Turn the header on upstream first; remove `trusted` downstream first |

Fine entries use v6 cache files on 64-bit time_t builds. The binary reads v5;
older binaries reject v6 and refill. A 304 merges metadata and serves the
validated body; the entry is then updated in place or retired, as in the
table above. These are implementation
choices, not wire requirements. The benchmark uses full playlist refreshes,
so its results do not establish the cost of repeated conditional validation.

The HLS benchmark ran on 25 November on one busy Linux machine, with a local
harness change to record failed reloads and resume completed runs. The
[measurements](https://github.com/randomizedcoder/draft-seddon-httpbis-subsecond-freshness/blob/main/measurements.md#test-8-simulated-hls-players)
record repetitions, results, local raw-artifact locations and missing build
provenance. They are observations of this prototype, not a real-player or
independent-cache result. It was rerun on 27 November with the later fixes.
Seven of the eight unpaced rows were close to the first run. `max-age=0`
under load no longer stalled, and the origin then served about 3400
playlist requests per segment.
