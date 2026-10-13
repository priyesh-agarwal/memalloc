#!/usr/bin/env python3
"""Run one bench binary under glibc, tcmalloc, jemalloc, and memalloc.

sudo /bin/sh experiment/compare.sh
"""

import json
import os
import re
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
bench_cpp = root / "experiment/bench.cpp"
bench = root / "experiment/build/memalloc_bench"
ours = root / "build/libmemalloc.so"
out = root / "experiment/results"

names = ["glibc", "tcmalloc", "jemalloc", "memalloc"]
tcmalloc_sonames = ["libtcmalloc.so.4", "libtcmalloc_minimal.so.4", "libtcmalloc.so"]
jemalloc_sonames = ["libjemalloc.so.2", "libjemalloc.so.1", "libjemalloc.so"]


def die(msg):
    print(msg, file=sys.stderr)
    sys.exit(1)


def find_lib(sonames):
    cache = {}
    try:
        text = subprocess.check_output(["ldconfig", "-p"], text=True, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.CalledProcessError):
        text = ""
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[-2] == "=>":
            cache.setdefault(parts[0], parts[-1])

    for soname in sonames:
        if soname in cache:
            return cache[soname]

    dirs = [Path("/usr/lib"), Path("/usr/lib64"), Path("/lib"), Path("/lib64"), Path("/usr/local/lib")]
    dirs += list(Path("/usr/lib").glob("*-linux-gnu"))
    dirs += list(Path("/lib").glob("*-linux-gnu"))
    for directory in dirs:
        for soname in sonames:
            path = directory / soname
            if path.is_file():
                return str(path)
    return None


def lock_cpu(cpu, freq):
    min_file = cpu / "cpufreq/scaling_min_freq"
    max_file = cpu / "cpufreq/scaling_max_freq"
    if not min_file.is_file() or not max_file.is_file():
        return False
    current_min = int(min_file.read_text())
    if freq < current_min:
        min_file.write_text(f"{freq}\n")
        max_file.write_text(f"{freq}\n")
    else:
        max_file.write_text(f"{freq}\n")
        min_file.write_text(f"{freq}\n")
    gov = cpu / "cpufreq/scaling_governor"
    if gov.is_file():
        gov.write_text("performance\n")
    print(f"{cpu.name} locked at {freq // 1000} MHz")
    return True


def lock_clocks():
    if os.geteuid() != 0:
        return
    turbo = Path("/sys/devices/system/cpu/intel_pstate/no_turbo")
    if turbo.is_file():
        turbo.write_text("1\n")
        print("no_turbo 1")

    cpus = sorted(Path("/sys/devices/system/cpu").glob("cpu[0-9]*"))
    caps = [int((cpu / "cpu_capacity").read_text()) for cpu in cpus if (cpu / "cpu_capacity").is_file()]
    max_cap = max(caps) if caps else None

    locked = 0
    for cpu in cpus:
        if max_cap is not None and int((cpu / "cpu_capacity").read_text()) != max_cap:
            continue
        if os.environ.get("FREQ_KHZ"):
            freq = int(os.environ["FREQ_KHZ"])
        elif (cpu / "cpufreq/base_frequency").is_file():
            freq = int((cpu / "cpufreq/base_frequency").read_text())
        else:
            die(f"no base_frequency for {cpu.name}. Set FREQ_KHZ to a rate this CPU holds with turbo off.")
        if lock_cpu(cpu, freq):
            locked += 1
    if locked == 0:
        die("no CPUs to lock")


def run(name, preload):
    print(f"=== {name} ===", flush=True)
    env = os.environ.copy()
    env.pop("LD_PRELOAD", None)
    if preload:
        env["LD_PRELOAD"] = preload
    log = out / f"{name}.txt"
    cmd = [
        str(bench),
        f"--benchmark_out={out / f'{name}.json'}",
        "--benchmark_out_format=json",
        "--benchmark_color=false",
    ]
    with log.open("w") as fh:
        fh.write(f"=== {name} ===\n")
        proc = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, text=True)
        for line in proc.stdout:
            sys.stdout.write(line)
            fh.write(line)
        if proc.wait() != 0:
            sys.exit(proc.returncode)


metrics = ["p50", "p99", "p99.9", "max"]

# Google Benchmark has no description field in JSON; SetLabel() runs inside timed loops.
# Docs live as // bench-desc: comments in bench.cpp (parsed here, zero runtime cost).
BENCH_DESC_RE = re.compile(r"^\s*//\s*bench-desc(?:-([a-z0-9_]+))?:\s*(.+?)\s*$")
BENCH_NAME_RE = re.compile(r'->Name\("([^"]+)"\)')


def load_bench_descriptions():
    """Map report row keys to text from bench.cpp (see bench-desc comments)."""
    if not bench_cpp.is_file():
        return {}
    by_key = {}
    pending = []
    lines = bench_cpp.read_text().splitlines()
    i = 0
    while i < len(lines):
        dm = BENCH_DESC_RE.match(lines[i])
        if dm:
            suffix = dm.group(1) or ""
            pending.append((suffix, dm.group(2)))
            i += 1
            continue
        if "BENCHMARK(" not in lines[i]:
            if lines[i].strip():
                pending = []
            i += 1
            continue
        block = lines[i]
        while ";" not in block and i + 1 < len(lines):
            i += 1
            block += "\n" + lines[i]
        nm = BENCH_NAME_RE.search(block)
        if nm and pending:
            base = nm.group(1)
            for suffix, text in pending:
                if suffix:
                    by_key[f"{base} {suffix}"] = text
                else:
                    by_key[base] = text
        pending = []
        i += 1
    return by_key


def fmt_arg(arg):
    if arg.isdigit() and int(arg) >= 1024 and int(arg) % 1024 == 0:
        n = int(arg)
        return f"{n >> 20} MiB" if n % (1 << 20) == 0 else f"{n >> 10} KiB"
    return arg


def describe_case(meta, desc):
    base, args, threads, sub = meta
    text = desc.get(f"{base} {sub}") if sub else None
    text = text or desc.get(base)
    if not text:
        return "(no // bench-desc above this BENCHMARK in experiment/bench.cpp)"
    extra = []
    if args:
        extra.append(", ".join(fmt_arg(a) for a in args))
    if threads:
        extra.append(f"{threads} thread" + ("" if threads == 1 else "s"))
    return f"{text} ({'; '.join(extra)})" if extra else text


def glossary_section(rows, meta, desc):
    if not rows:
        return []
    lines = [
        "### What each case measures",
        "",
        "| Case | Meaning |",
        "| --- | --- |",
    ]
    for row in rows:
        meaning = describe_case(meta[row], desc).replace("|", "\\|")
        lines.append(f"| {row} | {meaning} |")
    lines.append("")
    return lines


# Path tokens Google Benchmark appends for run options, not user args.
GB_OPTION_RE = re.compile(r"^(iterations|repeats|threads|min_time|min_warmup_time):|^(real|manual|process)_time$")


def parse_name(b):
    """(base, args, threads) from a Google Benchmark run name."""
    base, *tokens = b["name"].split("/")
    args = [t for t in tokens if not GB_OPTION_RE.match(t)]
    threads = int(b.get("threads", 1)) if any(t.startswith("threads:") for t in tokens) else 0
    return base, args, threads


def row_label(base, args, threads, sub=""):
    label = "/".join([base, *args])
    if threads:
        label += f" x{threads}"
    if sub:
        label += f" {sub}"
    return label


def table(title, note, rows, values, fmt):
    lines = [f"### {title}", ""]
    if note:
        lines += [note, ""]
    lines += [
        "| Case | " + " | ".join(names) + " |",
        "| --- |" + " ---: |" * len(names),
    ]
    for row in rows:
        cols = [fmt(values[name, row]) if (name, row) in values else "-" for name in names]
        lines.append("| " + " | ".join([row, *cols]) + " |")
    lines.append("")
    return lines


def write_report():
    header = {name: {} for name in names}
    # Table placement comes from the JSON, not from benchmark names:
    #   "<sub>.p50_ns" counters      -> latency tables (one row per counter prefix)
    #   threads + items_per_second   -> throughput table
    #   anything else                -> time per iteration table
    lat_rows, lat = [], {}
    tput_rows, tput = [], {}
    time_rows, time_ns = [], {}
    meta = {}
    to_ns = {"ns": 1.0, "us": 1e3, "ms": 1e6, "s": 1e9}

    def add(rows, row, info):
        if row not in rows:
            rows.append(row)
        meta[row] = info

    # Counters arrive sorted by name; list a benchmark's rows in the order their
    # bench-desc-<row> lines appear in bench.cpp (e.g. malloc before free).
    bench_desc = load_bench_descriptions()
    desc_order = {key: i for i, key in enumerate(bench_desc)}

    def sub_order(base, sub):
        return desc_order.get(f"{base} {sub}", len(desc_order)), sub

    for name in names:
        for line in (out / f"{name}.txt").read_text().splitlines():
            if line.startswith(("sched ", "cpu ", "tsc ")):
                header[name][line.split()[0]] = line
        data = json.loads((out / f"{name}.json").read_text())
        for b in data["benchmarks"]:
            if b.get("run_type", "iteration") != "iteration":
                continue
            base, args, threads = parse_name(b)
            subs = sorted((k[: -len("p50_ns")].rstrip(".") for k in b if k.endswith("p50_ns")),
                          key=lambda sub: sub_order(base, sub))
            for sub in subs:
                prefix = f"{sub}." if sub else ""
                row = row_label(base, args, threads, sub)
                add(lat_rows, row, (base, args, threads, sub))
                lat[name, row] = [b[f"{prefix}{m}_ns"] for m in metrics]
            if subs:
                continue
            row = row_label(base, args, threads)
            if threads and "items_per_second" in b:
                add(tput_rows, row, (base, args, threads, ""))
                tput[name, row] = b["items_per_second"] / 1e6
            else:
                add(time_rows, row, (base, args, threads, ""))
                time_ns[name, row] = b["real_time"] * to_ns[b.get("time_unit", "ns")]

    missing = sorted({m[0] for m in meta.values() if m[0] not in bench_desc
                      and not any(k.startswith(m[0] + " ") for k in bench_desc)})
    for base in missing:
        print(f"warning: no // bench-desc for {base} in {bench_cpp}", file=sys.stderr)

    lines = ["# Allocator comparison", ""]
    for name in names:
        h = header[name]
        lines.append(f"{name}: {h.get('sched', '')}; {h.get('cpu', '')}; {h.get('tsc', '')}")
    lines += [
        "",
        "Single-thread latency in ns. Loop cases store the mean of each batch of 64 calls; "
        "one-shot cases time every call, so with 14-50 samples p99 is the max.",
        "",
        "Case meanings come from the `// bench-desc:` comments above each `BENCHMARK` in "
        "`experiment/bench.cpp`.",
        "",
    ]
    lines += glossary_section(lat_rows + tput_rows + time_rows, meta, bench_desc)
    for index, metric in enumerate(metrics):
        lines += table(f"{metric} (ns)", "", lat_rows,
                       {k: v[index] for k, v in lat.items()}, lambda v: f"{v:.1f}")
    lines += table(
        "Multithread throughput (M items/s, higher is better)",
        "Items per second summed over threads (for these cases, malloc and free calls). "
        "`xN` is the thread count; each thread is pinned to its own core.",
        tput_rows, tput, lambda v: f"{v:.1f}")
    lines += table(
        "Time per iteration (ns, lower is better)",
        "The case meaning says what one iteration does.",
        time_rows, time_ns, lambda v: f"{v:.1f}")
    text = "\n".join(lines) + "\n"
    (out / "report.md").write_text(text)
    print(text, end="")


def main():
    if not bench.is_file():
        die(f"missing {bench}\nconfigure and build experiment/ first")
    if not ours.is_file():
        die(f"missing {ours}\nconfigure and build the allocator first")
    if os.geteuid() != 0:
        print("warning: not root, so the bench cannot get SCHED_FIFO and the clock stays as it is", file=sys.stderr)
        print("run: sudo /bin/sh experiment/compare.sh", file=sys.stderr)

    lock_clocks()
    out.mkdir(parents=True, exist_ok=True)

    tcmalloc = os.environ.get("TCMALLOC_LIB") or find_lib(tcmalloc_sonames)
    jemalloc = os.environ.get("JEMALLOC_LIB") or find_lib(jemalloc_sonames)
    if not tcmalloc:
        die("tcmalloc not found. Install libgoogle-perftools4 (Debian/Ubuntu) or gperftools-libs (Fedora), or set TCMALLOC_LIB.")
    if not jemalloc:
        die("jemalloc not found. Install libjemalloc2 (Debian/Ubuntu) or jemalloc (Fedora), or set JEMALLOC_LIB.")

    print(f"tcmalloc {tcmalloc}")
    print(f"jemalloc {jemalloc}")
    print(f"memalloc {ours}")

    run("glibc", None)
    run("tcmalloc", tcmalloc)
    run("jemalloc", jemalloc)
    run("memalloc", str(ours))
    write_report()

    uid = os.environ.get("SUDO_UID")
    if uid:
        gid = int(os.environ.get("SUDO_GID", uid))
        uid = int(uid)
        os.chown(out, uid, gid)
        for path in out.iterdir():
            os.chown(path, uid, gid)


if __name__ == "__main__":
    main()
