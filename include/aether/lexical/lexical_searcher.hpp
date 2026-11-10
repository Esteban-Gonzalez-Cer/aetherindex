#pragma once

#include <cstddef>
#include <functional>
#include <string_view>
#include <vector>

#include "aether/core/search_result.hpp"
#include "aether/core/types.hpp"

namespace aetherindex::lexical {

// What the application needs from a lexical index: the Top-K documents for a query text,
// optionally restricted to documents with allow(id) == true (an empty function allows all).
// Ranking: score descending, then DocumentId ascending.
class LexicalSearcher {
   public:
    virtual ~LexicalSearcher() = default;
    [[nodiscard]] virtual std::vector<RankedResult> search(
        std::string_view query, std::size_t k,
        const std::function<bool(DocumentId)>& allow) const = 0;
};

}  // namespace aetherindex::lexical
