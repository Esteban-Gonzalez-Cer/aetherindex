#!/usr/bin/env python3
"""Five-minute live showcase of the AetherIndex CLI (presentation helper only).

Runs the real `aetherindex search` commands, prints each command line, the key metrics
and the result table with document titles (the CLI prints IDs only). Steps:

  1. BM25 over TREC-COVID (171K papers): index build time and query latency.
  2. Hybrid query Q002 with the default convex fusion: the relevant document comes first.
  3. The same query with classic RRF (--fusion rrf), for comparison.
  4. Determinism: default maxscore and exhaustive return bit-identical scores.

Standard library only. Run from the repository root after building the CLI:

  python3 scripts/showcase_demo.py [--binary PATH] [--no-pause] [--steps 1,2,3,4]
"""

import argparse
import json
import re
import shlex
import subprocess
import sys
import textwrap
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TREC = ROOT / "datasets/market/beir/trec-covid/corpus.jsonl"
TREC_QUERY = "coronavirus transmission in children"
HYBRID = ROOT / "datasets/hybrid"
HYBRID_QUERY_ID = "Q002"
BOLD, DIM, GREEN, RESET = "\033[1m", "\033[2m", "\033[32m", "\033[0m"


def run(binary, arguments):
    command = [str(binary), "search", *arguments]
    shown = " ".join(shlex.quote(str(part).replace(str(ROOT) + "/", "")) for part in command)
    print(f"{DIM}$ {shown}{RESET}\n")
    result = subprocess.run(command, capture_output=True, text=True, cwd=ROOT)
    if result.returncode != 0:
        sys.exit(f"Command failed ({result.returncode}):\n{result.stderr}")
    return result.stdout


def metadata(output):
    return dict(line.split(": ", 1) for line in output.splitlines() if ": " in line)


def rows(output):
    """Result table rows as lists of cell strings (rank first)."""
    table = []
    for line in output.splitlines():
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if line.startswith("|") and cells[0].isdigit():
            table.append(cells)
    return table


def titles(corpus, ids):
    """Titles of the wanted document IDs, scanning the JSONL once."""
    wanted, found = set(ids), {}
    pattern = re.compile(r'^\{"document_id":\s*"?([^",]+)"?')
    with corpus.open(encoding="utf-8") as stream:
        for line in stream:
            match = pattern.match(line)
            if match and match.group(1) in wanted:
                found[match.group(1)] = json.loads(line).get("title", "")
                if len(found) == len(wanted):
                    break
    return found


def short(text, width):
    return textwrap.shorten(text, width=width, placeholder="…")


def pause(enabled, message="Press Enter for the next step"):
    if enabled:
        input(f"\n{DIM}{message}…{RESET}")
    print()


def header(number, title):
    print(f"{BOLD}── {number}. {title} {'─' * max(0, 70 - len(title))}{RESET}\n")


def step_scale(binary):
    header(1, "BM25 at scale: TREC-COVID scientific papers")
    output = run(binary, ["--mode", "lexical", "--dataset", TREC, "--analyzer", "english-v1",
                          "--query-text", TREC_QUERY, "--k", "5"])
    info = metadata(output)
    print(f"  Query:             {BOLD}{TREC_QUERY}{RESET}")
    print(f"  Documents indexed: {BOLD}{int(info['Documents indexed']):,}{RESET}")
    print(f"  Index build:       {BOLD}{float(info['Index build time (ms)']) / 1000:.1f} s{RESET}"
          f"  (one thread, from scratch)")
    print(f"  Query latency:     {BOLD}{float(info['Query time (ms)']):.2f} ms{RESET}\n")
    table = rows(output)
    names = titles(TREC, [row[1] for row in table])
    for rank, doc, score in table:
        print(f"  {rank}. {short(names.get(doc, '?'), 80):<80}  {DIM}BM25 {float(score):.2f}{RESET}")


def hybrid(binary, extra):
    output = run(binary, ["--mode", "hybrid", "--dataset", HYBRID / "corpus.jsonl", "--query",
                          HYBRID / "queries.jsonl", "--query-id", HYBRID_QUERY_ID, "--k", "5",
                          *extra])
    return metadata(output), rows(output)


def show_hybrid(info, table, relevant):
    names = titles(HYBRID / "corpus.jsonl", [row[1] for row in table])
    print(f"  Fusion: {BOLD}{info['Fusion']}{RESET}\n")
    print(f"  {'Rank':<5}{'Document':<56}{'Dense':>7}{'BM25':>6}")
    for rank, doc, _score, dense, lexical in table:
        mark = f"{GREEN}★{RESET}" if doc in relevant else " "
        print(f"  {rank:<4}{mark}{doc + '  ' + short(names.get(doc, '?'), 48):<56}"
              f"{dense:>7}{lexical:>6}")
    print(f"\n  {GREEN}★{RESET} = labelled relevant.  Dense/BM25 = rank in each retriever alone.")


def query_record():
    for line in (HYBRID / "queries.jsonl").read_text(encoding="utf-8").splitlines():
        record = json.loads(line)
        if record["query_id"] == HYBRID_QUERY_ID:
            return record
    sys.exit(f"{HYBRID_QUERY_ID} not found")


def rank_of(table, doc):
    return next((int(row[0]) for row in table if row[1] == doc), None)


def step_hybrid(binary, state):
    header(2, "Hybrid search: meaning + keywords in one engine")
    record = query_record()
    relevant = set(record.get("relevant_document_ids", []))
    print(f"  Query: {BOLD}{record['query_text']}{RESET}\n")
    info, table = hybrid(binary, [])
    show_hybrid(info, table, relevant)
    state["convex"] = table
    state["relevant"] = relevant


def step_rrf(binary, state):
    header(3, "Same query with classic RRF (what most products use)")
    info, table = hybrid(binary, ["--fusion", "rrf"])
    relevant = state.get("relevant") or set(query_record().get("relevant_document_ids", []))
    show_hybrid(info, table, relevant)
    if "convex" in state:
        print()
        for doc in sorted(relevant):
            convex, rrf = rank_of(state["convex"], doc), rank_of(table, doc)
            print(f"  {doc}: rank {BOLD}{convex or '>5'}{RESET} with convex fusion, "
                  f"{BOLD}{rrf or '>5'}{RESET} with RRF")
        print("  Convex fusion (alpha 0.65) was chosen on BEIR dev splits, never on test data:")
        print("  mean nDCG@10 0.6313 vs 0.6086 for RRF on the 6 BEIR test sets.")


def step_determinism(binary):
    header(4, "Determinism: pruning never changes the answer")
    common = ["--mode", "lexical", "--dataset", TREC, "--analyzer", "english-v1",
              "--query-text", TREC_QUERY, "--k", "10"]
    fast = run(binary, common)
    slow = run(binary, [*common, "--bm25-strategy", "exhaustive"])
    same = [row[1:] for row in rows(fast)] == [row[1:] for row in rows(slow)]
    print(f"  maxscore (default): {BOLD}{float(metadata(fast)['Query time (ms)']):.2f} ms{RESET}")
    print(f"  exhaustive:         {BOLD}{float(metadata(slow)['Query time (ms)']):.2f} ms{RESET}")
    verdict = f"{GREEN}identical, bit for bit{RESET}" if same else "DIFFERENT"
    print(f"  Top-10 IDs and scores: {BOLD}{verdict}{RESET}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path, default=ROOT / "build-optimization-release/aetherindex")
    parser.add_argument("--no-pause", action="store_true", help="run every step without waiting")
    parser.add_argument("--steps", default="1,2,3,4", help="comma-separated subset of 1-4")
    args = parser.parse_args()
    if not args.binary.exists():
        sys.exit(f"CLI not found: {args.binary} (build it or pass --binary)")
    for required in (TREC, HYBRID / "corpus.jsonl"):
        if not required.exists():
            sys.exit(f"Missing dataset: {required}")
    steps = [int(step) for step in args.steps.split(",")]
    state = {}
    actions = {1: lambda: step_scale(args.binary), 2: lambda: step_hybrid(args.binary, state),
               3: lambda: step_rrf(args.binary, state), 4: lambda: step_determinism(args.binary)}
    for position, step in enumerate(steps):
        actions[step]()
        if position + 1 < len(steps):
            pause(not args.no_pause)
    print()


if __name__ == "__main__":
    main()
