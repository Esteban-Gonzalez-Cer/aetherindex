#include "aether/application/search_service.hpp"

#include <memory>
#include <utility>
#include <chrono>
#include <limits>
#include <string>

#include "aether/ann/hnsw/hnsw_index.hpp"
#include "aether/application/query_engine.hpp"
#include "aether/application/jsonl_loader.hpp"
#include "aether/lexical/bm25.hpp"
#include "aether/lexical/inverted_index.hpp"
#include "aether/lexical/tokenizer.hpp"
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
        case SearchAlgorithm::Bm25:
            return;
    }
    throw std::invalid_argument("Unsupported search algorithm.");
}

SearchResponse SearchService::search(const SearchRequest& request) const {
    validate_search_request(request);

    // 1. Detección del tipo de entrada (JSONL para evaluación híbrida o CSV para búsqueda vectorial simple)
    const bool is_jsonl = request.dataset_path.extension() == ".jsonl";

    // Búsqueda léxica BM25: requiere documentos y consultas JSONL.
if (request.algorithm == SearchAlgorithm::Bm25) {
    if (!is_jsonl) {
        throw std::invalid_argument(
            "BM25 requires a JSONL document corpus."
        );
    }

    // 1. Cargar documentos de texto.
    auto [store, docs] =
        detail::load_jsonl_corpus(request.dataset_path.string());

    // 2. Preparar el índice BM25 y la correspondencia de IDs.
    lexical::Bm25Index bm25;
    std::vector<std::string> document_ids;
    document_ids.reserve(docs.size());

    if (docs.size() >
        static_cast<std::size_t>(
            std::numeric_limits<DocumentId>::max()
        ) + 1ULL) {
        throw std::invalid_argument(
            "Too many documents for the internal DocumentId type."
        );
    }

    for (std::size_t i = 0; i < docs.size(); ++i) {
        const auto internal_id = static_cast<DocumentId>(i);

        bm25.add_document(internal_id, docs[i].text);
        document_ids.push_back(docs[i].id);
    }

    // 3. Construir el índice y medir su tiempo.
    const auto build_start = std::chrono::steady_clock::now();

    bm25.build();

    const auto build_time =
        std::chrono::steady_clock::now() - build_start;

    // 4. Cargar la consulta textual.
    const auto queries =
        detail::load_jsonl_queries(request.query_path.string());

    if (queries.empty()) {
        throw std::invalid_argument(
            "The JSONL query file contains no queries."
        );
    }

    // 5. Buscar y medir únicamente la consulta.
    const auto query_start = std::chrono::steady_clock::now();

    auto ranked_documents =
        bm25.search(queries.front().text, request.k);

    const auto query_time =
        std::chrono::steady_clock::now() - query_start;

    // 6. Devolver resultados BM25, sin confundirlos con distancias.
    return {
        bm25.document_count(),  // vectors_indexed: reutilizado como contador
        0,                       // dimension: no aplica a BM25
        {},                      // neighbors: no hay resultados vectoriales
        query_time,
        build_time,
        std::move(ranked_documents),
        std::move(document_ids)
    };
}

    std::shared_ptr<VectorStore> store;
    std::vector<DocumentRecord> docs;

    if (is_jsonl) {
        // Carga de corpus JSONL (Vectores + Texto)
        auto [parsed_store, parsed_docs] = detail::load_jsonl_corpus(request.dataset_path);
        store = parsed_store;
        docs = std::move(parsed_docs);
    } else {
        store = detail::load_csv_dataset(request.dataset_path, request.metric);
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

    // 3. Indexación Léxica BM25 (si los documentos contienen texto)
    InvertedIndex inverted_index;
    Tokenizer tokenizer;
    if (is_jsonl && !docs.empty()) {
        for (size_t i = 0; i < docs.size(); ++i) {
            auto tokens = tokenizer.tokenize(docs[i].text);
            inverted_index.add_document(i, tokens);
        }
    }

    // 4. Ejecución de la búsqueda
    const auto start = std::chrono::steady_clock::now();
    
    // Si es JSONL, procesamos las consultas estructuradas
    if (is_jsonl) {
        const auto queries = detail::load_jsonl_queries(request.query_path);
        const QueryEngine engine;

        // Ejecutar la consulta en los índices correspondientes
        // (Soporta combinación o ejecución híbrida según las configuraciones del QueryEngine)
        auto neighbors = engine.search(*store, queries[0].vector, request.k); 
        const auto elapsed = std::chrono::steady_clock::now() - start;

        return {store->size(), store->dimension(), std::move(neighbors), elapsed, build_time};
    }

    // Flujo por defecto para archivos CSV
    const auto query = detail::load_csv_query(request.query_path, store->dimension(), request.metric);
    const QueryEngine engine;
    auto neighbors = index ? engine.search(*index, query, request.k) : engine.search(*store, query, request.k);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    return {store->size(), store->dimension(), std::move(neighbors), elapsed, build_time};
}

}  // namespace aetherindex