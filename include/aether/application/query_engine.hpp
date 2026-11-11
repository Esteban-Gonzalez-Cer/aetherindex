#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "aether/core/search_result.hpp"
#include "aether/storage/vector_store.hpp"

namespace aetherindex {

class HnswIndex;
namespace lexical {
class LexicalSearcher;
}
namespace hybrid {
class RrfFusion;
}
class QuantizedVectors;

// Explicit dense-to-document association. Every VectorId that HNSW can return must
// resolve here before fusion; numeric equality of the two IDs is never assumed.
using VectorDocumentBinding = std::unordered_map<VectorId, DocumentId>;

// Restricts results to matching documents. allow_document filters BM25; allow_vector
// filters HNSW traversal; exact_vectors, when set (and FP32 rows exist), switches the
// dense channel to exact search over exactly those vectors (low selectivity).
struct SearchFilter {
    std::function<bool(DocumentId)> allow_document;
    std::function<bool(VectorId)> allow_vector;
    const std::vector<VectorId>* exact_vectors = nullptr;
};

struct HybridQuery {
    std::string_view text;                        // BM25 input, analysed by the index tokenizer.
    std::span<const EmbeddingValue> embedding;    // HNSW input, computed outside the engine.
    std::size_t k = 10;                           // Final fused Top-K.
    std::size_t candidate_k = 50;                 // Depth requested from each retriever; >= k.
    bool parallel_retrievers = false;             // Run BM25 on a second thread during HNSW.
    const QuantizedVectors* quantized = nullptr;  // Optional compressed dense traversal.
    std::size_t rerank = 0;                       // With quantized: exact FP32 re-rank depth.
    const SearchFilter* filter = nullptr;         // Applied to both retrievers before RRF.
};

// Dense hit after VectorId -> DocumentId resolution; smaller distance is better.
struct DenseDocumentHit {
    DocumentId document_id;
    float distance;
};

// Component rankings are kept in their own relevance order so callers can audit fusion.
struct HybridRankings {
    std::vector<DenseDocumentHit> dense;  // HNSW order: increasing distance.
    std::vector<RankedResult> lexical;    // BM25 order: decreasing score; may be empty.
    std::vector<RankedResult> fused;      // RRF order: decreasing score, then DocumentId.
};

// Application entry point for exact, approximate and prepared lexical indexes.
// The caller owns the VectorStore; the engine neither stores nor copies embeddings.
class QueryEngine {
   public:
    std::vector<SearchResult> search(const VectorStore& database,
                                     std::span<const EmbeddingValue> query, std::size_t k) const;
    std::vector<SearchResult> search(const HnswIndex& index, std::span<const EmbeddingValue> query,
                                     std::size_t k) const;
    std::vector<RankedResult> search(const lexical::LexicalSearcher& index, std::string_view query,
                                     std::size_t k) const;
    // HNSW traversal on compressed codes; see HnswIndex::search_quantized for rerank.
    std::vector<SearchResult> search(const HnswIndex& index, const QuantizedVectors& codes,
                                     std::span<const EmbeddingValue> query, std::size_t k,
                                     std::size_t rerank) const;
    // Filtered variants (see SearchFilter). Brute force requires filter.exact_vectors.
    std::vector<SearchResult> search(const HnswIndex& index, std::span<const EmbeddingValue> query,
                                     std::size_t k, const SearchFilter& filter,
                                     const QuantizedVectors* codes = nullptr,
                                     std::size_t rerank = 0) const;
    std::vector<SearchResult> search(const VectorStore& database,
                                     std::span<const EmbeddingValue> query, std::size_t k,
                                     const SearchFilter& filter) const;
    std::vector<RankedResult> search(const lexical::LexicalSearcher& index, std::string_view query,
                                     std::size_t k, const SearchFilter& filter) const;

    // Hybrid orchestration: HNSW and BM25 each return candidate_k candidates, dense
    // VectorIds are resolved through binding, and RRF fuses both DocumentId rankings.
    // Throws invalid_argument for k=0, candidate_k<k or a wrong embedding dimension,
    // and logic_error if HNSW returns a VectorId without a document binding. An empty
    // BM25 ranking is valid: the fused result then contains the dense candidates only.
    HybridRankings search(const HnswIndex& dense_index, const VectorDocumentBinding& binding,
                          const lexical::LexicalSearcher& lexical_index, const hybrid::RrfFusion& fusion,
                          const HybridQuery& query) const;
};

}  // namespace aetherindex
