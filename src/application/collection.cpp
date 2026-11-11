#include "aether/application/collection.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>
#include <mutex>
#include <shared_mutex>
#include <limits>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "aether/ann/hnsw/hnsw_index.hpp"
#include "aether/application/json_loader.hpp"
#include "aether/application/query_engine.hpp"
#include "aether/core/parallel.hpp"
#include "aether/hybrid/rrf.hpp"
#include "aether/io/binary_io.hpp"
#include "aether/lexical/bm25.hpp"
#include "aether/lexical/segmented_bm25.hpp"
#include "aether/persistence/snapshot.hpp"
#include "aether/persistence/write_ahead_log.hpp"
#include "aether/quantization/pq.hpp"
#include "aether/quantization/sq8.hpp"
#include "aether/storage/vector_file.hpp"
#include "csv_loader.hpp"
#include "json_value.hpp"
#include "request_util.hpp"

namespace aetherindex {
namespace {

namespace fs = std::filesystem;
using detail::avec_path;
using detail::jsonl_path;
using Clock = std::chrono::steady_clock;

constexpr std::string_view kCurrentFile = "CURRENT";
constexpr std::string_view kSnapshotPrefix = "snapshot-";
constexpr std::string_view kManifestFile = "manifest.json";
constexpr int kManifestVersion = 1;

// Storage-level file errors are input-data errors at the application boundary.
std::unique_ptr<VectorStore> load_vector_file(const fs::path& path, DistanceKind metric) {
    try {
        return read_vector_file(path, metric);
    } catch (const VectorFileError& error) {
        throw InputDataError(error.what());
    }
}

// One selected query (by --query-id, or the only record) or, with all_queries, every
// record in file order. Never silently picks the first of several queries.
std::vector<QueryRecord> select_queries(const SearchRequest& request, bool vectors) {
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
    const auto start = Clock::now();
    parallel_for(queries.size(), request.query_threads, [&](std::size_t i) {
        execute(queries[i], response.batch[i]);
        response.batch[i].selected_query_id = queries[i].id;
    });
    response.batch_time = Clock::now() - start;
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

// The build-time fields of a request; query-time fields keep their defaults.
SearchRequest build_fields(const SearchRequest& request) {
    SearchRequest built;
    built.dataset_path = request.dataset_path;
    built.vectors_path = request.vectors_path;
    built.algorithm = request.algorithm;
    built.metric = request.metric;
    built.mode = request.mode;
    built.hnsw_config = request.hnsw_config;
    built.bm25_config = request.bm25_config;
    built.quantization = request.quantization;
    built.pq_subspaces = request.pq_subspaces;
    built.drop_fp32 = request.drop_fp32;
    built.build_threads = request.build_threads;
    return built;
}

// ----- Manifest ------------------------------------------------------------------------

std::string number_text(double value) {
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value);
    return {buffer, result.ptr};
}

const char* mode_name(SearchMode mode) {
    return mode == SearchMode::Lexical ? "lexical" : mode == SearchMode::Hybrid ? "hybrid" : "vector";
}
const char* quantization_name(Quantization quantization) {
    return quantization == Quantization::Sq8  ? "sq8"
           : quantization == Quantization::Pq ? "pq"
                                              : "none";
}

using detail::JsonValue;

const JsonValue& member(const JsonValue::Object& object, std::string_view name,
                        const fs::path& path) {
    const auto found = object.find(name);
    if (found == object.end())
        throw io::IoError(path.string() + ": missing manifest field '" + std::string(name) + "'.");
    return found->second;
}

const JsonValue::Object& object_of(const JsonValue& value, const fs::path& path) {
    const auto* object = std::get_if<JsonValue::Object>(&value.value);
    if (!object) throw io::IoError(path.string() + ": manifest object expected.");
    return *object;
}

std::string text_of(const JsonValue& value, const fs::path& path) {
    const auto* text = std::get_if<std::string>(&value.value);
    if (!text) throw io::IoError(path.string() + ": manifest string expected.");
    return *text;
}

template <class Number>
Number number_of(const JsonValue& value, const fs::path& path) {
    const auto* number = std::get_if<detail::JsonNumber>(&value.value);
    Number result{};
    if (number) {
        const auto* first = number->text.data();
        const auto* last = first + number->text.size();
        const auto [end, error] = std::from_chars(first, last, result);
        if (error == std::errc{} && end == last) return result;
    }
    throw io::IoError(path.string() + ": manifest number expected.");
}

bool flag_of(const JsonValue& value, const fs::path& path) {
    const auto* flag = std::get_if<bool>(&value.value);
    if (!flag) throw io::IoError(path.string() + ": manifest boolean expected.");
    return *flag;
}

std::string snapshot_name(std::uint64_t sequence) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%020llu", static_cast<unsigned long long>(sequence));
    return std::string(kSnapshotPrefix) + buffer;
}

std::optional<std::string> read_current(const fs::path& directory) {
    std::ifstream input(directory / kCurrentFile, std::ios::binary);
    if (!input) return std::nullopt;
    std::string name;
    std::getline(input, name);
    return name;
}

std::uint64_t sequence_of(const std::string& name, const fs::path& directory) {
    std::uint64_t sequence = 0;
    const std::string_view digits = std::string_view(name).substr(kSnapshotPrefix.size());
    const auto [end, error] =
        std::from_chars(digits.data(), digits.data() + digits.size(), sequence);
    if (!name.starts_with(kSnapshotPrefix) || error != std::errc{} ||
        end != digits.data() + digits.size())
        throw io::IoError((directory / kCurrentFile).string() + ": invalid snapshot name.");
    return sequence;
}

// ----- Documents section ---------------------------------------------------------------

void save_documents(const std::vector<DocumentRecord>& docs, const fs::path& path) {
    io::write_file_atomically(path, [&](std::ostream& output) {
        io::SectionWriter out(output, static_cast<std::uint32_t>(snapshot::Section::Documents));
        out.u64(docs.size());
        for (const auto& doc : docs) {
            out.string(doc.id);
            out.u32(doc.document_id);
            out.u32(doc.vector_id);
            out.u64(doc.metadata.size());
            for (const auto& [key, value] : doc.metadata) {
                out.string(key);
                out.string(value);
            }
        }
        out.finish();
    });
}

std::vector<DocumentRecord> load_documents(const fs::path& path, bool verify) {
    const io::FileView file(path);
    io::SectionReader in(file, static_cast<std::uint32_t>(snapshot::Section::Documents), verify);
    const std::uint64_t count = in.u64();
    if (count > in.payload_bytes()) in.fail("document count exceeds the payload");
    std::vector<DocumentRecord> docs(static_cast<std::size_t>(count));
    for (auto& doc : docs) {
        doc.id = in.string();
        doc.document_id = in.u32();
        doc.vector_id = in.u32();
        const std::uint64_t entries = in.u64();
        if (entries > in.payload_bytes()) in.fail("metadata count exceeds the payload");
        doc.metadata.reserve(static_cast<std::size_t>(entries));
        for (std::uint64_t i = 0; i < entries; ++i) {
            auto key = in.string();
            doc.metadata.emplace_back(std::move(key), in.string());
        }
    }
    in.expect_end();
    return docs;
}

}  // namespace

struct Collection::State {
    // Concurrency: searches hold `state` shared. Writers (add, remove, compact, save) are
    // serialized by `writer`, do their expensive work while searches run, and hold `state`
    // exclusively only to publish (row appends, tombstones, catalog, segment and pointer
    // swaps). HNSW nodes are linked after that, alongside searches (lock-free reads).
    mutable std::shared_mutex state;
    mutable std::mutex writer;
    // Write-ahead log (when opened with write_ahead_log) and the directory it belongs to.
    std::unique_ptr<WriteAheadLog> log;
    fs::path directory;
    std::uint64_t last_operation = 0;
    SearchRequest config;  // Build-time fields only.
    std::shared_ptr<VectorStore> store;
    // Identity and metadata; title, text and embeddings are dropped after the build.
    std::vector<DocumentRecord> docs;
    std::unique_ptr<HnswIndex> hnsw;
    std::unique_ptr<QuantizedVectors> codes;
    std::unique_ptr<lexical::SegmentedBm25> bm25;
    bool jsonl_vectors = false;  // Vector mode over JSONL: show source document IDs.
    bool built = false;
    CollectionStats stats;
    // Derived from docs.
    VectorDocumentBinding binding;
    std::unordered_map<DocumentId, std::string_view> identities;
    std::unordered_map<VectorId, std::string_view> source_ids;

    void index_documents() {
        binding.clear();
        identities.clear();
        source_ids.clear();
        for (const auto& doc : docs) {
            identities.emplace(doc.document_id, doc.id);
            source_ids.emplace(doc.vector_id, doc.id);
        }
    }
};

Collection::Collection() : state_(std::make_unique<State>()) {}
Collection::~Collection() = default;

const SearchRequest& Collection::configuration() const noexcept { return state_->config; }
CollectionStats Collection::stats() const {
    const std::shared_lock lock(state_->state);
    return state_->stats;
}

std::unique_ptr<Collection> Collection::load(const SearchRequest& request) {
    std::unique_ptr<Collection> collection(new Collection());
    auto& s = *collection->state_;
    s.config = build_fields(request);
    if (request.mode == SearchMode::Lexical) {
        s.docs = detail::load_jsonl_documents(request.dataset_path);
    } else if (request.mode == SearchMode::Hybrid) {
        auto [store, docs] = load_hybrid_corpus(request);
        s.store = std::move(store);
        s.docs = std::move(docs);
    } else if (avec_path(request.dataset_path)) {
        s.store = load_vector_file(request.dataset_path, request.metric);
    } else if (jsonl_path(request.dataset_path)) {
        auto [store, docs] = detail::load_jsonl_corpus(request.dataset_path.string(), request.metric);
        s.store = std::move(store);
        s.docs = std::move(docs);
        s.jsonl_vectors = true;
    } else {
        s.store = detail::load_csv_dataset(request.dataset_path, request.metric);
    }
    return collection;
}

std::vector<QueryRecord> Collection::load_queries(const SearchRequest& query_request) const {
    const auto& s = *state_;
    const std::shared_lock lock(s.state);
    const SearchRequest request = merged_request(query_request);
    std::vector<QueryRecord> queries;
    if (request.mode == SearchMode::Lexical) {
        if (request.query_text) {
            queries.emplace_back().text = *request.query_text;
        } else {
            queries = select_queries(request, false);
            for (const auto& query : queries) require_query_text(request, query, "selected query");
        }
    } else if (request.mode == SearchMode::Hybrid) {
        queries = select_queries(request, true);
        for (const auto& query : queries) {
            require_query_text(request, query, "hybrid search");
            require_query_embedding(request, query.vector, *s.store);
        }
    } else if (jsonl_path(request.query_path)) {
        queries = select_queries(request, true);
        for (const auto& query : queries) require_query_embedding(request, query.vector, *s.store);
    } else {
        queries.emplace_back().vector =
            detail::load_csv_query(request.query_path, s.store->dimension(), request.metric);
    }
    return queries;
}

void Collection::build_indexes() {
    auto& s = *state_;
    if (s.built) throw std::logic_error("Collection indexes are already built.");
    const auto& config = s.config;
    const auto build_codes = [&] {
        if (config.quantization == Quantization::None) return;
        const auto start = Clock::now();
        s.codes = make_codes(config, *s.store);
        s.stats.quantization_time = Clock::now() - start;
        s.stats.fp32_bytes = s.store->size() * s.store->padded_dimension() * sizeof(float);
        s.stats.code_bytes = s.codes->memory_bytes();
        if (config.drop_fp32) {
            s.store->release_rows();
            s.stats.fp32_released = true;
        }
    };
    const auto build_lexical = [&] {
        auto index = std::make_shared<lexical::Bm25Index>(config.bm25_config);
        const auto start = Clock::now();
        for (const auto& doc : s.docs) index->add_document(doc.document_id, doc.title + "\n" + doc.text);
        index->build();
        const auto elapsed = Clock::now() - start;
        s.bm25 = std::make_unique<lexical::SegmentedBm25>(std::move(index));
        return elapsed;
    };
    if (config.mode == SearchMode::Lexical) {
        s.stats.index_build_time = build_lexical();
    } else {
        if (config.mode == SearchMode::Hybrid) {
            // One identity system: dense VectorIds resolve to the same DocumentIds as BM25.
            std::unordered_set<VectorId> bound;
            for (const auto& doc : s.docs)
                if (!bound.insert(doc.vector_id).second)
                    throw InputDataError(config.dataset_path.string() +
                                         ": two documents share one vector binding.");
        }
        if (config.mode == SearchMode::Hybrid || config.algorithm == SearchAlgorithm::Hnsw) {
            const auto start = Clock::now();
            s.hnsw = std::make_unique<HnswIndex>(*s.store, config.hnsw_config);
            s.hnsw->insert_parallel(s.store->ids(), config.build_threads);
            s.stats.index_build_time = Clock::now() - start;
        }
        build_codes();
        if (config.mode == SearchMode::Hybrid) s.stats.lexical_build_time = build_lexical();
    }
    for (auto& doc : s.docs) {
        std::string().swap(doc.text);
        std::string().swap(doc.title);
        std::vector<float>().swap(doc.vector);
    }
    s.stats.vectors = s.store ? s.store->size() : 0;
    s.stats.dimension = s.store ? s.store->dimension() : 0;
    s.stats.documents = s.docs.size();
    s.index_documents();
    for (const auto& doc : s.docs) s.binding.emplace(doc.vector_id, doc.document_id);
    s.built = true;
}

SearchRequest Collection::merged_request(const SearchRequest& request) const {
    const auto& config = state_->config;
    SearchRequest merged = request;
    merged.dataset_path = config.dataset_path;
    merged.vectors_path = config.vectors_path;
    merged.algorithm = config.algorithm;
    merged.metric = config.metric;
    merged.mode = config.mode;
    merged.hnsw_config = config.hnsw_config;
    merged.bm25_config = config.bm25_config;
    merged.quantization = config.quantization;
    merged.pq_subspaces = config.pq_subspaces;
    merged.drop_fp32 = config.drop_fp32;
    merged.build_threads = config.build_threads;
    return merged;
}

SearchResponse Collection::search(const SearchRequest& query_request,
                                  std::vector<QueryRecord>& queries) const {
    const auto& s = *state_;
    // One consistent view per call (a batch included); see State for the protocol.
    const std::shared_lock lock(s.state);
    if (!s.built) throw std::logic_error("Collection::search requires built indexes.");
    const SearchRequest request = merged_request(query_request);
    const QueryEngine engine;
    SearchResponse base;
    if (s.codes) {
        base.quantization_time = s.stats.quantization_time;
        base.fp32_bytes = s.stats.fp32_bytes;
        base.code_bytes = s.stats.code_bytes;
        base.fp32_released = s.stats.fp32_released;
        base.rerank = request.drop_fp32
                          ? 0
                          : request.rerank.value_or(std::numeric_limits<std::size_t>::max());
    }

    if (request.mode == SearchMode::Lexical) {
        base.index_build_time = s.stats.index_build_time;
        base.documents_indexed = s.docs.size();
        const auto filter = build_filter(request, s.docs, false, false, base);
        if (filter) base.exact_filter = false;  // No dense channel in lexical mode.
        return run_queries(
            request, queries, std::move(base), [&](const QueryRecord& query, SearchResponse& out) {
                const auto start = Clock::now();
                const auto ranked = filter
                                        ? engine.search(*s.bm25, query.text, request.k, filter->filter)
                                        : engine.search(*s.bm25, query.text, request.k);
                out.query_time = Clock::now() - start;
                out.documents.reserve(ranked.size());
                for (const auto& row : ranked)
                    out.documents.push_back({row.document_id,
                                             std::string(s.identities.at(row.document_id)),
                                             row.score});
            });
    }

    if (request.mode == SearchMode::Hybrid) {
        base.index_build_time = s.stats.index_build_time;
        base.lexical_build_time = s.stats.lexical_build_time;
        base.vectors_indexed = s.store->live_size();
        base.documents_indexed = s.docs.size();
        base.dimension = s.store->dimension();
        base.candidate_k = effective_candidate_k(request);
        const auto filter = build_filter(request, s.docs, s.store->has_rows(), false, base);
        const hybrid::RrfFusion fusion(request.rrf_config);
        const std::size_t base_rerank = base.rerank;
        return run_queries(
            request, queries, std::move(base), [&](const QueryRecord& query, SearchResponse& out) {
                HybridQuery hybrid_query{query.text, query.vector, request.k,
                                         effective_candidate_k(request)};
                hybrid_query.parallel_retrievers = request.parallel_retrievers;
                hybrid_query.quantized = s.codes.get();
                hybrid_query.rerank = base_rerank;
                hybrid_query.filter = filter ? &filter->filter : nullptr;
                const auto start = Clock::now();
                const auto rankings = engine.search(*s.hnsw, s.binding, *s.bm25, fusion, hybrid_query);
                out.query_time = Clock::now() - start;

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
                    const auto identity = s.identities.find(row.document_id);
                    if (identity == s.identities.end())
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

    if (s.hnsw) base.index_build_time = s.stats.index_build_time;
    const std::size_t rerank = base.rerank;
    const auto filter = build_filter(request, s.docs, s.store->has_rows(),
                                     request.algorithm == SearchAlgorithm::BruteForce, base);
    base.vectors_indexed = s.store->live_size();
    base.dimension = s.store->dimension();
    const HnswIndex* index = s.hnsw.get();
    return run_queries(
        request, queries, std::move(base), [&](const QueryRecord& query, SearchResponse& out) {
            const auto start = Clock::now();
            if (filter)
                out.neighbors = index ? engine.search(*index, query.vector, request.k,
                                                      filter->filter, s.codes.get(), rerank)
                                      : engine.search(*s.store, query.vector, request.k,
                                                      filter->filter);
            else
                out.neighbors =
                    s.codes ? engine.search(*index, *s.codes, query.vector, request.k, rerank)
                    : index ? engine.search(*index, query.vector, request.k)
                            : engine.search(*s.store, query.vector, request.k);
            out.query_time = Clock::now() - start;
            if (s.jsonl_vectors) {
                out.neighbor_source_ids.reserve(out.neighbors.size());
                for (const auto& neighbor : out.neighbors)
                    out.neighbor_source_ids.emplace_back(s.source_ids.at(neighbor.id));
            }
        });
}

// ----- Snapshots -----------------------------------------------------------------------

bool Collection::is_collection(const fs::path& directory) {
    std::error_code error;
    return fs::is_regular_file(directory / kCurrentFile, error);
}

void Collection::save(const fs::path& directory) const {
    const auto& s = *state_;
    // No writer may change the collection while it is written; searches may continue.
    const std::lock_guard writer(s.writer);
    if (!s.built) throw std::logic_error("Collection::save requires built indexes.");
    fs::create_directories(directory);
    std::uint64_t sequence = 1;
    if (const auto current = read_current(directory))
        sequence = sequence_of(*current, directory) + 1;
    const std::string name = snapshot_name(sequence);
    const fs::path staging = directory / (name + ".tmp");
    fs::remove_all(staging);
    fs::create_directory(staging);

    std::vector<std::pair<std::string, std::string>> files;  // (role, file name)
    if (s.store) {
        snapshot::save(*s.store, staging / "vectors.sec");
        files.emplace_back("vectors", "vectors.sec");
    }
    if (s.hnsw) {
        snapshot::save(*s.hnsw, staging / "hnsw.sec");
        files.emplace_back("hnsw", "hnsw.sec");
    }
    if (s.codes) {
        snapshot::save(*s.codes, staging / "codes.sec");
        files.emplace_back("codes", "codes.sec");
    }
    if (s.bm25) {
        snapshot::save(*s.bm25, staging / "bm25.sec");
        files.emplace_back("bm25", "bm25.sec");
    }
    if (!s.docs.empty()) {
        save_documents(s.docs, staging / "documents.sec");
        files.emplace_back("documents", "documents.sec");
    }

    const auto& c = s.config;
    std::ostringstream manifest;
    manifest << "{\n  \"format\": \"aetherindex-collection\",\n  \"version\": " << kManifestVersion
             << ",\n  \"sequence\": " << sequence
             << ",\n  \"last_operation\": " << s.last_operation
             << ",\n  \"mode\": " << detail::json_string(mode_name(c.mode))
             << ",\n  \"algorithm\": "
             << detail::json_string(c.algorithm == SearchAlgorithm::Hnsw ? "hnsw" : "brute-force")
             << ",\n  \"metric\": "
             << detail::json_string(c.metric == DistanceKind::Cosine ? "cosine" : "squared-euclidean")
             << ",\n  \"dataset\": " << detail::json_string(c.dataset_path.generic_string())
             << ",\n  \"vectors\": " << detail::json_string(c.vectors_path.generic_string())
             << ",\n  \"quantization\": " << detail::json_string(quantization_name(c.quantization))
             << ",\n  \"pq_subspaces\": " << c.pq_subspaces
             << ",\n  \"drop_fp32\": " << (c.drop_fp32 ? "true" : "false")
             << ",\n  \"build_threads\": " << c.build_threads
             << ",\n  \"jsonl_vectors\": " << (s.jsonl_vectors ? "true" : "false")
             << ",\n  \"stats\": {\"vectors\": " << s.stats.vectors
             << ", \"documents\": " << s.stats.documents << ", \"dimension\": " << s.stats.dimension
             << ", \"index_build_ms\": " << number_text(s.stats.index_build_time.count())
             << ", \"lexical_build_ms\": " << number_text(s.stats.lexical_build_time.count())
             << ", \"quantization_ms\": " << number_text(s.stats.quantization_time.count())
             << ", \"fp32_bytes\": " << s.stats.fp32_bytes
             << ", \"code_bytes\": " << s.stats.code_bytes
             << ", \"fp32_released\": " << (s.stats.fp32_released ? "true" : "false")
             << "},\n  \"files\": {";
    for (std::size_t i = 0; i < files.size(); ++i)
        manifest << (i ? ", " : "") << detail::json_string(files[i].first) << ": {\"name\": "
                 << detail::json_string(files[i].second)
                 << ", \"bytes\": " << fs::file_size(staging / files[i].second) << "}";
    manifest << "}\n}\n";
    const auto text = manifest.str();
    io::write_file_atomically(staging / kManifestFile,
                              [&](std::ostream& output) { output << text; });
    io::sync_path(staging);

    const fs::path target = directory / name;
    fs::remove_all(target);
    fs::rename(staging, target);
    io::sync_path(directory);
    crash_point("checkpoint-before-current");
    io::write_file_atomically(directory / kCurrentFile,
                              [&](std::ostream& output) { output << name << '\n'; });
    // The snapshot holds every logged operation: empty the log. A crash before this is
    // harmless, since replay skips records the snapshot already contains.
    std::error_code same_error;
    if (s.log && fs::equivalent(directory, s.directory, same_error)) {
        crash_point("checkpoint-before-log-reset");
        s.log->reset(s.last_operation + 1);
    }
    // Older snapshots and abandoned staging directories are no longer reachable.
    for (const auto& entry : fs::directory_iterator(directory)) {
        const auto file = entry.path().filename().string();
        if (entry.is_directory() && file.starts_with(kSnapshotPrefix) && file != name)
            fs::remove_all(entry.path());
    }
}

std::unique_ptr<Collection> Collection::open(const fs::path& directory,
                                             const OpenOptions& options) {
    const auto start = Clock::now();
    const auto current = read_current(directory);
    if (!current)
        throw io::IoError(directory.string() + ": not an AetherIndex index (no CURRENT file).");
    const std::uint64_t sequence = sequence_of(*current, directory);
    const fs::path folder = directory / *current;
    const fs::path manifest_path = folder / kManifestFile;
    std::string manifest_text;
    {
        std::ifstream input(manifest_path, std::ios::binary);
        if (!input) throw io::IoError(manifest_path.string() + ": cannot read the manifest.");
        manifest_text.assign(std::istreambuf_iterator<char>(input), {});
    }
    detail::JsonValue manifest_value;
    try {
        manifest_value = detail::parse_json(manifest_text);
    } catch (const std::invalid_argument& error) {
        throw io::IoError(manifest_path.string() + ": " + error.what());
    }
    const auto& manifest = object_of(manifest_value, manifest_path);
    const auto field = [&](std::string_view name) -> const JsonValue& {
        return member(manifest, name, manifest_path);
    };
    if (text_of(field("format"), manifest_path) != "aetherindex-collection" ||
        number_of<int>(field("version"), manifest_path) != kManifestVersion)
        throw io::IoError(manifest_path.string() + ": unsupported index format or version.");
    if (number_of<std::uint64_t>(field("sequence"), manifest_path) != sequence)
        throw io::IoError(manifest_path.string() + ": manifest does not match CURRENT.");

    std::unique_ptr<Collection> collection(new Collection());
    auto& s = *collection->state_;
    auto& c = s.config;
    const auto mode = text_of(field("mode"), manifest_path);
    c.mode = mode == "lexical"  ? SearchMode::Lexical
             : mode == "hybrid" ? SearchMode::Hybrid
                                : SearchMode::Vector;
    c.algorithm = text_of(field("algorithm"), manifest_path) == "hnsw" ? SearchAlgorithm::Hnsw
                                                                      : SearchAlgorithm::BruteForce;
    c.metric = text_of(field("metric"), manifest_path) == "cosine" ? DistanceKind::Cosine
                                                                  : DistanceKind::SquaredEuclidean;
    c.dataset_path = text_of(field("dataset"), manifest_path);
    c.vectors_path = text_of(field("vectors"), manifest_path);
    const auto quantization = text_of(field("quantization"), manifest_path);
    c.quantization = quantization == "sq8" ? Quantization::Sq8
                     : quantization == "pq" ? Quantization::Pq
                                            : Quantization::None;
    c.pq_subspaces = number_of<std::size_t>(field("pq_subspaces"), manifest_path);
    c.drop_fp32 = flag_of(field("drop_fp32"), manifest_path);
    c.build_threads = number_of<std::size_t>(field("build_threads"), manifest_path);
    s.jsonl_vectors = flag_of(field("jsonl_vectors"), manifest_path);
    const auto& stats = object_of(field("stats"), manifest_path);
    const auto stat = [&](std::string_view name) -> const JsonValue& {
        return member(stats, name, manifest_path);
    };
    s.stats.index_build_time =
        std::chrono::duration<double, std::milli>(number_of<double>(stat("index_build_ms"), manifest_path));
    s.stats.lexical_build_time = std::chrono::duration<double, std::milli>(
        number_of<double>(stat("lexical_build_ms"), manifest_path));
    s.stats.quantization_time = std::chrono::duration<double, std::milli>(
        number_of<double>(stat("quantization_ms"), manifest_path));
    s.stats.fp32_bytes = number_of<std::size_t>(stat("fp32_bytes"), manifest_path);
    s.stats.code_bytes = number_of<std::size_t>(stat("code_bytes"), manifest_path);
    s.stats.fp32_released = flag_of(stat("fp32_released"), manifest_path);

    const auto& files = object_of(field("files"), manifest_path);
    const auto file_of = [&](std::string_view role) -> std::optional<fs::path> {
        const auto found = files.find(role);
        if (found == files.end()) return std::nullopt;
        const auto& entry = object_of(found->second, manifest_path);
        const auto name = text_of(member(entry, "name", manifest_path), manifest_path);
        if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
            name.starts_with("."))
            throw io::IoError(manifest_path.string() + ": invalid file name in the manifest.");
        const fs::path path = folder / name;
        std::error_code error;
        const auto bytes = fs::file_size(path, error);
        if (error || bytes != number_of<std::uint64_t>(member(entry, "bytes", manifest_path),
                                                       manifest_path))
            throw io::IoError(path.string() + ": missing or size differs from the manifest.");
        s.stats.snapshot_bytes += bytes;
        return path;
    };
    const bool verify = options.verify_checksums;
    if (const auto path = file_of("vectors"))
        s.store = snapshot::load_vector_store(*path, verify);
    if (const auto path = file_of("hnsw")) {
        if (!s.store) throw io::IoError(manifest_path.string() + ": HNSW without vectors.");
        s.hnsw = snapshot::load_hnsw(*path, *s.store, verify, options.ef_search);
    }
    if (const auto path = file_of("codes")) s.codes = snapshot::load_quantized(*path, verify);
    if (const auto path = file_of("bm25"))
        s.bm25 = snapshot::load_segmented_bm25(
            *path, verify, {options.bm25_strategy, options.bm25_query_weighting});
    if (const auto path = file_of("documents")) s.docs = load_documents(*path, verify);

    const bool dense = c.mode == SearchMode::Hybrid ||
                       (c.mode == SearchMode::Vector && c.algorithm == SearchAlgorithm::Hnsw);
    if ((c.mode != SearchMode::Lexical && !s.store) || (dense && !s.hnsw) ||
        (c.mode != SearchMode::Vector && !s.bm25) ||
        ((c.quantization != Quantization::None) != static_cast<bool>(s.codes)) ||
        (s.codes && (s.codes->size() != s.store->size() ||
                     s.codes->dimension() != s.store->dimension())))
        throw io::IoError(manifest_path.string() + ": the files do not match the index mode.");
    if (options.ef_search && !s.hnsw)
        throw std::invalid_argument("--ef-search requires an index with an HNSW graph.");
    if ((options.bm25_strategy || options.bm25_query_weighting) && !s.bm25)
        throw std::invalid_argument("BM25 options require an index with a BM25 index.");
    if (s.hnsw) c.hnsw_config = s.hnsw->config();
    if (s.bm25) c.bm25_config = s.bm25->config();
    if (s.store) {
        c.metric = s.store->distance_kind();
        if (s.hnsw && s.hnsw->size() != s.store->size())
            throw io::IoError(manifest_path.string() + ": the graph does not index every vector.");
    }
    if (c.mode != SearchMode::Vector && s.bm25 && s.bm25->live_count() != s.docs.size())
        throw io::IoError(manifest_path.string() + ": BM25 and documents disagree.");
    if (s.jsonl_vectors || c.mode == SearchMode::Hybrid)
        for (const auto& doc : s.docs)
            if (!s.store->contains(doc.vector_id))
                throw io::IoError(manifest_path.string() + ": a document has no vector.");
    s.stats.vectors = s.store ? s.store->size() : 0;
    s.stats.dimension = s.store ? s.store->dimension() : 0;
    s.stats.documents = s.docs.size();
    s.stats.snapshot_sequence = sequence;
    s.index_documents();
    for (const auto& doc : s.docs) s.binding.emplace(doc.vector_id, doc.document_id);
    s.built = true;
    s.directory = directory;
    const auto last = manifest.find("last_operation");
    s.last_operation = last == manifest.end() ? 0
                                              : number_of<std::uint64_t>(last->second, manifest_path);
    collection->replay(directory / "wal.log", options);
    s.stats.vectors = s.store ? s.store->live_size() : 0;
    s.stats.documents = s.docs.size();
    s.stats.last_operation = s.last_operation;
    s.stats.load_time = Clock::now() - start;
    return collection;
}

// ----- Write-ahead log ------------------------------------------------------------------

namespace {

enum class Operation : std::uint8_t { Add = 1, Remove = 2, Compact = 3 };

void put_text(std::string& out, std::string_view text) {
    unsigned char size[8];
    io::put<std::uint64_t>(size, text.size());
    out.append(reinterpret_cast<const char*>(size), sizeof size);
    out.append(text);
}

std::string_view take_text(std::string_view& in) {
    if (in.size() < 8) throw io::IoError("Malformed write-ahead log record.");
    const auto size = io::get<std::uint64_t>(reinterpret_cast<const unsigned char*>(in.data()));
    if (size > in.size() - 8) throw io::IoError("Malformed write-ahead log record.");
    const auto text = in.substr(8, size);
    in.remove_prefix(8 + size);
    return text;
}

std::string file_bytes(const fs::path& path) {
    if (path.empty()) return {};
    std::ifstream input(path, std::ios::binary);
    if (!input) throw InputDataError("Cannot read " + path.string());
    return {std::istreambuf_iterator<char>(input), {}};
}

// A replayed add reads its records from temporary files with the original extensions.
struct TemporaryFile {
    fs::path path;
    TemporaryFile(std::string_view extension, std::string_view bytes) {
        static std::atomic<unsigned> counter{0};
        static const unsigned process_tag = std::random_device{}();
        path = fs::temp_directory_path() /
               ("aether-replay-" + std::to_string(process_tag) + "-" +
                std::to_string(counter++) + std::string(extension));
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!output) throw io::IoError("Cannot write replay file " + path.string());
    }
    ~TemporaryFile() {
        std::error_code ignored;
        fs::remove(path, ignored);
    }
};

}  // namespace

void Collection::replay(const fs::path& path, const OpenOptions& options) {
    auto& s = *state_;
    const auto contents = WriteAheadLog::read(path);
    s.stats.log_tail_discarded = contents.torn;
    for (const auto& record : contents.records) {
        if (record.sequence <= s.last_operation) continue;  // Already in the snapshot.
        if (record.sequence != s.last_operation + 1)
            throw io::IoError(path.string() + ": operation " +
                              std::to_string(s.last_operation + 1) + " is missing.");
        std::string_view payload = record.payload;
        switch (static_cast<Operation>(record.operation)) {
            case Operation::Add: {
                if (payload.empty()) throw io::IoError(path.string() + ": malformed add.");
                const bool replace = payload.front() != 0;
                payload.remove_prefix(1);
                const auto extension = take_text(payload);
                const auto dataset = take_text(payload);
                const auto vectors = take_text(payload);
                const TemporaryFile dataset_file(extension, dataset);
                std::optional<TemporaryFile> vectors_file;
                if (!vectors.empty()) vectors_file.emplace(".avec", vectors);
                add(dataset_file.path, vectors_file ? vectors_file->path : fs::path(), replace);
                break;
            }
            case Operation::Remove: {
                if (payload.size() < 8) throw io::IoError(path.string() + ": malformed delete.");
                const auto count =
                    io::get<std::uint64_t>(reinterpret_cast<const unsigned char*>(payload.data()));
                payload.remove_prefix(8);
                if (count > payload.size()) throw io::IoError(path.string() + ": malformed delete.");
                std::vector<std::string> ids;
                for (std::uint64_t i = 0; i < count; ++i) ids.emplace_back(take_text(payload));
                remove(ids);
                break;
            }
            case Operation::Compact:
                compact();
                break;
            default:
                throw io::IoError(path.string() + ": unknown operation " +
                                  std::to_string(record.operation) + ".");
        }
        s.last_operation = record.sequence;
        ++s.stats.replayed_operations;
    }
    if (options.write_ahead_log)
        s.log = std::make_unique<WriteAheadLog>(path, contents.valid_bytes, s.last_operation + 1,
                                                options.sync);
}

void Collection::log_operation(std::uint8_t operation, const std::string& payload) {
    auto& s = *state_;
    if (!s.log) return;
    s.last_operation = s.log->append(operation, payload);
    s.stats.last_operation = s.last_operation;
}

void Collection::checkpoint() {
    auto& s = *state_;
    if (!s.log) throw std::logic_error("checkpoint() requires a collection opened with its log.");
    save(s.directory);
}

// ----- Mutations ---------------------------------------------------------------------

std::size_t Collection::deleted_count() const {
    const auto& s = *state_;
    const std::shared_lock lock(s.state);
    std::size_t deleted = s.store ? s.store->deleted_count() : 0;
    if (s.bm25) deleted = std::max(deleted, s.bm25->deleted_count());
    return deleted;
}

Collection::AddResult Collection::add(const fs::path& dataset, const fs::path& vectors,
                                      bool replace) {
    auto& s = *state_;
    const std::lock_guard writer(s.writer);
    // The log keeps the input exactly as it was read.
    std::string record;
    if (s.log) {
        record.push_back(replace ? 1 : 0);
        put_text(record, dataset.extension().string());
        put_text(record, file_bytes(dataset));
        put_text(record, file_bytes(vectors));
    }
    if (!s.built) throw std::logic_error("Collection::add requires built indexes.");
    const auto& c = s.config;
    if (s.store && !s.store->has_rows())
        throw std::invalid_argument(
            "This index was built with --drop-fp32; it cannot accept new vectors.");
    if (!vectors.empty() && c.mode != SearchMode::Hybrid)
        throw std::invalid_argument("--vectors applies to hybrid indexes only.");
    if (c.mode == SearchMode::Hybrid && vectors.empty() != c.vectors_path.empty())
        throw std::invalid_argument(c.vectors_path.empty()
                                        ? "This hybrid index reads embeddings from the JSONL; "
                                          "do not pass --vectors."
                                        : "This hybrid index was built with --vectors; pass "
                                          "the new embeddings with --vectors FILE.avec.");
    SearchRequest source = c;
    source.dataset_path = dataset;
    source.vectors_path = vectors;
    if (c.mode != SearchMode::Vector || s.jsonl_vectors) {
        if (!jsonl_path(dataset))
            throw std::invalid_argument("This index takes JSONL documents.");
    } else if (jsonl_path(dataset)) {
        throw std::invalid_argument("This index takes CSV or .avec vectors.");
    }
    auto incoming = Collection::load(source);
    auto& in = *incoming->state_;
    if (s.store && in.store && in.store->dimension() != s.store->dimension())
        throw InputDataError(dataset.string() + ": vector dimension " +
                             std::to_string(in.store->dimension()) + " differs from the index (" +
                             std::to_string(s.store->dimension()) + ").");

    AddResult result;
    const bool documents = c.mode != SearchMode::Vector || s.jsonl_vectors;
    // (incoming position, existing doc index or npos) and the identity each record gets.
    struct Plan {
        std::size_t existing;
        DocumentId document_id;
        VectorId vector_id;
        std::size_t row;  // Position in the incoming store (vectors only).
    };
    constexpr std::size_t kNew = std::numeric_limits<std::size_t>::max();
    std::vector<Plan> plan;
    if (documents) {
        std::unordered_map<std::string_view, std::size_t> by_source;
        std::unordered_set<DocumentId> used;
        for (std::size_t i = 0; i < s.docs.size(); ++i) {
            by_source.emplace(s.docs[i].id, i);
            used.insert(s.docs[i].document_id);
        }
        // Opaque new IDs are numbered in source-ID order, like a fresh load.
        std::vector<std::size_t> order(in.docs.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(),
                  [&](std::size_t a, std::size_t b) { return in.docs[a].id < in.docs[b].id; });
        plan.resize(in.docs.size());
        std::uint64_t next = 0;
        const auto allocate = [&] {
            while (next <= std::numeric_limits<DocumentId>::max() &&
                   used.contains(static_cast<DocumentId>(next)))
                ++next;
            if (next > std::numeric_limits<DocumentId>::max())
                throw InputDataError("Too many document identifiers.");
            used.insert(static_cast<DocumentId>(next));
            return static_cast<DocumentId>(next++);
        };
        // Existing and numeric IDs first, so allocation never takes them.
        for (std::size_t i = 0; i < in.docs.size(); ++i) {
            const auto& doc = in.docs[i];
            const auto found = by_source.find(doc.id);
            if (found != by_source.end()) {
                if (!replace)
                    throw InputDataError(dataset.string() + ": document '" + doc.id +
                                         "' already exists (use --replace to update it).");
                const auto& old = s.docs[found->second];
                plan[i] = {found->second, old.document_id, old.vector_id, 0};
            } else {
                plan[i] = {kNew, 0, 0, 0};
            }
        }
        for (std::size_t i = 0; i < in.docs.size(); ++i) {
            if (plan[i].existing != kNew) continue;
            DocumentId numeric = 0;
            const auto& text = in.docs[i].id;
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), numeric);
            if (error == std::errc{} && end == text.data() + text.size() && !text.empty() &&
                (text.size() == 1 || text[0] != '0') && !used.contains(numeric)) {
                used.insert(numeric);
                plan[i].document_id = numeric;
                plan[i].existing = kNew - 1;  // Marked as assigned.
            }
        }
        for (std::size_t position : order) {
            if (plan[position].existing == kNew) plan[position].document_id = allocate();
            if (plan[position].existing == kNew - 1) plan[position].existing = kNew;
        }
        for (std::size_t i = 0; i < in.docs.size(); ++i) {
            if (plan[i].existing == kNew) plan[i].vector_id = plan[i].document_id;
            if (in.store) plan[i].row = *in.store->position_of(in.docs[i].vector_id);
        }
    } else {
        plan.resize(in.store->size());
        for (std::size_t row = 0; row < in.store->size(); ++row) {
            const VectorId id = in.store->ids()[row];
            if (s.store->contains(id) && !replace)
                throw InputDataError(dataset.string() + ": vector " + std::to_string(id) +
                                     " already exists (use --replace to update it).");
            plan[row] = {s.store->contains(id) ? 0 : kNew, id, id, row};
        }
    }

    // Prepare outside the exclusive section: the BM25 segment and corpus-wide scoring data.
    std::vector<DocumentId> replacing;
    for (const auto& item : plan)
        if (item.existing != kNew) replacing.push_back(item.document_id);
    result.replaced = replacing.size();
    result.added = plan.size() - result.replaced;
    std::optional<lexical::SegmentedBm25::PendingSegment> segment;
    if (s.bm25) {
        std::vector<std::pair<DocumentId, std::string>> batch;
        batch.reserve(plan.size());
        for (std::size_t i = 0; i < plan.size(); ++i)
            batch.emplace_back(plan[i].document_id, in.docs[i].title + "\n" + in.docs[i].text);
        s.bm25->prepare_for_changes();
        segment = s.bm25->prepare_add(batch, replacing);
    }
    std::vector<DocumentRecord> records;
    if (documents) {
        records.reserve(plan.size());
        for (std::size_t i = 0; i < plan.size(); ++i) {
            DocumentRecord record;
            record.id = std::move(in.docs[i].id);
            record.document_id = plan[i].document_id;
            record.vector_id = plan[i].vector_id;
            record.metadata = std::move(in.docs[i].metadata);
            records.push_back(std::move(record));
        }
    }

    // Publish: tombstones of replaced records, new rows, codes, BM25 segment, catalog, and
    // graph storage for the new nodes.
    bool link = false;
    {
        const std::unique_lock lock(s.state);
        for (const auto& item : plan) {
            if (item.existing == kNew) continue;
            if (s.store) s.store->erase(item.vector_id);
            if (s.bm25) s.bm25->remove(item.document_id);
        }
        if (s.store) {
            std::vector<VectorId> inserted;
            inserted.reserve(plan.size());
            for (const auto& item : plan) {
                if (s.store->insert_from(*in.store, item.row, item.vector_id) !=
                    InsertStatus::Success)
                    throw InputDataError(dataset.string() + ": cannot insert vector " +
                                         std::to_string(item.vector_id) + ".");
                inserted.push_back(item.vector_id);
            }
            if (s.codes) s.codes->extend(*s.store);
            if (s.hnsw && !inserted.empty()) {
                s.hnsw->prepare_insert(inserted);
                s.hnsw->begin_concurrent_reads();
                link = true;
            }
        }
        if (segment) s.bm25->commit_add(std::move(*segment));
        if (documents) {
            // Replaced records keep their slot; new ones are appended.
            for (std::size_t i = 0; i < plan.size(); ++i) {
                if (plan[i].existing == kNew)
                    s.docs.push_back(std::move(records[i]));
                else
                    s.docs[plan[i].existing] = std::move(records[i]);
            }
            s.index_documents();
            s.binding.clear();
            for (const auto& doc : s.docs) s.binding.emplace(doc.vector_id, doc.document_id);
        }
        s.stats.vectors = s.store ? s.store->live_size() : 0;
        s.stats.documents = s.docs.size();
    }
    // Link the new graph nodes while searches run; they become reachable as they finish.
    if (link) {
        try {
            s.hnsw->insert_prepared(std::max<std::size_t>(c.build_threads, 1));
        } catch (...) {
            s.hnsw->end_concurrent_reads();
            throw;
        }
        s.hnsw->end_concurrent_reads();
    }
    log_operation(static_cast<std::uint8_t>(Operation::Add), record);
    return result;
}

std::size_t Collection::remove(std::span<const std::string> ids) {
    auto& s = *state_;
    const std::lock_guard writer(s.writer);
    std::string record;
    if (s.log) {
        unsigned char count[8];
        io::put<std::uint64_t>(count, ids.size());
        record.append(reinterpret_cast<const char*>(count), sizeof count);
        for (const auto& id : ids) put_text(record, id);
    }
    if (!s.built) throw std::logic_error("Collection::remove requires built indexes.");
    if (s.bm25) s.bm25->prepare_for_changes();  // Outside the exclusive section.
    const bool documents = s.config.mode != SearchMode::Vector || s.jsonl_vectors;
    std::unordered_set<std::string_view> seen;
    for (const auto& id : ids)
        if (!seen.insert(id).second) throw InputDataError("ID repeated in the delete list: " + id);
    if (documents) {
        std::unordered_map<std::string_view, std::size_t> by_source;
        for (std::size_t i = 0; i < s.docs.size(); ++i) by_source.emplace(s.docs[i].id, i);
        std::vector<std::size_t> doomed;
        for (const auto& id : ids) {
            const auto found = by_source.find(id);
            if (found == by_source.end()) throw InputDataError("Unknown document ID: " + id);
            doomed.push_back(found->second);
        }
        std::unordered_set<std::size_t> gone(doomed.begin(), doomed.end());
        const std::unique_lock lock(s.state);
        for (std::size_t index : doomed) {
            const auto& doc = s.docs[index];
            if (s.store) s.store->erase(doc.vector_id);
            if (s.bm25) s.bm25->remove(doc.document_id);
        }
        std::vector<DocumentRecord> kept;
        kept.reserve(s.docs.size() - gone.size());
        for (std::size_t i = 0; i < s.docs.size(); ++i)
            if (!gone.contains(i)) kept.push_back(std::move(s.docs[i]));
        s.docs = std::move(kept);
        s.index_documents();
        s.binding.clear();
        for (const auto& doc : s.docs) s.binding.emplace(doc.vector_id, doc.document_id);
    } else {
        std::vector<VectorId> doomed;
        for (const auto& id : ids) {
            VectorId value = 0;
            const auto [end, error] = std::from_chars(id.data(), id.data() + id.size(), value);
            if (error != std::errc{} || end != id.data() + id.size() || !s.store->contains(value))
                throw InputDataError("Unknown vector ID: " + id);
            doomed.push_back(value);
        }
        {
            const std::unique_lock lock(s.state);
            for (VectorId id : doomed) s.store->erase(id);
            s.stats.vectors = s.store->live_size();
        }
        log_operation(static_cast<std::uint8_t>(Operation::Remove), record);
        return ids.size();
    }
    s.stats.vectors = s.store ? s.store->live_size() : 0;
    s.stats.documents = s.docs.size();
    log_operation(static_cast<std::uint8_t>(Operation::Remove), record);
    return ids.size();
}

void Collection::compact() {
    auto& s = *state_;
    const std::lock_guard writer(s.writer);
    if (!s.built) throw std::logic_error("Collection::compact requires built indexes.");
    const auto& c = s.config;
    // Build the compacted components while searches keep using the current ones.
    std::shared_ptr<VectorStore> store;
    std::unique_ptr<HnswIndex> hnsw;
    std::unique_ptr<QuantizedVectors> codes;
    std::unique_ptr<lexical::SegmentedBm25> bm25;
    CollectionStats stats = s.stats;
    if (s.store && s.store->deleted_count() != 0 && s.store->has_rows()) {
        store = s.store->compacted();
        if (s.hnsw) {
            const auto start = Clock::now();
            hnsw = std::make_unique<HnswIndex>(*store, s.hnsw->config());
            hnsw->insert_parallel(store->ids(), c.build_threads);
            stats.index_build_time = Clock::now() - start;
        }
        if (s.codes) {
            const auto start = Clock::now();
            codes = make_codes(c, *store);
            stats.quantization_time = Clock::now() - start;
            stats.code_bytes = codes->memory_bytes();
        }
        stats.fp32_bytes = store->size() * store->padded_dimension() * sizeof(float);
    }
    if (s.bm25) {
        const auto start = Clock::now();
        bm25 = s.bm25->compacted();
        (c.mode == SearchMode::Lexical ? stats.index_build_time : stats.lexical_build_time) =
            Clock::now() - start;
    }
    // Swap. The old components are destroyed after the lock is released, the graph before
    // the store it reads.
    std::shared_ptr<VectorStore> old_store;
    std::unique_ptr<HnswIndex> old_hnsw;
    std::unique_ptr<QuantizedVectors> old_codes;
    std::unique_ptr<lexical::SegmentedBm25> old_bm25;
    {
        const std::unique_lock lock(s.state);
        if (store) {
            old_hnsw = std::exchange(s.hnsw, std::move(hnsw));
            old_store = std::exchange(s.store, std::move(store));
            if (codes) old_codes = std::exchange(s.codes, std::move(codes));
        }
        if (bm25) old_bm25 = std::exchange(s.bm25, std::move(bm25));
        stats.vectors = s.store ? s.store->live_size() : 0;
        stats.documents = s.docs.size();
        s.stats = stats;
    }
    old_hnsw.reset();
    log_operation(static_cast<std::uint8_t>(Operation::Compact), {});
}

bool Collection::validate(std::string& report) const {
    const auto& s = *state_;
    const std::lock_guard writer(s.writer);  // A quiescent graph: no insertion in flight.
    const std::shared_lock lock(s.state);
    std::ostringstream out;
    bool ok = true;
    if (s.hnsw) {
        const auto graph = s.hnsw->validate_graph();
        out << "HNSW graph: " << graph.nodes << " nodes, "
            << (graph.valid() ? "structurally valid" : "INVALID") << '\n';
        ok = ok && graph.valid();
    }
    if (s.bm25)
        out << "BM25 index: " << s.bm25->live_count() << " live documents, "
            << s.bm25->deleted_count() << " deleted, " << s.bm25->segment_count()
            << " segment(s)\n";
    if (s.store && s.store->deleted_count() != 0)
        out << "Vectors: " << s.store->live_size() << " live, " << s.store->deleted_count()
            << " deleted\n";
    if (s.codes)
        out << "Quantized codes: " << s.codes->name() << ", " << s.codes->size() << " rows\n";
    report = out.str();
    return ok;
}

}  // namespace aetherindex
