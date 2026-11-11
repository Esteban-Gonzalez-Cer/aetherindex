#!/usr/bin/env python3
"""Linux-only perf stat comparison, counting only 1000 warmed DBpedia searches.

The common driver sends enable/disable commands to perf and waits for ack.
Graph building, loading, warmup, recall calculation and output are excluded.
"""
import argparse
import csv
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile

EVENTS = ("L1-dcache-load-misses", "LLC-load-misses")


def measure(binary, selection, data_dir, output, cpu):
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="aether-perf-") as temporary:
        control, ack = (Path(temporary) / name for name in ("control", "ack"))
        for path in (control, ack):
            os.mkfifo(path)
        # Hold both ends open so neither perf nor the child deadlocks at open.
        descriptors = [os.open(path, os.O_RDWR | os.O_NONBLOCK) for path in (control, ack)]
        environment = dict(os.environ, AETHER_PERF_CTL_FIFO=str(control), AETHER_PERF_ACK_FIFO=str(ack))
        command = ["perf", "stat", "--delay=-1", f"--control=fifo:{control},{ack}",
                   "--no-big-num", "-x", ";", "-e", ",".join(EVENTS), "-o", str(output / "perf.csv"), "--"]
        if cpu is not None:
            command += ["taskset", "-c", str(cpu)]
        command += [str(binary.resolve()), str(data_dir.resolve()), str(output.resolve()), selection]
        try:
            result = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=1800)
        finally:
            for descriptor in descriptors:
                os.close(descriptor)
        (output / "command.json").write_text(json.dumps(command, indent=2) + "\n")
        (output / "stdout.log").write_text(result.stdout)
        (output / "stderr.log").write_text(result.stderr)
        if result.returncode:
            raise RuntimeError(f"perf failed; inspect {output / 'stderr.log'}")
    counts = {}
    for row in csv.reader((output / "perf.csv").read_text().splitlines(), delimiter=";"):
        if len(row) < 3 or row[0].startswith("#"):
            continue
        event = row[2].strip()
        if event in EVENTS:
            if not row[0].strip().replace(".", "", 1).isdigit():
                raise RuntimeError(f"PMU event unavailable: {event}={row[0]}; no acceptance claim possible")
            counts[event] = float(row[0])
    if set(counts) != set(EVENTS):
        raise RuntimeError("Missing required hardware counter; no acceptance claim possible")
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--optimized", type=Path, required=True)
    parser.add_argument("--data-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cpu", type=int)
    args = parser.parse_args()
    if platform.system() != "Linux" or not shutil.which("perf"):
        parser.error("CA-2.3 requires Linux perf with PMU access; this platform cannot measure it.")
    args.output.mkdir(parents=True, exist_ok=True)
    baseline = measure(args.baseline, "simple", args.data_dir, args.output / "baseline", args.cpu)
    optimized = measure(args.optimized, "heuristic", args.data_dir, args.output / "optimized", args.cpu)
    if baseline["LLC-load-misses"] == 0:
        raise RuntimeError("Baseline LLC misses are zero; relative reduction is undefined")
    reduction = 1 - optimized["LLC-load-misses"] / baseline["LLC-load-misses"]
    report = {"baseline": baseline, "optimized": optimized, "llc_miss_reduction": reduction,
              "ca_2_3_pass": reduction >= .50,
              "scope": "1000 QueryEngine searches after warmup; old-vs-new combines multiple changes"}
    (args.output / "cache.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0 if report["ca_2_3_pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
