#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "aether/core/search_result.hpp"
#include "aether/lexical/bm25_config.hpp"
#include "aether/lexical/inverted_index.hpp"

namespace aetherindex::lexical {

// Pure scoring functions, also usable for independent mathematical checks.
// Invalid counts/configuration/statistics throw invalid_argument; a result
// outside finite double range throws overflow_error.
[[nodiscard]] double compute_idf(std::size_t document_count, std::size_t document_frequency);
[[nodiscard]] double bm25_term_score(std::uint32_t tf, std::uint32_t document_length, double avgdl,
                                     double idf, const Bm25Config& config = {});

namespace detail {
// Shared arithmetic of bm25_term_score after validation, used by every strategy so that
// all of them produce bitwise-identical scores.
double bm25_length_norm(std::uint32_t document_length, double avgdl, const Bm25Config& config);
double bm25_score_from_norm(std::uint32_t tf, double normalized, double idf,
                            const Bm25Config& config);
}  // namespace detail

// Bytes held by postings: raw (vector<Posting>) and compressed block layout.
struct PostingMemory {
    std::size_t raw_bytes = 0;
    std::size_t compressed_bytes = 0;
    std::size_t postings = 0;
    std::size_t blocks = 0;
};

class Bm25Index {
   public:
    explicit Bm25Index(Bm25Config config = {});

    void add_document(DocumentId id, std::string_view text);
    void build();

    // Requires build() even for an empty query. k=0: invalid_argument.
    // Unique, lexicographically sorted query terms; postings-only candidates.
    // Empty/separator-only/OOV query or empty corpus: empty ranking.
    // Sort by score descending, then DocumentId ascending; never pad to k.
    [[nodiscard]] std::vector<RankedResult> search(std::string_view query, std::size_t k) const;
    // Same ranking restricted to documents with allow(id) == true (scores unchanged;
    // excluded documents are never returned). An empty function allows every document.
    [[nodiscard]] std::vector<RankedResult> search(
        std::string_view query, std::size_t k, const std::function<bool(DocumentId)>& allow) const;

    [[nodiscard]] bool is_built() const noexcept;
    [[nodiscard]] std::size_t document_count() const noexcept;
    [[nodiscard]] double average_document_length() const noexcept;
    [[nodiscard]] const InvertedIndex& inverted_index() const noexcept;
    [[nodiscard]] Bm25Config config() const noexcept;
    // Measured after build(); raw_bytes is the size before compressed strategies release
    // the raw postings.
    [[nodiscard]] PostingMemory posting_memory() const noexcept;

   private:
    struct Block {
        std::uint32_t last;    // Last document ordinal in the block.
        std::uint32_t offset;  // Byte offset of the block in TermLayout::bytes.
        std::uint32_t count;   // Postings in the block (<= 128).
        float max_score;       // Largest term score in the block, rounded up (a safe bound).
    };
    struct TermLayout {
        double idf = 0;
        double max_score = 0;
        std::vector<Block> blocks;
        // Per posting: varint (delta << 1 | tf==1), then varint tf only when tf != 1.
        std::vector<std::uint8_t> bytes;
    };
    struct TermHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
    };
    class Cursor;
    // A distinct analyzed query term and the factor applied to its contribution
    // (1 under QueryTermWeighting::Unique, its query occurrences under Frequency).
    struct QueryTerm {
        std::string text;
        double weight;
    };

    std::vector<QueryTerm> query_terms(std::string_view query) const;
    void build_layout();
    using Allow = std::function<bool(DocumentId)>;
    std::vector<RankedResult> search_exhaustive(const std::vector<QueryTerm>& terms,
                                                std::size_t k, const Allow& allow) const;
    std::vector<RankedResult> search_term_at_a_time(const std::vector<QueryTerm>& terms,
                                                    std::size_t k, const Allow& allow) const;
    std::vector<RankedResult> search_wand(const std::vector<QueryTerm>& terms, std::size_t k,
                                          bool block_max, const Allow& allow) const;

    const Bm25Config config_;
    InvertedIndex index_;
    std::unordered_map<std::string, TermLayout, TermHash, std::equal_to<>> layout_;
    std::vector<DocumentId> document_ids_;  // Ordinal -> DocumentId, ascending.
    std::vector<double> length_norm_;       // Ordinal -> BM25 length normalization.
    PostingMemory memory_;
};

}  // namespace aetherindex::lexical
