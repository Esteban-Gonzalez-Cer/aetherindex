#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <span>
#include <unordered_set>
#include <vector>

#include "aether/ann/hnsw/hnsw_index.hpp"
#include "aether/distance/distance.hpp"
#include "test_helpers.hpp"

namespace hnsw_test {

using namespace aetherindex;

inline VectorId fixture_id(std::size_t row) { return static_cast<VectorId>(row * 7 + 10); }

inline std::vector<std::vector<float>> embeddings(std::size_t count, std::size_t dimension,
                                                  std::uint32_t seed) {
    std::mt19937 random(seed);
    std::vector<std::vector<float>> data(count, std::vector<float>(dimension));
    for (auto& vector : data) {
        // Raw generator integers avoid implementation-specific normal/uniform distributions.
        for (float& value : vector) {
            value = static_cast<float>(static_cast<int>(random() % 2000001) - 1000000) / 1000000.0f;
        }
        if (std::all_of(vector.begin(), vector.end(), [](float value) { return value == 0; })) {
            vector.front() = 1.0f;
        }
    }
    return data;
}

inline void populate(VectorStore& store, const std::vector<std::vector<float>>& data) {
    for (std::size_t row = 0; row < data.size(); ++row) {
        require(store.insert(fixture_id(row), data[row]) == InsertStatus::Success,
                "HNSW fixture insertion failed.");
    }
}

inline void check_graph(const HnswIndex& index, const VectorStore& store,
                        std::span<const VectorId> indexed_ids) {
    const auto report = index.validate_graph();
    require(report.valid() && report.usable, "Graph diagnostics report an invalid graph.");
    require(report.nodes == indexed_ids.size() && index.size() == indexed_ids.size(),
            "Graph node count must equal successful index insertions.");
    require(report.invalid_references == 0 && report.missing_embeddings == 0 &&
                report.invalid_levels == 0 && report.layer_violations == 0 &&
                report.self_references == 0 && report.duplicate_edges == 0 &&
                report.degree_violations == 0 && report.entry_point_violations == 0 &&
                report.max_level_violations == 0 && report.insertion_count_violations == 0,
            "Graph validation must report zero invariant violations.");
    if (indexed_ids.empty()) {
        require(!index.entry_point() && !index.max_level(),
                "An empty graph must not expose an entry point or maximum level.");
        return;
    }

    std::size_t actual_max_level = 0;
    for (VectorId id : indexed_ids) {
        require(index.contains(id) && store.contains(id), "Indexed IDs must exist in both owners.");
        const auto* node = index.find_node(id);
        require(node && node->id == id, "Node lookup must preserve the full vector ID.");
        require(node->max_level <= index.config().max_level,
                "A generated level exceeds the configured safety limit.");
        require(node->neighbors.size() == node->max_level + 1,
                "Every node must have all layers through its maximum level.");
        actual_max_level = std::max(actual_max_level, node->max_level);
        for (std::size_t layer = 0; layer < node->neighbors.size(); ++layer) {
            const auto limit = layer == 0 ? index.config().m_max_0 : index.config().m;
            require(node->neighbors[layer].size() <= limit, "Per-layer neighbor limit exceeded.");
            std::unordered_set<VectorId> seen;
            for (VectorId neighbor : node->neighbors[layer]) {
                require(neighbor != id && seen.insert(neighbor).second,
                        "A graph contains a self edge or duplicate neighbor.");
                const auto* target = index.find_node(neighbor);
                require(target && store.contains(neighbor), "An edge points to a missing node.");
                require(target->max_level >= layer && target->neighbors.size() > layer,
                        "An edge points to a node absent from its layer.");
            }
        }
    }
    require(index.max_level() && *index.max_level() == actual_max_level,
            "Index maximum level must match the highest indexed node.");
    require(index.entry_point() && index.contains(*index.entry_point()),
            "A populated graph needs an indexed entry point.");
    require(index.find_node(*index.entry_point())->max_level == actual_max_level,
            "Entry point must be present at the graph's highest layer.");
}

inline void check_results(const HnswIndex& index, const VectorStore& store,
                          std::span<const float> query, const std::vector<SearchResult>& results,
                          std::size_t expected_count) {
    require(results.size() == expected_count, "HNSW returned the wrong number of neighbors.");
    std::unordered_set<VectorId> seen;
    float previous = -std::numeric_limits<float>::infinity();
    for (const auto& result : results) {
        require(index.contains(result.id) && store.contains(result.id),
                "Search returned a missing or unindexed ID.");
        require(seen.insert(result.id).second, "Search returned duplicate IDs.");
        require(std::isfinite(result.distance) && result.distance >= previous,
                "Search distances must be finite and sorted in ascending order.");
        const auto expected =
            compute_distance(query, *store.find(result.id), store.distance_kind());
        require(result.distance == expected, "Search must use the shared Distance result exactly.");
        previous = result.distance;
    }
}

template <typename Function>
inline void require_unusable(Function function) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        throw std::runtime_error("A poisoned index must report a state error, not an input error.");
    } catch (const std::logic_error&) {
        return;
    }
    throw std::runtime_error("An index with a failed graph mutation must require rebuilding.");
}

}  // namespace hnsw_test
