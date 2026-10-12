#include <array>
#include <exception>
#include <iostream>
#include <limits>

#include "../test_helpers.hpp"
#include "aether/ann/brute_force/brute_force_index.hpp"

int main() {
    using namespace aetherindex;
    try {
        VectorStore database(2, DistanceKind::SquaredEuclidean);
        const std::array<float, 2> far = {3.0f, 4.0f};
        const std::array<float, 2> zero = {0.0f, 0.0f};
        const std::array<float, 2> near = {-1.0f, 0.0f};
        require(database.insert(42, far) == InsertStatus::Success, "Setup insertion failed.");
        require(database.insert(7, zero) == InsertStatus::Success, "Setup insertion failed.");
        require(database.insert(99, near) == InsertStatus::Success, "Setup insertion failed.");
        const auto neighbors = bruteForceSearch(database, zero, 2);
        require(neighbors.size() == 2, "Search must return exactly k available neighbors.");
        require(neighbors[0].id == 7 && neighbors[1].id == 99,
                "Results must be sorted by distance and preserve IDs.");
        requireNear(neighbors[0].distance, 0.0f, "Exact matches must rank first.");
        requireNear(neighbors[1].distance, 1.0f, "Neighbor distance is incorrect.");
        const auto all = bruteForceSearch(database, zero, 100);
        require(all.size() == 3 && all[2].id == 42, "k must be capped to the database size.");
        requireNear(all[2].distance, 25.0f, "Farthest result distance is incorrect.");
        require(bruteForceSearch(database, zero, 1)[0].id == 7, "Nearest vector is incorrect.");
        VectorStore empty(2);
        require(bruteForceSearch(empty, {}, 3).empty(), "An empty database has no results.");
        require(bruteForceSearch(database, {}, 0).empty(),
                "k == 0 must return before validating the query.");
        const std::array<float, 1> invalid_dimension = {1.0f};
        requireInvalidArgument([&] { bruteForceSearch(database, invalid_dimension, 1); },
                               "Query dimension mismatch must be rejected.");
        const std::array<float, 2> invalid_numeric = {std::numeric_limits<float>::quiet_NaN(),
                                                      0.0f};
        requireInvalidArgument([&] { bruteForceSearch(database, invalid_numeric, 1); },
                               "Invalid numeric queries must be rejected.");

        VectorStore ties(1, DistanceKind::SquaredEuclidean);
        const std::array<float, 1> positive = {1.0f};
        const std::array<float, 1> negative = {-1.0f};
        const std::array<float, 1> query = {0.0f};
        require(ties.insert(10, positive) == InsertStatus::Success, "Setup insertion failed.");
        require(ties.insert(20, negative) == InsertStatus::Success, "Setup insertion failed.");
        const auto tied = bruteForceSearch(ties, query, 2);
        require(tied.size() == 2 && tied[0].id != tied[1].id, "Tied results must be retained.");
        requireNear(tied[0].distance, 1.0f, "First tied distance is incorrect.");
        requireNear(tied[1].distance, 1.0f, "Second tied distance is incorrect.");
        require(database.ids()[0] == 42 && (*database.find(42))[0] == 3.0f,
                "Search must not reorder or modify the database.");

        VectorStore cosine(2);
        require(cosine.insert(std::numeric_limits<VectorId>::max(), far) == InsertStatus::Success,
                "Setup insertion failed.");
        require(bruteForceSearch(cosine, far, 1)[0].id == std::numeric_limits<VectorId>::max(),
                "Search results must preserve the full uint32 ID range.");
        requireInvalidArgument([&] { bruteForceSearch(cosine, zero, 1); },
                               "Cosine searches must reject zero queries.");
        const float maximum = std::numeric_limits<float>::max();
        VectorStore extreme(1, DistanceKind::SquaredEuclidean);
        const std::array<float, 1> huge = {maximum};
        const std::array<float, 1> opposite = {-maximum};
        require(extreme.insert(1, huge) == InsertStatus::Success, "Setup insertion failed.");
        requireOverflow([&] { bruteForceSearch(extreme, opposite, 1); },
                        "Search must propagate numeric overflow.");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
