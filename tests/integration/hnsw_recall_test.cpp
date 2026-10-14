#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <string>
#include <unordered_set>
#include <vector>

#include "../hnsw_test_helpers.hpp"
#include "aether/ann/brute_force/brute_force_index.hpp"

using namespace aetherindex;

namespace {

constexpr std::size_t vector_count = 1000;
constexpr std::size_t dimension = 128;
constexpr std::size_t query_count = 100;
constexpr std::size_t k = 10;
constexpr std::uint32_t dataset_seed = 42;
constexpr std::uint32_t query_seed = 424242;

void write_ids(std::ostream& output, const std::vector<SearchResult>& results) {
    output << '"';
    for (std::size_t rank = 0; rank < results.size(); ++rank) {
        if (rank) output << ' ';
        output << results[rank].id;
    }
    output << '"';
}

void evaluate(DistanceKind metric, const std::vector<std::vector<float>>& data,
              const std::vector<std::vector<float>>& queries, std::ostream* report) {
    VectorStore store(dimension, metric);
    hnsw_test::populate(store, data);
    const HnswConfig config;
    HnswIndex index(store, config);
    const auto build_start = std::chrono::steady_clock::now();
    for (VectorId id : store.ids()) index.insert(id);
    const auto build_time =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - build_start);
    hnsw_test::check_graph(index, store, store.ids());

    std::vector<std::vector<SearchResult>> exact;
    exact.reserve(query_count);
    for (const auto& query : queries) {
        auto neighbors = bruteForceSearch(store, query, k + 1);
        hnsw_test::check_results(index, store, query, neighbors, k + 1);
        // BruteForce permits unspecified tie order. This protocol rejects a tied cutoff,
        // so its exact top-10 ID set is unambiguous on every evaluated query.
        require(neighbors[k - 1].distance < neighbors[k].distance,
                "Recall fixture has an exact top-10 cutoff tie; revise the documented seed.");
        neighbors.resize(k);
        exact.push_back(std::move(neighbors));
    }

    const char* metric_name = metric == DistanceKind::Cosine ? "cosine" : "squared-euclidean";
    std::cout << "Protocol metric=" << metric_name << " vectors=" << vector_count
              << " dimensions=" << dimension << " queries=" << query_count << " k=" << k
              << " dataset_seed=" << dataset_seed << " query_seed=" << query_seed
              << " graph_seed=" << config.random_seed << " M=" << config.m
              << " M0=" << config.m_max_0 << " efConstruction=" << config.ef_construction
              << " max_level=" << config.max_level << " build_ms=" << build_time.count() << '\n';

    for (std::size_t ef : {10u, 30u, 50u, 100u}) {
        // Each width searches this same graph. Warmup and all exact reference work are
        // outside the timer; timings are observations, not pass/fail performance limits.
        index.search(queries.front(), k, ef);
        double recall_sum = 0;
        double elapsed_sum = 0;
        for (std::size_t query_index = 0; query_index < query_count; ++query_index) {
            const auto& query = queries[query_index];
            const auto begin = std::chrono::steady_clock::now();
            const auto approximate = index.search(query, k, ef);
            const double elapsed_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
                    .count();
            require(std::isfinite(elapsed_ms) && elapsed_ms >= 0,
                    "Recorded search durations must be finite and nonnegative.");
            hnsw_test::check_results(index, store, query, approximate, k);
            std::unordered_set<VectorId> exact_ids;
            for (const auto& neighbor : exact[query_index]) exact_ids.insert(neighbor.id);
            std::size_t matches = 0;
            for (const auto& neighbor : approximate) matches += exact_ids.contains(neighbor.id);
            const double recall = static_cast<double>(matches) / k;
            recall_sum += recall;
            elapsed_sum += elapsed_ms;
            if (report) {
                *report << metric_name << ',' << dataset_seed << ',' << query_seed << ','
                        << config.random_seed << ',' << vector_count << ',' << dimension << ','
                        << query_count << ',' << k << ',' << config.m << ',' << config.m_max_0
                        << ',' << config.ef_construction << ',' << ef << ',' << query_index << ','
                        << recall << ',' << elapsed_ms << ',';
                write_ids(*report, exact[query_index]);
                *report << ',';
                write_ids(*report, approximate);
                *report << '\n';
            }
        }
        const double mean_recall = recall_sum / query_count;
        std::cout << "Evaluation metric=" << metric_name << " efSearch=" << ef
                  << " mean_recall_at_10=" << mean_recall
                  << " mean_query_ms=" << elapsed_sum / query_count
                  << " invalid_results=0 crashes=0\n";
        if (ef == config.ef_search) {
            require(mean_recall >= 0.85,
                    "Default HNSW mean Recall@10 must meet the MVP acceptance threshold 0.85.");
        }
        require(index.config().ef_search == config.ef_search,
                "A query-width override must not mutate the index configuration.");
        hnsw_test::check_graph(index, store, store.ids());
    }
    // The regular overload must apply the configured width to the same graph.
    const auto regular = index.search(queries.front(), k);
    const auto explicit_default = index.search(queries.front(), k, config.ef_search);
    require(regular.size() == explicit_default.size(), "Default search width was not applied.");
    for (std::size_t rank = 0; rank < regular.size(); ++rank) {
        require(regular[rank].id == explicit_default[rank].id &&
                    regular[rank].distance == explicit_default[rank].distance,
                "Configured and explicit default search widths must agree.");
    }
    hnsw_test::check_graph(index, store, store.ids());
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc <= 2, "Usage: aether_hnsw_recall_tests [report.csv]");
        std::ofstream report;
        if (argc == 2) {
            const std::filesystem::path output_path(argv[1]);
            if (!output_path.parent_path().empty()) {
                std::filesystem::create_directories(output_path.parent_path());
            }
            report.open(output_path, std::ios::trunc);
            require(report.is_open(), "Cannot open the per-query recall evidence report.");
            report.imbue(std::locale::classic());
            report << std::setprecision(17)
                   << "metric,dataset_seed,query_seed,graph_seed,vectors,dimensions,queries,k,m,m0,"
                      "ef_construction,ef_search,query_index,recall_at_10,query_ms,exact_ids,hnsw_"
                      "ids\n";
        }
        std::cout.imbue(std::locale::classic());
        std::cout << std::fixed << std::setprecision(6);
        const auto data = hnsw_test::embeddings(vector_count, dimension, dataset_seed);
        const auto queries = hnsw_test::embeddings(query_count, dimension, query_seed);
        for (const auto& query : queries) {
            for (const auto& embedding : data) {
                require(query != embedding,
                        "Recall queries must be held out rather than reused indexed embeddings.");
            }
        }
        for (DistanceKind metric : {DistanceKind::Cosine, DistanceKind::SquaredEuclidean}) {
            evaluate(metric, data, queries, argc == 2 ? &report : nullptr);
        }
        if (report.is_open()) {
            report.flush();
            require(report.good(), "Failed to save the per-query recall evidence report.");
        }
        std::cout << "Structural validation before/after: VALID; exact cutoff ties=0; "
                     "100 independent queries per metric, 400 measured searches per graph\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
