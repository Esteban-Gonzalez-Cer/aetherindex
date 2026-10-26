#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
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

    [[nodiscard]] bool is_built() const noexcept;
    [[nodiscard]] std::size_t document_count() const noexcept;
    [[nodiscard]] double average_document_length() const noexcept;
    [[nodiscard]] const InvertedIndex& inverted_index() const noexcept;
    [[nodiscard]] Bm25Config config() const noexcept;

   private:
    const Bm25Config config_;
    InvertedIndex index_;
};

}  // namespace aetherindex::lexical
