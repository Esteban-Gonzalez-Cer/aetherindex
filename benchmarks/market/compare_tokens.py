#!/usr/bin/env python3
"""Token-level agreement between two analyzer dumps (one document per line).

Standard library only. Agreement = sum over documents of the multiset intersection of
the two token lists, divided by the larger total token count; also reports exact
document matches and the most frequent disagreements, for the report's mismatch list.
"""

import argparse
from collections import Counter
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path, help="Lucene/Anserini dump")
    parser.add_argument("candidate", type=Path, help="AetherIndex dump")
    parser.add_argument("--json", type=Path)
    parser.add_argument("--top", type=int, default=25)
    args = parser.parse_args()
    reference = args.reference.read_text(encoding="utf-8").splitlines()
    candidate = args.candidate.read_text(encoding="utf-8").splitlines()
    if len(reference) != len(candidate):
        raise SystemExit("Dumps have different line counts")
    shared = ref_total = cand_total = exact = 0
    only_ref, only_cand = Counter(), Counter()
    for ref_line, cand_line in zip(reference, candidate):
        ref, cand = Counter(ref_line.split()), Counter(cand_line.split())
        shared += sum((ref & cand).values())
        ref_total += sum(ref.values())
        cand_total += sum(cand.values())
        exact += ref_line.split() == cand_line.split()
        only_ref.update(ref - cand)
        only_cand.update(cand - ref)
    result = {
        "documents": len(reference), "exact_documents": exact,
        "reference_tokens": ref_total, "candidate_tokens": cand_total,
        "shared_tokens": shared, "agreement": shared / max(ref_total, cand_total),
        "top_reference_only": only_ref.most_common(args.top),
        "top_candidate_only": only_cand.most_common(args.top),
    }
    print(f"agreement {result['agreement']:.4%} ({shared}/{max(ref_total, cand_total)} tokens), "
          f"exact documents {exact}/{len(reference)}")
    print("reference only:", result["top_reference_only"][:12])
    print("candidate only:", result["top_candidate_only"][:12])
    if args.json:
        args.json.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
