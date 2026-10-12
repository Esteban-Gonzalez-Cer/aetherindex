
#include "aether/storage/vector_store.hpp"
#include "hnsw/HNSWIndex.h"

#include <iostream>
#include <vector>

using namespace aetherindex;

int main() {
    VectorStore store(2, DistanceKind::SquaredEuclidean);

    const std::vector<std::vector<EmbeddingValue>> data = {
        {0.0f, 0.0f},
        {1.0f, 1.0f},
        {5.0f, 5.0f},
        {1.1f, 1.0f},
        {8.0f, 8.0f}
    };

    for (std::size_t i = 0; i < data.size(); ++i) {
        const auto status = store.insert(
            static_cast<VectorId>(i),
            data[i]
        );

        if (status != InsertStatus::Success) {
            std::cerr << "Error al insertar en VectorStore\n";
            return 1;
        }
    }

    HNSWIndex index(store, 2, 50, 20);

    for (std::size_t i = 0; i < data.size(); ++i) {
        index.insert(static_cast<VectorId>(i));
    }

    const std::vector<EmbeddingValue> query = {
        1.0f, 0.9f
    };

    const auto results = index.search(query, 3);

    std::cout << "Vectores almacenados: "
              << store.size() << '\n';

    std::cout << "Vectores indexados: "
              << index.size() << '\n';

    std::cout << "Resultados:\n";

    for (VectorId id : results) {
        std::cout << "ID: " << id << '\n';
    }

    return 0;
}
