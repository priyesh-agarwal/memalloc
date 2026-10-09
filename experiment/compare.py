#!/usr/bin/env python3
"""Run one bench binary under glibc, tcmalloc, jemalloc, and memalloc.

sudo /bin/sh experiment/compare.sh
"""

import os
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
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
    with log.open("w") as fh:
        fh.write(f"=== {name} ===\n")
        proc = subprocess.Popen([str(bench)], env=env, stdout=subprocess.PIPE, text=True)
        for line in proc.stdout:
            sys.stdout.write(line)
            fh.write(line)
        if proc.wait() != 0:
            sys.exit(proc.returncode)


def write_report():
    header = {name: {} for name in names}
    cases = []
    units = {}
    values = {}
    for name in names:
        for line in (out / f"{name}.txt").read_text().splitlines():
            if line.startswith(("sched ", "cpu ", "tsc ")):
                header[name][line.split()[0]] = line
            elif line.endswith(" ns") or line.endswith(" us"):
                parts = line.split()
                case = " ".join(parts[:-5])
                if case not in units:
                    cases.append(case)
                    units[case] = parts[-1]
                values[name, case] = parts[-5:-1]  # p50 p99 p99.9 max

    lines = ["# Allocator comparison", ""]
    for name in names:
        h = header[name]
        lines.append(f"{name}: {h.get('sched', '')}; {h.get('cpu', '')}; {h.get('tsc', '')}")
    lines += [
        "",
        "Median of batch-of-64 rdtsc samples, except the short rows, which time every call. Rows with 14, 16, or 50 samples have p99 equal to the max.",
        "",
    ]
    for index, metric in enumerate(["p50", "p99", "p99.9", "max"]):
        lines += [
            f"### {metric}",
            "",
            "| Case | glibc | tcmalloc | jemalloc | memalloc | Unit |",
            "| --- | ---: | ---: | ---: | ---: | --- |",
        ]
        for case in cases:
            cols = [values[name, case][index] for name in names]
            lines.append("| " + " | ".join([case, *cols, units[case]]) + " |")
        lines.append("")
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
