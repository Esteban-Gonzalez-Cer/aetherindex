#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <vector>

#include "../hnsw_test_helpers.hpp"

using namespace aetherindex;

void check_first_node_and_full_id_range() {
    VectorStore store(2);
    const std::array<float, 2> first = {1, 0};
    const std::array<float, 2> second = {0, 1};
    require(store.insert(0, first) == InsertStatus::Success, "ID-zero fixture insertion failed.");
    HnswIndex index(store);
    index.insert(0);
    require(index.entry_point() && *index.entry_point() == 0,
            "ID zero must be usable as the first entry point.");
    require(index.max_level() && *index.max_level() == index.find_node(0)->max_level,
            "First entry point and index maximum level must agree.");
    hnsw_test::check_graph(index, store, store.ids());
    for (std::size_t layer = 0; layer <= index.find_node(0)->max_level; ++layer) {
        require(index.neighbors(0, layer).empty(), "The first node cannot already have neighbors.");
    }
    hnsw_test::check_results(index, store, first, index.search(first, 1), 1);
    require(index.search(first, 1)[0].id == 0, "Singleton search must return its indexed ID.");

    const VectorId maximum = std::numeric_limits<VectorId>::max();
    require(store.insert(maximum, second) == InsertStatus::Success,
            "The full uint32 ID range must be supported.");
    index.insert(maximum);
    hnsw_test::check_graph(index, store, store.ids());
    const auto neighbors = index.search(second, 2);
    hnsw_test::check_results(index, store, second, neighbors, 2);
    require(neighbors[0].id == maximum, "Maximum uint32 ID must survive insertion and retrieval.");
}

void check_incremental_insertion_and_storage_reallocation() {
    const auto data = hnsw_test::embeddings(200, 8, 2026);
    VectorStore store(8);
    require(store.insert(hnsw_test::fixture_id(0), data[0]) == InsertStatus::Success,
            "Initial fixture insertion failed.");
    const auto initial_address = reinterpret_cast<std::uintptr_t>(store.find(10)->data());
    HnswIndex index(store);
    index.insert(10);
    const auto first_level = index.find_node(10)->max_level;
    for (std::size_t row = 1; row < 100; ++row) {
        const auto id = hnsw_test::fixture_id(row);
        require(store.insert(id, data[row]) == InsertStatus::Success,
                "Incremental store insertion failed.");
        index.insert(id);
    }
    hnsw_test::check_graph(index, store, store.ids());
    hnsw_test::check_results(index, store, data[31], index.search(data[31], 10), 10);
    const auto previous_entry = index.entry_point();
    std::vector<std::size_t> previous_levels;
    for (VectorId id : store.ids()) previous_levels.push_back(index.find_node(id)->max_level);

    for (std::size_t row = 100; row < data.size(); ++row) {
        const auto id = hnsw_test::fixture_id(row);
        require(store.insert(id, data[row]) == InsertStatus::Success,
                "Second-batch store insertion failed.");
        index.insert(id);
        // Check after every completed insertion, not just at the final graph size.
        hnsw_test::check_graph(index, store, store.ids());
    }
    require(index.size() == 200 && store.size() == 200, "The second batch must add 100 nodes.");
    require(reinterpret_cast<std::uintptr_t>(store.find(10)->data()) != initial_address,
            "This test must actually grow and relocate the embedding storage.");
    require(index.find_node(10)->max_level == first_level && previous_entry,
            "Incremental insertion must preserve the existing nodes.");
    for (std::size_t row = 0; row < 100; ++row) {
        require(index.find_node(hnsw_test::fixture_id(row))->max_level == previous_levels[row],
                "Adding nodes must not regenerate the existing nodes' levels.");
    }
    hnsw_test::check_results(index, store, data[137], index.search(data[137], 10), 10);
    for (std::size_t row = 0; row < data.size(); ++row) {
        const auto stored = *store.find(hnsw_test::fixture_id(row));
        const PreparedQuery expected(data[row], store.dimension(), store.distance_kind());
        require(std::equal(stored.begin(), stored.end(), expected.values().begin()),
                "HNSW insertion/search must not change normalized embedding components.");
    }

    std::vector<HnswNode> snapshot;
    std::vector<std::vector<std::vector<VectorId>>> edge_snapshot;
    for (VectorId id : store.ids()) snapshot.push_back(*index.find_node(id));
    for (VectorId id : store.ids()) edge_snapshot.push_back(hnsw_test::topology(index, id));
    const auto entry = index.entry_point();
    const auto max_level = index.max_level();
    requireInvalidArgument([&] { index.insert(10); }, "Duplicate index IDs must be rejected.");
    requireInvalidArgument([&] { index.insert(123456789); },
                           "IDs absent from VectorStore must be rejected.");
    require(index.entry_point() == entry && index.max_level() == max_level && index.size() == 200,
            "Rejected insertions must preserve entry point, levels and count.");
    for (std::size_t i = 0; i < snapshot.size(); ++i) {
        const auto& before = snapshot[i];
        const auto after = index.find_node(before.id);
        require(after->max_level == before.max_level &&
                    hnsw_test::topology(index, before.id) == edge_snapshot[i],
                "Rejected insertion must not modify graph topology.");
    }
    hnsw_test::check_graph(index, store, store.ids());
    hnsw_test::check_results(index, store, data[31], index.search(data[31], 10), 10);
    std::cout << "Incremental insertion: 100 + 100 nodes; storage reallocation exercised; VALID\n";
}

void check_failed_mutation_requires_rebuild() {
    VectorStore store(1, DistanceKind::SquaredEuclidean);
    const std::array<float, 1> huge = {std::numeric_limits<float>::max()};
    const std::array<float, 1> opposite = {-std::numeric_limits<float>::max()};
    require(store.insert(1, huge) == InsertStatus::Success &&
                store.insert(2, opposite) == InsertStatus::Success,
            "Extreme finite fixture insertion failed.");
    HnswIndex index(store);
    index.insert(1);
    requireOverflow([&] { index.insert(2); },
                    "HNSW insertion must propagate the shared distance overflow exception.");
    const auto report = index.validate_graph();
    require(!report.usable && !report.valid(),
            "An unexpected partial mutation must be reported as unusable, not valid.");
    hnsw_test::require_unusable([&] { index.search(huge, 1); });
    hnsw_test::require_unusable([&] { index.insert(1); });
    require(store.size() == 2 && store.contains(1) && store.contains(2),
            "Index failure must not delete or modify caller-owned embeddings.");
    HnswIndex rebuilt(store);
    rebuilt.insert(1);
    const std::array<VectorId, 1> indexed = {1};
    hnsw_test::check_graph(rebuilt, store, indexed);
    hnsw_test::check_results(rebuilt, store, huge, rebuilt.search(huge, 1), 1);
}

int main() {
    try {
        check_first_node_and_full_id_range();
        check_incremental_insertion_and_storage_reallocation();
        check_failed_mutation_requires_rebuild();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
