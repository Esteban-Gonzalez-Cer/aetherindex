#include <array>
#include <exception>
#include <iostream>

#include "../test_helpers.hpp"
#include "aether/distance/distance.hpp"
#include "aether/storage/vector_store.hpp"

using namespace aetherindex;

void checkPublicConsumerContract() {
    VectorStore store(3);
    const std::array<float, 3> a = {1.0f, 0.0f, 0.0f};
    const std::array<float, 3> b = {0.0f, 1.0f, 0.0f};
    require(store.insert(10, a) == InsertStatus::Success, "Insertion failed.");
    require(store.insert(20, b) == InsertStatus::Success, "Insertion failed.");
    const auto lhs = *store.find(10);
    const auto rhs = *store.find(20);
    requireNear(compute_distance(lhs, rhs, store.distance_kind()), 1.0f,
                "Distances must accept store views directly.");
    require(lhs.data() == store.find(10)->data(), "Distance must not replace store memory.");
    // Future HNSW nodes can retain this ID and acquire a fresh view on each read.
    // This checks the public contract, not an HNSW implementation.
    const VectorId node_vector_id = 20;
    requireNear(compute_distance(a, *store.find(node_vector_id), store.distance_kind()), 1.0f,
                "An ID-only consumer must be able to calculate query-to-node distance.");
    require(store.ids().size() == store.size(), "Public traversal must cover all embeddings.");
}

int main() {
    try {
        checkPublicConsumerContract();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
