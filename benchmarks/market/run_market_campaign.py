#!/usr/bin/env python3
"""Runs the in-process market comparison campaign (ANN + BEIR) and writes raw results.

Every step is one fresh process; finished steps are recorded in
evaluation/reports/market/done.txt and skipped on a rerun, so the campaign can resume.
Run nothing else on the machine while it measures. Docker servers are a separate step
(run_servers.py). Usage (repository root, after building build-market in Release):

  python3 benchmarks/market/run_market_campaign.py [--only ann|build1|beir|beir-ann|parity]
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "evaluation/reports/market"
BUILD = ROOT / "build-market/benchmarks/market"
ANN = BUILD / "aether_ann_compare"
ANN_FAST = BUILD / "aether_ann_compare_fastmath"
IR = BUILD / "aether_ir_compare"
JAR = ROOT / "tools/anserini/anserini-2.3.0-fatjar.jar"
JAVA_CP = f"{JAR}:{ROOT / 'build-market/java'}"
JVM = ["java", "--add-modules", "jdk.incubator.vector", "--enable-native-access=ALL-UNNAMED",
       "-Xms4g", "-Xmx4g"]
VENV_PY = ROOT / ".venv-bench/bin/python"
BEIR = ["scifact", "nfcorpus", "fiqa", "arguana", "trec-covid", "quora"]
ANN_DATA = {
    "sift1m": dict(base="datasets/real/sift1m/sift/sift_base.fvecs",
                   queries="datasets/real/sift1m/sift/sift_query.fvecs",
                   gt="datasets/real/sift1m/sift/sift_groundtruth.ivecs", metric="l2", pq_m=32),
    "glove-100": dict(base="datasets/market/ann/glove-100/base.fvecs",
                      queries="datasets/market/ann/glove-100/queries.fvecs",
                      gt="datasets/market/ann/glove-100/groundtruth.ivecs", metric="cosine",
                      pq_m=25),
}
PARAMS = "10,16,24,32,48,64,96,128,192,256,384,512,800"
NPROBE = "1,2,4,8,16,32,64,128,256"


def log(message):
    line = f"[{time.strftime('%H:%M:%S')}] {message}"
    print(line, flush=True)
    with (OUT / "campaign.log").open("a", encoding="utf-8") as stream:
        stream.write(line + "\n")


def done(step):
    path = OUT / "done.txt"
    return path.exists() and step in path.read_text(encoding="utf-8").split("\n")


def mark(step):
    with (OUT / "done.txt").open("a", encoding="utf-8") as stream:
        stream.write(step + "\n")


def run(step, command, **kwargs):
    if done(step):
        return
    log(f"start {step}")
    start = time.time()
    with (OUT / "logs" / f"{step.replace('/', '_')}.log").open("w", encoding="utf-8") as stream:
        subprocess.run([str(c) for c in command], check=True, stdout=stream,
                       stderr=subprocess.STDOUT, cwd=ROOT, **kwargs)
    mark(step)
    log(f"done  {step} ({time.time() - start:.0f} s)")


def ann_steps():
    folder = OUT / "ann"
    folder.mkdir(parents=True, exist_ok=True)
    for dataset, spec in ANN_DATA.items():
        common = ["--base", spec["base"], "--queries", spec["queries"], "--groundtruth",
                  spec["gt"], "--metric", spec["metric"], "--dataset", dataset,
                  "--out", folder / f"{dataset}.csv"]
        run(f"ann/{dataset}/faiss-flat", [ANN, "--system", "faiss-flat", *common])
        for m in (16, 32):
            for efc in (100, 200):
                raw = ["--raw", folder / f"{dataset}_latency.csv"] if (m, efc) == (16, 200) else []
                for system in ("aether-hnsw", "hnswlib", "faiss-hnsw"):
                    run(f"ann/{dataset}/{system}/M{m}/efc{efc}",
                        [ANN, "--system", system, "--M", m, "--ef-construction", efc,
                         "--params", PARAMS, *common, *raw])
                run(f"ann/{dataset}/hnswlib-fastmath/M{m}/efc{efc}",
                    [ANN_FAST, "--system", "hnswlib", "--M", m, "--ef-construction", efc,
                     "--params", PARAMS, *common])
        for system in ("aether-sq8", "aether-sq8-lean", "aether-pq", "faiss-hnsw-sq8"):
            run(f"ann/{dataset}/{system}/M16/efc200",
                [ANN, "--system", system, "--M", 16, "--ef-construction", 200,
                 "--params", PARAMS, "--pq-m", spec["pq_m"], *common])
        run(f"ann/{dataset}/faiss-ivfpq-refine",
            [ANN, "--system", "faiss-ivfpq-refine", "--params", NPROBE, "--pq-m", spec["pq_m"],
             "--nlist", 1024, "--refine", 4, *common])


def build1_steps():
    folder = OUT / "ann"
    for dataset, spec in ANN_DATA.items():
        for system in ("aether-hnsw", "hnswlib", "faiss-hnsw"):
            run(f"build1/{dataset}/{system}",
                [ANN, "--system", system, "--M", 16, "--ef-construction", 200, "--params", 128,
                 "--build-threads", 1, "--latency-queries", 100, "--repetitions", 1,
                 "--base", spec["base"], "--queries", spec["queries"], "--groundtruth", spec["gt"],
                 "--metric", spec["metric"], "--dataset", dataset,
                 "--out", folder / f"{dataset}_build1.csv"])


def lucene_index(dataset, name, flags):
    index = ROOT / "datasets/market/indexes" / f"{dataset}-{name}"
    step = f"beir/{dataset}/lucene-index-{name}"
    if not done(step):
        if index.exists():
            shutil.rmtree(index)
        log(f"start {step}")
        start = time.time()
        result = subprocess.run(
            [*JVM, "-cp", str(JAR), "io.anserini.index.IndexCollection", "-collection",
             "JsonCollection", "-input", f"datasets/market/beir/{dataset}/anserini", "-index",
             str(index), "-generator", "DefaultLuceneDocumentGenerator", "-threads", "1",
             "-optimize", *flags], check=True, capture_output=True, text=True, cwd=ROOT)
        seconds = time.time() - start
        size = sum(p.stat().st_size for p in index.rglob("*") if p.is_file())
        reported = re.findall(r"documents indexed in (\d\d:\d\d:\d\d)", result.stdout + result.stderr)
        (OUT / "beir" / dataset / f"lucene-index-{name}.json").write_text(json.dumps({
            "flags": flags, "threads": 1, "optimize": True, "wall_seconds": seconds,
            "anserini_reported": reported[-1] if reported else None, "index_bytes": size,
        }, indent=2) + "\n", encoding="utf-8")
        mark(step)
        log(f"done  {step} ({seconds:.0f} s)")
    return index


def beir_steps():
    for dataset in BEIR:
        data = ROOT / "datasets/market/beir" / dataset
        folder = OUT / "beir" / dataset
        folder.mkdir(parents=True, exist_ok=True)
        def ir(name, *options):
            run(f"beir/{dataset}/{name}", [IR, *options, "--data", data, "--out", folder / name])
        ir("aether-bm25-english", "bm25", "--analyzer", "english-v1", "--strategy", "bmw")
        ir("aether-bm25-english-exhaustive", "bm25", "--analyzer", "english-v1",
           "--strategy", "exhaustive")
        ir("aether-bm25-ascii", "bm25", "--analyzer", "ascii-tech-v1", "--strategy", "bmw")
        ir("aether-bm25-english-k1.2", "bm25", "--analyzer", "english-v1", "--strategy", "bmw",
           "--k1", "1.2", "--b", "0.75")
        freq = ("--query-weighting", "frequency")
        ir("aether-bm25-english-freq", "bm25", "--analyzer", "english-v1", "--strategy", "bmw", *freq)
        ir("aether-bm25-english-freq-exhaustive", "bm25", "--analyzer", "english-v1",
           "--strategy", "exhaustive", *freq)
        ir("aether-bm25-english-freq-k1.2", "bm25", "--analyzer", "english-v1", "--strategy",
           "bmw", "--k1", "1.2", "--b", "0.75", *freq)
        ir("aether-bm25-english-maxscore", "bm25", "--analyzer", "english-v1", "--strategy",
           "maxscore")
        ir("aether-bm25-english-freq-maxscore", "bm25", "--analyzer", "english-v1", "--strategy",
           "maxscore", *freq)
        porter = lucene_index(dataset, "porter", [])
        plain = lucene_index(dataset, "none-keepstop", ["-stemmer", "none", "-keepStopwords"])
        def lucene(name, index, k1, b, stemmer, keep):
            run(f"beir/{dataset}/{name}",
                [*JVM, "-cp", JAVA_CP, "LuceneSearch", index, data / "topics.tsv", folder / name,
                 k1, b, stemmer, keep, 101, 10, 1000, 5, 10, 2])
        lucene("lucene-bm25-porter", porter, "0.9", "0.4", "porter", "false")
        lucene("lucene-bm25-none-keepstop", plain, "0.9", "0.4", "none", "true")
        lucene("lucene-bm25-porter-k1.2", porter, "1.2", "0.75", "porter", "false")
        for config in ("default", "matched"):
            run(f"beir/{dataset}/bm25s-{config}",
                [VENV_PY, "benchmarks/market/run_bm25s.py", "--data", data, "--out",
                 folder / f"bm25s-{config}", "--config", config])
        run(f"beir/{dataset}/exact-dense",
            [VENV_PY, "benchmarks/market/dense_reference.py", "--data", data,
             "--out", folder / "exact-dense"])
        ir("aether-dense-hnsw", "dense", "--index", "hnsw")
        ir("aether-dense-sq8", "dense", "--index", "sq8")
        ir("aether-hybrid", "hybrid", "--analyzer", "english-v1", "--strategy", "bmw")
        ir("aether-hybrid-freq", "hybrid", "--analyzer", "english-v1", "--strategy", "bmw", *freq)
        ir("aether-hybrid-freq-maxscore", "hybrid", "--analyzer", "english-v1", "--strategy",
           "maxscore", *freq)
        run(f"beir/{dataset}/reference-hybrid",
            [sys.executable, "-I", "benchmarks/market/fuse_rrf.py", "--dense",
             folder / "exact-dense.run", "--lexical", folder / "lucene-bm25-porter.run",
             "--doc-ids", data / "doc_ids.txt", "--out", folder / "reference-hybrid.run",
             "--tag", "lucene+exact"])


def parity_steps():
    """Deterministic (1-thread) HNSW so that dense + BM25 runs re-fuse to the hybrid run."""
    for dataset in BEIR:
        data = ROOT / "datasets/market/beir" / dataset
        folder = OUT / "parity" / dataset
        folder.mkdir(parents=True, exist_ok=True)
        freq = ["--query-weighting", "frequency"]
        for name, mode, extra in (("bm25", "bm25", []), ("dense", "dense", ["--index", "hnsw"]),
                                  ("hybrid", "hybrid", []), ("bm25-freq", "bm25", freq),
                                  ("hybrid-freq", "hybrid", freq)):
            run(f"parity/{dataset}/{name}",
                [IR, mode, *extra, "--data", data, "--out", folder / name, "--build-threads", 1,
                 "--timing", 0])
        for suffix in ("", "-freq"):
            run(f"parity/{dataset}/check{suffix}",
                [sys.executable, "-I", "benchmarks/market/fuse_rrf.py", "--dense",
                 folder / "dense.run", "--lexical", folder / f"bm25{suffix}.run", "--doc-ids",
                 data / "doc_ids.txt", "--out", folder / f"refused{suffix}.run", "--check",
                 folder / f"hybrid{suffix}.run"])


def beir_ann_steps():
    """ANN libraries on the BEIR bge-small vectors, with exact search as ground truth."""
    folder = OUT / "beir-ann"
    folder.mkdir(parents=True, exist_ok=True)
    for dataset in BEIR:
        data = f"datasets/market/beir/{dataset}"
        gt = OUT / "beir" / dataset / "exact-dense_gt.ivecs"
        for system in ("aether-hnsw", "hnswlib", "faiss-hnsw"):
            run(f"beir-ann/{dataset}/{system}",
                [ANN, "--system", system, "--M", 16, "--ef-construction", 200,
                 "--params", "16,32,64,128,256", "--base", f"{data}/corpus.fvecs",
                 "--queries", f"{data}/queries.fvecs", "--groundtruth", gt,
                 "--metric", "cosine", "--dataset", dataset, "--out", folder / "beir_ann.csv"])


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def manifest():
    def output(command):
        try:
            return subprocess.run(command, capture_output=True, text=True, cwd=ROOT).stdout.strip()
        except OSError:
            return None
    hnswlib = ROOT / "build-market/_deps/hnswlib-src"
    data = {
        "date": time.strftime("%Y-%m-%d %H:%M:%S %Z"),
        "git_commit": output(["git", "rev-parse", "HEAD"]),
        "git_dirty": bool(output(["git", "status", "--porcelain"])),
        "machine": {"platform": platform.platform(), "processor": platform.processor(),
                    "cpu": output(["sysctl", "-n", "machdep.cpu.brand_string"]),
                    "cores_performance": output(["sysctl", "-n", "hw.perflevel0.physicalcpu"]),
                    "cores_efficiency": output(["sysctl", "-n", "hw.perflevel1.physicalcpu"]),
                    "memory_bytes": output(["sysctl", "-n", "hw.memsize"])},
        "compiler": output(["c++", "--version"]).split("\n")[0],
        "binaries": {p.name: sha256(p) for p in (ANN, ANN_FAST, IR)},
        "hnswlib_commit": output(["git", "-C", str(hnswlib), "rev-parse", "HEAD"]),
        "faiss": output(["brew", "list", "--versions", "faiss"]),
        "anserini_jar_sha256": sha256(JAR),
        "java": output(["java", "-version"]) or subprocess.run(
            ["java", "-version"], capture_output=True, text=True).stderr.split("\n")[0],
        "python_packages": output([str(VENV_PY), "-m", "pip", "freeze"]).split("\n"),
        "datasets": {
            name: json.loads((ROOT / "datasets/market/beir" / name / "manifest.json").read_text())
            for name in BEIR},
        "embeddings": {
            name: json.loads((ROOT / "datasets/market/beir" / name / "embedding.json").read_text())
            for name in BEIR if (ROOT / "datasets/market/beir" / name / "embedding.json").exists()},
        "ann_datasets": json.loads((ROOT / "datasets/market/ann/manifest.json").read_text()),
    }
    (OUT / "manifest.json").write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--only", choices=["ann", "build1", "beir", "beir-ann", "parity"])
    args = parser.parse_args()
    (OUT / "logs").mkdir(parents=True, exist_ok=True)
    manifest()
    steps = {"parity": parity_steps, "beir": beir_steps, "beir-ann": beir_ann_steps,
             "ann": ann_steps, "build1": build1_steps}
    for name, function in steps.items():
        if args.only in (None, name):
            function()
    log("campaign finished")


if __name__ == "__main__":
    os.environ.setdefault("PYTHONHASHSEED", "0")
    main()
