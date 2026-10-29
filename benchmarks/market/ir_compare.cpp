// Market comparison, BEIR: AetherIndex BM25, dense and hybrid runs on prepared datasets
// (benchmarks/market/prepare_beir.py, embed_beir.py), written as TREC run files with
// BEIR document ids, plus build time, memory and query latency.
//
//   aether_ir_compare bm25   --data DIR --out PREFIX [--analyzer english-v1]
//                            [--strategy bmw|maxscore|...] [--k1 0.9] [--b 0.4]
//                            [--query-weighting unique|frequency]
//   aether_ir_compare dense  --data DIR --out PREFIX [--index hnsw|exact|sq8] [--M 16]
//                            [--ef-construction 200] [--ef 128]
//   aether_ir_compare hybrid --data DIR --out PREFIX (BM25 and HNSW options above)
//   aether_ir_compare tokens --input FILE --out FILE [--analyzer english-v1]
//
// Common: [--depth 101] [--latency-k 10] [--candidate-k 50] [--latency-queries 1000]
// [--repetitions 5] [--threads 10] [--build-threads 10] [--timing 1].
// The quality run retrieves --depth results per query (101 leaves 100 after removing a
// query's own document on ArguAna/Quora). Latency uses --latency-k (hybrid: with
// --candidate-k per retriever), single-threaded over the first --latency-queries
// queries after one warm-up pass; batch throughput uses --threads over every query.
// Outputs PREFIX.run, PREFIX.json and PREFIX_latency.csv (per-query median).
#include <cstdint>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "aether/ann/brute_force/brute_force_index.hpp"
#include "aether/ann/hnsw/hnsw_index.hpp"
#include "aether/application/json_loader.hpp"
#include "aether/application/query_engine.hpp"
#include "aether/core/parallel.hpp"
#include "aether/hybrid/rrf.hpp"
#include "aether/lexical/bm25.hpp"
#include "aether/quantization/sq8.hpp"
#include "market_common.hpp"

using namespace aetherindex;
using namespace aetherindex::market;
namespace bench = aetherindex::bench;

namespace {

struct Hit {
    DocumentId document;
    double score;
};
// Retrieves k documents for query q; candidate_k applies to hybrid only.
using Retrieve = std::function<std::vector<Hit>(std::size_t q, std::size_t k, std::size_t candidate_k)>;

lexical::Analyzer parse_analyzer(const std::string& name) {
    if (name == "english-v1") return lexical::Analyzer::EnglishV1;
    if (name == "ascii-tech-v1") return lexical::Analyzer::AsciiTechV1;
    throw std::invalid_argument("Unknown --analyzer " + name);
}

lexical::Bm25Strategy parse_strategy(const std::string& name) {
    if (name == "exhaustive") return lexical::Bm25Strategy::Exhaustive;
    if (name == "taat") return lexical::Bm25Strategy::TermAtATime;
    if (name == "wand") return lexical::Bm25Strategy::Wand;
    if (name == "bmw") return lexical::Bm25Strategy::BlockMaxWand;
    if (name == "maxscore") return lexical::Bm25Strategy::MaxScore;
    throw std::invalid_argument("Unknown --strategy " + name);
}

lexical::QueryTermWeighting parse_weighting(const std::string& name) {
    if (name == "unique") return lexical::QueryTermWeighting::Unique;
    if (name == "frequency") return lexical::QueryTermWeighting::Frequency;
    throw std::invalid_argument("Unknown --query-weighting " + name);
}

std::vector<std::string> read_lines(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Cannot read " + path);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    return lines;
}

int tokens(const Args& args) {
    const lexical::Tokenizer tokenizer(parse_analyzer(args.get("analyzer", "english-v1")));
    const auto lines = read_lines(args.get("input"));
    std::ofstream out(args.get("out"));
    args.require_all_used();
    for (const auto& line : lines) {
        const auto terms = tokenizer.tokenize(line);
        for (std::size_t i = 0; i < terms.size(); ++i) out << (i ? " " : "") << terms[i];
        out << '\n';
    }
    return 0;
}

int retrieval(const std::string& mode, const Args& args) {
    const std::string data = args.get("data"), prefix = args.get("out");
    const std::size_t depth = args.number("depth", 101), latency_k = args.number("latency-k", 10);
    const std::size_t candidate_k = args.number("candidate-k", 50);
    const std::size_t latency_queries = args.number("latency-queries", 1000);
    const std::size_t repetitions = args.number("repetitions", 5);
    const std::size_t threads = args.number("threads", 10);
    const std::size_t build_threads = args.number("build-threads", 10);
    const bool timing = args.number("timing", 1) != 0;
    const bool lexical_on = mode == "bm25" || mode == "hybrid";
    const bool dense_on = mode == "dense" || mode == "hybrid";

    const auto doc_ids = read_lines(data + "/doc_ids.txt");
    const auto queries = detail::load_jsonl_queries(data + "/queries.jsonl", false);
    Matrix query_vectors;
    if (dense_on) {
        query_vectors = read_fvecs(data + "/queries.fvecs");
        if (query_vectors.rows != queries.size())
            throw std::runtime_error("queries.fvecs does not match queries.jsonl");
    }

    lexical::Bm25Config bm25;
    std::string index_kind = "hnsw";
    HnswConfig hnsw;
    std::size_t ef = 128;
    if (lexical_on) {
        bm25.k1 = args.real("k1", 0.9);
        bm25.b = args.real("b", 0.4);
        bm25.strategy = parse_strategy(args.get("strategy", "bmw"));
        bm25.analyzer = parse_analyzer(args.get("analyzer", "english-v1"));
        bm25.query_weighting = parse_weighting(args.get("query-weighting", "unique"));
    }
    if (dense_on) {
        index_kind = args.get("index", "hnsw");
        if (mode == "hybrid" && index_kind != "hnsw")
            throw std::invalid_argument("Hybrid uses --index hnsw");
        hnsw.m = args.number("M", 16);
        hnsw.m_max_0 = 2 * hnsw.m;
        hnsw.ef_construction = args.number("ef-construction", 200);
        ef = args.number("ef", 128);
        hnsw.ef_search = ef;
    }
    args.require_all_used();

    const double baseline_mib = bench::resident_mib();
    double lexical_seconds = 0, dense_seconds = 0;
    std::unique_ptr<lexical::Bm25Index> bm25_index;
    std::unique_ptr<VectorStore> store;
    std::unique_ptr<HnswIndex> hnsw_index;
    std::unique_ptr<Sq8Vectors> codes;
    if (lexical_on) {
        auto docs = detail::load_jsonl_documents(data + "/corpus.jsonl");
        const auto start = bench::Clock::now();
        bm25_index = std::make_unique<lexical::Bm25Index>(bm25);
        for (const auto& doc : docs)
            bm25_index->add_document(doc.document_id, doc.title + "\n" + doc.text);
        bm25_index->build();
        lexical_seconds = bench::elapsed_ms(start) / 1000.0;
    }
    if (dense_on) {
        Matrix corpus = read_fvecs(data + "/corpus.fvecs");
        if (corpus.rows != doc_ids.size()) throw std::runtime_error("corpus.fvecs size mismatch");
        const auto start = bench::Clock::now();
        store = std::make_unique<VectorStore>(corpus.dimension, DistanceKind::Cosine);
        store->reserve(corpus.rows);
        for (std::size_t i = 0; i < corpus.rows; ++i)
            store->insert(static_cast<VectorId>(i), {corpus.row(i), corpus.dimension});
        corpus = Matrix{};
        if (index_kind != "exact") {
            hnsw_index = std::make_unique<HnswIndex>(*store, hnsw);
            const std::vector<VectorId> ids(store->ids().begin(), store->ids().end());
            hnsw_index->insert_parallel(ids, build_threads);
        }
        if (index_kind == "sq8") codes = std::make_unique<Sq8Vectors>(*store);
        dense_seconds = bench::elapsed_ms(start) / 1000.0;
    }
    const double memory_mib = bench::resident_mib() - baseline_mib;

    const QueryEngine engine;
    const hybrid::RrfFusion fusion;
    VectorDocumentBinding binding;
    if (mode == "hybrid")
        for (VectorId id : store->ids()) binding.emplace(id, static_cast<DocumentId>(id));
    const auto embedding = [&](std::size_t q) {
        return std::span<const float>(query_vectors.row(q), query_vectors.dimension);
    };
    Retrieve retrieve;
    if (mode == "bm25") {
        retrieve = [&](std::size_t q, std::size_t k, std::size_t) {
            std::vector<Hit> hits;
            for (const auto& r : engine.search(*bm25_index, queries[q].text, k))
                hits.push_back({r.document_id, r.score});
            return hits;
        };
    } else if (mode == "dense") {
        retrieve = [&](std::size_t q, std::size_t k, std::size_t) {
            std::vector<SearchResult> results;
            if (index_kind == "exact") {
                results = engine.search(*store, embedding(q), k);
            } else {
                const PreparedQuery prepared(embedding(q), store->dimension(),
                                             DistanceKind::Cosine);
                results = codes ? hnsw_index->search_quantized(prepared, *codes, k, ef,
                                                               std::max(k, ef))
                                : hnsw_index->search_prepared(prepared, k, ef);
            }
            std::vector<Hit> hits;
            for (const auto& r : results)
                hits.push_back({static_cast<DocumentId>(r.id), 1.0 - static_cast<double>(r.distance)});
            return hits;
        };
    } else {
        retrieve = [&](std::size_t q, std::size_t k, std::size_t candidates) {
            HybridQuery query;
            query.text = queries[q].text;
            query.embedding = embedding(q);
            query.k = k;
            query.candidate_k = std::max(k, candidates);
            std::vector<Hit> hits;
            for (const auto& r : engine.search(*hnsw_index, binding, *bm25_index, fusion, query).fused)
                hits.push_back({r.document_id, r.score});
            return hits;
        };
    }

    // Quality run (parallel, deterministic per query), written in query order.
    std::vector<std::vector<Hit>> runs(queries.size());
    parallel_for(queries.size(), threads,
                 [&](std::size_t q) { runs[q] = retrieve(q, depth, depth); });
    {
        std::ofstream run(prefix + ".run");
        run << std::setprecision(17);
        for (std::size_t q = 0; q < queries.size(); ++q)
            for (std::size_t r = 0; r < runs[q].size(); ++r)
                run << queries[q].id << " Q0 " << doc_ids.at(runs[q][r].document) << ' ' << r + 1
                    << ' ' << runs[q][r].score << " aether\n";
    }

    std::vector<double> p50, p95, p99, mean, qps1, qps;
    const std::size_t latency_rows = std::min(latency_queries, queries.size());
    std::vector<std::vector<double>> per_query(latency_rows);
    if (timing) {
        for (std::size_t rep = 0; rep <= repetitions; ++rep) {
            std::vector<double> latencies(latency_rows);
            for (std::size_t q = 0; q < latency_rows; ++q) {
                const auto start = bench::Clock::now();
                const auto hits = retrieve(q, latency_k, candidate_k);
                latencies[q] = bench::elapsed_ms(start);
                if (hits.size() > latency_k) throw std::logic_error("Too many results");
            }
            if (rep == 0) continue;
            for (std::size_t q = 0; q < latency_rows; ++q) per_query[q].push_back(latencies[q]);
            const auto stats = bench::summarize(latencies);
            p50.push_back(stats.p50);
            p95.push_back(stats.p95);
            p99.push_back(stats.p99);
            mean.push_back(stats.mean);
            qps1.push_back(stats.qps);
        }
        for (std::size_t rep = 0; rep <= repetitions; ++rep) {
            const auto start = bench::Clock::now();
            parallel_for(queries.size(), threads,
                         [&](std::size_t q) { static_cast<void>(retrieve(q, latency_k, candidate_k)); });
            if (rep > 0) qps.push_back(static_cast<double>(queries.size()) /
                                       (bench::elapsed_ms(start) / 1000.0));
        }
        std::ofstream raw(prefix + "_latency.csv");
        raw << std::setprecision(9) << "query_index,query_id,median_latency_ms\n";
        for (std::size_t q = 0; q < latency_rows; ++q)
            raw << q << ',' << queries[q].id << ',' << bench::percentile(per_query[q], 0.5) << '\n';
    }

    std::ofstream summary(prefix + ".json");
    summary << std::setprecision(9) << "{\n  \"system\": \"aetherindex-" << mode << "\",\n"
            << "  \"data\": \"" << json_escape(data) << "\",\n"
            << "  \"documents\": " << doc_ids.size() << ",\n  \"queries\": " << queries.size()
            << ",\n  \"build_type\": \"" << bench::build_type() << "\",\n";
    if (lexical_on) {
        const auto memory = bm25_index->posting_memory();
        summary << "  \"bm25\": {\"k1\": " << bm25.k1 << ", \"b\": " << bm25.b
                << ", \"analyzer\": \"" << lexical::analyzer_name(bm25.analyzer)
                << "\", \"strategy\": \"" << args.get("strategy", "bmw")
                << "\", \"query_weighting\": \"" << args.get("query-weighting", "unique")
                << "\", \"build_seconds_single_thread\": " << lexical_seconds
                << ", \"postings_raw_bytes\": " << memory.raw_bytes
                << ", \"postings_compressed_bytes\": " << memory.compressed_bytes << "},\n";
    }
    if (dense_on) {
        summary << "  \"dense\": {\"index\": \"" << index_kind << "\", \"M\": " << hnsw.m
                << ", \"ef_construction\": " << hnsw.ef_construction << ", \"ef\": " << ef
                << ", \"build_threads\": " << build_threads << ", \"build_seconds\": "
                << dense_seconds << ", \"dimension\": " << store->dimension() << "},\n";
    }
    summary << "  \"memory_mib\": " << memory_mib << ",\n  \"depth\": " << depth
            << ",\n  \"latency_k\": " << latency_k << ",\n  \"candidate_k\": " << candidate_k;
    if (timing) {
        const auto batch = spread(qps);
        summary << ",\n  \"latency_queries\": " << latency_rows << ",\n  \"repetitions\": "
                << repetitions << ",\n  \"lat_p50_ms\": " << spread(p50).median
                << ",\n  \"lat_p95_ms\": " << spread(p95).median << ",\n  \"lat_p99_ms\": "
                << spread(p99).median << ",\n  \"lat_mean_ms\": " << spread(mean).median
                << ",\n  \"qps_single\": " << spread(qps1).median << ",\n  \"threads\": "
                << threads << ",\n  \"qps_batch\": " << batch.median << ",\n  \"qps_batch_q1\": "
                << batch.q1 << ",\n  \"qps_batch_q3\": " << batch.q3;
    }
    summary << "\n}\n";
    std::cout << mode << ' ' << data << ": memory " << memory_mib << " MiB";
    if (timing) std::cout << ", p50 " << spread(p50).median << " ms, batch QPS " << spread(qps).median;
    std::cout << '\n';
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) throw std::invalid_argument("Usage: aether_ir_compare MODE --name value ...");
        const std::string mode = argv[1];
        const Args args(argc, argv, 2);
        if (mode == "tokens") return tokens(args);
        if (mode == "bm25" || mode == "dense" || mode == "hybrid") return retrieval(mode, args);
        throw std::invalid_argument("Unknown mode " + mode);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
