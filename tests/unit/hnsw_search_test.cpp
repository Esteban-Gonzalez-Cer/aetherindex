#include <array>
#include <exception>
#include <iostream>
#include <limits>
#include <vector>

#include "../hnsw_test_helpers.hpp"

using namespace aetherindex;

void check_known_cosine_ranking() {
    VectorStore store(2);
    const std::array<VectorId, 4> ids = {10, 20, 30, 40};
    const std::array<std::array<float, 2>, 4> data = {{{1, 0}, {0.8f, 0.6f}, {0, 1}, {-1, 0}}};
    for (std::size_t row = 0; row < ids.size(); ++row) {
        require(store.insert(ids[row], data[row]) == InsertStatus::Success,
                "Known cosine fixture insertion failed.");
    }
    HnswIndex index(store);
    for (VectorId id : ids) index.insert(id);
    const std::array<float, 2> query = {1, 0};
    const auto results = index.search(query, 4);
    hnsw_test::check_results(index, store, query, results, 4);
    const std::array<float, 4> expected = {0, 0.2f, 1, 2};
    for (std::size_t rank = 0; rank < ids.size(); ++rank) {
        require(results[rank].id == ids[rank], "Known cosine ranking is incorrect.");
        requireNear(results[rank].distance, expected[rank],
                    "Known cosine distances must match with absolute error < 1e-5.");
    }
    hnsw_test::check_graph(index, store, ids);
}

void check_metric_selection_and_known_squared_distance() {
    const std::array<float, 2> aligned = {10, 0};
    const std::array<float, 2> close = {1, 1};
    const std::array<float, 2> query = {1, 0};
    for (DistanceKind metric : {DistanceKind::Cosine, DistanceKind::SquaredEuclidean}) {
        VectorStore store(2, metric);
        require(store.insert(10, aligned) == InsertStatus::Success &&
                    store.insert(20, close) == InsertStatus::Success,
                "Metric comparison fixture insertion failed.");
        HnswIndex index(store);
        index.insert(10);
        index.insert(20);
        const auto results = index.search(query, 2);
        hnsw_test::check_results(index, store, query, results, 2);
        require(results[0].id == (metric == DistanceKind::Cosine ? 10u : 20u),
                "HNSW must respect the store metric when rankings differ.");
        hnsw_test::check_graph(index, store, store.ids());
    }
    VectorStore squared(2, DistanceKind::SquaredEuclidean);
    const std::array<float, 2> zero = {0, 0};
    const std::array<float, 2> far = {3, 4};
    require(squared.insert(0, zero) == InsertStatus::Success &&
                squared.insert(42, far) == InsertStatus::Success,
            "Squared Euclidean must accept zero embeddings.");
    HnswIndex index(squared);
    index.insert(0);
    index.insert(42);
    const auto results = index.search(zero, 2);
    hnsw_test::check_results(index, squared, zero, results, 2);
    require(results[0].id == 0 && results[1].id == 42, "Known squared ranking is incorrect.");
    requireNear(results[0].distance, 0, "Zero squared distance must be preserved.");
    requireNear(results[1].distance, 25, "Squared distance must be 25, without a square root.");
}

void check_k_values_and_recoverable_errors() {
    const auto data = hnsw_test::embeddings(25, 3, 55);
    const auto queries = hnsw_test::embeddings(1, 3, 66);
    for (DistanceKind metric : {DistanceKind::Cosine, DistanceKind::SquaredEuclidean}) {
        VectorStore store(3, metric);
        hnsw_test::populate(store, data);
        HnswConfig config;
        config.m = 32;
        config.m_max_0 = 64;
        config.ef_search = 1;
        HnswIndex index(store, config);
        for (VectorId id : store.ids()) index.insert(id);
        const auto original_query = queries[0];
        auto query = original_query;
        for (std::size_t k : {std::size_t{1}, std::size_t{5}, std::size_t{10}, std::size_t{20},
                              std::size_t{100}, std::numeric_limits<std::size_t>::max()}) {
            const auto results = index.search(query, k);
            hnsw_test::check_results(index, store, query, results, std::min(k, store.size()));
            require(query == original_query, "Searching must not mutate the query embedding.");
        }
        requireInvalidArgument([&] { index.search(query, 0); }, "Zero K must be rejected.");
        requireInvalidArgument([&] { index.search(query, 10, 0); }, "Zero efSearch is invalid.");
        requireInvalidArgument([&] { index.search({}, 1); }, "Empty query dimensions are invalid.");
        const std::array<float, 2> short_query = {1, 0};
        const std::array<float, 4> long_query = {1, 0, 0, 0};
        requireInvalidArgument([&] { index.search(short_query, 1); },
                               "A short query must produce a controlled dimension error.");
        requireInvalidArgument([&] { index.search(long_query, 1); },
                               "A long query must produce a controlled dimension error.");
        for (float value :
             {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
              -std::numeric_limits<float>::infinity()}) {
            const std::array<float, 3> invalid = {1, value, 0};
            requireInvalidArgument([&] { index.search(invalid, 1); },
                                   "Nonfinite queries must produce controlled errors.");
        }
        const std::array<float, 3> zero = {0, 0, 0};
        if (metric == DistanceKind::Cosine) {
            requireInvalidArgument([&] { index.search(zero, 1); },
                                   "Zero cosine queries must be rejected.");
        } else {
            hnsw_test::check_results(index, store, zero, index.search(zero, 10), 10);
        }
        hnsw_test::check_graph(index, store, store.ids());
        hnsw_test::check_results(index, store, query, index.search(query, 10), 10);
    }
}

void check_extreme_finite_inputs_and_search_overflow() {
    VectorStore cosine(2);
    const float maximum = std::numeric_limits<float>::max();
    const std::array<float, 2> positive = {maximum, 0};
    const std::array<float, 2> perpendicular = {0, maximum};
    const std::array<float, 2> negative = {-maximum, 0};
    require(cosine.insert(10, positive) == InsertStatus::Success &&
                cosine.insert(20, perpendicular) == InsertStatus::Success &&
                cosine.insert(30, negative) == InsertStatus::Success,
            "Finite extreme cosine inputs must be accepted.");
    HnswIndex index(cosine);
    for (VectorId id : cosine.ids()) index.insert(id);
    const auto results = index.search(positive, 3);
    hnsw_test::check_results(index, cosine, positive, results, 3);
    requireNear(results[0].distance, 0, "Extreme same-direction cosine distance must be zero.");
    requireNear(results[1].distance, 1, "Extreme perpendicular cosine distance must be one.");
    requireNear(results[2].distance, 2, "Extreme opposite cosine distance must be two.");
    const std::array<float, 2> tiny = {std::numeric_limits<float>::denorm_min(), 0};
    const auto tiny_results = index.search(tiny, 3);
    hnsw_test::check_results(index, cosine, tiny, tiny_results, 3);
    requireNear(tiny_results[0].distance, 0,
                "Nonzero subnormal queries must retain cosine meaning.");

    VectorStore squared(1, DistanceKind::SquaredEuclidean);
    const std::array<float, 1> huge = {maximum};
    const std::array<float, 1> opposite = {-maximum};
    require(squared.insert(1, huge) == InsertStatus::Success, "Extreme squared setup failed.");
    HnswIndex squared_index(squared);
    squared_index.insert(1);
    requireOverflow([&] { squared_index.search(opposite, 1); },
                    "Search must propagate overflow from the shared Distance module.");
    hnsw_test::check_graph(squared_index, squared, squared.ids());
    hnsw_test::check_results(squared_index, squared, huge, squared_index.search(huge, 1), 1);
}

void check_ties_are_valid_and_reproducible() {
    VectorStore store(2);
    const std::array<float, 2> query = {1, 0};
    for (VectorId id : {50u, 3u, 100u, 1u}) {
        require(store.insert(id, query) == InsertStatus::Success, "Tie fixture insertion failed.");
    }
    HnswIndex first(store);
    HnswIndex second(store);
    for (VectorId id : store.ids()) {
        first.insert(id);
        second.insert(id);
    }
    const auto lhs = first.search(query, 4);
    const auto rhs = second.search(query, 4);
    hnsw_test::check_results(first, store, query, lhs, 4);
    hnsw_test::check_results(second, store, query, rhs, 4);
    for (std::size_t rank = 0; rank < lhs.size(); ++rank) {
        requireNear(lhs[rank].distance, 0, "Tied exact matches must have zero distance.");
        require(lhs[rank].id == rhs[rank].id, "Tie handling must be reproducible.");
    }
}

int main() {
    try {
        check_known_cosine_ranking();
        check_metric_selection_and_known_squared_distance();
        check_k_values_and_recoverable_errors();
        check_extreme_finite_inputs_and_search_overflow();
        check_ties_are_valid_and_reproducible();
        std::cout
            << "HNSW search: known cosine 0/0.2/1/2; squared 25; both metrics; edge cases PASS\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
