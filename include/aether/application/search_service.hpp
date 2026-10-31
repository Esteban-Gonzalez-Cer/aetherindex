#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "aether/ann/hnsw/hnsw_config.hpp"
#include "aether/application/filter.hpp"
#include "aether/core/search_result.hpp"
#include "aether/hybrid/rrf_config.hpp"
#include "aether/lexical/bm25_config.hpp"

namespace aetherindex {

// Explicit selection: approximate search never falls back to exact search.
enum class SearchAlgorithm { BruteForce, Hnsw };
// Hybrid always uses HNSW for the dense channel, BM25 for the lexical channel and RRF.
enum class SearchMode { Vector, Lexical, Hybrid };
// Compressed HNSW traversal (vector HNSW or hybrid dense channel).
enum class Quantization { None, Sq8, Pq };

// Hybrid default depth per retriever when candidate_k is not set: max(k, 50).
inline constexpr std::size_t kDefaultHybridCandidateK = 50;

struct SearchRequest {
    std::filesystem::path dataset_path;
    std::filesystem::path query_path;
    SearchAlgorithm algorithm = SearchAlgorithm::BruteForce;
    DistanceKind metric = DistanceKind::Cosine;
    std::size_t k = 10;
    HnswConfig hnsw_config{};
    SearchMode mode = SearchMode::Vector;
    std::optional<std::string> query_text{};
    std::optional<std::string> query_id{};
    lexical::Bm25Config bm25_config{};
    // Hybrid only: depth requested from HNSW and BM25 (>= k) and the fusion (default:
    // convex, alpha = hybrid::kTunedConvexAlpha).
    std::optional<std::size_t> candidate_k{};
    hybrid::RrfConfig rrf_config = hybrid::kDefaultSearchFusion;
    // Hybrid only: binary .avec embeddings for the JSONL documents (IDs = DocumentIds).
    // When set, document embeddings are read from this file instead of the JSONL text.
    std::filesystem::path vectors_path{};
    // Run every query of a JSONL --query file against one index build (batch mode).
    bool all_queries = false;
    // HNSW construction threads. 1 keeps the reproducible sequential graph; more threads
    // build faster but the topology depends on scheduling.
    std::size_t build_threads = 1;
    // Threads for independent batch queries (all_queries); results keep file order.
    std::size_t query_threads = 1;
    // Hybrid only: run BM25 concurrently with HNSW inside each query.
    bool parallel_retrievers = false;
    // HNSW traversal on SQ8/PQ codes, then an exact FP32 re-rank of `rerank` candidates
    // (default: all candidates; 0 disables). drop_fp32 frees the FP32 rows after the
    // build and implies rerank 0. pq_subspaces applies to PQ (0 = automatic).
    Quantization quantization = Quantization::None;
    std::optional<std::size_t> rerank{};
    bool drop_fp32 = false;
    std::size_t pq_subspaces = 0;
    // Metadata filters (AND). Require JSONL documents; an unknown field is input error.
    std::vector<FilterClause> filters{};
};

// Dense channel under a filter: exact search over the matching vectors when they are
// few (< kExactFilterMinimum or < 1/kExactFilterDivisor of the corpus), else filtered
// HNSW traversal.
inline constexpr std::size_t kExactFilterMinimum = 2000;
inline constexpr std::size_t kExactFilterDivisor = 20;

// Converts CSV or JSONL embeddings to the binary .avec format, preserving raw values.
// JSONL vectors are written under their DocumentIds; documents_output optionally writes
// the same documents (document_id, title, text) without embeddings for hybrid search.
struct ConvertRequest {
    std::filesystem::path dataset_path;
    std::filesystem::path output_path;
    std::filesystem::path documents_output{};
};

struct ConvertResponse {
    std::size_t vectors = 0;
    std::size_t dimension = 0;
    std::size_t documents_written = 0;
};

// Throws invalid_argument before file access for bad paths/combinations.
void validate_convert_request(const ConvertRequest& request);

// Validates first, then throws InputDataError for invalid input data and runtime_error
// when an output file cannot be written.
ConvertResponse convert_vector_file(const ConvertRequest& request);

// Effective hybrid candidate depth: candidate_k, or max(k, kDefaultHybridCandidateK).
std::size_t effective_candidate_k(const SearchRequest& request) noexcept;

struct DocumentSearchResult {
    DocumentId id;
    std::string source_id;
    double score;
};

// One fused hybrid document. Ranks are 1-based positions in each component ranking;
// zero means the document was absent from that retriever's candidates.
struct HybridDocumentResult {
    DocumentId id;
    std::string source_id;
    double rrf_score;
    std::size_t dense_rank = 0;
    std::size_t lexical_rank = 0;
};

struct SearchResponse {
    std::size_t vectors_indexed = 0;
    std::size_t dimension = 0;
    std::vector<SearchResult> neighbors;
    // Wall time around QueryEngine::search only; excludes input loading, index build and output.
    std::chrono::duration<double, std::milli> query_time;
    // HNSW graph or complete BM25 indexing; zero for brute force. No input loading/query time.
    std::chrono::duration<double, std::milli> index_build_time{};
    std::size_t documents_indexed = 0;
    std::vector<DocumentSearchResult> documents;
    std::string selected_query_id;
    // JSONL vector search: source document ID for each neighbor, in the same order.
    std::vector<std::string> neighbor_source_ids;
    // Hybrid only. index_build_time holds the HNSW build; lexical build is separate.
    std::chrono::duration<double, std::milli> lexical_build_time{};
    std::size_t candidate_k = 0;
    std::size_t dense_candidates = 0;
    std::size_t lexical_candidates = 0;
    std::size_t fused_candidates = 0;  // Size of the union before Top-K truncation.
    std::vector<HybridDocumentResult> fused;
    // Batch mode: one entry per query (per-query fields and query_time), in file order.
    // Shared fields (counts, build times) stay on this top-level response.
    std::vector<SearchResponse> batch;
    std::chrono::duration<double, std::milli> batch_time{};  // Wall time of all queries.
    // Quantization (training + encoding time, memory of FP32 rows and of the codes).
    std::chrono::duration<double, std::milli> quantization_time{};
    std::size_t fp32_bytes = 0;
    std::size_t code_bytes = 0;
    bool fp32_released = false;
    std::size_t rerank = 0;  // Effective re-rank depth (SIZE_MAX = all candidates).
    // Filtering: documents matching every clause, out of the corpus, and dense strategy.
    bool filtered = false;
    std::size_t filter_matches = 0;
    std::size_t filter_population = 0;
    bool exact_filter = false;
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
