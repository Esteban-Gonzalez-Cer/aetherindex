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
    // A block of up to 128 postings, bit-packed at fixed widths in postings_: the ordinal
    // deltas (count * doc_bits bits, the first relative to the previous block's last
    // ordinal), then tf - 1 (count * tf_bits bits; 0 bits when every tf is 1).
    struct Block {
        std::uint32_t last;    // Last document ordinal in the block.
        std::uint32_t offset;  // Byte offset of the block in postings_.
        float max_score;       // Largest term score in the block, rounded up (a safe bound).
        std::uint16_t count;   // Postings in the block (<= 128).
        std::uint8_t doc_bits;
        std::uint8_t tf_bits;
    };
    // A term's blocks are blocks_[first_block, first_block + ceil(df / 128)).
    struct TermLayout {
        std::uint32_t df;
        std::uint32_t first_block;
        float max_score;  // Largest block bound of the term (a safe bound).
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
    void build_layout(const InvertedIndex& built, PostingMemory& memory);
    using Allow = std::function<bool(DocumentId)>;
    std::vector<RankedResult> search_exhaustive(const std::vector<QueryTerm>& terms,
                                                std::size_t k, const Allow& allow) const;
    std::vector<RankedResult> search_term_at_a_time(const std::vector<QueryTerm>& terms,
                                                    std::size_t k, const Allow& allow) const;
    std::vector<RankedResult> search_wand(const std::vector<QueryTerm>& terms, std::size_t k,
                                          bool block_max, const Allow& allow) const;
    std::vector<RankedResult> search_maxscore(const std::vector<QueryTerm>& terms, std::size_t k,
                                              const Allow& allow) const;

    const Bm25Config config_;
    InvertedIndex index_;
    std::unordered_map<std::string, TermLayout, TermHash, std::equal_to<>> layout_;
    std::vector<Block> blocks_;
    std::vector<std::uint8_t> postings_;  // Padded so 8-byte reads never leave it.
    std::vector<DocumentId> document_ids_;  // Ordinal -> DocumentId, ascending.
    std::vector<double> length_norm_;       // Ordinal -> BM25 length normalization.
    PostingMemory memory_;
};

}  // namespace aetherindex::lexical
