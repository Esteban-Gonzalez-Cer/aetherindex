#include "aether/application/search_service.hpp"

#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "../test_helpers.hpp"

using namespace aetherindex;

void write_file(const std::filesystem::path& path, std::string_view text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
    require(static_cast<bool>(file), "Could not write a service test fixture.");
}

template <typename Function>
void requireInputError(Function function, std::string_view message_fragment) {
    try {
        function();
    } catch (const InputDataError& error) {
        if (std::string_view(error.what()).find(message_fragment) == std::string_view::npos) {
            throw std::runtime_error("Expected diagnostic '" + std::string(message_fragment) +
                                     "', received: " + error.what());
        }
        return;
    }
    throw std::runtime_error("Expected an input validation error.");
}

void checkValidSearches(const std::filesystem::path& root, SearchAlgorithm algorithm) {
    const auto dataset = root / "dataset with spaces.csv";
    const auto query = root / "query with spaces.csv";
    write_file(dataset, "\r\n 10, +1.0, 0.0 \r\n20,0,1\r\n30,-1,0\r\n40,8e-1,6e-1\r\n\t\r\n");
    write_file(query, "\n\t 1.0 , 0 \r\n\n");
    SearchRequest request{dataset, query};
    request.algorithm = algorithm;
    const SearchService service;
    request.k = 2;
    const auto response = service.search(request);
    require(
        response.vectors_indexed == 4 && response.dimension == 2 && response.neighbors.size() == 2,
        "Dataset metadata and requested count must be preserved.");
    require(response.neighbors[0].id == 10 && response.neighbors[1].id == 40,
            "The card's reference cosine ranking must be reproduced.");
    requireNear(response.neighbors[0].distance, 0, "Reference cosine distance must be zero.");
    requireNear(response.neighbors[1].distance, 0.2f, "Reference cosine distance must be 0.2.");
    require(std::isfinite(response.query_time.count()) && response.query_time.count() >= 0,
            "Query timing must be finite, nonnegative and expressed in milliseconds.");
    require(
        std::isfinite(response.index_build_time.count()) && response.index_build_time.count() >= 0,
        "Graph build timing must be finite, nonnegative and expressed in milliseconds.");
    if (algorithm == SearchAlgorithm::BruteForce) {
        require(response.index_build_time.count() == 0,
                "Brute force must not report time spent constructing an HNSW graph.");
    }
    const auto repeated = service.search(request);
    require(response.neighbors.size() == repeated.neighbors.size(),
            "Repeated same-seed requests must return the same number of neighbors.");
    for (std::size_t rank = 0; rank < response.neighbors.size(); ++rank) {
        require(response.neighbors[rank].id == repeated.neighbors[rank].id &&
                    response.neighbors[rank].distance == repeated.neighbors[rank].distance,
                "Same-seed builds and queries must reproduce the result table.");
    }
    if (algorithm == SearchAlgorithm::Hnsw) {
        request.hnsw_config.m = 2;
        request.hnsw_config.m_max_0 = 4;
        request.hnsw_config.ef_construction = 10;
        request.hnsw_config.ef_search = 1;
        request.hnsw_config.random_seed = 0;
    }
    for (const auto k : {std::size_t{1}, std::size_t{5}, std::size_t{10}, std::size_t{20},
                         std::numeric_limits<std::size_t>::max()}) {
        request.k = k;
        require(service.search(request).neighbors.size() == (k < 4 ? k : 4),
                "Search must return min(k, N).");
    }
    write_file(dataset, "0,10,0\n4294967295,1,1\n");
    request.k = 2;
    const auto cosine = service.search(request);
    request.metric = DistanceKind::SquaredEuclidean;
    const auto squared = service.search(request);
    require(cosine.neighbors.front().id == 0 &&
                squared.neighbors.front().id == std::numeric_limits<VectorId>::max(),
            "Both metrics must be forwarded to storage and search; full ID range is valid.");
    requireNear(squared.neighbors[0].distance, 1, "Squared Euclidean best distance is incorrect.");
    requireNear(squared.neighbors[1].distance, 81,
                "Squared Euclidean farthest distance is incorrect.");
    write_file(dataset, "0,0,0\n1,1,1\n");
    write_file(query, "0,0\n");
    require(service.search(request).neighbors.front().id == 0,
            "Squared Euclidean must accept zero dataset vectors and queries.");
}

void checkInvalidFiles(const std::filesystem::path& root, SearchAlgorithm algorithm) {
    SearchRequest request{root / "invalid.csv", root / "query.csv"};
    request.algorithm = algorithm;
    const SearchService service;
    write_file(request.query_path, "1,0\n");
    const std::pair<std::string_view, std::string_view> invalid_datasets[] = {
        {"", "empty"},
        {" \t\r\n\n", "empty"},
        {"id,x,y\n", "column 1"},
        {"-1,1,0\n", "column 1"},
        {"4294967296,1,0\n", "column 1"},
        {"1x,1,0\n", "column 1"},
        {",1,0\n", "column 1"},
        {"1\n", "at least one component"},
        {"1,,0\n", "column 2"},
        {"1,1,\n", "column 3"},
        {"1,one,0\n", "column 2"},
        {"1,1x,0\n", "column 2"},
        {"1,\"1\",0\n", "column 2"},
        {"1,nan,0\n", "finite float"},
        {"1,inf,0\n", "finite float"},
        {"1,-inf,0\n", "finite float"},
        {"1,1e100,0\n", "finite float"},
        {"1,1,0\n\n1,0,1\n", "line 3: duplicate vector ID"},
        {"1,1,0\n2,1\n", "line 2: expected dimension"},
        {"1,1,0\n2,1,0,0\n", "line 2: expected dimension"},
        {"1,1,0\n2,0,0\n", "all-zero"},
    };
    for (const auto& [text, diagnostic] : invalid_datasets) {
        write_file(request.dataset_path, text);
        requireInputError([&] { service.search(request); }, diagnostic);
    }
    write_file(request.dataset_path, "1,1,0\n");
    const std::pair<std::string_view, std::string_view> invalid_queries[] = {
        {"", "empty"},
        {" \t\r\n", "empty"},
        {"1\n", "dimension mismatch"},
        {"1,0,0\n", "dimension mismatch"},
        {"1,0\n0,1\n", "exactly one"},
        {"1,\n", "column 2"},
        {"hello,0\n", "finite float"},
        {"nan,0\n", "finite float"},
        {"inf,0\n", "finite float"},
        {"0,0\n", "all-zero query"},
    };
    for (const auto& [text, diagnostic] : invalid_queries) {
        write_file(request.query_path, text);
        requireInputError([&] { service.search(request); }, diagnostic);
    }
    write_file(request.query_path, "1,0\n");
    auto missing = request;
    missing.dataset_path = root / "missing dataset.csv";
    requireInputError([&] { service.search(missing); }, "Cannot open");
    missing = request;
    missing.query_path = root / "missing query.csv";
    requireInputError([&] { service.search(missing); }, "Cannot open");
    missing.dataset_path = root;
    requireInputError([&] { service.search(missing); }, "input file");
    require(service.search(request).neighbors.size() == 1,
            "A failed load must not affect a subsequent valid operation.");
}

void checkInvalidRequests() {
    const SearchService service;
    SearchRequest request{"not-opened.csv", "not-opened-query.csv"};
    request.algorithm = SearchAlgorithm::Hnsw;
    for (std::size_t invalid_m : {0u, 1u}) {
        request.hnsw_config.m = invalid_m;
        requireInvalidArgument([&] { service.search(request); },
                               "Invalid HNSW M must fail before any file access.");
    }
    request.hnsw_config = HnswConfig{};
    request.hnsw_config.m_max_0 = 15;
    requireInvalidArgument([&] { service.search(request); },
                           "Invalid HNSW M0 must fail before any file access.");
    request.hnsw_config = HnswConfig{};
    request.hnsw_config.ef_construction = 15;
    requireInvalidArgument([&] { service.search(request); },
                           "Invalid efConstruction must fail before any file access.");
    request.hnsw_config = HnswConfig{};
    request.hnsw_config.ef_search = 0;
    requireInvalidArgument([&] { service.search(request); },
                           "Invalid efSearch must fail before any file access.");
    request.hnsw_config = HnswConfig{};
    request.hnsw_config.max_level = 33;
    requireInvalidArgument([&] { service.search(request); },
                           "Invalid level caps must fail before any file access.");
    request.hnsw_config = HnswConfig{};
    request.algorithm = static_cast<SearchAlgorithm>(99);
    requireInvalidArgument([&] { service.search(request); },
                           "Unknown enum values must be rejected.");
    request.algorithm = SearchAlgorithm::BruteForce;
    request.k = 0;
    requireInvalidArgument([&] { service.search(request); },
                           "Application must enforce positive k.");
    request.k = 10;
    request.metric = static_cast<DistanceKind>(99);
    requireInvalidArgument([&] { service.search(request); },
                           "Invalid metrics must fail before I/O.");
    request.metric = DistanceKind::Cosine;
    request.dataset_path.clear();
    requireInvalidArgument([&] { service.search(request); },
                           "Empty dataset paths must be rejected.");
    request.dataset_path = "data";
    request.query_path.clear();
    requireInvalidArgument([&] { service.search(request); }, "Empty query paths must be rejected.");
}

int main(int argc, char* argv[]) {
    try {
        require(argc == 2, "Expected an isolated fixture directory.");
        const std::filesystem::path root(argv[1]);
        std::filesystem::create_directories(root);
        for (SearchAlgorithm algorithm : {SearchAlgorithm::BruteForce, SearchAlgorithm::Hnsw}) {
            checkValidSearches(root, algorithm);
            checkInvalidFiles(root, algorithm);
        }
        checkInvalidRequests();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
