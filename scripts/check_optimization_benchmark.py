#!/usr/bin/env python3
"""Run one opt-in performance acceptance check and retain raw evidence."""
import argparse
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--benchmark", type=Path, required=True)
parser.add_argument("--workload", choices=("topk", "memory", "clustered", "dbpedia"), required=True)
parser.add_argument("--data-dir", type=Path)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
if args.workload == "dbpedia":
    if not args.data_dir:
        parser.error("dbpedia requires --data-dir")
    command = [str(args.benchmark.resolve()), str(args.data_dir.resolve()), str(args.output.resolve())]
else:
    command = [str(args.benchmark.resolve()), args.workload, str(args.output.resolve())]
subprocess.run(command, check=True)
report = json.loads((args.output / f"{args.workload}.json").read_text())
criterion = {"topk": "ca_1_3_pass", "memory": "ca_2_2_owned_capacity_pass", "clustered": "ca_2_4_pass",
             "dbpedia": "ca_2_5_pass"}[args.workload]
print(json.dumps(report, indent=2))
if not report[criterion]:
    raise SystemExit(f"Acceptance threshold not reached: {criterion}; measurements retained in {args.output}")
