#ifndef AETHERINDEX_APPLICATION_JSONL_LOADER_HPP_
#define AETHERINDEX_APPLICATION_JSONL_LOADER_HPP_

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include "aether/storage/vector_store.hpp"

namespace aetherindex {

struct DocumentRecord {
    std::string id;
    std::string text;
    std::vector<float> vector;
};

struct QueryRecord {
    std::string id;
    std::string text;
    std::vector<float> vector;
    std::vector<std::string> relevant_doc_ids; // Ground truth para RRF/BM25
};

namespace detail {

// Carga el corpus desde corpus.jsonl y llena tanto el VectorStore como la lista de textos
std::pair<std::shared_ptr<VectorStore>, std::vector<DocumentRecord>> load_jsonl_corpus(
    const std::string& path);

// Carga las consultas formateadas desde queries.jsonl
std::vector<QueryRecord> load_jsonl_queries(const std::string& path);

}  // namespace detail
}  // namespace aetherindex

#endif  // AETHERINDEX_APPLICATION_JSONL_LOADER_HPP_