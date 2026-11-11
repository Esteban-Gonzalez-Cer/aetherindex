#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <string>
#include <unordered_set>
#include <vector>

#include "../hnsw_test_helpers.hpp"
#include "aether/application/query_engine.hpp"

using namespace aetherindex;

namespace {

// Acceptance protocol v2: same frozen data/queries; normalized cosine storage
// and diversified selection replace the Sprint 1 raw-vector/simple baseline.
// Seed 424242 remains the development recall fixture; do not tune on this set.
constexpr std::size_t vector_count = 1000;
constexpr std::size_t dimension = 128;
constexpr std::size_t query_count = 100;
constexpr std::size_t k = 10;
constexpr std::uint32_t dataset_seed = 42;
constexpr std::uint32_t query_seed = 20261014;
constexpr double recall_threshold = 0.85;
using Dataset = std::vector<std::vector<float>>;

std::ofstream open_report(const std::filesystem::path& path) {
    std::ofstream stream(path, std::ios::trunc);
    require(stream.is_open(), "Cannot open an OKR1 evidence file.");
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    stream.imbue(std::locale::classic());
    stream << std::setprecision(17);
    return stream;
}

const char* status_name(InsertStatus status) {
    switch (status) {
        case InsertStatus::Success:
            return "Success";
        case InsertStatus::DuplicateId:
            return "DuplicateId";
        case InsertStatus::DimensionMismatch:
            return "DimensionMismatch";
        case InsertStatus::InvalidEmbedding:
            return "InvalidEmbedding";
    }
    return "Unknown";
}

// Exact retrieval comparison against the normalized representation. Independent
// traditional-cosine parity is verified by unit.optimization_contracts.
void check_store(const VectorStore& store, const Dataset& data, std::size_t count) {
    require(store.size() == count && store.ids().size() == count,
            "VectorStore size differs from successful insertions.");
    require(store.dimension() == dimension && store.distance_kind() == DistanceKind::Cosine,
            "VectorStore configuration changed.");
    for (std::size_t row = 0; row < count; ++row) {
        const auto id = hnsw_test::fixture_id(row);
        require(store.ids()[row] == id && store.contains(id),
                "An inserted ID was lost or reordered.");
        const auto view = store.find(id);
        require(view && view->size() == dimension, "Missing embedding or incorrect dimension.");
        const PreparedQuery expected(data[row], dimension, DistanceKind::Cosine);
        require(std::equal(view->begin(), view->end(), expected.values().begin()),
                "A normalized FP32 embedding component was altered.");
    }
}

std::size_t violations(const HnswGraphValidation& report) {
    return report.invalid_references + report.missing_embeddings + report.invalid_levels +
           report.layer_violations + report.self_references + report.duplicate_edges +
           report.degree_violations + report.entry_point_violations + report.max_level_violations +
           report.insertion_count_violations;
}

void graph_checkpoint(const char* phase, const HnswIndex& index, const VectorStore& store,
                      std::ostream& output) {
    const auto report = index.validate_graph();
    output << phase << ',' << report.nodes << ',' << report.invalid_references << ','
           << report.missing_embeddings << ',' << report.invalid_levels << ','
           << report.layer_violations << ',' << report.self_references << ','
           << report.duplicate_edges << ',' << report.degree_violations << ','
           << report.entry_point_violations << ',' << report.max_level_violations << ','
           << report.insertion_count_violations << ',' << report.usable << ',' << violations(report)
           << '\n';
    // Independent traversal supplements the index's own diagnostic implementation.
    hnsw_test::check_graph(index, store, store.ids());
}

void write_ids(std::ostream& output, const std::vector<SearchResult>& results) {
    output << '"';
    for (std::size_t row = 0; row < results.size(); ++row) {
        if (row) output << ' ';
        output << results[row].id;
    }
    output << '"';
}

void write_fixtures(const std::filesystem::path& directory, const Dataset& data,
                    const Dataset& queries) {
    std::filesystem::create_directories(directory / "queries");
    auto dataset = open_report(directory / "vectors.csv");
    dataset << std::setprecision(std::numeric_limits<float>::max_digits10);
    for (std::size_t row = 0; row < data.size(); ++row) {
        dataset << hnsw_test::fixture_id(row);
        for (float value : data[row]) dataset << ',' << value;
        dataset << '\n';
    }
    dataset.close();
    for (std::size_t row = 0; row < queries.size(); ++row) {
        auto query = open_report(directory / "queries" / ("query_" + std::to_string(row) + ".csv"));
        query << std::setprecision(std::numeric_limits<float>::max_digits10);
        for (std::size_t column = 0; column < dimension; ++column) {
            if (column) query << ',';
            query << queries[row][column];
        }
        query << '\n';
        query.close();
    }
}

void validate_rejections(VectorStore& store, const Dataset& data, std::ostream& ledger) {
    const auto reject = [&](const char* test, VectorId id, const std::vector<float>& embedding,
                            InsertStatus expected) {
        const auto actual = store.insert(id, embedding);
        ledger << test << ',' << id << ',' << status_name(actual) << ',' << status_name(expected)
               << ',' << store.size() << '\n';
        require(actual == expected, "Invalid embedding was not rejected with the expected status.");
        // Reacquire views after every attempted write, including failed insertions.
        check_store(store, data, vector_count);
    };
    reject("duplicate_id", hnsw_test::fixture_id(0), data[1], InsertStatus::DuplicateId);
    reject("short_dimension", 0, std::vector<float>(dimension - 1, 1),
           InsertStatus::DimensionMismatch);
    reject("long_dimension", 0, std::vector<float>(dimension + 1, 1),
           InsertStatus::DimensionMismatch);
    for (const auto& [name, value] :
         {std::pair{"nan", std::numeric_limits<float>::quiet_NaN()},
          std::pair{"positive_infinity", std::numeric_limits<float>::infinity()},
          std::pair{"negative_infinity", -std::numeric_limits<float>::infinity()}}) {
        auto invalid = data.front();
        invalid[64] = value;
        reject(name, 0, invalid, InsertStatus::InvalidEmbedding);
    }
    reject("cosine_zero", 0, std::vector<float>(dimension, 0), InsertStatus::InvalidEmbedding);
    require(!store.contains(0) && !store.find(0), "A missing ID must return an empty optional.");
    ledger << "missing_id,0,Missing,Missing," << store.size() << '\n';
    check_store(store, data, vector_count);
}

struct QueryEvidence {
    std::vector<SearchResult> exact;
    std::vector<SearchResult> approximate;
    std::size_t matches = 0;
    double exact_ms = 0;
    double approximate_ms = 0;
    bool cutoff_tie = false;
    bool valid = false;
    bool execution_error = false;
};

template <typename Function>
double timed(Function operation) {
    const auto start = std::chrono::steady_clock::now();
    operation();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

void validate(const std::filesystem::path& directory) {
    static_assert(std::numeric_limits<float>::is_iec559 && sizeof(float) == 4,
                  "The acceptance protocol requires IEEE-754 FP32 embeddings.");
    const auto data = hnsw_test::embeddings(vector_count, dimension, dataset_seed);
    const auto queries = hnsw_test::embeddings(query_count, dimension, query_seed);
    for (std::size_t row = 0; row < queries.size(); ++row) {
        for (const auto& embedding : data) {
            require(queries[row] != embedding,
                    "Acceptance queries must be held out from the dataset.");
        }
        for (std::size_t previous = 0; previous < row; ++previous) {
            require(queries[row] != queries[previous], "Acceptance queries must be distinct.");
        }
    }
    write_fixtures(directory / "fixtures", data, queries);
    auto insertions = open_report(directory / "insertions.csv");
    insertions << "batch,row,vector_id,store_status,index_status,store_size,index_size\n";
    auto graph = open_report(directory / "graph_checkpoints.csv");
    graph << "phase,nodes,invalid_references,missing_embeddings,invalid_levels,layer_violations,"
             "self_references,duplicate_edges,degree_violations,entry_point_violations,"
             "max_level_violations,insertion_count_violations,usable,total_violations\n";
    VectorStore store(dimension, DistanceKind::Cosine);
    // Explicit values guard the acceptance protocol against future changes to defaults.
    HnswConfig config;
    config.m = 16;
    config.m_max_0 = 32;
    config.ef_construction = 100;
    config.ef_search = 50;
    config.random_seed = 42;
    config.max_level = 32;
    HnswIndex index(store, config);
    QueryEngine engine;
    std::vector<std::size_t> initial_levels;
    double build_ms = 0;
    for (std::size_t batch = 0; batch < 2; ++batch) {
        for (std::size_t row = batch * 500; row < (batch + 1) * 500; ++row) {
            const auto id = hnsw_test::fixture_id(row);
            const auto status = store.insert(id, data[row]);
            insertions << batch + 1 << ',' << row << ',' << id << ',' << status_name(status) << ',';
            require(status == InsertStatus::Success, "Official dataset insertion failed.");
            try {
                build_ms += timed([&] { index.insert(id); });
            } catch (...) {
                insertions << "Exception," << store.size() << ',' << index.size() << '\n';
                throw;
            }
            insertions << "Success," << store.size() << ',' << index.size() << '\n';
        }
        graph_checkpoint(batch == 0 ? "after_500" : "after_1000", index, store, graph);
        check_store(store, data, (batch + 1) * 500);
        if (batch == 0) {
            for (VectorId id : store.ids())
                initial_levels.push_back(index.find_node(id)->max_level);
            for (std::size_t row = 0; row < 5; ++row) {
                hnsw_test::check_results(index, store, queries[row],
                                         engine.search(index, queries[row], k), k);
            }
        }
    }
    std::size_t retained = 0;
    for (std::size_t row = 0; row < initial_levels.size(); ++row) {
        const auto node = index.find_node(hnsw_test::fixture_id(row));
        require(node && node->max_level == initial_levels[row],
                "Incremental insertion replaced an old node.");
        ++retained;
    }
    auto invalid = open_report(directory / "invalid_inputs.csv");
    invalid << "case,vector_id,actual_status,expected_status,store_size\n";
    validate_rejections(store, data, invalid);
    invalid.close();
    insertions.close();

    std::vector<QueryEvidence> evidence(query_count);
    std::size_t successful_queries = 0;
    std::size_t invalid_queries = 0;
    std::size_t execution_errors = 0;
    std::size_t total_matches = 0;
    std::size_t cutoff_ties = 0;
    for (std::size_t row = 0; row < query_count; ++row) {
        auto& record = evidence[row];
        try {
            // Full exact traversal/sort is provided by BruteForce through QueryEngine.
            record.exact_ms =
                timed([&] { record.exact = engine.search(store, queries[row], vector_count); });
            hnsw_test::check_results(index, store, queries[row], record.exact, vector_count);
            // Deterministic ground truth for ties, without changing the public BruteForce contract.
            std::sort(record.exact.begin(), record.exact.end(), [](const auto& a, const auto& b) {
                return a.distance < b.distance || (a.distance == b.distance && a.id < b.id);
            });
            record.cutoff_tie = record.exact[k - 1].distance == record.exact[k].distance;
            cutoff_ties += record.cutoff_tie;
            record.exact.resize(k);
            record.approximate_ms =
                timed([&] { record.approximate = engine.search(index, queries[row], k); });
        } catch (const std::exception& error) {
            record.execution_error = true;
            ++execution_errors;
            std::cerr << "Query " << row << " execution error: " << error.what() << '\n';
            continue;
        }
        try {
            hnsw_test::check_results(index, store, queries[row], record.approximate, k);
            record.valid = true;
            ++successful_queries;
        } catch (const std::exception& error) {
            ++invalid_queries;
            std::cerr << "Query " << row << " invalid result: " << error.what() << '\n';
        }
        std::unordered_set<VectorId> expected;
        for (const auto& result : record.exact) expected.insert(result.id);
        std::unordered_set<VectorId> matched;
        for (const auto& result : record.approximate) {
            if (expected.contains(result.id)) matched.insert(result.id);
        }
        record.matches = matched.size();
        total_matches += record.matches;
    }
    graph_checkpoint("after_queries", index, store, graph);
    graph.close();
    check_store(store, data, vector_count);
    const double mean_recall = static_cast<double>(total_matches) / (query_count * k);
    const bool functional =
        successful_queries == query_count && invalid_queries == 0 && execution_errors == 0;
    const bool recall_passed = functional && mean_recall >= recall_threshold;

    auto storage = open_report(directory / "kr1_1_vectorstore_report.csv");
    storage
        << "expected_vectors,inserted_vectors,failed_insertions,stored_vectors,retrieved_vectors,"
           "retrieval_errors,altered_vectors,compared_components,dimension,recovery_percent,"
           "invalid_input_checks,metric,dataset_seed,status\n";
    storage << vector_count << ',' << vector_count << ",0," << store.size() << ',' << vector_count
            << ",0,0," << vector_count * dimension << ',' << dimension << ",100,8,cosine,"
            << dataset_seed << ",Cumplido\n";
    storage.close();
    auto functional_report = open_report(directory / "kr1_2_hnsw_functional_report.csv");
    functional_report
        << "indexed_nodes,failed_insertions,m,m0,ef_construction,ef_search,graph_seed,"
           "executed_queries,successful_queries,success_percent,invalid_result_queries,"
           "graph_violations,execution_errors,initial_nodes,added_nodes,retained_nodes,"
           "pre_increment_queries,maximum_level,metric,dataset_seed,query_seed,status\n";
    functional_report << index.size() << ",0," << config.m << ',' << config.m_max_0 << ','
                      << config.ef_construction << ',' << config.ef_search << ','
                      << config.random_seed << ',' << query_count << ',' << successful_queries
                      << ',' << 100.0 * successful_queries / query_count << ',' << invalid_queries
                      << ',' << violations(index.validate_graph()) << ',' << execution_errors
                      << ",500,500," << retained << ",5," << *index.max_level() << ",cosine,"
                      << dataset_seed << ',' << query_seed << ','
                      << (functional ? "Cumplido" : "No cumplido") << '\n';
    functional_report.close();
    auto recall = open_report(directory / "kr1_6_hnsw_recall_report.csv");
    recall
        << "query_id,exact_ids,hnsw_ids,matches,recall_at_10,mean_recall_at_10,threshold,k,metric,"
           "vectors,dimension,dataset_seed,query_seed,graph_seed,m,m0,ef_construction,ef_search,"
           "cutoff_tie,valid_result,execution_error,exact_ms,hnsw_ms,status\n";
    for (std::size_t row = 0; row < evidence.size(); ++row) {
        const auto& record = evidence[row];
        recall << row << ',';
        write_ids(recall, record.exact);
        recall << ',';
        write_ids(recall, record.approximate);
        recall << ',' << record.matches << ',' << static_cast<double>(record.matches) / k << ','
               << mean_recall << ',' << recall_threshold << ',' << k << ",cosine," << vector_count
               << ',' << dimension << ',' << dataset_seed << ',' << query_seed << ','
               << config.random_seed << ',' << config.m << ',' << config.m_max_0 << ','
               << config.ef_construction << ',' << config.ef_search << ',' << record.cutoff_tie
               << ',' << record.valid << ',' << record.execution_error << ',' << record.exact_ms
               << ',' << record.approximate_ms << ','
               << (recall_passed ? "Cumplido" : "No cumplido") << '\n';
    }
    recall.close();
    auto rankings = open_report(directory / "query_results.csv");
    rankings << "query_id,algorithm,rank,vector_id,distance_fp32,display_distance\n";
    for (std::size_t row = 0; row < evidence.size(); ++row) {
        for (const auto& [algorithm, results] : {std::pair{"brute-force", &evidence[row].exact},
                                                 std::pair{"hnsw", &evidence[row].approximate}}) {
            for (std::size_t rank = 0; rank < results->size(); ++rank) {
                const auto& result = (*results)[rank];
                rankings << row << ',' << algorithm << ',' << rank + 1 << ',' << result.id << ','
                         << std::defaultfloat << std::setprecision(17) << result.distance << ','
                         << std::fixed << std::setprecision(9) << result.distance << '\n';
            }
        }
    }
    rankings.close();
    auto protocol = open_report(directory / "protocol.json");
    protocol << "{\n  \"protocol_version\": 2,\n  \"generator\": "
                "\"tests/hnsw_test_helpers.hpp:embeddings\",\n"
                "  \"vectors\": 1000, \"dimension\": 128, \"queries\": 100, \"k\": 10,\n"
                "  \"metric\": \"cosine\", \"component_type\": \"IEEE-754 FP32\",\n"
                "  \"storage_representation\": \"L2 normalized FP32, zero-padded stride\",\n"
                "  \"neighbor_selection\": \"heuristic\",\n"
                "  \"dataset_seed\": 42, \"query_seed\": 20261014, \"graph_seed\": 42,\n"
                "  \"m\": 16, \"m0\": 32, \"ef_construction\": 100, \"ef_search\": 50, "
                "\"max_level_cap\": 32,\n"
                "  \"tie_policy\": \"ascending (FP32 distance, VectorId)\",\n"
                "  \"exact_distance_evaluations_derived\": 100000,\n"
                "  \"threshold\": 0.85, \"total_matches\": "
             << total_matches << ", \"mean_recall_at_10\": " << mean_recall
             << ",\n  \"cutoff_ties\": " << cutoff_ties << ", \"index_insert_ms\": " << build_ms
             << "\n}\n";
    protocol.close();
    std::cout
        << "OKR1 protocol v2: normalized cosine, N=1000, D=128, Q=100, k=10; seeds=42/20261014/42\n"
           "KR1.1: inserted=1000 retrieved=1000 altered=0 compared_components=128000; "
           "recovery=100%\n"
           "KR1.2: incremental=500+500 retained="
        << retained << " successful_queries=" << successful_queries
        << "/100 invalid_queries=" << invalid_queries << " execution_errors=" << execution_errors
        << " graph_violations=0\n"
        << "KR1.6: matches=" << total_matches << "/1000 meanRecall@10=" << mean_recall
        << " threshold=0.85 cutoff_ties=" << cutoff_ties << '\n';
    require(functional, "KR1.2 did not meet the functional acceptance criteria.");
    require(recall_passed, "KR1.6 mean Recall@10 did not reach 0.85.");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "Usage: aether_sprint1_okr1_tests <evidence-directory>");
        const std::filesystem::path directory(argv[1]);
        std::filesystem::create_directories(directory);
        validate(directory);
    } catch (const std::exception& error) {
        std::cerr << "OKR1 acceptance failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
