#!/usr/bin/env python3
"""Prepare the ANN datasets in TEXMEX format for aether_ann_compare.

Requires .venv-bench (numpy, h5py). Run with `python -I` (inputs are downloaded data).

  sift1m      datasets/real/sift1m/sift/*.fvecs|ivecs are used as they are (L2); this
              script only records their hashes.
  glove-100   ann-benchmarks glove-100-angular.hdf5 -> datasets/market/ann/glove-100/
              base.fvecs and queries.fvecs (unit-normalized, so inner product, cosine and
              angular rankings agree) and groundtruth.ivecs (the file's official neighbors).
Writes datasets/market/ann/manifest.json with source and output SHA-256.
"""

import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GLOVE_URL = "http://ann-benchmarks.com/glove-100-angular.hdf5"


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def write_vecs(path, matrix, dtype):
    import numpy as np

    rows, dimension = matrix.shape
    out = np.empty((rows, dimension + 1), dtype=dtype)
    out[:, 0] = np.array([dimension], dtype=np.int32).view(dtype)[0]
    out[:, 1:] = matrix
    out.tofile(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--glove", type=Path,
                        default=ROOT / "datasets/market/downloads/glove/glove-100-angular.hdf5")
    parser.add_argument("--sift", type=Path, default=ROOT / "datasets/real/sift1m/sift")
    parser.add_argument("--output", type=Path, default=ROOT / "datasets/market/ann")
    args = parser.parse_args()

    import h5py
    import numpy as np

    out = args.output / "glove-100"
    out.mkdir(parents=True, exist_ok=True)
    with h5py.File(args.glove, "r") as data:
        if data.attrs.get("distance") not in ("angular", b"angular"):
            raise ValueError("Expected an angular ann-benchmarks file")
        base = np.asarray(data["train"], dtype=np.float32)
        queries = np.asarray(data["test"], dtype=np.float32)
        neighbors = np.asarray(data["neighbors"], dtype=np.int32)
    for name, matrix in (("base", base), ("queries", queries)):
        norms = np.linalg.norm(matrix, axis=1, keepdims=True)
        zero = int((norms[:, 0] == 0).sum())
        if zero:
            raise ValueError(f"{name}: {zero} zero vectors cannot be normalized")
        write_vecs(out / f"{name}.fvecs", matrix / norms, np.float32)
    write_vecs(out / "groundtruth.ivecs", neighbors, np.int32)

    manifest = {
        "glove-100": {
            "source": GLOVE_URL, "source_sha256": sha256(args.glove),
            "base": int(base.shape[0]), "queries": int(queries.shape[0]),
            "dimension": int(base.shape[1]), "metric": "angular (unit vectors, inner product)",
            "groundtruth": "ann-benchmarks 'neighbors' (top-100, angular)",
            "outputs": {p.name: sha256(p) for p in sorted(out.glob("*.?vecs"))},
        },
        "sift1m": {
            "source": "ftp://ftp.irisa.fr/local/texmex/corpus/sift.tar.gz",
            "metric": "l2", "base": 1000000, "queries": 10000, "dimension": 128,
            "files": {p.name: sha256(p) for p in sorted(args.sift.glob("sift_*vecs"))
                      if "learn" not in p.name},
        },
    }
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n",
                                               encoding="utf-8")
    print(f"glove-100: {base.shape[0]} base, {queries.shape[0]} queries, d={base.shape[1]}")


if __name__ == "__main__":
    main()
