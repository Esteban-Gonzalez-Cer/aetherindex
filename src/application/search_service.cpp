#include "aether/application/search_service.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "aether/ann/hnsw/hnsw_index.hpp"
#include "aether/application/json_loader.hpp"
#include "aether/application/query_engine.hpp"
#include "aether/core/parallel.hpp"
#include "aether/hybrid/rrf.hpp"
#include "aether/lexical/bm25.hpp"
#include "aether/lexical/tokenizer.hpp"
#include "aether/quantization/pq.hpp"
#include "aether/quantization/sq8.hpp"
#include "aether/storage/vector_file.hpp"
#include "csv_loader.hpp"

namespace aetherindex {
namespace {

std::string lower_extension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    for (auto& c : extension)
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return extension;
}

bool jsonl_path(const std::filesystem::path& path) { return lower_extension(path) == ".jsonl"; }
bool avec_path(const std::filesystem::path& path) { return lower_extension(path) == ".avec"; }

// Storage-level file errors are input-data errors at the application boundary.
std::unique_ptr<VectorStore> load_vector_file(const std::filesystem::path& path,
                                              DistanceKind metric) {
    try {
        return read_vector_file(path, metric);
    } catch (const VectorFileError& error) {
        throw InputDataError(error.what());
    }
}

// One selected query (by --query-id, or the only record) or, with all_queries, every
// record in file order. Never silently picks the first of several queries.
std::vector<QueryRecord> load_queries(const SearchRequest& request, bool vectors) {
    auto queries = detail::load_jsonl_queries(request.query_path.string(), vectors);
    const auto prefix = request.query_path.string() + ": ";
    if (request.all_queries) {
        if (queries.empty()) throw InputDataError(prefix + "the query file has no records.");
        return queries;
    }
    if (request.query_id) {
        const auto found = std::find_if(queries.begin(), queries.end(), [&](const auto& query) {
            return query.id == *request.query_id;
        });
        if (found == queries.end())
            throw InputDataError(prefix + "query ID not found: " + *request.query_id);
        return {std::move(*found)};
    }
    if (queries.size() != 1)
        throw InputDataError(
            prefix +
            "expected exactly one query; use --query-id to select from a multi-query file.");
    return queries;
}

void require_query_text(const SearchRequest& request, const QueryRecord& query, const char* mode) {
    if (!query.has_text)
        throw InputDataError(request.query_path.string() + ": " + mode +
                             " requires 'query_text', 'query' or 'text'" +
                             (query.id.empty() ? std::string(".") : " (query " + query.id + ")."));
}

void require_query_embedding(const SearchRequest& request, const std::vector<float>& query,
                             const VectorStore& store) {
    const auto prefix = request.query_path.string() + ": ";
    if (query.size() != store.dimension())
        throw InputDataError(prefix + "query dimension mismatch or missing embedding.");
    if (request.metric == DistanceKind::Cosine &&
        std::all_of(query.begin(), query.end(), [](float v) { return v == 0; }))
        throw InputDataError(prefix + "cosine metric does not accept an all-zero query.");
}

// Runs execute(query, response) for the selected query, or for every query in parallel
// when all_queries is set. Each execute call measures its own query_time; the indexes it
// reads are immutable at this point, so concurrent calls are safe.
template <class Execute>
SearchResponse run_queries(const SearchRequest& request, std::vector<QueryRecord>& queries,
                           SearchResponse response, Execute execute) {
    if (!request.all_queries) {
        execute(queries.front(), response);
        response.selected_query_id = std::move(queries.front().id);
        return response;
    }
    response.batch.resize(queries.size());
    const auto start = std::chrono::steady_clock::now();
    parallel_for(queries.size(), request.query_threads, [&](std::size_t i) {
        execute(queries[i], response.batch[i]);
        response.batch[i].selected_query_id = queries[i].id;
    });
    response.batch_time = std::chrono::steady_clock::now() - start;
    return response;
}

// Matching documents/vectors for the request's filters, evaluated once per request.
struct FilterState {
    std::unordered_set<DocumentId> documents;
    std::unordered_set<VectorId> vector_set;
    std::vector<VectorId> vectors;
    SearchFilter filter;
};

// rows_available: FP32 rows exist for exact search; force_exact: brute-force mode.
std::unique_ptr<FilterState> build_filter(const SearchRequest& request,
                                          const std::vector<DocumentRecord>& docs,
                                          bool rows_available, bool force_exact,
                                          SearchResponse& response) {
    if (request.filters.empty()) return nullptr;
    for (const auto& clause : request.filters) {
        const bool known = std::any_of(docs.begin(), docs.end(), [&](const DocumentRecord& doc) {
            return std::any_of(doc.metadata.begin(), doc.metadata.end(),
                               [&](const auto& entry) { return entry.first == clause.field; });
        });
        if (!known)
            throw InputDataError(request.dataset_path.string() + ": unknown metadata field '" +
                                 clause.field + "'.");
    }
    auto state = std::make_unique<FilterState>();
    for (const auto& doc : docs) {
        if (!matches_filters(doc.metadata, request.filters)) continue;
        state->documents.insert(doc.document_id);
        state->vector_set.insert(doc.vector_id);
        state->vectors.push_back(doc.vector_id);
    }
    const std::size_t matches = state->documents.size();
    const bool exact =
        force_exact || (rows_available && (matches < kExactFilterMinimum ||
                                           matches * kExactFilterDivisor < docs.size()));
    const auto* raw = state.get();
    state->filter.allow_document = [raw](DocumentId id) { return raw->documents.contains(id); };
    state->filter.allow_vector = [raw](VectorId id) { return raw->vector_set.contains(id); };
    state->filter.exact_vectors = exact ? &state->vectors : nullptr;
    response.filtered = true;
    response.filter_matches = matches;
    response.filter_population = docs.size();
    response.exact_filter = exact;
    return state;
}

SearchResponse lexical_search(const SearchRequest& request) {
    auto docs = detail::load_jsonl_documents(request.dataset_path);
    std::vector<QueryRecord> queries;
    if (request.query_text) {
        queries.emplace_back().text = *request.query_text;
    } else {
        queries = load_queries(request, false);
        for (const auto& query : queries) require_query_text(request, query, "selected query");
    }
    std::unordered_map<DocumentId, std::string_view> identities;
    for (const auto& doc : docs) identities.emplace(doc.document_id, doc.id);
    lexical::Bm25Index index(request.bm25_config);
    const auto build_start = std::chrono::steady_clock::now();
    for (const auto& doc : docs) index.add_document(doc.document_id, doc.title + "\n" + doc.text);
    index.build();
    SearchResponse base;
    base.index_build_time = std::chrono::steady_clock::now() - build_start;
    base.documents_indexed = docs.size();
    const auto filter = build_filter(request, docs, false, false, base);
    if (filter) base.exact_filter = false;  // No dense channel in lexical mode.
    const QueryEngine engine;
    return run_queries(
        request, queries, std::move(base), [&](const QueryRecord& query, SearchResponse& out) {
            const auto start = std::chrono::steady_clock::now();
            const auto ranked = filter ? engine.search(index, query.text, request.k, filter->filter)
                                       : engine.search(index, query.text, request.k);
            out.query_time = std::chrono::steady_clock::now() - start;
            out.documents.reserve(ranked.size());
            for (const auto& row : ranked)
                out.documents.push_back(
                    {row.document_id, std::string(identities.at(row.document_id)), row.score});
        });
}

void build_hnsw(HnswIndex& index, const VectorStore& store, std::size_t threads) {
    index.insert_parallel(store.ids(), threads);
}

std::unique_ptr<QuantizedVectors> make_codes(const SearchRequest& request,
                                             const VectorStore& store) {
    switch (request.quantization) {
        case Quantization::Sq8:
            return std::make_unique<Sq8Vectors>(store);
        case Quantization::Pq: {
            // Training uses every core; codebooks do not depend on the thread count.
            PqConfig config;
            config.subspaces = request.pq_subspaces;
            if (config.subspaces && store.dimension() % config.subspaces != 0)
                throw InputDataError("--pq-subspaces " + std::to_string(config.subspaces) +
                                     " does not divide the vector dimension " +
                                     std::to_string(store.dimension()) + ".");
            return std::make_unique<PqVectors>(store, config);
        }
        default:
            throw std::invalid_argument("Unsupported quantization.");
    }
}

// Builds the requested codes after the graph (which always uses FP32 rows) and, if
// asked, releases the FP32 rows. Fills the quantization fields of the response.
std::unique_ptr<QuantizedVectors> build_codes(const SearchRequest& request, VectorStore& store,
                                              SearchResponse& response) {
    if (request.quantization == Quantization::None) return nullptr;
    const auto start = std::chrono::steady_clock::now();
    std::unique_ptr<QuantizedVectors> codes = make_codes(request, store);
    response.quantization_time = std::chrono::steady_clock::now() - start;
    response.fp32_bytes = store.size() * store.padded_dimension() * sizeof(float);
    response.code_bytes = codes->memory_bytes();
    response.rerank =
        request.drop_fp32 ? 0 : request.rerank.value_or(std::numeric_limits<std::size_t>::max());
    if (request.drop_fp32) {
        store.release_rows();
        response.fp32_released = true;
    }
    return codes;
}

// Documents with embeddings from the JSONL itself, or text documents plus an .avec file
// whose IDs must be exactly the documents' DocumentIds (one vector per document).
std::pair<std::shared_ptr<VectorStore>, std::vector<DocumentRecord>> load_hybrid_corpus(
    const SearchRequest& request) {
    if (request.vectors_path.empty())
        return detail::load_jsonl_corpus(request.dataset_path.string(), request.metric);
    auto docs = detail::load_jsonl_documents(request.dataset_path);
    std::shared_ptr<VectorStore> store = load_vector_file(request.vectors_path, request.metric);
    const auto prefix = request.vectors_path.string() + ": ";
    if (store->size() != docs.size())
        throw InputDataError(prefix + "vector count " + std::to_string(store->size()) +
                             " differs from document count " + std::to_string(docs.size()) + ".");
    for (auto& doc : docs) {
        if (!store->contains(doc.document_id))
            throw InputDataError(prefix + "no vector for document '" + doc.id + "'.");
        doc.vector_id = doc.document_id;
    }
    return {std::move(store), std::move(docs)};
}

SearchResponse hybrid_search(const SearchRequest& request) {
    auto [store, docs] = load_hybrid_corpus(request);
    auto queries = load_queries(request, true);
    for (const auto& query : queries) {
        require_query_text(request, query, "hybrid search");
        require_query_embedding(request, query.vector, *store);
    }

    // One identity system: dense VectorIds resolve to the same DocumentIds as BM25.
    VectorDocumentBinding binding;
    std::unordered_map<DocumentId, std::string_view> identities;
    for (const auto& doc : docs) {
        if (!binding.emplace(doc.vector_id, doc.document_id).second)
            throw InputDataError(request.dataset_path.string() +
                                 ": two documents share one vector binding.");
        identities.emplace(doc.document_id, doc.id);
    }

    SearchResponse base;
    const auto dense_start = std::chrono::steady_clock::now();
    HnswIndex dense_index(*store, request.hnsw_config);
    build_hnsw(dense_index, *store, request.build_threads);
    base.index_build_time = std::chrono::steady_clock::now() - dense_start;
    const auto codes = build_codes(request, *store, base);

    lexical::Bm25Index lexical_index(request.bm25_config);
    const auto lexical_start = std::chrono::steady_clock::now();
    for (const auto& doc : docs)
        lexical_index.add_document(doc.document_id, doc.title + "\n" + doc.text);
    lexical_index.build();
    base.lexical_build_time = std::chrono::steady_clock::now() - lexical_start;

    base.vectors_indexed = store->size();
    base.documents_indexed = docs.size();
    base.dimension = store->dimension();
    base.candidate_k = effective_candidate_k(request);
    const auto filter = build_filter(request, docs, store->has_rows(), false, base);
    const hybrid::RrfFusion fusion(request.rrf_config);
    const QueryEngine engine;
    const std::size_t base_rerank = base.rerank;
    return run_queries(
        request, queries, std::move(base), [&](const QueryRecord& query, SearchResponse& out) {
            HybridQuery hybrid_query{query.text, query.vector, request.k,
                                     effective_candidate_k(request)};
            hybrid_query.parallel_retrievers = request.parallel_retrievers;
            hybrid_query.quantized = codes.get();
            hybrid_query.rerank = base_rerank;
            hybrid_query.filter = filter ? &filter->filter : nullptr;
            const auto start = std::chrono::steady_clock::now();
            const auto rankings =
                engine.search(dense_index, binding, lexical_index, fusion, hybrid_query);
            out.query_time = std::chrono::steady_clock::now() - start;

            std::unordered_map<DocumentId, std::size_t> dense_rank, lexical_rank;
            for (std::size_t i = 0; i < rankings.dense.size(); ++i)
                dense_rank.emplace(rankings.dense[i].document_id, i + 1);
            for (std::size_t i = 0; i < rankings.lexical.size(); ++i)
                lexical_rank.emplace(rankings.lexical[i].document_id, i + 1);
            std::unordered_set<DocumentId> pooled;
            for (const auto& [id, rank] : dense_rank) pooled.insert(id);
            for (const auto& [id, rank] : lexical_rank) pooled.insert(id);

            out.candidate_k = hybrid_query.candidate_k;
            out.dense_candidates = rankings.dense.size();
            out.lexical_candidates = rankings.lexical.size();
            out.fused_candidates = pooled.size();
            out.fused.reserve(rankings.fused.size());
            for (const auto& row : rankings.fused) {
                const auto identity = identities.find(row.document_id);
                if (identity == identities.end())
                    throw std::logic_error(
                        "Fused DocumentId does not resolve to a loaded document.");
                const auto dense = dense_rank.find(row.document_id);
                const auto lexical = lexical_rank.find(row.document_id);
                out.fused.push_back({row.document_id, std::string(identity->second), row.score,
                                     dense == dense_rank.end() ? 0 : dense->second,
                                     lexical == lexical_rank.end() ? 0 : lexical->second});
            }
        });
}

SearchResponse vector_search(const SearchRequest& request) {
    const bool is_jsonl = jsonl_path(request.dataset_path);
    std::shared_ptr<VectorStore> store;
    std::unordered_map<VectorId, std::string> source_ids;
    std::vector<DocumentRecord> docs;
    if (avec_path(request.dataset_path)) {
        store = load_vector_file(request.dataset_path, request.metric);
    } else if (is_jsonl) {
        auto loaded = detail::load_jsonl_corpus(request.dataset_path.string(), request.metric);
        store = std::move(loaded.first);
        docs = std::move(loaded.second);
        for (const auto& doc : docs) source_ids.emplace(doc.vector_id, doc.id);
    } else
        store = detail::load_csv_dataset(request.dataset_path, request.metric);

    std::vector<QueryRecord> queries;
    if (jsonl_path(request.query_path)) {
        queries = load_queries(request, true);
        for (const auto& query : queries) require_query_embedding(request, query.vector, *store);
    } else {
        queries.emplace_back().vector =
            detail::load_csv_query(request.query_path, store->dimension(), request.metric);
    }

    SearchResponse base;
    std::unique_ptr<HnswIndex> index;
    if (request.algorithm == SearchAlgorithm::Hnsw) {
        const auto build_start = std::chrono::steady_clock::now();
        index = std::make_unique<HnswIndex>(*store, request.hnsw_config);
        build_hnsw(*index, *store, request.build_threads);
        base.index_build_time = std::chrono::steady_clock::now() - build_start;
    }
    const auto codes = build_codes(request, *store, base);
    const std::size_t rerank = base.rerank;
    const auto filter = build_filter(request, docs, store->has_rows(),
                                     request.algorithm == SearchAlgorithm::BruteForce, base);
    base.vectors_indexed = store->size();
    base.dimension = store->dimension();
    const QueryEngine engine;
    return run_queries(
        request, queries, std::move(base), [&](const QueryRecord& query, SearchResponse& out) {
            const auto start = std::chrono::steady_clock::now();
            if (filter)
                out.neighbors =
                    index ? engine.search(*index, query.vector, request.k, filter->filter,
                                          codes.get(), rerank)
                          : engine.search(*store, query.vector, request.k, filter->filter);
            else
                out.neighbors = codes
                                    ? engine.search(*index, *codes, query.vector, request.k, rerank)
                                : index ? engine.search(*index, query.vector, request.k)
                                        : engine.search(*store, query.vector, request.k);
            out.query_time = std::chrono::steady_clock::now() - start;
            if (is_jsonl) {
                out.neighbor_source_ids.reserve(out.neighbors.size());
                for (const auto& neighbor : out.neighbors)
                    out.neighbor_source_ids.push_back(source_ids.at(neighbor.id));
            }
        });
}

}  // namespace

std::size_t effective_candidate_k(const SearchRequest& request) noexcept {
    return request.candidate_k.value_or(std::max(request.k, kDefaultHybridCandidateK));
}

void validate_search_request(const SearchRequest& request) {
    if (request.dataset_path.empty()) throw std::invalid_argument("Dataset path is required.");
    const bool have_query_file = !request.query_path.empty();
    if (have_query_file == request.query_text.has_value())
        throw std::invalid_argument("Provide exactly one of --query and --query-text.");
    if (request.query_id &&
        (request.query_id->empty() || request.query_text || !jsonl_path(request.query_path)))
        throw std::invalid_argument("--query-id requires a JSONL --query file and a nonempty ID.");
    if (request.k == 0)
        throw std::invalid_argument(
            "Invalid value for --k: the number of results must be greater than zero.");
    if (request.build_threads == 0 || request.query_threads == 0)
        throw std::invalid_argument("Thread counts must be at least 1.");
    if (request.all_queries &&
        (request.query_id || request.query_text || !jsonl_path(request.query_path)))
        throw std::invalid_argument(
            "--all-queries requires a JSONL --query file and excludes --query-id/--query-text.");
    if (request.parallel_retrievers && request.mode != SearchMode::Hybrid)
        throw std::invalid_argument("--parallel-retrievers requires hybrid mode.");
    if (!request.filters.empty() && !jsonl_path(request.dataset_path))
        throw std::invalid_argument("--filter requires JSONL documents with a metadata object.");
    for (const auto& clause : request.filters)
        if (clause.field.empty() || (!clause.date_range && clause.values.empty()))
            throw std::invalid_argument("Invalid metadata filter.");
    const bool uses_hnsw =
        request.mode == SearchMode::Hybrid ||
        (request.mode == SearchMode::Vector && request.algorithm == SearchAlgorithm::Hnsw);
    if (request.quantization == Quantization::None) {
        if (request.rerank || request.drop_fp32 || request.pq_subspaces)
            throw std::invalid_argument(
                "--rerank, --drop-fp32 and --pq-subspaces require "
                "--quantization sq8|pq.");
    } else {
        if (!uses_hnsw)
            throw std::invalid_argument("--quantization requires --algorithm hnsw or hybrid mode.");
        if (request.drop_fp32 && request.rerank && *request.rerank != 0)
            throw std::invalid_argument("--drop-fp32 removes the FP32 rows; use --rerank 0.");
        const std::size_t depth =
            request.mode == SearchMode::Hybrid ? effective_candidate_k(request) : request.k;
        if (request.rerank && *request.rerank != 0 && *request.rerank < depth)
            throw std::invalid_argument(
                "Invalid value for --rerank: use 0 or at least the number of dense results.");
        if (request.pq_subspaces && request.quantization != Quantization::Pq)
            throw std::invalid_argument("--pq-subspaces requires --quantization pq.");
    }
    if (request.mode == SearchMode::Lexical) {
        if (!jsonl_path(request.dataset_path) ||
            (!request.query_text && !jsonl_path(request.query_path)))
            throw std::invalid_argument(
                "BM25 requires JSONL documents and a JSONL query file, or --query-text.");
        if (request.candidate_k || !request.vectors_path.empty())
            throw std::invalid_argument("--candidate-k and --vectors require hybrid mode.");
        lexical::validate_bm25_config(request.bm25_config);
        if (request.query_text)
            static_cast<void>(lexical::Tokenizer{}.tokenize(*request.query_text));
        return;
    }
    if (request.mode == SearchMode::Hybrid) {
        if (request.query_text)
            throw std::invalid_argument(
                "Hybrid search requires a JSONL --query file with query_text and embedding.");
        if (!jsonl_path(request.dataset_path) || !jsonl_path(request.query_path))
            throw std::invalid_argument(
                "Hybrid search requires JSONL documents and a JSONL query file.");
        if (!request.vectors_path.empty() && !avec_path(request.vectors_path))
            throw std::invalid_argument("--vectors requires a binary .avec file.");
        if (request.metric != DistanceKind::Cosine &&
            request.metric != DistanceKind::SquaredEuclidean)
            throw std::invalid_argument("Unsupported distance metric.");
        if (request.candidate_k && *request.candidate_k < request.k)
            throw std::invalid_argument(
                "Invalid value for --candidate-k: it must be greater than or equal to --k.");
        validate_hnsw_config(request.hnsw_config);
        lexical::validate_bm25_config(request.bm25_config);
        hybrid::validate_rrf_config(request.rrf_config);
        return;
    }
    if (request.mode != SearchMode::Vector) throw std::invalid_argument("Unsupported search mode.");
    if (request.candidate_k) throw std::invalid_argument("--candidate-k requires hybrid mode.");
    if (!request.vectors_path.empty())
        throw std::invalid_argument("--vectors requires hybrid mode; use --dataset FILE.avec.");
    if (request.query_text)
        throw std::invalid_argument("--query-text requires lexical mode (BM25).");
    if (request.metric != DistanceKind::Cosine && request.metric != DistanceKind::SquaredEuclidean)
        throw std::invalid_argument("Unsupported distance metric.");
    if (!avec_path(request.dataset_path) &&
        jsonl_path(request.dataset_path) != jsonl_path(request.query_path))
        throw std::invalid_argument(
            "Vector dataset and query must both use CSV or both use JSONL (or an .avec "
            "dataset with either query format).");
    if (avec_path(request.query_path))
        throw std::invalid_argument("Queries use CSV or JSONL; .avec holds datasets only.");
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
    if (request.mode == SearchMode::Lexical) return lexical_search(request);
    if (request.mode == SearchMode::Hybrid) return hybrid_search(request);
    return vector_search(request);
}

namespace {

std::string json_string(std::string_view text) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out += hex[c >> 4];
                    out += hex[c & 0xf];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "\"";
}

}  // namespace

void validate_convert_request(const ConvertRequest& request) {
    if (request.dataset_path.empty() || request.output_path.empty())
        throw std::invalid_argument("convert requires --dataset and --out.");
    if (!avec_path(request.output_path))
        throw std::invalid_argument("--out must name a .avec file.");
    const bool jsonl = jsonl_path(request.dataset_path);
    if (!jsonl && lower_extension(request.dataset_path) != ".csv")
        throw std::invalid_argument("convert reads a .csv or .jsonl dataset.");
    if (!request.documents_output.empty() && (!jsonl || !jsonl_path(request.documents_output)))
        throw std::invalid_argument("--documents-out requires JSONL input and a .jsonl output.");
}

ConvertResponse convert_vector_file(const ConvertRequest& request) {
    validate_convert_request(request);
    const bool jsonl = jsonl_path(request.dataset_path);
    // SquaredEuclidean keeps the raw components; the reader applies the search metric.
    std::shared_ptr<VectorStore> store;
    std::vector<DocumentRecord> docs;
    if (jsonl) {
        auto loaded = detail::load_jsonl_corpus(request.dataset_path.string(),
                                                DistanceKind::SquaredEuclidean);
        store = std::move(loaded.first);
        docs = std::move(loaded.second);
    } else {
        store = detail::load_csv_dataset(request.dataset_path, DistanceKind::SquaredEuclidean);
    }
    try {
        write_vector_file(request.output_path, *store);
    } catch (const VectorFileError& error) {
        throw std::runtime_error(error.what());
    }
    ConvertResponse response{store->size(), store->dimension(), 0};
    if (!request.documents_output.empty()) {
        std::ofstream output(request.documents_output, std::ios::binary | std::ios::trunc);
        for (const auto& doc : docs)
            output << "{\"document_id\":" << json_string(doc.id)
                   << ",\"title\":" << json_string(doc.title)
                   << ",\"text\":" << json_string(doc.text) << "}\n";
        output.close();
        if (!output)
            throw std::runtime_error("Cannot write documents file: " +
                                     request.documents_output.string());
        response.documents_written = docs.size();
    }
    return response;
}

}  // namespace aetherindex
