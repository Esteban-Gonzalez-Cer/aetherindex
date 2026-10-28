#include <array>
#include <cmath>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

#include "../test_helpers.hpp"
#include "aether/hybrid/rrf.hpp"

using namespace aetherindex;
using namespace aetherindex::hybrid;

namespace {
// Test-only stand-in for CP-04's application adapter. IDs deliberately differ;
// no HNSW/BM25 classes or QueryEngine implementation are included or linked.
std::vector<DocumentId> prepared_dense_ids(
    std::span<const SearchResult> rows, const std::unordered_map<VectorId, DocumentId>& binding) {
    std::vector<DocumentId> result;
    for (const auto& row : rows) {
        const auto found = binding.find(row.id);
        if (found == binding.end())
            throw std::invalid_argument("Unmapped VectorId in test adapter.");
        result.push_back(found->second);
    }
    return result;
}

std::vector<DocumentId> prepared_lexical_ids(std::span<const RankedResult> rows) {
    std::vector<DocumentId> result;
    for (const auto& row : rows) result.push_back(row.document_id);
    return result;
}

// Simulates a consumer of the public response type, not a hybrid QueryEngine.
void consume(std::span<const RankedResult> result) {
    const std::array<DocumentId, 4> ids = {102, 101, 104, 103};
    const std::array<double, 4> scores = {1.0 / 62 + 1.0 / 61, 1.0 / 61 + 1.0 / 63, 1.0 / 62,
                                          1.0 / 63};
    require(result.size() == ids.size(), "Consumer received wrong union size.");
    for (std::size_t rank = 1; rank <= result.size(); ++rank) {
        const auto& row = result[rank - 1];
        require(row.document_id == ids[rank - 1], "Consumer received wrong document rank.");
        require(std::isfinite(row.score) && std::abs(row.score - scores[rank - 1]) <= 1e-12,
                "Consumer received an incorrect RRF score.");
    }
}
}  // namespace

int main() {
    static_assert(std::is_same_v<decltype(RankedResult{}.document_id), DocumentId>);
    static_assert(std::is_same_v<decltype(RankedResult{}.score), double>);
    static_assert(std::is_same_v<DocumentId, std::uint32_t>);
    static_assert(std::is_same_v<decltype(RrfFusion{}.fuse({}, {}, 1)), std::vector<RankedResult>>);
    try {
        const RrfFusion fusion;
        std::vector<SearchResult> dense = {{9010, 0.001f}, {7000, 0.01f}, {42, 0.9f}};
        std::vector<RankedResult> lexical = {{102, 7.4}, {104, 5.1}, {101, 2.8}};
        const std::unordered_map<VectorId, DocumentId> binding = {
            {9010, 101}, {7000, 102}, {42, 103}};
        const auto dense_ids = prepared_dense_ids(dense, binding);
        const auto lexical_ids = prepared_lexical_ids(lexical);
        require(dense_ids == std::vector<DocumentId>{101, 102, 103},
                "ID conversion changed rank order.");
        require(lexical_ids == std::vector<DocumentId>{102, 104, 101},
                "Lexical extraction changed rank.");
        const auto original = fusion.fuse(dense_ids, lexical_ids, 10);
        consume(original);
        // A monotone rescaling leaves both input orders intact. Raw scores do
        // not reach RRF, so only the rank positions can affect the result.
        for (auto& row : dense) row.distance *= 1000;
        for (auto& row : lexical) row.score *= 1e-6;
        const auto rescaled =
            fusion.fuse(prepared_dense_ids(dense, binding), prepared_lexical_ids(lexical), 10);
        consume(rescaled);
        for (std::size_t i = 0; i < original.size(); ++i) {
            require(original[i].score == rescaled[i].score,
                    "Original retrieval scores affected RRF.");
        }
        requireInvalidArgument([&] { static_cast<void>(prepared_dense_ids(dense, {})); },
                               "Missing mapping must fail before calling RRF.");
        const auto collapsed = prepared_dense_ids(dense, {{9010, 101}, {7000, 101}, {42, 103}});
        requireInvalidArgument([&] { static_cast<void>(fusion.fuse(collapsed, lexical_ids, 10)); },
                               "Duplicated document after mapping must not be silently merged.");
        consume(fusion.fuse(dense_ids, lexical_ids, 10));
        std::cout
            << "T10: public core types, explicit VectorId mapping, rank-only fusion and consumer "
               "passed.\n"
               "Contract fixture only: no HNSW/BM25 queries or hybrid QueryEngine executed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
