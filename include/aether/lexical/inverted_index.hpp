#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "aether/core/types.hpp"

namespace aetherindex::lexical {

struct Posting {
    DocumentId document_id;
    std::uint32_t term_frequency;
};
using PostingList = std::vector<Posting>;

// Owns term frequencies while collecting, and immutable postings once built.
// No original document text or embeddings are retained. Noncopyable/nonmovable
// like VectorStore: read-only posting views belong to this owner's lifetime.
class InvertedIndex {
   public:
    InvertedIndex() = default;
    InvertedIndex(const InvertedIndex&) = delete;
    InvertedIndex& operator=(const InvertedIndex&) = delete;
    InvertedIndex(InvertedIndex&&) = delete;
    InvertedIndex& operator=(InvertedIndex&&) = delete;

    // Full uint32 ID range, including 0. Empty documents are counted with dl=0.
    // Duplicate ID/invalid UTF-8: invalid_argument. After build: logic_error.
    // Validation/allocation failures leave logical contents unchanged.
    void add_document(DocumentId id, std::string_view text);

    // Publishes only after all allocations/sorting succeed. A failed build is
    // still collecting and may be retried. Repeated build throws logic_error.
    void build();

    // A normalized term, not a query. Requires a built index (logic_error).
    // Missing term: empty view; views remain valid until owner destruction.
    [[nodiscard]] std::span<const Posting> find(std::string_view term) const;
    [[nodiscard]] std::size_t document_frequency(std::string_view term) const;
    [[nodiscard]] std::size_t vocabulary_size() const;

    [[nodiscard]] bool is_built() const noexcept;
    [[nodiscard]] std::size_t document_count() const noexcept;
    [[nodiscard]] std::uint64_t total_token_count() const noexcept;
    // Registered-document statistics are also readable while collecting.
    [[nodiscard]] double average_document_length() const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> document_length(DocumentId id) const;

   private:
    struct TermHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
    };
    struct DocumentTermData {
        std::uint32_t length;
        std::unordered_map<std::string, std::uint32_t> frequencies;
    };
    using InvertedIndexData =
        std::unordered_map<std::string, PostingList, TermHash, std::equal_to<>>;

    void require_built() const;

    bool ready_ = false;
    std::size_t document_count_ = 0;
    std::uint64_t total_tokens_ = 0;
    double average_length_ = 0.0;
    std::unordered_map<DocumentId, DocumentTermData> documents_;
    std::unordered_map<DocumentId, std::uint32_t> lengths_;
    InvertedIndexData postings_;
};

}  // namespace aetherindex::lexical
