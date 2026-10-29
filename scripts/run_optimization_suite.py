#!/usr/bin/env python3
"""Run the fixed optimization workload matrix and store it under one phase folder.

    python3 scripts/run_optimization_suite.py --phase phase0_baseline \
        --suite build-opt-bench/benchmarks/aether_optimization_suite [--only NAME ...]
        [--skip-heavy] [--extra-args NAME="--flag value"]

Every phase uses the same named workloads so before/after numbers are comparable.
Outputs go to evaluation/reports/optimization/<phase>/<workload>/ together with a
manifest of source SHA-256 hashes, git state, machine and timings. Run nothing else
on the machine while this executes; workloads run one at a time.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import shlex
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DBPEDIA = ROOT / "datasets" / "real" / "dbpedia" / "vectors.csv"
DBPEDIA_100K = ROOT / "datasets" / "real" / "dbpedia-100k" / "vectors.csv"
SIFT = ROOT / "datasets" / "real" / "sift1m" / "sift"

# name: (arguments, heavy, required input or None). Heavy workloads (1M rows, DBpedia
# 100K) run one repetition to bound campaign time; light workloads keep three.
WORKLOADS = {
    "vec_synth_10k_128": ("vectors --synthetic 10000 128", False, None),
    "vec_synth_100k_128": ("vectors --synthetic 100000 128", False, None),
    "vec_synth_1m_128": ("vectors --synthetic 1000000 128 --repeat 1", True, None),
    "vec_dbpedia_10k": (f"vectors --csv {DBPEDIA}", False, DBPEDIA),
    "vec_dbpedia_100k": (f"vectors --csv {DBPEDIA_100K} --repeat 1", True, DBPEDIA_100K),
    "vec_sift1m": (f"vectors --fvecs {SIFT}/sift_base.fvecs {SIFT}/sift_query.fvecs "
                   f"{SIFT}/sift_groundtruth.ivecs --repeat 1", True, SIFT / "sift_base.fvecs"),
    "load_synth_10k_1024": ("load --synthetic 10000 1024", False, None),
    "load_synth_100k_128": ("load --synthetic 100000 128", False, None),
    "bm25_cp01": ("bm25 --corpus datasets/corpus.jsonl datasets/queries.jsonl", False, None),
    "bm25_zipf_100k": ("bm25 --zipf 100000", False, None),
    "bm25_zipf_1m": ("bm25 --zipf 1000000 --repeat 1", True, None),
    "hybrid_zipf_100k_128": ("hybrid --zipf 100000 --dimension 128", False, None),
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_manifest() -> dict[str, str]:
    files = []
    for folder in ("include", "src", "apps", "benchmarks", "tests"):
        files += [p for p in (ROOT / folder).rglob("*") if p.is_file()
                  and p.suffix in {".cpp", ".hpp", ".cmake", ".txt"}]
    files.append(ROOT / "CMakeLists.txt")
    return {str(p.relative_to(ROOT)): sha256(p) for p in sorted(files)}


def git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True).stdout.strip()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--phase", required=True)
    parser.add_argument("--suite", required=True, type=Path)
    parser.add_argument("--only", nargs="*", default=None)
    parser.add_argument("--skip-heavy", action="store_true")
    parser.add_argument("--extra-args", action="append", default=[],
                        help='NAME="--flag value" appended to one workload (or ALL=...)')
    args = parser.parse_args()
    extras: dict[str, str] = {}
    for item in args.extra_args:
        name, _, flags = item.partition("=")
        extras[name] = flags
    out = ROOT / "evaluation" / "reports" / "optimization" / args.phase
    out.mkdir(parents=True, exist_ok=True)
    manifest_path = out / "manifest.json"
    manifest = json.loads(manifest_path.read_text()) if manifest_path.is_file() else {"runs": {}}
    manifest.update({
        "phase": args.phase,
        "updated_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "git_commit": git("rev-parse", "HEAD"),
        "working_tree_dirty": bool(git("status", "--porcelain")),
        "machine": platform.platform(),
        "cpu": subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True,
                              text=True).stdout.strip(),
        "suite_binary_sha256": sha256(args.suite),
        "source_sha256": source_manifest(),
    })
    selected = args.only or list(WORKLOADS)
    failures = 0
    for name in selected:
        command, heavy, required = WORKLOADS[name]
        if heavy and args.skip_heavy and not args.only:
            continue
        if required is not None and not required.is_file():
            print(f"skip {name}: missing {required}")
            manifest["runs"][name] = {"status": "skipped", "reason": f"missing {required}"}
            continue
        mode, *rest = shlex.split(command)
        flags = rest + shlex.split(extras.get("ALL", "")) + shlex.split(extras.get(name, ""))
        argv = [str(args.suite), mode, str(out / name), *flags]
        print(f"== {name}: {' '.join(argv)}", flush=True)
        start = time.monotonic()
        result = subprocess.run(argv, cwd=ROOT)
        manifest["runs"][name] = {"command": argv[1:], "returncode": result.returncode,
                                  "wall_seconds": round(time.monotonic() - start, 1)}
        failures += result.returncode != 0
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
