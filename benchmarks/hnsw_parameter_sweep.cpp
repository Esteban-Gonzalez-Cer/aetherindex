// Sprint 2 / CP-06: HNSW parameter, recall-latency and latency-percentile experiments.
//
//   aether_hnsw_parameter_sweep OUTPUT_DIR [--quick | --dbpedia VECTORS_CSV |
//       --fvecs BASE.fvecs QUERIES.fvecs [--limit N] [--metric l2|cosine]
//       [--queries N] [--build-threads N] [--ef-search-only]]
//
// Default workload: seeded synthetic Gaussian mixture (no download). --dbpedia uses the
// prepared real embeddings (datasets/real/dbpedia/vectors.csv, 10,000 x 1,024): every
// 10th row (row % 10 == 9) is held out as one of 1,000 independent queries and the
// remaining 9,000 rows are indexed. Queries are therefore never indexed vectors.
// --fvecs (KR3.3, KR3.7) indexes the first --limit rows of a TEXMEX base file (e.g. SIFT1M
// at 10K, 100K and 1M) and measures the first --queries rows (default 1,000) of the
// separate query file; ground truth is recomputed by BruteForce over exactly the indexed
// prefix. --ef-search-only skips the KR3.1 grid (one graph instead of five; use it at 1M).
// --build-threads > 1 builds with insert_parallel: faster, but the topology then depends
// on thread scheduling (search timing is always one thread).
//
// KR3.1  M in {8,16,32} x efSearch in {10,50,100} (efConstruction=100), plus
//        efConstruction in {50,200} at M=16 to show it is configurable and effective.
// KR3.2  M=16, efConstruction=100, efSearch in {10,25,50,100,200} (plus 400 with --fvecs).
// KR3.8  p50/p95/p99 (nearest rank) for every configuration.
//
// Every configuration measures the same 1,000 independent queries after 100 warm-up
// searches, one thread, against one fixed dataset. Each (M, efConstruction) graph is built
// once and serves every efSearch value of that pair. Ground truth is the exact
// BruteForce Top-10. Timing covers only HnswIndex::search (query preparation + graph
// search); dataset generation, ground truth and index construction are timed apart.
// Raw per-query latencies are written for reproducibility. --quick shrinks the run
// for smoke testing and must not be reported as KR evidence.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "aether/ann/brute_force/brute_force_index.hpp"
#include "aether/ann/hnsw/hnsw_index.hpp"
#include "aether/core/parallel.hpp"
#include "aether/storage/vector_store.hpp"

using namespace aetherindex;
using Clock = std::chrono::steady_clock;

namespace {

struct Workload {
    std::size_t vectors = 10000;
    std::size_t dimension = 128;
    std::size_t clusters = 100;
    // Per-coordinate stddev around unit-variance centers. 1.0 makes clusters overlap so
    // recall is not saturated; chosen after a --quick smoke run at 0.35 reached 1.0.
    double spread = 1.0;
    std::size_t queries = 1000;
    std::size_t warmup = 100;
    std::size_t k = 10;
    std::uint64_t dataset_seed = 42;
    std::uint64_t query_seed = 4242;
    std::uint64_t graph_seed = 42;
    std::size_t build_threads = 1;
    DistanceKind metric = DistanceKind::Cosine;
};

struct Options {
    bool quick = false, ef_search_only = false;
    std::filesystem::path dbpedia, base, query_file;
    std::size_t limit = 0, queries = 1000, build_threads = 1;
    DistanceKind metric = DistanceKind::SquaredEuclidean;
};

struct Experiment {
    std::string group;  // kr3_1, kr3_1_efc, kr3_2
    std::size_t m, ef_construction, ef_search;
};

struct Measurement {
    Experiment experiment;
    double build_ms = 0;
    double mean_recall = 0;
    double mean_ms = 0, p50_ms = 0, p95_ms = 0, p99_ms = 0, max_ms = 0;
    std::vector<double> latencies_ms;
    std::vector<double> recalls;
};

// Gaussian mixture: queries come from the same centers but are never dataset rows.
std::vector<std::vector<float>> generate(const Workload& w, std::size_t count,
                                         std::uint64_t seed,
                                         const std::vector<std::vector<float>>& centers) {
    std::mt19937_64 engine(seed);
    std::normal_distribution<float> noise(0.0f, static_cast<float>(w.spread));
    std::uniform_int_distribution<std::size_t> pick(0, centers.size() - 1);
    std::vector<std::vector<float>> rows(count, std::vector<float>(w.dimension));
    for (auto& row : rows) {
        const auto& center = centers[pick(engine)];
        for (std::size_t d = 0; d < w.dimension; ++d) row[d] = center[d] + noise(engine);
    }
    return rows;
}

// fvecs: per row an int32 dimension followed by that many float32 values. limit 0 = all.
std::vector<std::vector<float>> read_fvecs(const std::filesystem::path& path, std::size_t limit) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open " + path.string());
    std::vector<std::vector<float>> rows;
    std::int32_t dimension = 0;
    while ((limit == 0 || rows.size() < limit) &&
           input.read(reinterpret_cast<char*>(&dimension), 4)) {
        if (dimension <= 0 || (!rows.empty() && rows[0].size() != static_cast<std::size_t>(dimension)))
            throw std::runtime_error("Malformed fvecs file " + path.string());
        auto& row = rows.emplace_back(static_cast<std::size_t>(dimension));
        if (!input.read(reinterpret_cast<char*>(row.data()), 4 * static_cast<std::streamsize>(dimension)))
            throw std::runtime_error("Truncated fvecs file " + path.string());
    }
    if (limit != 0 && rows.size() < limit)
        throw std::runtime_error(path.string() + " has fewer rows than requested");
    return rows;
}

// Reads "VectorId,x1,...,xD" rows; holds out every 10th row as a query.
void load_dbpedia(const std::filesystem::path& path, Workload& w, VectorStore*& store_out,
                  std::vector<std::vector<float>>& queries, std::unique_ptr<VectorStore>& owner) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot open " + path.string());
    std::string line;
    std::size_t row = 0;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        std::stringstream fields(line);
        std::string field;
        std::getline(fields, field, ',');
        const auto id = static_cast<VectorId>(std::stoul(field));
        std::vector<float> values;
        while (std::getline(fields, field, ',')) values.push_back(std::stof(field));
        if (!owner) {
            w.dimension = values.size();
            owner = std::make_unique<VectorStore>(w.dimension, DistanceKind::Cosine);
        }
        if (row % 10 == 9)
            queries.push_back(std::move(values));
        else if (owner->insert(id, values) != InsertStatus::Success)
            throw std::runtime_error("DBpedia row rejected: " + std::to_string(row));
        ++row;
    }
    w.vectors = owner->size();
    w.queries = queries.size();
    w.clusters = 0;
    w.spread = 0;
    store_out = owner.get();
}

double percentile(std::vector<double> sorted, double p) {
    std::sort(sorted.begin(), sorted.end());
    const auto rank = static_cast<std::size_t>(std::ceil(p * static_cast<double>(sorted.size())));
    return sorted[std::max<std::size_t>(rank, 1) - 1];
}

std::unique_ptr<HnswIndex> build(const Workload& w, const VectorStore& store, std::size_t m,
                                 std::size_t ef_construction, double& build_ms) {
    HnswConfig config;
    config.m = m;
    config.m_max_0 = 2 * m;
    config.ef_construction = ef_construction;
    config.random_seed = w.graph_seed;
    const auto build_start = Clock::now();
    auto index = std::make_unique<HnswIndex>(store, config);
    index->reserve(store.size());
    const auto ids = store.ids();
    if (w.build_threads > 1)
        index->insert_parallel(ids, w.build_threads);
    else
        for (VectorId id : ids) index->insert(id);
    build_ms = std::chrono::duration<double, std::milli>(Clock::now() - build_start).count();
    if (!index->validate_graph().valid()) throw std::runtime_error("HNSW graph invariants violated.");
    std::cout << "built M=" << m << " efC=" << ef_construction << " in " << build_ms << " ms\n";
    return index;
}

Measurement run(const Workload& w, const HnswIndex& index, double build_ms,
                const Experiment& experiment, const std::vector<std::vector<float>>& queries,
                const std::vector<std::unordered_set<VectorId>>& truth) {
    Measurement result;
    result.experiment = experiment;
    result.build_ms = build_ms;
    for (std::size_t i = 0; i < w.warmup; ++i)
        static_cast<void>(index.search(queries[i % queries.size()], w.k, experiment.ef_search));
    result.latencies_ms.reserve(queries.size());
    result.recalls.reserve(queries.size());
    for (std::size_t q = 0; q < queries.size(); ++q) {
        const auto start = Clock::now();
        const auto found = index.search(queries[q], w.k, experiment.ef_search);
        const auto elapsed = Clock::now() - start;
        result.latencies_ms.push_back(std::chrono::duration<double, std::milli>(elapsed).count());
        std::size_t matches = 0;
        for (const auto& hit : found) matches += truth[q].contains(hit.id);
        result.recalls.push_back(static_cast<double>(matches) / static_cast<double>(w.k));
    }
    const double n = static_cast<double>(queries.size());
    for (std::size_t q = 0; q < queries.size(); ++q) {
        result.mean_recall += result.recalls[q] / n;
        result.mean_ms += result.latencies_ms[q] / n;
    }
    result.p50_ms = percentile(result.latencies_ms, 0.50);
    result.p95_ms = percentile(result.latencies_ms, 0.95);
    result.p99_ms = percentile(result.latencies_ms, 0.99);
    result.max_ms = *std::max_element(result.latencies_ms.begin(), result.latencies_ms.end());
    std::cout << experiment.group << " M=" << experiment.m << " efC=" << experiment.ef_construction
              << " efS=" << experiment.ef_search << " recall@10=" << result.mean_recall
              << " p50=" << result.p50_ms << "ms p99=" << result.p99_ms << "ms\n";
    return result;
}

std::ofstream open(const std::filesystem::path& path) {
    std::ofstream file(path);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.imbue(std::locale::classic());
    file << std::setprecision(9);
    return file;
}

void write_summary(std::ofstream& file, const Measurement& m) {
    file << m.experiment.group << ',' << m.experiment.m << ',' << 2 * m.experiment.m << ','
         << m.experiment.ef_construction << ',' << m.experiment.ef_search << ','
         << m.latencies_ms.size() << ',' << m.mean_recall << ',' << m.build_ms << ','
         << m.mean_ms << ',' << m.p50_ms << ',' << m.p95_ms << ',' << m.p99_ms << ',' << m.max_ms
         << '\n';
}

}  // namespace

Options parse(int argc, char** argv) {
    const auto usage = std::invalid_argument(
        "Usage: aether_hnsw_parameter_sweep OUTPUT_DIR [--quick | --dbpedia VECTORS_CSV | "
        "--fvecs BASE.fvecs QUERIES.fvecs [--limit N] [--metric l2|cosine] [--queries N] "
        "[--build-threads N] [--ef-search-only]]");
    if (argc < 2) throw usage;
    Options options;
    for (int i = 2; i < argc; ++i) {
        const std::string name = argv[i];
        const auto value = [&]() -> std::string {
            if (++i >= argc) throw usage;
            return argv[i];
        };
        if (name == "--quick") options.quick = true;
        else if (name == "--dbpedia") options.dbpedia = value();
        else if (name == "--fvecs") {
            options.base = value();
            options.query_file = value();
        } else if (name == "--limit") options.limit = std::stoul(value());
        else if (name == "--queries") options.queries = std::stoul(value());
        else if (name == "--build-threads") options.build_threads = std::stoul(value());
        else if (name == "--ef-search-only") options.ef_search_only = true;
        else if (name == "--metric") {
            const std::string metric = value();
            if (metric == "l2") options.metric = DistanceKind::SquaredEuclidean;
            else if (metric == "cosine") options.metric = DistanceKind::Cosine;
            else throw usage;
        } else throw usage;
    }
    const int modes = options.quick + !options.dbpedia.empty() + !options.base.empty();
    if (modes > 1 || options.queries == 0) throw usage;
    return options;
}

int main(int argc, char** argv) {
    try {
        const Options options = parse(argc, argv);
        const bool quick = options.quick;
        const bool dbpedia = !options.dbpedia.empty();
        const bool fvecs = !options.base.empty();
        Workload w;
        w.build_threads = options.build_threads;
        if (quick) {
            w.vectors = 2000;
            w.queries = 200;
            w.warmup = 20;
        }
        const std::filesystem::path output(argv[1]);
        std::filesystem::create_directories(output);

        std::unique_ptr<VectorStore> owner;
        VectorStore* store_ptr = nullptr;
        std::vector<std::vector<float>> queries;
        if (dbpedia) {
            load_dbpedia(options.dbpedia, w, store_ptr, queries, owner);
        } else if (fvecs) {
            auto base = read_fvecs(options.base, options.limit);
            queries = read_fvecs(options.query_file, options.queries);
            w.metric = options.metric;
            w.dimension = base.front().size();
            w.vectors = base.size();
            w.queries = queries.size();
            w.clusters = 0;
            w.spread = 0;
            owner = std::make_unique<VectorStore>(w.dimension, w.metric);
            owner->reserve(base.size());
            for (std::size_t i = 0; i < base.size(); ++i) {
                if (owner->insert(static_cast<VectorId>(i), base[i]) != InsertStatus::Success)
                    throw std::runtime_error("fvecs row rejected: " + std::to_string(i));
                std::vector<float>().swap(base[i]);
            }
            store_ptr = owner.get();
        } else {
            std::mt19937_64 center_engine(w.dataset_seed ^ 0x9e3779b97f4a7c15ULL);
            std::normal_distribution<float> unit(0.0f, 1.0f);
            std::vector<std::vector<float>> centers(w.clusters, std::vector<float>(w.dimension));
            for (auto& center : centers)
                for (auto& value : center) value = unit(center_engine);
            const auto data = generate(w, w.vectors, w.dataset_seed, centers);
            queries = generate(w, w.queries, w.query_seed, centers);
            owner = std::make_unique<VectorStore>(w.dimension, DistanceKind::Cosine);
            owner->reserve(w.vectors);
            for (std::size_t i = 0; i < data.size(); ++i)
                if (owner->insert(static_cast<VectorId>(7 * i + 10), data[i]) !=
                    InsertStatus::Success)
                    throw std::runtime_error("Dataset insertion failed.");
            store_ptr = owner.get();
        }
        const VectorStore& store = *store_ptr;

        const auto truth_start = Clock::now();
        // Exact search is read-only, so queries run on every hardware thread.
        std::vector<std::unordered_set<VectorId>> truth(queries.size());
        parallel_for(queries.size(), hardware_threads(), [&](std::size_t q) {
            for (const auto& hit : bruteForceSearch(store, queries[q], w.k)) truth[q].insert(hit.id);
        });
        const double truth_ms =
            std::chrono::duration<double, std::milli>(Clock::now() - truth_start).count();

        std::vector<Experiment> experiments;
        if (!options.ef_search_only) {
            for (std::size_t m : {8, 16, 32})
                for (std::size_t ef : {10, 50, 100}) experiments.push_back({"kr3_1", m, 100, ef});
            for (std::size_t efc : {50, 200}) experiments.push_back({"kr3_1_efc", 16, efc, 50});
        }
        for (std::size_t ef : {10, 25, 50, 100, 200}) experiments.push_back({"kr3_2", 16, 100, ef});
        if (fvecs) experiments.push_back({"kr3_2", 16, 100, 400});

        // One graph per (M, efConstruction); search never mutates it, so every efSearch of
        // that pair reuses it and its build time is reported on each of its rows.
        std::vector<Measurement> results;
        std::vector<bool> done(experiments.size(), false);
        for (std::size_t i = 0; i < experiments.size(); ++i) {
            if (done[i]) continue;
            double build_ms = 0;
            const auto index =
                build(w, store, experiments[i].m, experiments[i].ef_construction, build_ms);
            for (std::size_t j = i; j < experiments.size(); ++j) {
                if (done[j] || experiments[j].m != experiments[i].m ||
                    experiments[j].ef_construction != experiments[i].ef_construction)
                    continue;
                results.push_back(run(w, *index, build_ms, experiments[j], queries, truth));
                done[j] = true;
            }
        }

        const char* header =
            "group,m,m0,ef_construction,ef_search,queries,mean_recall_at_10,build_ms,"
            "mean_latency_ms,p50_ms,p95_ms,p99_ms,max_ms\n";
        auto kr31 = open(output / "kr3_1_hnsw_parameters.csv");
        auto kr32 = open(output / "kr3_2_recall_latency.csv");
        auto kr38 = open(output / "kr3_8_latency_percentiles.csv");
        auto raw = open(output / "raw_latencies.csv");
        kr31 << header;
        kr32 << header;
        kr38 << header;
        raw << "group,m,ef_construction,ef_search,query_index,latency_ms,recall_at_10\n";
        for (const auto& m : results) {
            if (m.experiment.group != "kr3_2") write_summary(kr31, m);
            if (m.experiment.group == "kr3_2") write_summary(kr32, m);
            write_summary(kr38, m);
            for (std::size_t q = 0; q < m.latencies_ms.size(); ++q)
                raw << m.experiment.group << ',' << m.experiment.m << ','
                    << m.experiment.ef_construction << ',' << m.experiment.ef_search << ',' << q
                    << ',' << m.latencies_ms[q] << ',' << m.recalls[q] << '\n';
        }
        auto meta = open(output / "workload.json");
        meta << "{\"quick\":" << (quick ? "true" : "false") << ",\"dataset\":\""
             << (dbpedia ? "dbpedia vectors.csv; rows with row%10==9 held out as queries"
                 : fvecs ? "fvecs base " + options.base.filename().string() + " (first rows) + " +
                               options.query_file.filename().string() + " (first rows)"
                         : std::string("synthetic Gaussian mixture"))
             << "\",\"vectors\":" << w.vectors << ",\"dimension\":" << w.dimension
             << ",\"metric\":\""
             << (w.metric == DistanceKind::Cosine ? "cosine" : "squared_euclidean")
             << "\",\"clusters\":"
             << w.clusters << ",\"cluster_spread_stddev\":" << w.spread
             << ",\"queries_per_configuration\":" << w.queries << ",\"warmup_searches\":"
             << w.warmup << ",\"k\":" << w.k << ",\"dataset_seed\":" << w.dataset_seed
             << ",\"query_seed\":" << w.query_seed << ",\"graph_seed\":" << w.graph_seed
             << ",\"m0_rule\":\"2*M\",\"threads\":1,\"build_threads\":" << w.build_threads
             << ",\"build_reproducible\":" << (w.build_threads > 1 ? "false" : "true") << ",\"ground_truth\":\"BruteForce exact Top-10\","
                "\"ground_truth_ms\":"
             << truth_ms
             << ",\"timing_scope\":\"HnswIndex::search only (query preparation + graph search); "
                "excludes data generation, ground truth and index build; one graph per (M, "
                "efConstruction) serves all its efSearch values\",\"percentile\":"
                "\"nearest-rank ceil(p*N)\",\"configurations\":"
             << results.size() << ",\"measured_searches\":" << results.size() * w.queries
             << ",\"build_type\":\""
#ifdef NDEBUG
             << "Release (NDEBUG)"
#else
             << "Debug (assertions enabled; not valid KR3 evidence)"
#endif
             << "\",\"compiler\":\"" << __VERSION__ << "\",\"cplusplus\":" << __cplusplus
             << "}\n";
        std::cout << "Wrote KR3.1/KR3.2/KR3.8 reports to " << output << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
