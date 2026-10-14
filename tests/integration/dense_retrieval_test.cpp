#include <array>
#include <exception>
#include <iostream>

#include "../test_helpers.hpp"
#include "aether/ann/hnsw/hnsw_index.hpp"
#include "aether/application/query_engine.hpp"

using namespace aetherindex;

void checkOriginalExample() {
    VectorStore database(3, DistanceKind::SquaredEuclidean);
    const std::array<std::array<float, 3>, 4> embeddings = {{
        {1.0f, 0.0f, 3.0f},
        {0.9f, 0.1f, 4.0f},
        {0.0f, 1.0f, 10.0f},
        {-1.0f, 0.0f, 3.5f},
    }};
    for (std::size_t i = 0; i < embeddings.size(); ++i) {
        require(
            database.insert(static_cast<VectorId>(i + 1), embeddings[i]) == InsertStatus::Success,
            "Example insertion failed.");
    }
    const QueryEngine engine;
    const std::array<float, 3> query = {1.2f, 2.2f, 3.2f};
    const auto neighbors = engine.search(database, query, 3);
    require(neighbors.size() == 3, "The imported example must return three neighbors.");
    require(neighbors[0].id == 1 && neighbors[1].id == 2 && neighbors[2].id == 4,
            "The application must preserve the original example ranking.");
    requireNear(neighbors[0].distance, 4.92f, "Original first distance changed.");
    requireNear(neighbors[1].distance, 5.14f, "Original second distance changed.");
    requireNear(neighbors[2].distance, 9.77f, "Original third distance changed.");
    const std::array<float, 1> wrong_query = {1.0f};
    requireInvalidArgument([&] { engine.search(database, wrong_query, 3); },
                           "The application must propagate dimension errors.");
    require(engine.search(database, {}, 0).empty(), "The application must preserve k == 0.");
}

void checkMetricSelection() {
    VectorStore cosine(2);
    VectorStore squared(2, DistanceKind::SquaredEuclidean);
    const std::array<float, 2> aligned = {10.0f, 0.0f};
    const std::array<float, 2> nearby = {1.0f, 1.0f};
    const std::array<float, 2> query = {1.0f, 0.0f};
    for (VectorStore* store : {&cosine, &squared}) {
        require(store->insert(100, aligned) == InsertStatus::Success, "Insertion failed.");
        require(store->insert(200, nearby) == InsertStatus::Success, "Insertion failed.");
    }
    const QueryEngine engine;
    const auto dense_cosine = engine.search(cosine, query, 2);
    const auto dense_squared = engine.search(squared, query, 2);
    require(dense_cosine[0].id == 100 && dense_cosine[1].id == 200,
            "Cosine must rank by direction rather than magnitude.");
    require(dense_squared[0].id == 200 && dense_squared[1].id == 100,
            "Squared Euclidean must use the store's selected metric.");
    requireNear(dense_cosine[0].distance, 0.0f, "Cosine best distance is incorrect.");
    requireNear(dense_squared[0].distance, 1.0f, "Squared best distance is incorrect.");
    requireNear(dense_squared[1].distance, 81.0f, "Squared farthest distance is incorrect.");

    for (VectorStore* store : {&cosine, &squared}) {
        HnswIndex index(*store);
        for (VectorId id : store->ids()) index.insert(id);
        const auto exact = engine.search(*store, query, 2);
        const auto approximate = engine.search(index, query, 2);
        require(approximate.size() == exact.size(),
                "QueryEngine must forward searches to the prepared HNSW index.");
        for (std::size_t rank = 0; rank < exact.size(); ++rank) {
            require(approximate[rank].id == exact[rank].id,
                    "The prepared-index overload must preserve the configured metric ranking.");
            requireNear(approximate[rank].distance, exact[rank].distance,
                        "The prepared-index overload must return shared SearchResult distances.");
        }
        const std::array<float, 1> wrong_dimension = {1};
        requireInvalidArgument([&] { engine.search(index, wrong_dimension, 1); },
                               "QueryEngine must preserve HNSW dimension validation.");
        requireInvalidArgument([&] { engine.search(index, query, 0); },
                               "The HNSW overload must preserve its positive-K contract.");
        require(index.validate_graph().valid(), "Queries must leave the prepared graph valid.");
    }
}

int main() {
    try {
        checkOriginalExample();
        checkMetricSelection();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
