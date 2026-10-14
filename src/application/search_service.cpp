#include "aether/application/search_service.hpp"

#include <memory>
#include <utility>

#include "aether/ann/hnsw/hnsw_index.hpp"
#include "aether/application/query_engine.hpp"
#include "csv_loader.hpp"

namespace aetherindex {

void validate_search_request(const SearchRequest& request) {
    if (request.dataset_path.empty() || request.query_path.empty()) {
        throw std::invalid_argument("Both dataset and query paths are required.");
    }
    if (request.k == 0) {
        throw std::invalid_argument(
            "Invalid value for --k: the number of neighbors must be greater than zero.");
    }
    if (request.metric != DistanceKind::Cosine &&
        request.metric != DistanceKind::SquaredEuclidean) {
        throw std::invalid_argument("Unsupported distance metric.");
    }
    switch (request.algorithm) {
        case SearchAlgorithm::BruteForce:
            return;
        case SearchAlgorithm::Hnsw:
            validate_hnsw_config(request.hnsw_config);
            return;
    }
    throw std::invalid_argument("Unsupported search algorithm.");
}

SearchResponse SearchService::search(const SearchRequest& request) const {
    validate_search_request(request);
    const auto store = detail::load_csv_dataset(request.dataset_path, request.metric);
    const auto query =
        detail::load_csv_query(request.query_path, store->dimension(), request.metric);
    const QueryEngine engine;
    std::unique_ptr<HnswIndex> index;
    std::chrono::duration<double, std::milli> build_time{};
    if (request.algorithm == SearchAlgorithm::Hnsw) {
        const auto build_start = std::chrono::steady_clock::now();
        index = std::make_unique<HnswIndex>(*store, request.hnsw_config);
        for (VectorId id : store->ids()) {
            index->insert(id);
        }
        build_time = std::chrono::steady_clock::now() - build_start;
    }
    const auto start = std::chrono::steady_clock::now();
    auto neighbors =
        index ? engine.search(*index, query, request.k) : engine.search(*store, query, request.k);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return {store->size(), store->dimension(), std::move(neighbors), elapsed, build_time};
}

}  // namespace aetherindex
