#include "aether/lexical/bm25.hpp"

int main() {
    aetherindex::lexical::Bm25Index index({.k1 = 1.2, .b = 0.75});
    index.add_document(101, "motor motor e47");
    index.add_document(102, "motor temperatura");
    index.add_document(103, "bomba");
    index.build();

    const auto results = index.search("e47", 10);
    // Un resultado: document_id=101, score≈0.8142733421.
    return results.size() == 1 && results.front().document_id == 101 ? 0 : 1;
}
