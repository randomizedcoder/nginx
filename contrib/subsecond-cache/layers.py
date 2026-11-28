"""Compare how quickly an origin update crosses a chain of loopback caches.

One origin serves its own clock on two paths that differ only in
Cache-Control.  A chain of caches sits in front, each on its own loopback
address, and wrk pulls both paths through the last cache.  The origin stamps
every response with X-Origin-Time, so the last cache's access log gives the
staleness of each response a client received.

The default chain follows a live stream from an origin in New York to viewers
in Melbourne.  --wan adds an illustrative one-way delay to each hop with a
small TCP relay, since loopback has none.
"""
import argparse
import asyncio
import contextlib
import ipaddress
import json
import math
import re
import socket
import subprocess
import tempfile
import threading
import time
from pathlib import Path

WARMUP = 2
# Cache sites from the origin outwards, with the illustrative one-way delay in
# milliseconds of the hop towards the origin.  The origin is in New York.
SITES = [("New York", 1), ("Chicago", 10), ("Los Angeles", 25),
         ("Sydney", 75), ("Melbourne", 6)]


async def relay(listen, port, target, delay):
    """Forward TCP both ways, holding every chunk for the one-way delay."""
    loop = asyncio.get_running_loop()

    async def pump(reader, writer):
        try:
            while data := await reader.read(65536):
                loop.call_later(delay, writer.write, data)
        finally:
            loop.call_later(delay, writer.close)

    async def accept(reader, writer):
        try:
            up_reader, up_writer = await asyncio.open_connection(*target)
        except OSError:
            writer.close()
            return
        await asyncio.gather(pump(reader, up_writer), pump(up_reader, writer),
                             return_exceptions=True)

    await asyncio.start_server(accept, listen, port)


def start_relays(links):
    """Run one relay per delayed hop on a background event loop."""
    loop = asyncio.new_event_loop()
    for link in links:
        loop.run_until_complete(relay(*link))
    threading.Thread(target=loop.run_forever, daemon=True).start()


def policies(fine_ms):
    return {
        "second": "max-age=1",
        "fine": f"max-age=0, max-age-ms={fine_ms}",
    }


def origin_conf(listen, paths):
    locations = "\n".join(f"""
        location = /{name} {{
            add_header Cache-Control "{policy}";
            add_header X-Origin-Time $msec;
            return 200 "$msec\\n";
        }}""" for name, policy in paths.items())
    return f"""
    access_log logs/access.log stamp;
    server {{
        listen {listen};
        default_type text/plain;
        {locations}
    }}"""


def cache_conf(listen, upstream, mode, lock=False):
    lock = "proxy_cache_lock on;" if lock else ""
    return f"""
    access_log logs/access.log stamp buffer=1m flush=500ms;
    proxy_cache_path cache keys_zone=layer:8m;
    upstream previous {{ server {upstream}; keepalive 64; }}
    server {{
        listen {listen};
        location / {{
            proxy_pass http://previous;
            proxy_http_version 1.1;
            proxy_set_header Connection "";
            proxy_cache layer;
            proxy_cache_fine_time {mode};
            proxy_cache_fine_age_header on;
            {lock}
        }}
    }}"""


@contextlib.contextmanager
def server(directory, binary, address, port, body):
    (directory / "logs").mkdir(parents=True)
    conf = directory / "nginx.conf"
    conf.write_text(f"""
daemon off;
worker_processes 1;
pid nginx.pid;
error_log logs/error.log notice;
events {{ worker_connections 1024; }}
http {{
    log_format stamp '$msec $uri $sent_http_x_origin_time '
                     '$upstream_cache_status';
    {body}
}}
""")
    proc = subprocess.Popen([binary, "-p", str(directory), "-c", str(conf)])
    try:
        for _ in range(100):
            if proc.poll() is not None:
                raise RuntimeError(f"nginx exited, see {directory}/logs")
            try:
                with socket.create_connection((address, port), 0.1):
                    break
            except OSError:
                time.sleep(0.03)
        else:
            raise TimeoutError(f"nginx startup: {directory}")
        yield
    finally:
        if proc.poll() is None:
            proc.terminate()
        proc.wait(timeout=10)


def percentile(values, fraction):
    """Nearest-rank percentile of a sorted list."""
    return values[max(0, math.ceil(fraction * len(values)) - 1)]


def read_log(path, begin, end):
    """Group log lines inside the measurement window by path name."""
    rows = {}
    for line in path.read_text().splitlines():
        stamp, uri, sent, status = line.split()
        if begin <= float(stamp) < end:
            rows.setdefault(uri[1:], []).append((float(stamp), sent, status))
    return rows


def summarise(name, policy, edge, origin, seconds):
    lag = sorted((stamp - float(sent)) * 1000
                 for stamp, sent, _ in edge if sent != "-")
    if not lag:
        raise RuntimeError(f"no responses measured for /{name}")
    return {
        "path": f"/{name}",
        "policy": policy,
        "responses": len(lag),
        "staleness_ms": {
            "mean": round(sum(lag) / len(lag), 1),
            "p50": round(percentile(lag, .50), 1),
            "p90": round(percentile(lag, .90), 1),
            "p99": round(percentile(lag, .99), 1),
            "max": round(lag[-1], 1),
        },
        "origin_requests_per_second": round(len(origin) / seconds, 1),
    }


def table(results):
    head = ("Path", "Cache-Control", "Responses", "Mean", "p50", "p90", "p99",
            "Max", "Origin req/s")
    lines = ["| " + " | ".join(head) + " |",
             "|---|---|" + "---:|" * (len(head) - 2)]
    for r in results:
        s = r["staleness_ms"]
        lines.append("| " + " | ".join(str(v) for v in (
            f"`{r['path']}`", f"`{r['policy']}`", r["responses"], s["mean"],
            s["p50"], s["p90"], s["p99"], s["max"],
            r["origin_requests_per_second"])) + " |")
    return "\n".join(lines)


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--nginx", required=True, help="nginx binary with fine time")
    p.add_argument("--layers", type=int, default=len(SITES),
                   help="caches in the chain")
    p.add_argument("--wan", action="store_true",
                   help="add an illustrative one-way delay to each hop")
    p.add_argument("--delays",
                   help="one-way delay of each hop in ms, origin side first, "
                        "comma separated; overrides --wan")
    p.add_argument("--base", default="127.0.0.10",
                   help="origin address; caches take the following addresses")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--seconds", type=int, default=10, help="measured duration")
    p.add_argument("--connections", type=int, default=4,
                   help="wrk connections per path")
    p.add_argument("--fine-ms", type=int, default=100,
                   help="max-age-ms on the fine path")
    p.add_argument("--path", action="append", metavar="NAME=POLICY",
                   help="replace the default paths; repeat for each path, "
                        "for example --path \"paired=max-age=1, "
                        "max-age-ms=1000\"")
    p.add_argument("--modes",
                   help="proxy_cache_fine_time of each cache, origin side "
                        "first, comma separated; for misconfiguration runs")
    p.add_argument("--lock", action="store_true",
                   help="proxy_cache_lock on in every cache")
    p.add_argument("--output", type=Path)
    args = p.parse_args()
    if min(args.layers, args.seconds, args.connections, args.fine_ms) < 1:
        p.error("layers, seconds, connections and fine-ms must be positive")

    root = (args.output or Path(tempfile.mkdtemp(prefix="nginx-layers-")))
    root = root.resolve()
    root.mkdir(exist_ok=True)
    base = ipaddress.ip_address(args.base)
    hosts = [str(base + i) for i in range(args.layers + 1)]
    paths = policies(args.fine_ms)
    if args.path:
        paths = dict(item.split("=", 1) for item in args.path if "=" in item)
        if len(paths) != len(args.path) or not all(
                re.fullmatch(r"[a-z0-9-]+", name) and "\"" not in policy
                for name, policy in paths.items()):
            p.error("--path takes NAME=POLICY with a distinct lower-case name")
    # Only the first cache hears the origin directly.
    modes = ["origin"] + ["trusted"] * (args.layers - 1)
    if args.modes:
        modes = args.modes.split(",")
        if len(modes) != args.layers or not all(
                mode in ("off", "origin", "trusted") for mode in modes):
            p.error("--modes needs off, origin or trusted for each cache")
    binary = str(Path(args.nginx).resolve())

    named = args.layers == len(SITES)
    names = ([site for site, _ in SITES] if named
             else [f"cache {i}" for i in range(1, args.layers + 1)])
    if args.delays:
        try:
            delays = [float(d) for d in args.delays.split(",")]
        except ValueError:
            delays = []
        if len(delays) != args.layers or min(delays) < 0:
            p.error("--delays needs one non-negative value per cache")
    elif args.wan:
        if not named:
            p.error(f"--wan describes {len(SITES)} caches; use --delays")
        delays = [delay for _, delay in SITES]
    else:
        delays = [0] * args.layers

    # A delayed hop goes through a relay on the upstream address, next port.
    upstreams = [f"{hosts[i]}:{args.port + (delays[i] > 0)}"
                 for i in range(args.layers)]
    start_relays([(hosts[i], args.port + 1, (hosts[i], args.port),
                   delays[i] / 1000)
                  for i in range(args.layers) if delays[i] > 0])

    print(f"{hosts[0]}  origin{', New York' if named else ''}")
    for i, host in enumerate(hosts[1:]):
        print(f"{host}  {names[i]:<12} {modes[i]:<8} "
              f"+{delays[i]:g} ms each way")
    print(f"results  {root}\n", flush=True)

    with contextlib.ExitStack() as stack:
        stack.enter_context(server(
            root / "origin", binary, hosts[0], args.port,
            origin_conf(f"{hosts[0]}:{args.port}", paths)))
        for i, host in enumerate(hosts[1:], 1):
            stack.enter_context(server(
                root / f"cache{i}", binary, host, args.port,
                cache_conf(f"{host}:{args.port}", upstreams[i - 1],
                           modes[i - 1], args.lock)))

        begin = time.time() + WARMUP
        end = begin + args.seconds
        url = f"http://{hosts[-1]}:{args.port}"
        load = []
        for name in paths:
            log = stack.enter_context((root / f"wrk-{name}.txt").open("w"))
            load.append(subprocess.Popen(
                ["wrk", "-t1", f"-c{args.connections}",
                 f"-d{WARMUP + args.seconds}s", f"{url}/{name}"],
                stdout=log, stderr=log))
        if any(proc.wait() for proc in load):
            raise RuntimeError(f"wrk failed, see {root}/wrk-*.txt")
        time.sleep(1)  # let the buffered access logs flush

    edge = read_log(root / f"cache{args.layers}/logs/access.log", begin, end)
    origin = read_log(root / "origin/logs/access.log", begin, end)
    results = [summarise(name, policy, edge.get(name, []),
                         origin.get(name, []), args.seconds)
               for name, policy in paths.items()]
    for r in results:
        report = (root / f"wrk-{r['path'][1:]}.txt").read_text()
        errors = re.search(r"Non-2xx or 3xx responses: (\d+)", report)
        r["wrk_non_2xx"] = int(errors.group(1)) if errors else 0
    (root / "summary.json").write_text(json.dumps(
        {"layers": args.layers, "seconds": args.seconds,
         "connections": args.connections, "delays_ms": delays,
         "modes": modes,
         "lock": args.lock,
         "results": results}, indent=2))
    print(f"Staleness in ms of responses leaving {names[-1]}, {args.seconds} s, "
          f"{args.connections} connections per path, "
          f"{sum(delays):g} ms one way from the origin\n")
    print(table(results))


if __name__ == "__main__":
    main()
