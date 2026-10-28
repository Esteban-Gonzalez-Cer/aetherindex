"""Recheck saved CP-03 matrix scores with exact rational arithmetic (stdlib only)."""

from decimal import Decimal
from fractions import Fraction
import json
from pathlib import Path


def verify(folder: Path) -> dict:
    profiles = {}
    tolerance = Fraction(1, 10**12)
    for profile in ("debug", "release", "sanitizers"):
        cases = []
        for number in range(1, 23):
            case_id = f"RRF-{number:02}"
            path = folder / profile / f"{case_id}.json"
            evidence = json.loads(path.read_text(), parse_float=Decimal)
            assert evidence["case_id"] == case_id and evidence["passed"]
            assert Fraction(evidence["absolute_tolerance"]) == tolerance
            observations = []
            for check in evidence["checks"]:
                dense, lexical = check["dense"], check["lexical"]
                constant, top_k = check["k_rrf"], check["top_k"]
                invalid = (
                    isinstance(constant, str)
                    or constant <= 0
                    or top_k == 0
                    or len(dense) != len(set(dense))
                    or len(lexical) != len(set(lexical))
                )
                if invalid:
                    assert check["expected_error"] == "std::invalid_argument"
                    assert check["actual_error"] == "std::invalid_argument"
                    assert check["expected"] == check["actual"] == []
                    observations.append({"invalid_input_rejected": True})
                    continue
                assert check["actual_error"] is None and check["expected_error"] is None
                constant = Fraction(constant)
                exact = []
                for doc in set(dense) | set(lexical):
                    score = sum(
                        (Fraction(1) / (constant + ranking.index(doc) + 1)
                         for ranking in (dense, lexical) if doc in ranking),
                        Fraction(0),
                    )
                    exact.append((doc, score))
                exact.sort(key=lambda row: (-row[1], row[0]))
                exact = exact[:top_k]
                errors = []
                for name in ("expected", "actual"):
                    rows = check[name]
                    assert [row["document_id"] for row in rows] == [doc for doc, _ in exact]
                    for row, (_, mathematical_score) in zip(rows, exact):
                        error = abs(Fraction(row["score"]) - mathematical_score)
                        assert error <= tolerance, (case_id, row, error)
                        if name == "actual":
                            errors.append(error)
                observations.append({
                    "ids": [doc for doc, _ in exact],
                    "exact_score_fractions": [str(score) for _, score in exact],
                    "max_actual_absolute_error": float(max(errors, default=Fraction(0))),
                })
            cases.append({"case_id": case_id, "passed": True, "checks": observations})
        profiles[profile] = {"passed_cases": len(cases), "executed_cases": 22, "cases": cases}
    return {
        "method": "Python fractions.Fraction from saved inputs; no production helper or C++ expected score reused",
        "absolute_tolerance": 1e-12,
        "profiles": profiles,
    }


if __name__ == "__main__":
    folder = Path(__file__).resolve().parent
    result = verify(folder)
    (folder / "independent-matrix-verification.json").write_text(
        json.dumps(result, indent=2) + "\n"
    )
    for profile, output in result["profiles"].items():
        print(f"{profile}: {output['passed_cases']}/22 verified against exact fractions")
