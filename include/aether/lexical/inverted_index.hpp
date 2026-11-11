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
#include "aether/lexical/tokenizer.hpp"

namespace aetherindex {
struct SnapshotAccess;
}  // namespace aetherindex

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
    explicit InvertedIndex(Analyzer analyzer = Analyzer::AsciiTechV1) : tokenizer_(analyzer) {}
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
    // Same, but first runs prepare on the staged index (readable as if built). If prepare
    // throws, nothing is published and the index is still collecting. With
    // release_postings, the raw postings are then dropped as by release_postings().
    void build(const std::function<void(const InvertedIndex&)>& prepare, bool release_postings);

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
    // Built index: every DocumentId, ascending.
    [[nodiscard]] std::vector<DocumentId> document_ids() const;
    // Visits every (term, postings) pair of the built index; order unspecified.
    void for_each_term(
        const std::function<void(std::string_view, std::span<const Posting>)>& visit) const;
    // Frees the raw postings after a compressed copy was built. Afterwards find(),
    // document_frequency(), vocabulary_size() and for_each_term() throw logic_error;
    // statistics and document lengths remain available. Irreversible.
    void release_postings();
    [[nodiscard]] bool has_postings() const noexcept { return !postings_released_; }

   private:
    friend struct ::aetherindex::SnapshotAccess;  // Index snapshots (aether_persistence).
    friend class Bm25Index;                       // Bm25Index::from_postings.

   public:
    struct TermHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
    };
   private:
    // While collecting, terms are interned: each document keeps (term id, frequency) pairs.
    struct DocumentTermData {
        std::uint32_t length;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> frequencies;
    };
    using InvertedIndexData =
        std::unordered_map<std::string, PostingList, TermHash, std::equal_to<>>;

    void require_built() const;

    Tokenizer tokenizer_;
    StemCache stems_;  // Collection-time only; cleared by build().
    std::unordered_map<std::string, std::uint32_t, TermHash, std::equal_to<>> term_ids_;
    std::vector<std::string_view> terms_;  // Term id -> text (keys of term_ids_).
    bool ready_ = false;
    bool postings_released_ = false;
    std::size_t document_count_ = 0;
    std::uint64_t total_tokens_ = 0;
    double average_length_ = 0.0;
    std::unordered_map<DocumentId, DocumentTermData> documents_;
    std::unordered_map<DocumentId, std::uint32_t> lengths_;
    InvertedIndexData postings_;
};

}  // namespace aetherindex::lexical
