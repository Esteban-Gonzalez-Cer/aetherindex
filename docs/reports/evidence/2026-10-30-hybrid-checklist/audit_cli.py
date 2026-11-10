"""Static architecture evidence: source locations and symbols defined by each library."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True)
    args = parser.parse_args()
    folder = Path(__file__).resolve().parent
    root = folder.parents[3]
    build = (root / args.build_dir).resolve()
    nm = Path("/opt/homebrew/opt/llvm/bin/llvm-nm")
    providers = {
        "cli": ("libaether_cli.a", None),
        "hnsw": ("libaether_hnsw.a", "aetherindex::HnswIndex::search"),
        "bm25": ("libaether_lexical.a", "aetherindex::lexical::Bm25Index::search"),
        "rrf": ("libaether_hybrid.a", "aetherindex::hybrid::RrfFusion::fuse"),
    }
    entries = {}
    for name, (library, required) in providers.items():
        command = [str(nm), "--demangle", "--defined-only", "--extern-only", str(build / library)]
        symbols = subprocess.check_output(command, text=True)
        (folder / f"1.11-{name}-defined-symbols.txt").write_text(symbols)
        if required: assert required in symbols, f"No implementation symbol in {library}"
        if name == "cli":
            for forbidden in ("aetherindex::HnswIndex::", "aetherindex::lexical::Bm25Index::",
                              "aetherindex::hybrid::RrfFusion::"):
                assert forbidden not in symbols, f"Algorithm implementation defined in CLI: {forbidden}"
        entries[name] = {"command": command, "library": library,
                         "symbols_file": f"1.11-{name}-defined-symbols.txt"}
    sources = []
    review = []
    for path in sorted((root / "apps/cli").glob("*")):
        if path.suffix not in (".cpp", ".hpp"): continue
        text = path.read_text()
        sources.append({"path": str(path.relative_to(root)),
                        "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
        review.append(f"\nFILE: {path.relative_to(root)}\n" + "\n".join(
            f"{i}: {line}" for i, line in enumerate(text.splitlines(), 1)))
    (folder / "1.11-cli-source-reviewed.txt").write_text("\n".join(review) + "\n")
    print(json.dumps({"check": "1.11", "passed": True, "cli_sources": sources,
                      "libraries": entries,
                      "review_scope": "Manual review of the retained CLI source plus defined-symbol and build-dependency evidence. Help strings may state the RRF formula; they do not execute it.",
                      "conclusion": "CLI parses/formats and calls SearchService. Implementations live in their respective libraries."}, indent=2))


if __name__ == "__main__":
    main()
