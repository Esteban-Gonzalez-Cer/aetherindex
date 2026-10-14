#include <array>
#include <exception>
#include <iostream>

#include "../hnsw_test_helpers.hpp"

using namespace aetherindex;

void check_nearest_neighbor_selection() {
    VectorStore store(2, DistanceKind::SquaredEuclidean);
    HnswConfig config;
    config.m = 2;
    config.m_max_0 = 4;
    config.max_level = 0;
    HnswIndex index(store, config);
    for (std::size_t row = 0; row < 20; ++row) {
        const std::array<float, 2> embedding = {static_cast<float>(row), 0};
        const auto id = hnsw_test::fixture_id(row);
        require(store.insert(id, embedding) == InsertStatus::Success,
                "Neighbor-selection fixture insertion failed.");
        index.insert(id);
    }
    const std::array<float, 2> query = {10.25f, 0};
    require(store.insert(999, query) == InsertStatus::Success,
            "Neighbor-selection target insertion failed.");
    index.insert(999);
    const std::vector<VectorId> expected = {hnsw_test::fixture_id(10), hnsw_test::fixture_id(11),
                                            hnsw_test::fixture_id(9), hnsw_test::fixture_id(12)};
    require(index.find_node(999)->neighbors[0] == expected,
            "Nearest-only selection must choose the four closest base-layer candidates in order.");
    hnsw_test::check_graph(index, store, store.ids());
}

int main() {
    try {
        check_nearest_neighbor_selection();
        const auto data = hnsw_test::embeddings(250, 4, 31415);
        VectorStore store(4);
        hnsw_test::populate(store, data);
        HnswConfig config;
        config.m = 4;
        config.m_max_0 = 8;
        config.ef_construction = 50;
        HnswIndex index(store, config);
        std::size_t inserted = 0;
        for (VectorId id : store.ids()) {
            index.insert(id);
            ++inserted;
            hnsw_test::check_graph(index, store, store.ids().first(inserted));
        }
        bool saturated_base = false;
        bool saturated_upper = false;
        bool base_exceeds_upper_limit = false;
        std::size_t edges = 0;
        for (VectorId id : store.ids()) {
            const auto* node = index.find_node(id);
            for (std::size_t layer = 0; layer < node->neighbors.size(); ++layer) {
                const auto degree = node->neighbors[layer].size();
                edges += degree;
                saturated_base = saturated_base || (layer == 0 && degree == config.m_max_0);
                saturated_upper = saturated_upper || (layer > 0 && degree == config.m);
                base_exceeds_upper_limit =
                    base_exceeds_upper_limit || (layer == 0 && degree > config.m);
            }
        }
        require(edges > store.size(), "The graph test must exercise substantial connectivity.");
        require(saturated_base && saturated_upper && base_exceeds_upper_limit,
                "This fixture must exercise pruning and distinct base/upper degree limits.");
        const auto queries = hnsw_test::embeddings(20, 4, 27182);
        for (const auto& query : queries) {
            hnsw_test::check_results(index, store, query, index.search(query, 10), 10);
        }
        hnsw_test::check_graph(index, store, store.ids());
        std::cout << "Graph validation: nodes=250 edges=" << edges
                  << " M=4 M0=8; all invariants VALID before/after queries\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
