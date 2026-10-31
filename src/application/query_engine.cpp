#include "aether/application/query_engine.hpp"

#include <future>
#include <stdexcept>

#include "aether/ann/brute_force/brute_force_index.hpp"
#include "aether/ann/hnsw/hnsw_index.hpp"
#include "aether/hybrid/rrf.hpp"
#include "aether/lexical/bm25.hpp"

namespace aetherindex {

std::vector<SearchResult> QueryEngine::search(const VectorStore& database,
                                              std::span<const EmbeddingValue> query,
                                              std::size_t k) const {
    if (k == 0 || database.size() == 0) return {};
    const PreparedQuery prepared(query, database.dimension(), database.distance_kind());
    return bruteForceSearchPrepared(database, prepared, k);
}

std::vector<SearchResult> QueryEngine::search(const HnswIndex& index,
                                              std::span<const EmbeddingValue> query,
                                              std::size_t k) const {
    const PreparedQuery prepared(query, index.dimension(), index.distance_kind());
    return index.search_prepared(prepared, k, index.config().ef_search);
}

std::vector<SearchResult> QueryEngine::search(const HnswIndex& index, const QuantizedVectors& codes,
                                              std::span<const EmbeddingValue> query, std::size_t k,
                                              std::size_t rerank) const {
    const PreparedQuery prepared(query, index.dimension(), index.distance_kind());
    return index.search_quantized(prepared, codes, k, index.config().ef_search, rerank);
}

std::vector<RankedResult> QueryEngine::search(const lexical::Bm25Index& index,
                                              std::string_view query, std::size_t k) const {
    return index.search(query, k);
}

std::vector<SearchResult> QueryEngine::search(const HnswIndex& index,
                                              std::span<const EmbeddingValue> query, std::size_t k,
                                              const SearchFilter& filter,
                                              const QuantizedVectors* codes,
                                              std::size_t rerank) const {
    const PreparedQuery prepared(query, index.dimension(), index.distance_kind());
    if (filter.exact_vectors && index.store().has_rows())
        return bruteForceSearchSubset(index.store(), prepared, k, *filter.exact_vectors);
    return index.search_filtered(prepared, k, index.config().ef_search, filter.allow_vector, codes,
                                 rerank);
}

std::vector<SearchResult> QueryEngine::search(const VectorStore& database,
                                              std::span<const EmbeddingValue> query, std::size_t k,
                                              const SearchFilter& filter) const {
    if (!filter.exact_vectors)
        throw std::invalid_argument("Filtered exact search needs the matching vector list.");
    if (k == 0 || database.size() == 0) return {};
    const PreparedQuery prepared(query, database.dimension(), database.distance_kind());
    return bruteForceSearchSubset(database, prepared, k, *filter.exact_vectors);
}

std::vector<RankedResult> QueryEngine::search(const lexical::Bm25Index& index,
                                              std::string_view query, std::size_t k,
                                              const SearchFilter& filter) const {
    return index.search(query, k, filter.allow_document);
}

HybridRankings QueryEngine::search(const HnswIndex& dense_index,
                                   const VectorDocumentBinding& binding,
                                   const lexical::Bm25Index& lexical_index,
                                   const hybrid::RrfFusion& fusion,
                                   const HybridQuery& query) const {
    if (query.k == 0) throw std::invalid_argument("Hybrid search requires k > 0.");
    if (query.candidate_k < query.k)
        throw std::invalid_argument("Hybrid search requires candidate_k >= k.");
    HybridRankings rankings;
    // Both retrievers only read immutable indexes, so they may overlap. The results and
    // their order are identical either way; only wall time can differ.
    std::future<std::vector<RankedResult>> lexical_future;
    const auto lexical_search = [&] {
        return query.filter ? search(lexical_index, query.text, query.candidate_k, *query.filter)
                            : search(lexical_index, query.text, query.candidate_k);
    };
    if (query.parallel_retrievers) lexical_future = std::async(std::launch::async, lexical_search);
    const auto dense = query.filter      ? search(dense_index, query.embedding, query.candidate_k,
                                                  *query.filter, query.quantized, query.rerank)
                       : query.quantized ? search(dense_index, *query.quantized, query.embedding,
                                                  query.candidate_k, query.rerank)
                                         : search(dense_index, query.embedding, query.candidate_k);
    // Dense similarity for fusion: 1 - distance (higher is better); RRF uses only the order.
    std::vector<RankedResult> dense_scored;
    dense_scored.reserve(dense.size());
    rankings.dense.reserve(dense.size());
    for (const auto& hit : dense) {
        const auto found = binding.find(hit.id);
        if (found == binding.end())
            throw std::logic_error("HNSW returned VectorId " + std::to_string(hit.id) +
                                   " without a DocumentId binding.");
        rankings.dense.push_back({found->second, hit.distance});
        dense_scored.push_back({found->second, 1.0 - static_cast<double>(hit.distance)});
    }
    rankings.lexical = query.parallel_retrievers ? lexical_future.get() : lexical_search();
    // Fusion rejects a DocumentId repeated within one ranking, which also exposes a
    // non-injective binding instead of silently merging two vectors.
    rankings.fused = fusion.fuse_scored(dense_scored, rankings.lexical, query.k);
    return rankings;
}

}  // namespace aetherindex
