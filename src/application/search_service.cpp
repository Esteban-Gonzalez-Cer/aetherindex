#include "aether/application/search_service.hpp"

#include <fstream>
#include <memory>
#include <string_view>

#include "aether/application/collection.hpp"
#include "aether/application/json_loader.hpp"
#include "aether/hybrid/rrf.hpp"
#include "aether/lexical/bm25.hpp"
#include "aether/lexical/tokenizer.hpp"
#include "aether/storage/vector_file.hpp"
#include "csv_loader.hpp"
#include "request_util.hpp"

namespace aetherindex {
namespace {

using detail::avec_path;
using detail::json_string;
using detail::jsonl_path;
using detail::lower_extension;

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
            static_cast<void>(
                lexical::Tokenizer(request.bm25_config.analyzer).tokenize(*request.query_text));
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

void validate_build_request(const SearchRequest& request) {
    if (!request.query_path.empty() || request.query_text || request.query_id ||
        request.all_queries || !request.filters.empty() || request.candidate_k ||
        request.rerank || request.parallel_retrievers)
        throw std::invalid_argument("Query options are not part of an index build.");
    // The build fields are valid exactly when a search with a neutral query would accept
    // them: reuse the search rules with the query format each mode expects.
    SearchRequest probe = request;
    if (request.mode == SearchMode::Lexical)
        probe.query_text = std::string();
    else if (request.mode == SearchMode::Hybrid || jsonl_path(request.dataset_path))
        probe.query_path = "query.jsonl";
    else
        probe.query_path = "query.csv";
    validate_search_request(probe);
}

SearchResponse SearchService::search(const SearchRequest& request) const {
    validate_search_request(request);
    // Data first, then queries (checked against the data), then the expensive build.
    auto collection = Collection::load(request);
    auto queries = collection->load_queries(request);
    collection->build_indexes();
    return collection->search(request, queries);
}

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
