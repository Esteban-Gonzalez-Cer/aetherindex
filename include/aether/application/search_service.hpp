#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <vector>

#include "aether/ann/hnsw/hnsw_config.hpp"
#include "aether/core/search_result.hpp"

namespace aetherindex {

// Explicit selection: approximate search never falls back to exact search.
enum class SearchAlgorithm { BruteForce, Hnsw };

struct SearchRequest {
    std::filesystem::path dataset_path;
    std::filesystem::path query_path;
    SearchAlgorithm algorithm = SearchAlgorithm::BruteForce;
    DistanceKind metric = DistanceKind::Cosine;
    std::size_t k = 10;
    HnswConfig hnsw_config{};
};

struct SearchResponse {
    std::size_t vectors_indexed;
    std::size_t dimension;
    std::vector<SearchResult> neighbors;
    // Wall time around QueryEngine::search only; excludes CSV, graph build and presentation.
    std::chrono::duration<double, std::milli> query_time;
    // HNSW graph construction only; zero for brute force. No CSV or query time included.
    std::chrono::duration<double, std::milli> index_build_time{};
};

// A malformed or unreadable input file. Presentation layers choose how to report it.
class InputDataError : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};

// Throws invalid_argument before file access for an invalid request/configuration.
void validate_search_request(const SearchRequest& request);

// Loads, validates and searches within one call. No index or embedding survives the call.
// File parsing and storage orchestration live here so other interfaces can reuse the flow.
class SearchService {
   public:
    SearchResponse search(const SearchRequest& request) const;
};

}  // namespace aetherindex
