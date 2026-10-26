#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <type_traits>
#include <vector>

#include "../hnsw_test_helpers.hpp"
#include "aether/ann/brute_force/brute_force_index.hpp"
#include "aether/application/query_engine.hpp"
#include "aether/core/prepared_query.hpp"
#include "aether/distance/kernels.hpp"

using namespace aetherindex;

static_assert(!std::is_copy_constructible_v<PreparedQuery>);
static_assert(!std::is_move_constructible_v<PreparedQuery>);
static_assert(!std::is_copy_assignable_v<PreparedQuery>);
static_assert(!std::is_move_assignable_v<PreparedQuery>);

namespace {

long double traditional_cosine(std::span<const float> a, std::span<const float> b) {
    long double dot = 0, aa = 0, bb = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        dot += static_cast<long double>(a[i]) * b[i];
        aa += static_cast<long double>(a[i]) * a[i];
        bb += static_cast<long double>(b[i]) * b[i];
    }
    return 1 - std::clamp(dot / (std::sqrt(aa) * std::sqrt(bb)), -1.0L, 1.0L);
}

void check_layout_and_normalization() {
    for (const std::size_t dimension : {1u, 3u, 16u, 17u, 127u, 128u, 129u}) {
        const auto data = hnsw_test::embeddings(23, dimension, 42);
        for (DistanceKind metric : {DistanceKind::Cosine, DistanceKind::SquaredEuclidean}) {
            VectorStore store(dimension, metric);
            store.reserve(data.size());
            for (std::size_t row = 0; row < data.size(); ++row) {
                const auto original = data[row];
                require(
                    store.insert(hnsw_test::fixture_id(row), data[row]) == InsertStatus::Success,
                    "Aligned fixture insertion failed.");
                require(original == data[row], "Insertion must not change the caller's data.");
            }
            require(store.padded_dimension() % 16 == 0 && store.padded_dimension() >= dimension,
                    "Physical stride must round up to 16 floats.");
            for (std::size_t row = 0; row < data.size(); ++row) {
                const auto* pointer = store.data_by_position(row);
                require(reinterpret_cast<std::uintptr_t>(pointer) % 64 == 0,
                        "Every embedding must start on a 64-byte boundary.");
                require(store.find(hnsw_test::fixture_id(row))->data() == pointer &&
                            store.position_of(hnsw_test::fixture_id(row)) == row,
                        "ID and dense position views must address the same row.");
                for (std::size_t i = dimension; i < store.padded_dimension(); ++i) {
                    require(pointer[i] == 0.0f, "Padding must remain zero.");
                }
                long double norm_squared = 0;
                for (float value : data[row])
                    norm_squared += static_cast<long double>(value) * value;
                for (std::size_t i = 0; i < dimension; ++i) {
                    if (metric == DistanceKind::SquaredEuclidean) {
                        require(pointer[i] == data[row][i],
                                "Squared Euclidean must preserve every FP32 component.");
                    } else {
                        const auto expected = data[row][i] / std::sqrt(norm_squared);
                        require(std::abs(pointer[i] - expected) < 2e-7L,
                                "Stored normalization differs from independent long-double norm.");
                    }
                }
            }
            store.reserve(100);
            require(store.size() == 23, "Reserve must preserve logical contents.");
        }
    }
    VectorStore threshold(1);
    const std::array<float, 1> below{std::nextafter(1e-7f, 0.0f)}, boundary{1e-7f};
    require(threshold.insert(0, below) == InsertStatus::InvalidEmbedding && threshold.size() == 0,
            "Norm strictly below 1e-7 must be rejected without mutation.");
    require(
        threshold.insert(0, boundary) == InsertStatus::Success && (*threshold.find(0))[0] == 1.0f,
        "Norm exactly 1e-7 must be accepted and normalized.");
    VectorStore extremes(3);
    const float maximum = std::numeric_limits<float>::max();
    const std::array<float, 3> huge{maximum, -maximum, maximum};
    require(extremes.insert(0, huge) == InsertStatus::Success,
            "Large finite components must normalize without overflowing their norm.");
    const auto view = *extremes.find(0);
    long double norm = 0;
    for (float value : view) norm += static_cast<long double>(value) * value;
    require(std::abs(norm - 1) < 1e-6L, "Extreme normalized values must have unit squared norm.");
}

void check_numeric_parity() {
    long double maximum_error = 0;
    std::size_t comparisons = 0;
    for (const std::size_t dimension :
         {1u, 2u, 3u, 15u, 16u, 17u, 31u, 32u, 33u, 127u, 128u, 129u, 1024u, 4097u}) {
        const auto data = hnsw_test::embeddings(48, dimension, 42);
        VectorStore store(dimension);
        hnsw_test::populate(store, data);
        for (std::size_t row = 0; row < data.size(); ++row) {
            for (const std::size_t other :
                 {row, (row + 1) % data.size(), (row + 7) % data.size()}) {
                auto input = data[other];
                if (other == row)
                    for (float& value : input) value = -value;
                const PreparedQuery query(input, dimension, DistanceKind::Cosine);
                const float distance = detail::cosine_unit_kernel(
                    query.data(), store.data_by_position(row), store.padded_dimension());
                const long double error = std::abs(distance - traditional_cosine(input, data[row]));
                require(error < 1e-6L, "CA-1.2 requires strict absolute cosine error < 1e-6.");
                maximum_error = std::max(maximum_error, error);
                ++comparisons;
            }
        }
    }
    std::cout << "CA-1.2: " << comparisons
              << " comparisons; max absolute error=" << static_cast<double>(maximum_error)
              << " (<1e-6); backend=" << detail::distance_kernel_backend() << '\n';
}

void check_heap_results() {
    const auto data = hnsw_test::embeddings(1000, 37, 42);
    const auto queries = hnsw_test::embeddings(5, 37, 424242);
    QueryEngine engine;
    for (DistanceKind metric : {DistanceKind::Cosine, DistanceKind::SquaredEuclidean}) {
        VectorStore store(37, metric);
        hnsw_test::populate(store, data);
        for (const auto& raw : queries) {
            const PreparedQuery query(raw, 37, metric);
            std::vector<std::pair<float, VectorId>> expected;
            const auto kernel = detail::distance_kernel(metric);
            for (std::size_t row = 0; row < store.size(); ++row) {
                expected.emplace_back(
                    kernel(query.data(), store.data_by_position(row), store.padded_dimension()),
                    store.ids()[row]);
            }
            std::sort(expected.begin(), expected.end());
            for (std::size_t k : {1u, 10u, 999u, 1000u, 1001u}) {
                const auto result = engine.search(store, raw, k);
                require(result.size() == std::min(k, store.size()), "Heap returned incorrect K.");
                for (std::size_t rank = 0; rank < result.size(); ++rank) {
                    require(
                        result[rank].id == expected[rank].second &&
                            result[rank].distance == expected[rank].first,
                        "Heap must reproduce a full-sort oracle exactly, including tie ordering.");
                }
            }
        }
    }
    VectorStore tied(1);
    const std::array<float, 1> x{1};
    for (VectorId id : {100u, 0u, 99u, 1u}) tied.insert(id, x);
    const auto result = engine.search(tied, x, 2);
    require(result[0].id == 0 && result[1].id == 1, "Heap cutoff ties must use external ID order.");
}

void check_flat_graph_and_diversification() {
    static_assert(std::is_trivially_copyable_v<HnswNode>);
    FlatGraphLayer flat(100, 2);
    flat.resize(4);
    require(reinterpret_cast<std::uintptr_t>(flat.data()) % 64 == 0,
            "FlatGraph buffer must be aligned.");
    const std::array<InternalId, 2> links{3, 1};
    flat.set_neighbors(0, links);
    flat.set_neighbors(1, flat.get_neighbors(0));
    require(flat.get_degree(0) == 2 && flat.get_neighbors(1)[1] == 1,
            "FlatGraph must preserve fixed-stride neighbor views and alias-safe copies.");
    requireInvalidArgument(
        [&] {
            const std::array<InternalId, 3> invalid{0, 1, 2};
            flat.set_neighbors(0, invalid);
        },
        "A fixed slot must reject an excessive degree before writing.");
    require(flat.get_degree(0) == 2 && flat.get_neighbors(0)[0] == 3,
            "Rejected writes must preserve the slot.");
    for (NeighborSelection selection : {NeighborSelection::Simple, NeighborSelection::Heuristic}) {
        VectorStore store(2, DistanceKind::SquaredEuclidean);
        const std::array<std::array<float, 2>, 4> data{{{1, 0}, {1.1f, 0}, {-2, 0}, {0, 0}}};
        for (std::size_t i = 0; i < data.size(); ++i)
            store.insert(static_cast<VectorId>(i), data[i]);
        HnswConfig config;
        config.m = config.m_max_0 = 2;
        config.max_level = 0;
        config.neighbor_selection = selection;
        HnswIndex index(store, config);
        for (VectorId id : store.ids()) index.insert(id);
        const auto target = hnsw_test::neighbor_ids(index, 3, 0);
        const std::vector<VectorId> expected = selection == NeighborSelection::Simple
                                                   ? std::vector<VectorId>{0, 1}
                                                   : std::vector<VectorId>{0, 2};
        require(target == expected,
                "Algorithm 4 must replace a shadowed near candidate with a diverse one.");
        hnsw_test::check_graph(index, store, store.ids());
    }
    for (bool extend : {false, true}) {
        VectorStore store(1, DistanceKind::SquaredEuclidean);
        for (VectorId id = 0; id < 6; ++id) {
            const std::array<float, 1> value{static_cast<float>(id + 1)};
            store.insert(id, value);
        }
        const std::array<float, 1> target{0};
        store.insert(100, target);
        HnswConfig config;
        config.m = 2;
        config.m_max_0 = 4;
        config.ef_construction = 2;
        config.max_level = 0;
        config.extend_candidates = extend;
        HnswIndex index(store, config);
        for (VectorId id : store.ids()) index.insert(id);
        const auto adjacent = hnsw_test::neighbor_ids(index, 100, 0);
        require(adjacent.size() == (extend ? 4u : 2u),
                "Candidate extension must add one-hop neighbors outside the original "
                "efConstruction pool.");
    }
    for (bool fill : {false, true}) {
        VectorStore store(1, DistanceKind::SquaredEuclidean);
        for (VectorId id = 0; id < 3; ++id) {
            const std::array<float, 1> value{static_cast<float>(id + 1)};
            store.insert(id, value);
        }
        const std::array<float, 1> target{0};
        store.insert(100, target);
        HnswConfig config;
        config.m = config.m_max_0 = 2;
        config.max_level = 0;
        config.keep_pruned_connections = fill;
        HnswIndex index(store, config);
        for (VectorId id : store.ids()) index.insert(id);
        require(
            index.neighbors(100, 0).size() == (fill ? 2u : 1u),
            "keep_pruned_connections must fill spare slots with the closest discarded candidates.");
    }
    const auto data = hnsw_test::embeddings(80, 8, 42);
    for (bool extend : {false, true})
        for (bool fill : {false, true}) {
            VectorStore store(8);
            hnsw_test::populate(store, data);
            HnswConfig config;
            config.m = 4;
            config.m_max_0 = 8;
            config.extend_candidates = extend;
            config.keep_pruned_connections = fill;
            HnswIndex index(store, config);
            // Reverse order exercises dense holes and nonsequential external IDs.
            for (std::size_t i = store.size(); i > 0; --i) index.insert(store.ids()[i - 1]);
            hnsw_test::check_graph(index, store, store.ids());
            const auto result = index.search(data[10], 10, 80);
            require(!result.empty() && result.front().id == hnsw_test::fixture_id(10),
                    "Algorithm 4 options must retain self-neighbor retrieval.");
            require(index.memory_usage().allocation_blocks == 4,
                    "Persistent graph topology must use four global buffers, not allocations per "
                    "node.");
        }
}

}  // namespace

int main() {
    try {
        check_layout_and_normalization();
        check_numeric_parity();
        check_heap_results();
        check_flat_graph_and_diversification();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
