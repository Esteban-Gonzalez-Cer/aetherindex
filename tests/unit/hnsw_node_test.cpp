#include <array>
#include <exception>
#include <iostream>
#include <limits>
#include <type_traits>
#include <vector>

#include "../hnsw_test_helpers.hpp"

using namespace aetherindex;

void check_configuration_and_empty_graph() {
    static_assert(std::is_trivially_copyable_v<HnswNode>);
    const HnswNode node{std::numeric_limits<VectorId>::max(), 0, 3};
    require(node.id == std::numeric_limits<VectorId>::max() && node.internal_id == 0 &&
                node.max_level == 3,
            "Node diagnostics must contain only scalar ID/position/level values.");
    const HnswConfig defaults;
    require(defaults.m == 16 && defaults.m_max_0 == 32 && defaults.ef_construction == 100 &&
                defaults.ef_search == 50 && defaults.random_seed == 42 && defaults.max_level == 32,
            "The agreed MVP defaults changed.");
    VectorStore store(2);
    HnswIndex empty(store);
    hnsw_test::check_graph(empty, store, {});
    require(!empty.contains(0) && !empty.find_node(0), "An empty graph has no indexed IDs.");
    const std::array<float, 2> query = {1, 0};
    require(empty.search(query, 1).empty(), "A valid empty-index search must return no results.");
    requireInvalidArgument([&] { empty.search(query, 0); }, "Zero K must be rejected even empty.");
    requireInvalidArgument([&] { empty.search({}, 1); },
                           "Empty queries must be rejected even empty.");
    requireInvalidArgument([&] { empty.search(query, 1, 0); },
                           "Zero search width must be rejected even empty.");
    const std::array<float, 2> zero = {0, 0};
    requireInvalidArgument([&] { empty.search(zero, 1); }, "Zero cosine queries must be rejected.");
    for (float nonfinite :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
          -std::numeric_limits<float>::infinity()}) {
        const std::array<float, 2> invalid = {nonfinite, 1};
        requireInvalidArgument([&] { empty.search(invalid, 1); },
                               "Nonfinite queries must be rejected even on an empty graph.");
    }
    std::vector<HnswConfig> invalid;
    auto config = defaults;
    config.m = 0;
    invalid.push_back(config);
    config.m = 1;
    invalid.push_back(config);
    config = defaults;
    config.m_max_0 = 0;
    invalid.push_back(config);
    config.m_max_0 = defaults.m - 1;
    invalid.push_back(config);
    config = defaults;
    config.ef_construction = defaults.m - 1;
    invalid.push_back(config);
    config = defaults;
    config.ef_search = 0;
    invalid.push_back(config);
    config = defaults;
    config.max_level = 33;
    invalid.push_back(config);
    for (const auto& candidate : invalid) {
        requireInvalidArgument([&] { HnswIndex rejected(store, candidate); },
                               "Invalid configurations must be rejected without normalization.");
    }
    hnsw_test::check_graph(empty, store, {});
}

void check_reproducible_levels_and_topology() {
    const auto data = hnsw_test::embeddings(1000, 3, 123);
    VectorStore store(3);
    hnsw_test::populate(store, data);
    HnswIndex first(store);
    HnswIndex second(store);
    for (VectorId id : store.ids()) {
        first.insert(id);
        second.insert(id);
    }
    hnsw_test::check_graph(first, store, store.ids());
    hnsw_test::check_graph(second, store, store.ids());
    std::array<std::size_t, 33> levels{};
    for (VectorId id : store.ids()) {
        const auto lhs = first.find_node(id);
        const auto rhs = second.find_node(id);
        require(lhs->max_level == rhs->max_level &&
                    hnsw_test::topology(first, id) == hnsw_test::topology(second, id),
                "Same seed and insertion order must reproduce levels and topology.");
        ++levels[lhs->max_level];
    }
    require(levels[0] > 500 && levels[0] < 1000,
            "This fixed-seed sample must favor layer 0 while retaining upper layers.");
    require(first.entry_point() == second.entry_point() && first.max_level() == second.max_level(),
            "Reproducible graphs must have the same entry point and maximum level.");
    std::cout << "Level reproduction: nodes=1000 dimensions=3 dataset_seed=123 graph_seed=42\n";
    for (std::size_t level = 0; level <= *first.max_level(); ++level) {
        std::cout << "Level " << level << ": " << levels[level] << " nodes\n";
    }

    auto config = HnswConfig{};
    config.max_level = 0;
    HnswIndex layer_zero(store, config);
    for (std::size_t row = 0; row < 30; ++row) layer_zero.insert(hnsw_test::fixture_id(row));
    hnsw_test::check_graph(layer_zero, store, store.ids().first(30));
    require(layer_zero.max_level() && *layer_zero.max_level() == 0,
            "A level-zero cap must produce a base-layer-only graph.");
    hnsw_test::check_results(layer_zero, store, data[0], layer_zero.search(data[0], 10), 10);
}

int main() {
    try {
        check_configuration_and_empty_graph();
        check_reproducible_levels_and_topology();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
