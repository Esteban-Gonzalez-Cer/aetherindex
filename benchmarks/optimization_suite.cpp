// Optimization baseline/regression suite (Release builds only).
//
//   aether_optimization_suite vectors OUT (--synthetic N DIM | --csv FILE |
//                                          --fvecs BASE QUERY GROUNDTRUTH) [options]
//   aether_optimization_suite load    OUT --synthetic N DIM [options]
//   aether_optimization_suite bm25    OUT (--zipf N | --corpus DOCS.jsonl QUERIES.jsonl) [options]
//   aether_optimization_suite hybrid  OUT --zipf N --dimension DIM [options]
//
// Options: --queries Q (default 1000), --repeat R (default 3), --k K (10),
//          --ef-search E (50), --m M (16), --ef-construction C (100),
//          --build-threads T (1), --query-threads T (1: batch throughput run),
//          --parallel-retrievers (hybrid only), --bm25-strategy NAME (bm25/hybrid),
//          vectors only: --quantization none|sq8|pq, --pq-subspaces M, --rerank N|all,
//          --drop-fp32, --filter-percent P (seeded random metadata bucket; recall against
//          the exact filtered ground truth, graph and exact strategies both timed).
//
// Every mode writes <mode>_summary.csv (one row per repetition), <mode>_results.csv
// (first repetition's Top-K, used to prove later optimizations leave default results
// unchanged) and <mode>_workload.json. Timings exclude data generation; each phase
// (load, build, ground truth, query) is timed separately. Run nothing else meanwhile.
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "aether/ann/brute_force/brute_force_index.hpp"
#include "aether/ann/hnsw/hnsw_index.hpp"
#include "aether/application/json_loader.hpp"
#include "aether/application/query_engine.hpp"
#include "aether/core/parallel.hpp"
#include "aether/hybrid/rrf.hpp"
#include "aether/lexical/bm25.hpp"
#include "aether/quantization/pq.hpp"
#include "aether/quantization/sq8.hpp"
#include "aether/storage/vector_file.hpp"
#include "bench_common.hpp"
#include "csv_loader.hpp"

using namespace aetherindex;
using namespace aetherindex::bench;

namespace {

struct Options {
    std::string mode;
    std::filesystem::path out;
    std::size_t synthetic = 0, dimension = 0, zipf = 0;
    std::filesystem::path csv, corpus, query_file, fvecs_base, fvecs_query, ivecs_truth;
    std::size_t queries = 1000, repeat = 3, k = 10, ef_search = 50, m = 16, ef_construction = 100;
    std::size_t warmup = 100;
    std::size_t build_threads = 1, query_threads = 1;
    bool parallel_retrievers = false;
    lexical::Bm25Strategy bm25_strategy = lexical::Bm25Strategy::Exhaustive;
    std::string quantization = "none";
    std::size_t pq_subspaces = 0;
    std::size_t rerank = std::numeric_limits<std::size_t>::max();  // all candidates
    bool drop_fp32 = false;
    std::size_t filter_percent = 0;
};

std::size_t number(const char* text) {
    std::size_t used = 0;
    const auto value = std::stoull(text, &used);
    if (text[used] != '\0') throw std::invalid_argument(std::string("Not an integer: ") + text);
    return static_cast<std::size_t>(value);
}

Options parse(int argc, char** argv) {
    if (argc < 3) throw std::invalid_argument("Usage: see the header of optimization_suite.cpp");
    Options o;
    o.mode = argv[1];
    o.out = argv[2];
    for (int i = 3; i < argc; ++i) {
        const std::string flag = argv[i];
        const auto next = [&] {
            if (i + 1 >= argc) throw std::invalid_argument("Missing value for " + flag);
            return argv[++i];
        };
        if (flag == "--synthetic") {
            o.synthetic = number(next());
            o.dimension = number(next());
        } else if (flag == "--fvecs") {
            o.fvecs_base = next();
            o.fvecs_query = next();
            o.ivecs_truth = next();
        } else if (flag == "--csv") {
            o.csv = next();
        } else if (flag == "--zipf") {
            o.zipf = number(next());
        } else if (flag == "--corpus") {
            o.corpus = next();
            o.query_file = next();
        } else if (flag == "--dimension") {
            o.dimension = number(next());
        } else if (flag == "--queries") {
            o.queries = number(next());
        } else if (flag == "--repeat") {
            o.repeat = number(next());
        } else if (flag == "--k") {
            o.k = number(next());
        } else if (flag == "--ef-search") {
            o.ef_search = number(next());
        } else if (flag == "--m") {
            o.m = number(next());
        } else if (flag == "--ef-construction") {
            o.ef_construction = number(next());
        } else if (flag == "--build-threads") {
            o.build_threads = number(next());
        } else if (flag == "--query-threads") {
            o.query_threads = number(next());
        } else if (flag == "--parallel-retrievers") {
            o.parallel_retrievers = true;
        } else if (flag == "--quantization") {
            o.quantization = next();
        } else if (flag == "--pq-subspaces") {
            o.pq_subspaces = number(next());
        } else if (flag == "--rerank") {
            const std::string value = next();
            o.rerank =
                value == "all" ? std::numeric_limits<std::size_t>::max() : number(value.c_str());
        } else if (flag == "--drop-fp32") {
            o.drop_fp32 = true;
        } else if (flag == "--filter-percent") {
            o.filter_percent = number(next());
        } else if (flag == "--bm25-strategy") {
            const std::string value = next();
            o.bm25_strategy = value == "taat"   ? lexical::Bm25Strategy::TermAtATime
                              : value == "wand" ? lexical::Bm25Strategy::Wand
                              : value == "bmw"  ? lexical::Bm25Strategy::BlockMaxWand
                              : value == "maxscore" ? lexical::Bm25Strategy::MaxScore
                                                : lexical::Bm25Strategy::Exhaustive;
        } else {
            throw std::invalid_argument("Unknown option " + flag);
        }
    }
    if (o.repeat == 0 || o.queries == 0 || o.k == 0)
        throw std::invalid_argument("repeat, queries and k must be positive");
    return o;
}

HnswConfig hnsw_config(const Options& o) {
    HnswConfig config;
    config.m = o.m;
    config.m_max_0 = 2 * o.m;
    config.ef_construction = o.ef_construction;
    config.ef_search = o.ef_search;
    return config;
}

void write_workload(const Options& o, const std::string& extra) {
    std::ofstream file(o.out / (o.mode + "_workload.json"));
    file.imbue(std::locale::classic());
    file << "{\"mode\":\"" << o.mode << "\",\"repeat\":" << o.repeat << ",\"queries\":" << o.queries
         << ",\"warmup\":" << o.warmup << ",\"k\":" << o.k << ",\"m\":" << o.m
         << ",\"m0\":" << 2 * o.m << ",\"ef_construction\":" << o.ef_construction
         << ",\"ef_search\":" << o.ef_search << ",\"build_threads\":" << o.build_threads
         << ",\"query_threads\":" << o.query_threads
         << ",\"parallel_retrievers\":" << (o.parallel_retrievers ? "true" : "false")
         << ",\"quantization\":\"" << o.quantization << "\",\"pq_subspaces\":" << o.pq_subspaces
         << ",\"rerank\":"
         << (o.rerank == std::numeric_limits<std::size_t>::max() ? std::string("\"all\"")
                                                                 : std::to_string(o.rerank))
         << ",\"drop_fp32\":" << (o.drop_fp32 ? "true" : "false")
         << ",\"filter_percent\":" << o.filter_percent << ",\"build_type\":\"" << build_type()
         << "\",\"compiler\":\"" << __VERSION__ << "\"," << extra << "}\n";
}

// ---------------------------------------------------------------- vectors / load

struct VectorWorkload {
    std::unique_ptr<VectorStore> store;
    std::vector<std::vector<float>> queries;
    std::string description;
    std::vector<std::unordered_set<VectorId>> truth;  // Official ground truth, if any.
    DistanceKind metric = DistanceKind::Cosine;
};

// TEXMEX .fvecs/.ivecs: per row an int32 dimension followed by that many values.
template <class T>
std::vector<std::vector<T>> read_vecs(const std::filesystem::path& path, std::size_t limit) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open " + path.string());
    std::vector<std::vector<T>> rows;
    std::int32_t dimension = 0;
    while (rows.size() < limit && input.read(reinterpret_cast<char*>(&dimension), 4)) {
        if (dimension <= 0) throw std::runtime_error("Invalid vecs dimension in " + path.string());
        std::vector<T> row(static_cast<std::size_t>(dimension));
        if (!input.read(reinterpret_cast<char*>(row.data()),
                        static_cast<std::streamsize>(row.size() * sizeof(T))))
            throw std::runtime_error("Truncated vecs file " + path.string());
        rows.push_back(std::move(row));
    }
    return rows;
}

// SIFT1M: squared Euclidean, IDs = base row numbers, official exact Top-100 truth.
VectorWorkload fvecs_vectors(const Options& o) {
    VectorWorkload w;
    w.metric = DistanceKind::SquaredEuclidean;
    const auto base = read_vecs<float>(o.fvecs_base, std::numeric_limits<std::size_t>::max());
    w.store = std::make_unique<VectorStore>(base.front().size(), w.metric);
    w.store->reserve(base.size());
    for (std::size_t i = 0; i < base.size(); ++i)
        if (w.store->insert(static_cast<VectorId>(i), base[i]) != InsertStatus::Success)
            throw std::runtime_error("fvecs row rejected");
    w.queries = read_vecs<float>(o.fvecs_query, o.queries);
    for (const auto& row : read_vecs<std::int32_t>(o.ivecs_truth, w.queries.size())) {
        if (row.size() < o.k) throw std::runtime_error("ground truth shorter than k");
        std::unordered_set<VectorId> ids;
        for (std::size_t i = 0; i < o.k; ++i) ids.insert(static_cast<VectorId>(row[i]));
        w.truth.push_back(std::move(ids));
    }
    w.description = "SIFT1M (TEXMEX) base/query with official ground truth";
    return w;
}

VectorWorkload synthetic_vectors(const Options& o) {
    MixtureSpec spec;
    spec.dimension = o.dimension;
    const GaussianMixture mixture(spec);
    VectorWorkload w;
    w.store = std::make_unique<VectorStore>(o.dimension, DistanceKind::Cosine);
    w.store->reserve(o.synthetic);
    const auto rows = mixture.sample(o.synthetic, 42);
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (w.store->insert(static_cast<VectorId>(i), rows[i]) != InsertStatus::Success)
            throw std::runtime_error("synthetic insert failed");
    w.queries = mixture.sample(o.queries, 4242);
    w.description = "synthetic Gaussian mixture (100 clusters, spread 1.0, seeds 42/4242)";
    return w;
}

// "VectorId,x1..xD" rows; every 10th row (row % 10 == 9) is a held-out query.
VectorWorkload csv_vectors(const Options& o) {
    std::ifstream input(o.csv);
    if (!input) throw std::runtime_error("Cannot open " + o.csv.string());
    VectorWorkload w;
    std::string line, field;
    for (std::size_t row = 0; std::getline(input, line); ++row) {
        std::stringstream fields(line);
        std::getline(fields, field, ',');
        const auto id = static_cast<VectorId>(std::stoul(field));
        std::vector<float> values;
        while (std::getline(fields, field, ',')) values.push_back(std::stof(field));
        if (!w.store) w.store = std::make_unique<VectorStore>(values.size(), DistanceKind::Cosine);
        if (row % 10 == 9) {
            if (w.queries.size() < o.queries) w.queries.push_back(std::move(values));
        } else if (w.store->insert(id, values) != InsertStatus::Success) {
            throw std::runtime_error("CSV row rejected");
        }
    }
    w.description = o.csv.filename().string() + "; rows with row%10==9 held out as queries";
    return w;
}

int run_vectors(const Options& o) {
    auto w = !o.fvecs_base.empty() ? fvecs_vectors(o)
             : o.csv.empty()       ? synthetic_vectors(o)
                                   : csv_vectors(o);
    auto& store = *w.store;
    // Optional metadata filter: a seeded bucket 0..99 per vector, independent of geometry.
    std::vector<VectorId> allowed;
    std::unordered_set<VectorId> allowed_set;
    if (o.filter_percent) {
        std::mt19937_64 engine(777);
        for (VectorId id : store.ids())
            if (engine() % 100 < o.filter_percent) allowed.push_back(id);
        allowed_set.insert(allowed.begin(), allowed.end());
    }
    const auto truth_start = Clock::now();
    auto truth = std::move(w.truth);
    const bool official_truth = !truth.empty() && !o.filter_percent;
    if (o.filter_percent) truth.clear();
    for (std::size_t q = truth.size(); q < w.queries.size(); ++q) {
        std::unordered_set<VectorId> ids;
        const PreparedQuery prepared(w.queries[q], store.dimension(), store.distance_kind());
        const auto exact = o.filter_percent ? bruteForceSearchSubset(store, prepared, o.k, allowed)
                                            : bruteForceSearchPrepared(store, prepared, o.k);
        for (const auto& hit : exact) ids.insert(hit.id);
        truth.push_back(std::move(ids));
    }
    const double truth_ms = elapsed_ms(truth_start);
    // Exact filtered search latency (the strategy used at low selectivity).
    double exact_filter_p50 = 0;
    if (o.filter_percent) {
        std::vector<double> exact_latency;
        for (const auto& q : w.queries) {
            const auto t = Clock::now();
            const PreparedQuery prepared(q, store.dimension(), store.distance_kind());
            static_cast<void>(bruteForceSearchSubset(store, prepared, o.k, allowed));
            exact_latency.push_back(elapsed_ms(t));
        }
        exact_filter_p50 = summarize(exact_latency).p50;
    }
    auto summary = open_csv((o.out / "vectors_summary.csv").string(),
                            "rep,vectors,dimension,build_threads,build_ms,graph_bytes,vector_bytes,"
                            "rss_mib,recall_at_k,mean_ms,p50_ms,p95_ms,p99_ms,max_ms,qps,"
                            "query_threads,batch_qps,quantization,code_bytes,code_build_ms,"
                            "rerank,fp32_released,filter_percent,filter_matches,"
                            "exact_filter_p50_ms");
    auto results = open_csv((o.out / "vectors_results.csv").string(), "query,ids");
    const std::size_t repeats = o.drop_fp32 ? 1 : o.repeat;  // Rows cannot come back.
    for (std::size_t rep = 1; rep <= repeats; ++rep) {
        const auto start = Clock::now();
        HnswIndex index(store, hnsw_config(o));
        index.insert_parallel(store.ids(), o.build_threads);
        const double build_ms = elapsed_ms(start);
        std::unique_ptr<QuantizedVectors> codes;
        double code_ms = 0;
        if (o.quantization != "none") {
            const auto code_start = Clock::now();
            if (o.quantization == "sq8") {
                codes = std::make_unique<Sq8Vectors>(store);
            } else if (o.quantization == "pq") {
                PqConfig config;
                config.subspaces = o.pq_subspaces;
                codes = std::make_unique<PqVectors>(store, config);
            } else {
                throw std::invalid_argument("Unknown quantization " + o.quantization);
            }
            code_ms = elapsed_ms(code_start);
            if (o.drop_fp32) store.release_rows();
        }
        const std::size_t rerank = o.drop_fp32 ? 0 : o.rerank;
        const std::function<bool(VectorId)> allow =
            o.filter_percent ? std::function<bool(VectorId)>(
                                   [&](VectorId id) { return allowed_set.contains(id); })
                             : std::function<bool(VectorId)>();
        const auto search = [&](const std::vector<float>& q) {
            const PreparedQuery prepared(q, store.dimension(), store.distance_kind());
            if (codes || allow)
                return index.search_filtered(prepared, o.k, o.ef_search, allow, codes.get(),
                                             codes ? rerank : 0);
            return index.search_prepared(prepared, o.k, o.ef_search);
        };
        for (std::size_t i = 0; i < o.warmup; ++i)
            static_cast<void>(search(w.queries[i % w.queries.size()]));
        std::vector<double> latencies;
        double recall = 0;
        for (std::size_t q = 0; q < w.queries.size(); ++q) {
            const auto t = Clock::now();
            const auto hits = search(w.queries[q]);
            latencies.push_back(elapsed_ms(t));
            std::size_t matches = 0;
            for (const auto& hit : hits) matches += truth[q].contains(hit.id);
            recall += static_cast<double>(matches) / static_cast<double>(o.k);
            if (rep == 1) {
                results << q << ',';
                for (std::size_t i = 0; i < hits.size(); ++i)
                    results << (i ? " " : "") << hits[i].id;
                results << '\n';
            }
        }
        // Batch throughput: all queries spread over query_threads (wall clock).
        const auto batch_start = Clock::now();
        parallel_for(w.queries.size(), o.query_threads,
                     [&](std::size_t q) { static_cast<void>(search(w.queries[q])); });
        const double batch_qps =
            1000.0 * static_cast<double>(w.queries.size()) / elapsed_ms(batch_start);
        const auto s = summarize(latencies);
        const double mean_recall = recall / static_cast<double>(w.queries.size());
        summary << rep << ',' << store.size() << ',' << store.dimension() << ',' << o.build_threads
                << ',' << build_ms << ',' << index.memory_usage().total_bytes() << ','
                << (store.has_rows() ? store.size() * store.padded_dimension() * sizeof(float) : 0)
                << ',' << resident_mib() << ',' << mean_recall << ',' << s.mean << ',' << s.p50
                << ',' << s.p95 << ',' << s.p99 << ',' << s.max << ',' << s.qps << ','
                << o.query_threads << ',' << batch_qps << ',' << o.quantization << ','
                << (codes ? codes->memory_bytes() : 0) << ',' << code_ms << ','
                << (rerank == std::numeric_limits<std::size_t>::max() ? std::string("all")
                                                                      : std::to_string(rerank))
                << ',' << (!store.has_rows()) << ',' << o.filter_percent << ',' << allowed.size()
                << ',' << exact_filter_p50 << '\n';
        std::cout << "vectors rep " << rep << ": build " << build_ms << " ms, recall@" << o.k << " "
                  << mean_recall << ", p50 " << s.p50 << " ms, p99 " << s.p99 << " ms, batch "
                  << batch_qps << " q/s\n";
    }
    std::ostringstream extra;
    extra << "\"dataset\":\"" << w.description << "\",\"vectors\":" << store.size()
          << ",\"dimension\":" << store.dimension() << ",\"metric\":\""
          << (w.metric == DistanceKind::Cosine ? "cosine" : "squared-euclidean")
          << "\",\"ground_truth\":\""
          << (official_truth     ? "official dataset ground truth"
              : o.filter_percent ? "BruteForce exact over the filtered set"
                                 : "BruteForce exact")
          << "\",\"ground_truth_ms\":" << truth_ms << ",\"peak_rss_mib\":" << peak_resident_mib();
    write_workload(o, extra.str());
    return 0;
}

int run_load(const Options& o) {
    if (o.synthetic == 0) throw std::invalid_argument("load requires --synthetic N DIM");
    MixtureSpec spec;
    spec.dimension = o.dimension;
    const auto rows = GaussianMixture(spec).sample(o.synthetic, 42);
    const auto tmp = o.out / "load-tmp";
    std::filesystem::create_directories(tmp);
    const auto csv = tmp / "vectors.csv", jsonl = tmp / "corpus.jsonl", avec = tmp / "vectors.avec";
    {
        std::ofstream c(csv), j(jsonl);
        c.imbue(std::locale::classic());
        j.imbue(std::locale::classic());
        c << std::setprecision(9);
        j << std::setprecision(9);
        for (std::size_t i = 0; i < rows.size(); ++i) {
            c << i;
            j << "{\"document_id\":" << i << ",\"text\":\"\",\"embedding\":[";
            for (std::size_t d = 0; d < rows[i].size(); ++d) {
                c << ',' << rows[i][d];
                j << (d ? "," : "") << rows[i][d];
            }
            c << '\n';
            j << "]}\n";
        }
    }
    {
        std::vector<VectorId> ids(rows.size());
        std::vector<float> flat;
        flat.reserve(rows.size() * o.dimension);
        for (std::size_t i = 0; i < rows.size(); ++i) {
            ids[i] = static_cast<VectorId>(i);
            flat.insert(flat.end(), rows[i].begin(), rows[i].end());
        }
        write_vector_file(avec, ids, flat, o.dimension);
    }
    auto summary = open_csv((o.out / "load_summary.csv").string(),
                            "rep,format,vectors,dimension,file_bytes,load_ms,mib_per_s");
    for (std::size_t rep = 1; rep <= o.repeat; ++rep) {
        for (const auto& [format, path] :
             {std::pair{"csv", csv}, std::pair{"jsonl", jsonl}, std::pair{"avec", avec}}) {
            const auto start = Clock::now();
            std::size_t loaded = 0;
            if (std::string(format) == "csv")
                loaded = detail::load_csv_dataset(path, DistanceKind::Cosine)->size();
            else if (std::string(format) == "avec")
                loaded = read_vector_file(path, DistanceKind::Cosine)->size();
            else
                loaded =
                    detail::load_jsonl_corpus(path.string(), DistanceKind::Cosine).first->size();
            const double ms = elapsed_ms(start);
            if (loaded != rows.size()) throw std::runtime_error("load count mismatch");
            const double bytes = static_cast<double>(std::filesystem::file_size(path));
            summary << rep << ',' << format << ',' << loaded << ',' << o.dimension << ',' << bytes
                    << ',' << ms << ',' << bytes / (1024.0 * 1024.0) / (ms / 1000.0) << '\n';
            std::cout << "load rep " << rep << " " << format << ": " << ms << " ms\n";
        }
    }
    std::filesystem::remove_all(tmp);
    std::ostringstream extra;
    extra << "\"dataset\":\"synthetic Gaussian mixture written as CSV, JSONL and .avec\","
          << "\"vectors\":" << o.synthetic << ",\"dimension\":" << o.dimension
          << ",\"scope\":\"production loaders: parse + validate + VectorStore insert\"";
    write_workload(o, extra.str());
    return 0;
}

// ---------------------------------------------------------------- bm25 / hybrid

struct TextWorkload {
    std::vector<std::pair<DocumentId, std::string>> documents;
    std::vector<std::string> queries;
    std::string description;
};

TextWorkload text_workload(const Options& o) {
    TextWorkload w;
    if (o.zipf) {
        const ZipfText zipf(ZipfSpec{});
        auto docs = zipf.documents(o.zipf);
        for (std::size_t i = 0; i < docs.size(); ++i)
            w.documents.emplace_back(static_cast<DocumentId>(i), std::move(docs[i]));
        w.queries = zipf.queries(o.queries, 4242);
        w.description = "synthetic Zipf text (vocabulary 200000, s=1.07, 20-200 tokens)";
        return w;
    }
    for (auto& doc : detail::load_jsonl_documents(o.corpus))
        w.documents.emplace_back(doc.document_id, doc.title + "\n" + doc.text);
    for (auto& query : detail::load_jsonl_queries(o.query_file.string(), false))
        if (w.queries.size() < o.queries) w.queries.push_back(std::move(query.text));
    w.description = o.corpus.filename().string();
    return w;
}

int run_bm25(const Options& o) {
    const auto w = text_workload(o);
    auto summary = open_csv((o.out / "bm25_summary.csv").string(),
                            "rep,documents,build_ms,rss_mib,mean_ms,p50_ms,p95_ms,p99_ms,max_ms,"
                            "qps,mean_results,strategy,postings,raw_posting_bytes,"
                            "compressed_posting_bytes");
    auto results = open_csv((o.out / "bm25_results.csv").string(), "query,results");
    results << std::setprecision(17);
    for (std::size_t rep = 1; rep <= o.repeat; ++rep) {
        const double rss_before = resident_mib();
        const auto start = Clock::now();
        lexical::Bm25Config bm25;
        bm25.strategy = o.bm25_strategy;
        lexical::Bm25Index index(bm25);
        for (const auto& [id, text] : w.documents) index.add_document(id, text);
        index.build();
        const double build_ms = elapsed_ms(start);
        const double rss = resident_mib() - rss_before;
        for (std::size_t i = 0; i < o.warmup; ++i)
            static_cast<void>(index.search(w.queries[i % w.queries.size()], o.k));
        std::vector<double> latencies;
        double returned = 0;
        for (std::size_t q = 0; q < w.queries.size(); ++q) {
            const auto t = Clock::now();
            const auto hits = index.search(w.queries[q], o.k);
            latencies.push_back(elapsed_ms(t));
            returned += static_cast<double>(hits.size());
            if (rep == 1) {
                results << q << ',';
                for (std::size_t i = 0; i < hits.size(); ++i)
                    results << (i ? " " : "") << hits[i].document_id << ':' << hits[i].score;
                results << '\n';
            }
        }
        const auto s = summarize(latencies);
        summary << rep << ',' << w.documents.size() << ',' << build_ms << ',' << rss << ','
                << s.mean << ',' << s.p50 << ',' << s.p95 << ',' << s.p99 << ',' << s.max << ','
                << s.qps << ',' << returned / static_cast<double>(w.queries.size()) << ','
                << static_cast<int>(o.bm25_strategy) << ',' << index.posting_memory().postings
                << ',' << index.posting_memory().raw_bytes << ','
                << index.posting_memory().compressed_bytes << '\n';
        std::cout << "bm25 rep " << rep << ": build " << build_ms << " ms, index ~" << rss
                  << " MiB, p50 " << s.p50 << " ms, p99 " << s.p99 << " ms\n";
    }
    std::ostringstream extra;
    extra << "\"dataset\":\"" << w.description << "\",\"documents\":" << w.documents.size()
          << ",\"bm25\":{\"k1\":1.2,\"b\":0.75},\"rss_scope\":\"resident delta across index "
             "build (approximate)\",\"peak_rss_mib\":"
          << peak_resident_mib();
    write_workload(o, extra.str());
    return 0;
}

int run_hybrid(const Options& o) {
    if (!o.zipf || !o.dimension)
        throw std::invalid_argument("hybrid requires --zipf N --dimension D");
    const auto text = text_workload(o);
    MixtureSpec spec;
    spec.dimension = o.dimension;
    const GaussianMixture mixture(spec);
    const auto rows = mixture.sample(o.zipf, 42);
    const auto query_vectors = mixture.sample(o.queries, 4242);
    VectorStore store(o.dimension, DistanceKind::Cosine);
    store.reserve(rows.size());
    VectorDocumentBinding binding;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        store.insert(static_cast<VectorId>(i), rows[i]);
        binding.emplace(static_cast<VectorId>(i), static_cast<DocumentId>(i));
    }
    auto summary = open_csv((o.out / "hybrid_summary.csv").string(),
                            "rep,documents,dimension,dense_build_ms,lexical_build_ms,channel,"
                            "mean_ms,p50_ms,p95_ms,p99_ms,max_ms,qps");
    auto results = open_csv((o.out / "hybrid_results.csv").string(), "query,results");
    results << std::setprecision(17);
    const std::size_t candidates = std::max<std::size_t>(o.k, 50);
    for (std::size_t rep = 1; rep <= o.repeat; ++rep) {
        auto start = Clock::now();
        HnswIndex dense(store, hnsw_config(o));
        dense.insert_parallel(store.ids(), o.build_threads);
        const double dense_ms = elapsed_ms(start);
        start = Clock::now();
        lexical::Bm25Config bm25;
        bm25.strategy = o.bm25_strategy;
        lexical::Bm25Index lexical(bm25);
        for (const auto& [id, body] : text.documents) lexical.add_document(id, body);
        lexical.build();
        const double lexical_ms = elapsed_ms(start);
        const hybrid::RrfFusion fusion;
        const QueryEngine engine;
        std::map<std::string, std::vector<double>> latency;
        for (std::size_t q = 0; q < o.warmup + text.queries.size(); ++q) {
            const std::size_t i = q % text.queries.size();
            const bool measured = q >= o.warmup;
            auto t = Clock::now();
            static_cast<void>(engine.search(dense, query_vectors[i], candidates));
            if (measured) latency["dense"].push_back(elapsed_ms(t));
            t = Clock::now();
            static_cast<void>(engine.search(lexical, text.queries[i], candidates));
            if (measured) latency["lexical"].push_back(elapsed_ms(t));
            t = Clock::now();
            HybridQuery hybrid_query{text.queries[i], query_vectors[i], o.k, candidates};
            hybrid_query.parallel_retrievers = o.parallel_retrievers;
            const auto fused = engine.search(dense, binding, lexical, fusion, hybrid_query);
            if (measured) {
                latency["hybrid"].push_back(elapsed_ms(t));
                if (rep == 1) {
                    results << i << ',';
                    for (std::size_t r = 0; r < fused.fused.size(); ++r)
                        results << (r ? " " : "") << fused.fused[r].document_id << ':'
                                << fused.fused[r].score;
                    results << '\n';
                }
            }
        }
        for (const auto& [channel, values] : latency) {
            const auto s = summarize(values);
            summary << rep << ',' << rows.size() << ',' << o.dimension << ',' << dense_ms << ','
                    << lexical_ms << ',' << channel << ',' << s.mean << ',' << s.p50 << ',' << s.p95
                    << ',' << s.p99 << ',' << s.max << ',' << s.qps << '\n';
            std::cout << "hybrid rep " << rep << " " << channel << ": p50 " << s.p50 << " ms, p99 "
                      << s.p99 << " ms\n";
        }
    }
    std::ostringstream extra;
    extra << "\"dataset\":\"Zipf text + Gaussian mixture vectors bound by DocumentId\","
          << "\"documents\":" << rows.size() << ",\"dimension\":" << o.dimension
          << ",\"candidate_k\":" << candidates
          << ",\"k_rrf\":60,\"peak_rss_mib\":" << peak_resident_mib();
    write_workload(o, extra.str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto o = parse(argc, argv);
        std::filesystem::create_directories(o.out);
        if (o.mode == "vectors") return run_vectors(o);
        if (o.mode == "load") return run_load(o);
        if (o.mode == "bm25") return run_bm25(o);
        if (o.mode == "hybrid") return run_hybrid(o);
        throw std::invalid_argument("Unknown mode " + o.mode);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
