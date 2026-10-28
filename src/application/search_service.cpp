#include "aether/application/search_service.hpp"

#include <chrono>
#include <memory>
#include <utility>

#include "aether/ann/hnsw/hnsw_index.hpp"
#include "aether/application/json_loader.hpp"
#include "aether/application/query_engine.hpp"
#include "aether/lexical/inverted_index.hpp"
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

    // Both formats feed the existing vector search; hybrid orchestration is CP-04.
    const bool is_jsonl = request.dataset_path.extension() == ".jsonl";

    std::shared_ptr<VectorStore> store;
    std::vector<DocumentRecord> docs;

    if (is_jsonl) {
        // Carga de corpus JSONL (Vectores + Texto)
        auto [parsed_store, parsed_docs] =
            detail::load_jsonl_corpus(request.dataset_path.string(), request.metric);
        store = std::move(parsed_store);
        docs = std::move(parsed_docs);
    } else {
        store = detail::load_csv_dataset(request.dataset_path, request.metric);
    }

    // Load the query before the search timer, as in the existing CSV contract.
    std::vector<EmbeddingValue> query;
    if (is_jsonl) {
        auto queries = detail::load_jsonl_queries(request.query_path.string());
        if (queries.empty() || queries.front().vector.empty()) {
            throw InputDataError("JSONL vector search requires a query with a nonempty 'vector'.");
        }
        query = std::move(queries.front().vector);
    } else {
        query = detail::load_csv_query(request.query_path, store->dimension(), request.metric);
    }

    // 2. Construcción e Indexación HNSW
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

    // Keep the existing lexical preparation on its current API; no fusion is run here.
    lexical::InvertedIndex inverted_index;
    if (is_jsonl && !docs.empty()) {
        for (size_t i = 0; i < docs.size(); ++i) {
            inverted_index.add_document(static_cast<DocumentId>(i), docs[i].text);
        }
        inverted_index.build();
    }

    // 4. Ejecución de la búsqueda
    const auto start = std::chrono::steady_clock::now();

    // Both formats keep the selected vector algorithm. Hybrid orchestration is CP-04.
    const QueryEngine engine;
    auto neighbors =
        index ? engine.search(*index, query, request.k) : engine.search(*store, query, request.k);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    return {store->size(), store->dimension(), std::move(neighbors), elapsed, build_time};
}

}  // namespace aetherindex
