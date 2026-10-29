#ifndef AETHERINDEX_APPLICATION_JSONL_LOADER_HPP_
#define AETHERINDEX_APPLICATION_JSONL_LOADER_HPP_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aether/application/filter.hpp"
#include "aether/storage/vector_store.hpp"

namespace aetherindex {

struct DocumentRecord {
    std::string id;
    std::string text;
    std::vector<float> vector;
    std::string title;
    DocumentId document_id = 0;
    // Explicit dense binding; JSONL vector import uses vector_id == document_id.
    VectorId vector_id = 0;
    // Optional "metadata" object: scalar values as text (numbers keep their syntax,
    // booleans become true/false, null entries are omitted). Used only by filters.
    Metadata metadata;
};

struct QueryRecord {
    std::string id;
    std::string text;
    std::vector<float> vector;
    // Evaluation labels from 'relevant_document_ids'; retrieval never reads them.
    std::vector<std::string> relevant_doc_ids;
    bool has_text = false;
};

namespace detail {

// Vector import requires finite, nonempty embeddings. Each embedding is stored
// under its document's DocumentId, so dense and lexical rankings share one identity
// system before fusion; insertion order still follows the file.
std::pair<std::shared_ptr<VectorStore>, std::vector<DocumentRecord>> load_jsonl_corpus(
    const std::string& path, DistanceKind metric = DistanceKind::Cosine);

// Text import reads title/text and identity, without requiring or converting
// embeddings. Numeric uint32 identities are preserved; opaque IDs get an explicit
// deterministic mapping, independent of input order, to unused DocumentIds.
std::vector<DocumentRecord> load_jsonl_documents(const std::filesystem::path& path);

// Query labels are never used by retrieval. read_vectors=false ignores embedding
// metadata; it does not fabricate vectors or require a model.
std::vector<QueryRecord> load_jsonl_queries(const std::string& path, bool read_vectors = true);

}  // namespace detail
}  // namespace aetherindex

#endif  // AETHERINDEX_APPLICATION_JSONL_LOADER_HPP_
