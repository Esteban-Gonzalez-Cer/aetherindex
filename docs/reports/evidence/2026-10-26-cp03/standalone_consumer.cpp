#include <array>
#include <iomanip>
#include <iostream>
#include "aether/hybrid/rrf.hpp"

int main() {
    using aetherindex::DocumentId;
    const aetherindex::hybrid::RrfFusion fusion({.k_rrf = 60.0});
    const std::array<DocumentId, 3> dense = {101, 102, 103};
    const std::array<DocumentId, 3> lexical = {102, 104, 101};
    const auto results = fusion.fuse(dense, lexical, 10);
    for (const auto& result : results) {
        std::cout << result.document_id << ' ' << std::fixed
                  << std::setprecision(10) << result.score << '\n';
    }
    return results.size() == 4 && results.front().document_id == 102 ? 0 : 1;
}
