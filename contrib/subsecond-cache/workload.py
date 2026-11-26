"""Measure HLS segment discovery through a shield and an edge on loopback."""
import argparse
import concurrent.futures
import contextlib
import csv
import json
import math
import random
import re
import shutil
import socket
import statistics
import subprocess
import tempfile
import time
import urllib.request
from pathlib import Path

POLICIES = {
    "zero": "max-age=0",
    "second": "max-age=1",
    "fine": "max-age=0, max-age-ms=200",
    "background": ("max-age=0, max-age-ms=200, stale-while-revalidate=0, "
                   "stale-while-revalidate-ms=200"),
}
CLOCK = time.monotonic


def wait_until(deadline):
    time.sleep(max(0, deadline - CLOCK()))


def port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def encode(media, count):
    subprocess.run([
        "ffmpeg", "-nostdin", "-loglevel", "error", "-f", "lavfi", "-i",
        "testsrc2=size=160x90:rate=25", "-t", str(count), "-c:v", "mpeg2video",
        "-g", "25", "-sc_threshold", "1000000000", "-f", "segment",
        "-segment_time", "1", "-segment_format", "mpegts", str(media / "%05d.ts"),
    ], check=True)
    if len(list(media.glob("*.ts"))) != count:
        raise RuntimeError("encoder did not produce one segment per second")


def publish(root, media, index):
    # Hard-link immutable segments before making their URIs discoverable.
    target = root / f"{index:05}.ts"
    target.hardlink_to(media / target.name)
    first = max(0, index - 5)
    text = ["#EXTM3U", "#EXT-X-VERSION:3", "#EXT-X-TARGETDURATION:1",
            f"#EXT-X-MEDIA-SEQUENCE:{first}"]
    for seq in range(first, index + 1):
        text.extend(["#EXTINF:1.000,", f"{seq:05}.ts"])
    tmp = root / "next.m3u8"
    tmp.write_text("\n".join(text) + "\n")
    tmp.replace(root / "live.m3u8")
    return CLOCK()


@contextlib.contextmanager
def server(directory, binary, locations):
    directory.mkdir()
    (directory / "logs").mkdir()
    listen = port()
    conf = directory / "nginx.conf"
    conf.write_text(f"""
daemon off;
worker_processes 1;
pid nginx.pid;
error_log logs/error.log notice;
events {{ worker_connections 1024; }}
http {{
    log_format events '$msec $uri $status $upstream_cache_status '
                      '$http_x_player_poll $sent_http_age $sent_http_age_ms';
    access_log logs/access.log events;
    proxy_cache_path cache keys_zone=demo:1m;
    server {{ listen 127.0.0.1:{listen}; {locations} }}
}}
""")
    proc = subprocess.Popen([binary, "-p", str(directory), "-c", str(conf)])
    try:
        for _ in range(100):
            if proc.poll() is not None:
                raise RuntimeError(f"nginx exited: {directory}")
            try:
                with socket.create_connection(("127.0.0.1", listen), 0.1):
                    break
            except OSError:
                time.sleep(0.03)
        else:
            raise TimeoutError(f"nginx startup: {directory}")
        yield f"http://127.0.0.1:{listen}"
    finally:
        if proc.poll() is None:
            proc.terminate()
        proc.wait(timeout=10)


def player(number, url, start, stop, paced, seed):
    wait_until(start + random.Random(seed + number).random())
    last = None
    buffer = 0.0
    playing = False
    previous = CLOCK()
    events = []

    def advance(now):
        nonlocal buffer, previous
        elapsed = now - previous if playing else 0.0
        stall = max(0.0, elapsed - buffer)
        buffer = max(0.0, buffer - elapsed)
        previous = now
        return stall

    poll = 0
    failed = 0
    while CLOCK() < stop:
        sent = CLOCK()
        poll += 1
        request_id = f"{number}-{poll}"
        request = urllib.request.Request(url + "/live.m3u8",
                                         headers={"X-Player-Poll": request_id})
        try:
            with urllib.request.urlopen(request, timeout=5) as reply:
                names = [n for n in reply.read().decode().splitlines()
                         if n and not n.startswith("#")]
                status = reply.headers.get("X-Edge", "")
                age = reply.headers.get("Age-Ms", "")
        except OSError:
            # A reload that fails or times out is a result, so count it.
            failed += 1
            continue
        discovered = CLOCK()
        if last is None:
            last = int(names[-3][:-3]) - 1
        fresh = [n for n in names if int(n[:-3]) > last]
        for name in fresh:
            seq = int(name[:-3])
            missed = seq - last - 1
            began = CLOCK()
            with urllib.request.urlopen(url + "/" + name, timeout=5) as reply:
                size = int(reply.headers["Content-Length"])
                received = 0
                while chunk := reply.read(4096):
                    received += len(chunk)
                    if paced:
                        wait_until(began + 0.75 * received / size)
                if received != size:
                    raise IOError("incomplete segment")
            done = CLOCK()
            stall = advance(done)
            if missed:
                buffer = 0.0
                playing = False
            before = buffer
            buffer += 1.0
            playing |= buffer >= 3
            events.append((number, seq, discovered, began, done, before, stall,
                           missed, status, age, request_id))
            last = seq
        wait_until(sent + (1.0 if fresh else 0.5))
    return events, failed


def percentile(values, fraction):
    return sorted(values)[math.ceil(fraction * len(values)) - 1] if values else None


def run(args, media, out, policy, paced, loaded, rep, mixed=False):
    if (out / "summary.json").exists():
        # Keep completed runs so an interrupted benchmark can resume.
        return
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir()
    root = out / "media"
    root.mkdir()
    published = {i: publish(root, media, i) for i in range(6)}
    with contextlib.ExitStack() as stack:
        def launch(name, binary, config):
            return stack.enter_context(server(out / name, binary, config))

        def origin(name, header, extra=""):
            return launch(name, args.nginx, f"""
                root "{root}";
                location = /live.m3u8 {{ add_header Cache-Control "{header}"; {extra} }}
                location / {{ add_header Cache-Control "max-age=60"; }}
            """)

        legacy = origin("origin-a", POLICIES["second" if policy == "second" else "zero"])
        if mixed and policy == "second":
            fine = origin("origin-b", "max-age=1, max-age-ms=200",
                          "add_header Age 0; add_header Age-Ms 175;")
        else:
            fine = origin("origin-b", POLICIES[policy])
        origin_url = fine if mixed or policy in ("fine", "background") else legacy

        def proxy(name, upstream, binary, mode):
            option = f"proxy_cache_fine_time {mode};" if mode else ""
            return launch(name, binary, f"""
                location / {{
                    proxy_pass {upstream};
                    proxy_cache demo;
                    proxy_cache_lock on;
                    proxy_cache_background_update on;
                    {option}
                    add_header X-{name} $upstream_cache_status;
                }}
            """)
        shield = proxy("shield", origin_url, args.baseline if mixed else args.nginx,
                       None if mixed else "origin")
        if mixed:
            # The shield is explicitly outside the fine-age trust boundary.
            edge = launch("edge", args.nginx, f"""
                location / {{
                    proxy_pass {shield};
                    proxy_cache demo;
                    proxy_hide_header Age-Ms;
                    add_header X-edge $upstream_cache_status;
                }}
            """)
        else:
            edge = proxy("edge", shield, args.nginx, "trusted")
        start = CLOCK() + 0.2
        wall_start = time.time() + start - CLOCK()
        stop = start + args.warmup + args.seconds
        if loaded:
            log = stack.enter_context((out / "wrk.txt").open("w"))
            load = subprocess.Popen(["wrk", "-t2", "-c16", "--latency",
                                     f"-d{args.seconds + args.warmup}s",
                                     edge + "/live.m3u8"], stdout=log, stderr=log)
            def stop_load():
                if load.poll() is None:
                    load.terminate()
                load.wait()
            stack.callback(stop_load)
        with concurrent.futures.ThreadPoolExecutor(args.players) as pool:
            futures = [pool.submit(player, n, edge, start, stop, paced, rep * 100)
                       for n in range(args.players)]
            for i in range(args.seconds + args.warmup):
                wait_until(start + i)
                published[i + 6] = publish(root, media, i + 6)
            results = [future.result() for future in futures]
            events = [event for found, _ in results for event in found]
            failed = sum(count for _, count in results)
        if loaded and load.wait(timeout=10):
            raise RuntimeError("wrk failed; see wrk.txt")

    fields = "player seq published discovered fetch_start complete buffer_s stall_s missed edge age_ms poll_id"
    with (out / "events.csv").open("w") as f:
        writer = csv.writer(f)
        writer.writerow(fields.split())
        writer.writerows((n, seq, published[seq], *rest) for n, seq, *rest in events)
    selected = [e for e in events if start + args.warmup <= e[2] < stop]
    discovery = [(e[2] - published[e[1]]) * 1000 for e in selected]
    completion = [(e[4] - published[e[1]]) * 1000 for e in selected]
    window = (wall_start + args.warmup, wall_start + args.warmup + args.seconds)
    origin_gets = 0
    for path in out.glob("origin-*/logs/access.log"):
        for line in path.read_text().splitlines():
            stamp, uri, *_ = line.split()
            origin_gets += uri == "/live.m3u8" and window[0] <= float(stamp) < window[1]
    if not selected:
        raise RuntimeError("no measured segment discoveries")
    if mixed and any(e[9] or (policy == "fine" and e[8] == "HIT") for e in selected):
        raise AssertionError("mixed path violated its whole-second fallback")
    # Allow a full target duration for discovery before the measurement ends.
    cohort = {seq for seq, stamp in published.items()
              if start + args.warmup <= stamp < stop - 1.0}
    before_end = {(e[0], e[1]) for e in events if e[1] in cohort and e[2] < stop}
    stats = dict(policy=policy, paced=paced, load=loaded, rep=rep, mixed=mixed,
                 discoveries=len(selected), discovery_p50_ms=percentile(discovery, .5),
                 discovery_p95_ms=percentile(discovery, .95),
                 completion_p95_ms=percentile(completion, .95),
                 buffer_p05_s=percentile([e[5] for e in selected], .05),
                 stalled_player_seconds=sum(e[6] for e in selected),
                 undiscovered_player_segments=args.players * len(cohort) - len(before_end),
                 late_downloads=sum(e[6] > 0 for e in selected),
                 missed=sum(e[7] for e in selected),
                 failed_playlist_requests=failed,
                 origin_playlist_gets_per_segment=origin_gets / args.seconds)
    if loaded:
        report = (out / "wrk.txt").read_text()
        rate = re.search(r"Requests/sec:\s+([\d.]+)", report)
        if rate is None:
            raise RuntimeError("missing wrk request rate")
        stats["wrk_requests_per_second"] = float(rate.group(1))
        stats["wrk_errors"] = [line.strip() for line in report.splitlines()
                               if "errors:" in line or "Non-2xx" in line]
    events_in_order = sorted(selected, key=lambda e: e[2])
    sample = events_in_order[len(events_in_order) // 2]
    stats["sample"] = dict(player=sample[0], segment=sample[1],
                           published=published[sample[1]], discovered=sample[2],
                           fetch_start=sample[3], complete=sample[4],
                           edge=sample[8], age_ms=sample[9], poll_id=sample[10])
    (out / "summary.json").write_text(json.dumps(stats, indent=2))
    print(json.dumps(stats), flush=True)


def compare(root):
    groups = {}
    for summary in sorted(root.glob("*/summary.json")):
        run_stats = json.loads(summary.read_text())
        key = tuple(run_stats[k] for k in ("policy", "paced", "load", "mixed"))
        groups.setdefault(key, []).append(run_stats)
    metrics = ("discovery_p50_ms", "discovery_p95_ms", "completion_p95_ms",
               "buffer_p05_s", "stalled_player_seconds", "late_downloads",
               "undiscovered_player_segments", "failed_playlist_requests",
               "origin_playlist_gets_per_segment")
    # Preserve paired per-run observations in the original summaries.
    with (root / "comparison.csv").open("w") as stream:
        writer = csv.writer(stream)
        writer.writerow(("policy", "paced", "load", "mixed", "repetitions", *metrics))
        for key, repetitions in sorted(groups.items()):
            writer.writerow((*key, len(repetitions),
                             *(statistics.median(r.get(m, 0) for r in repetitions)
                               for m in metrics)))
    print(f"Comparison (median of per-run metrics): {root / 'comparison.csv'}")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("mode", choices=["demo", "bench"])
    p.add_argument("--nginx", required=True)
    p.add_argument("--baseline", required=True)
    p.add_argument("--output", type=Path)
    p.add_argument("--seconds", type=int)
    p.add_argument("--players", type=int, default=20)
    args = p.parse_args()
    bench = args.mode == "bench"
    if args.seconds is None:
        args.seconds = 60 if bench else 6
    args.warmup = 10 if bench else 2
    if args.seconds < 1 or args.players < 1:
        p.error("seconds and players must be positive")
    root = (args.output or Path(tempfile.mkdtemp(prefix="nginx-hls-"))).resolve()
    root.mkdir(exist_ok=True)
    print(f"Results: {root}", flush=True)
    media = root / "encoded"
    if not media.exists():
        media.mkdir()
        encode(media, 6 + args.seconds + args.warmup)
    for rep in range(3 if bench else 1):
        for paced in ([False, True] if bench else [False]):
            for loaded in ([False, True] if bench else [False]):
                for policy in POLICIES:
                    run(args, media, root / f"{policy}-{rep}-{int(paced)}-{int(loaded)}",
                        policy, paced, loaded, rep)
    for policy in ("fine", "second"):
        run(args, media, root / f"mixed-{policy}", policy, False, False, 0,
            mixed=True)
    compare(root)


if __name__ == "__main__":
    main()
