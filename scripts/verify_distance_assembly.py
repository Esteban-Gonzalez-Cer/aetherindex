#!/usr/bin/env python3
"""Verify optimized hot distance assembly; preserve it as acceptance evidence."""
import argparse
import json
from pathlib import Path
import platform
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--compiler", required=True)
parser.add_argument("--source-root", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--native", action="store_true")
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
assembly_path = args.output / "distance-kernels.s"
command = [args.compiler, "-std=c++20", "-O3", "-S", "-I", str(args.source_root / "include"),
           str(args.source_root / "src/distance/scalar_distance.cpp"), "-o", str(assembly_path)]
if args.native:
    command.append("-march=native")
subprocess.run(command, check=True)
assembly = assembly_path.read_text()
machine = platform.machine().lower()
if machine in ("arm64", "aarch64"):
    vector_fma = re.findall(r"\bfmla(?:\s+v\d+\.4s|\.4s\s+v\d+)[^\n]*", assembly)
    conversion = re.findall(r"\bfcvt\s+d\d+,\s*s\d+", assembly)
    root_divide = re.findall(r"\bf(?:sqrt|div)\b[^\n]*", assembly)
else:
    vector_fma = re.findall(r"\bvfm(?:add|sub)\w*ps\b[^\n]*", assembly)
    conversion = re.findall(r"\b(?:v?cvtss2sd|v?cvtps2pd)\b[^\n]*", assembly)
    root_divide = re.findall(r"\b(?:v?(?:sqrt|div)(?:ss|ps)|(?:sqrt|sqrtf|__divsf3))\b[^\n]*", assembly)
report = {"compiler_command": command, "machine": machine,
          "packed_fp32_fma_count": len(vector_fma), "fma_examples": vector_fma[:8],
          "float_to_double_instructions": conversion, "sqrt_divide_instructions": root_divide,
          "ca_1_1_pass": not root_divide, "ca_1_4_pass": bool(vector_fma) and not conversion}
(args.output / "assembly.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
if not report["ca_1_1_pass"] or not report["ca_1_4_pass"]:
    raise SystemExit("Hot distance assembly does not satisfy CA-1.1/CA-1.4; see retained assembly.")
