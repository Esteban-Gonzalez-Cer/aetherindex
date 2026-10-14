#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "aether/ann/brute_force/brute_force_index.hpp"
#include "aether/ann/hnsw/hnsw_index.hpp"
#include "aether/distance/distance.hpp"
#include "aether/storage/vector_store.hpp"

namespace {

using namespace aetherindex;
using Clock = std::chrono::steady_clock;

constexpr std::size_t kDimension = 1024;
constexpr std::size_t kVectorCount = 10000;
constexpr std::size_t kQueryCount = 20;
constexpr std::size_t kNeighbors = 10;
constexpr std::array<std::size_t, 2> kSearchWidths = {50, 100};

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

double milliseconds(Clock::duration elapsed) {
    return std::chrono::duration<double, std::milli>(elapsed).count();
}

std::ifstream open_fixture(const std::filesystem::path& path) {
    std::ifstream input(path);
    input.imbue(std::locale::classic());
    check(input.is_open(), "Cannot open prepared real-data fixture: " + path.string());
    return input;
}

// This deliberately narrow reader accepts the numeric CSV produced by the
// project's DBpedia preparation script. It is not a production CSV parser.
std::vector<float> read_components(std::istringstream& row, bool preceding_id,
                                   const std::string& context) {
    std::vector<float> embedding;
    embedding.reserve(kDimension);
    for (std::size_t component = 0; component < kDimension; ++component) {
        if (preceding_id || component != 0) {
            char separator = '\0';
            check(static_cast<bool>(row >> separator) && separator == ',',
                  context + ": expected comma before component " + std::to_string(component));
        }
        float value = 0.0f;
        check(static_cast<bool>(row >> value) && std::isfinite(value),
              context + ": invalid component " + std::to_string(component));
        embedding.push_back(value);
    }
    row >> std::ws;
    check(row.eof(), context + ": unexpected extra components or characters");
    return embedding;
}

void load_vectors(const std::filesystem::path& directory, VectorStore& store) {
    const auto path = directory / "vectors.csv";
    auto input = open_fixture(path);
    std::string line;
    std::size_t position = 0;
    while (std::getline(input, line)) {
        const auto context = path.string() + ":" + std::to_string(position + 1);
        check(position < kVectorCount, context + ": too many vectors for the prepared fixture");
        check(!line.empty() && line.front() >= '0' && line.front() <= '9',
              context + ": expected an unsigned vector ID");
        std::istringstream row(line);
        row.imbue(std::locale::classic());
        std::uint64_t raw_id = 0;
        check(static_cast<bool>(row >> raw_id), context + ": invalid vector ID");
        // The preparation script maps source rows 0..9999 to these IDs; query
        // rows 10000..10019 are held out and have no IDs in this VectorStore.
        const auto expected_id = static_cast<VectorId>(7 * position + 10);
        check(raw_id == expected_id, context + ": unexpected source-row ID mapping");
        const auto embedding = read_components(row, true, context);
        check(store.insert(expected_id, embedding) == InsertStatus::Success,
              context + ": VectorStore rejected a prepared embedding");
        ++position;
    }
    check(input.eof() && !input.bad(), "Failed while reading " + path.string());
    check(store.size() == kVectorCount, "The real-data fixture must contain 10,000 vectors");
    check(store.dimension() == kDimension, "The real-data fixture must have dimension 1,024");
}

std::vector<float> load_query(const std::filesystem::path& directory, std::size_t query_index) {
    std::ostringstream filename;
    filename.imbue(std::locale::classic());
    filename << "query_" << std::setw(2) << std::setfill('0') << query_index << ".csv";
    const auto path = directory / "queries" / filename.str();
    auto input = open_fixture(path);
    std::string line;
    check(static_cast<bool>(std::getline(input, line)), "Missing query row: " + path.string());
    std::istringstream row(line);
    row.imbue(std::locale::classic());
    auto query = read_components(row, false, path.string());
    check(!std::getline(input, line), "Prepared query must have exactly one row: " + path.string());
    check(input.eof() && !input.bad(), "Failed while reading " + path.string());
    // Also reject zero-norm queries before the relatively expensive graph build.
    check(std::isfinite(compute_distance(query, query, DistanceKind::Cosine)),
          "Prepared cosine query is invalid: " + path.string());
    return query;
}

void check_results(const VectorStore& store, std::span<const float> query,
                   const std::vector<SearchResult>& results, const std::string& context) {
    check(results.size() == kNeighbors, context + ": expected exactly ten results");
    std::unordered_set<VectorId> seen;
    float previous_distance = -1.0f;
    for (const auto& result : results) {
        check(seen.insert(result.id).second, context + ": duplicate result ID");
        check(std::isfinite(result.distance) && result.distance >= 0.0f && result.distance <= 2.0f,
              context + ": invalid cosine distance");
        check(result.distance >= previous_distance, context + ": results are not sorted");
        const auto embedding = store.find(result.id);
        check(embedding.has_value(), context + ": result ID is absent from VectorStore");
        const auto expected = compute_distance(query, *embedding, store.distance_kind());
        check(std::abs(static_cast<double>(result.distance) - expected) < 1e-5,
              context + ": per-ID distance differs from the distance module");
        previous_distance = result.distance;
    }
}

double recall_at_ten(const std::vector<SearchResult>& exact,
                     const std::vector<SearchResult>& approximate) {
    std::unordered_set<VectorId> exact_ids;
    for (const auto& result : exact) {
        exact_ids.insert(result.id);
    }
    std::size_t matches = 0;
    for (const auto& result : approximate) {
        matches += exact_ids.contains(result.id) ? 1 : 0;
    }
    return static_cast<double>(matches) / kNeighbors;
}

std::string result_ids(const std::vector<SearchResult>& results) {
    std::ostringstream ids;
    ids.imbue(std::locale::classic());
    for (std::size_t rank = 0; rank < results.size(); ++rank) {
        if (rank != 0) {
            ids << '|';
        }
        ids << results[rank].id;
    }
    return ids.str();
}

void run(const std::filesystem::path& directory, const std::filesystem::path& report_path) {
    std::ofstream report;
    if (!report_path.empty()) {
        report.open(report_path);
        report.imbue(std::locale::classic());
        check(report.is_open(), "Cannot write real-data report: " + report_path.string());
        report << "query_index,ef_search,recall_at_10,exact_query_ms,hnsw_query_ms,exact_ids,hnsw_"
                  "ids\n"
               << std::fixed << std::setprecision(9);
    }

    const auto load_start = Clock::now();
    VectorStore store(kDimension, DistanceKind::Cosine);
    load_vectors(directory, store);
    std::vector<std::vector<float>> queries;
    queries.reserve(kQueryCount);
    for (std::size_t query_index = 0; query_index < kQueryCount; ++query_index) {
        queries.push_back(load_query(directory, query_index));
    }
    std::cout << "Loaded real DBpedia embeddings: " << store.size() << " x " << store.dimension()
              << ", held-out queries: " << queries.size()
              << ", load time (ms): " << milliseconds(Clock::now() - load_start) << '\n';

    HnswConfig config;
    config.m = 16;
    config.m_max_0 = 32;
    config.ef_construction = 100;
    config.ef_search = 50;
    config.random_seed = 42;
    HnswIndex index(store, config);
    const auto build_start = Clock::now();
    for (const auto id : store.ids()) {
        index.insert(id);
        if (index.size() % 1000 == 0) {
            std::cout << "Indexed " << index.size() << '/' << store.size() << " vectors\n"
                      << std::flush;
        }
    }
    const auto build_ms = milliseconds(Clock::now() - build_start);
    const auto graph = index.validate_graph();
    check(index.size() == store.size(), "HNSW did not index every real vector");
    check(graph.valid() && graph.nodes == store.size(), "Real-data HNSW graph validation failed");
    check(index.entry_point().has_value() && store.contains(*index.entry_point()),
          "Real-data HNSW entry point is invalid");
    check(index.max_level().has_value() && *index.max_level() > 0,
          "The real-data HNSW graph must have upper layers");
    std::cout << "Graph validation: valid; nodes: " << graph.nodes
              << "; max level: " << *index.max_level() << "; build time (ms): " << build_ms << '\n';

    std::array<double, kSearchWidths.size()> recall_sum{};
    std::array<double, kSearchWidths.size()> approximate_time_sum{};
    double exact_time_sum = 0.0;
    for (std::size_t query_index = 0; query_index < queries.size(); ++query_index) {
        const auto& query = queries[query_index];
        const auto exact_start = Clock::now();
        const auto exact = bruteForceSearch(store, query, kNeighbors);
        const auto exact_ms = milliseconds(Clock::now() - exact_start);
        exact_time_sum += exact_ms;
        check_results(store, query, exact, "Brute force query " + std::to_string(query_index));
        for (std::size_t setting = 0; setting < kSearchWidths.size(); ++setting) {
            const auto ef_search = kSearchWidths[setting];
            const auto approximate_start = Clock::now();
            const auto approximate = index.search(query, kNeighbors, ef_search);
            const auto approximate_ms = milliseconds(Clock::now() - approximate_start);
            check_results(store, query, approximate,
                          "HNSW query " + std::to_string(query_index) + " efSearch " +
                              std::to_string(ef_search));
            const auto recall = recall_at_ten(exact, approximate);
            recall_sum[setting] += recall;
            approximate_time_sum[setting] += approximate_ms;
            std::cout << "Query " << query_index << ", efSearch " << ef_search
                      << ", Recall@10: " << recall << ", query time (ms): " << approximate_ms
                      << '\n';
            if (report.is_open()) {
                report << query_index << ',' << ef_search << ',' << recall << ',' << exact_ms << ','
                       << approximate_ms << ',' << result_ids(exact) << ','
                       << result_ids(approximate) << '\n';
            }
        }
    }
    check(index.validate_graph().valid(), "Search changed real-data graph integrity");
    std::cout << "Mean brute-force query time (ms): " << exact_time_sum / kQueryCount << '\n';
    for (std::size_t setting = 0; setting < kSearchWidths.size(); ++setting) {
        std::cout << "efSearch " << kSearchWidths[setting]
                  << ": mean Recall@10: " << recall_sum[setting] / kQueryCount
                  << "; mean query time (ms): " << approximate_time_sum[setting] / kQueryCount
                  << '\n';
    }
    if (report.is_open()) {
        report.flush();
        check(static_cast<bool>(report),
              "Failed to write real-data report: " + report_path.string());
    }
    // This is approximation accuracy against exact neighbors for this fixed
    // slice. It is not a semantic-relevance score or a production latency SLA.
    check(recall_sum[1] / kQueryCount >= 0.85,
          "Mean real-data Recall@10 at efSearch=100 is below 0.85");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 2 || argc == 3,
              "Usage: hnsw_real_dataset_test <prepared DBpedia directory> [report.csv]");
        std::cout.imbue(std::locale::classic());
        std::cout << std::fixed << std::setprecision(6);
        run(argv[1], argc == 3 ? std::filesystem::path(argv[2]) : std::filesystem::path{});
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
