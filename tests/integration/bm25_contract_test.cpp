#include <exception>
#include <iostream>
#include <type_traits>
#include <unordered_set>
#include <utility>

#include "../bm25_test_helpers.hpp"

using namespace aetherindex;
using namespace aetherindex::lexical;

// Contract consumer only: extracts one-based ranks without RRF or HNSW.
std::vector<std::pair<DocumentId, std::size_t>> consume(std::span<const RankedResult> results) {
    std::vector<std::pair<DocumentId, std::size_t>> ranks;
    std::unordered_set<DocumentId> seen;
    for (std::size_t i = 0; i < results.size(); ++i) {
        require(seen.insert(results[i].document_id).second, "Duplicate result ID.");
        require(std::isfinite(results[i].score) && results[i].score > 0, "Invalid BM25 score.");
        if (i > 0) {
            require(results[i - 1].score > results[i].score ||
                        (results[i - 1].score == results[i].score &&
                         results[i - 1].document_id < results[i].document_id),
                    "Not ordered by score descending and ID ascending.");
        }
        ranks.emplace_back(results[i].document_id, i + 1);
    }
    return ranks;
}

int main() {
    static_assert(std::is_same_v<decltype(RankedResult{}.document_id), DocumentId>);
    static_assert(std::is_same_v<decltype(RankedResult{}.score), double>);
    static_assert(std::is_same_v<DocumentId, std::uint32_t>);
    try {
        Bm25Index index;
        addBm25Reference(index);
        index.build();
        const auto results = index.search("motor e47", 10);
        const auto ranks = consume(results);
        require(ranks == std::vector<std::pair<DocumentId, std::size_t>>{{101, 1}, {102, 2}},
                "T11: consumer cannot extract one-based document ranks.");
        require(consume(index.search("absent", 10)).empty(), "Empty contract consumption failed.");
        std::cout << "T11: public RankedResult contract passed; RRF algorithm not executed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
