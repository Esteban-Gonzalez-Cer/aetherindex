#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>

#include "aether/application/query_engine.hpp"

int main() {
    using namespace aetherindex;

    const std::array<EmbeddingValue, 3> query = {1.2f, 2.2f, 3.2f};
    const std::size_t k = 3;

    try {
        VectorStore database(3, DistanceKind::SquaredEuclidean);
        const std::array<std::array<EmbeddingValue, 3>, 4> embeddings = {{
            {1.0f, 0.0f, 3.0f},
            {0.9f, 0.1f, 4.0f},
            {0.0f, 1.0f, 10.0f},
            {-1.0f, 0.0f, 3.5f},
        }};
        for (std::size_t i = 0; i < embeddings.size(); ++i) {
            if (database.insert(static_cast<VectorId>(i + 1), embeddings[i]) !=
                InsertStatus::Success) {
                throw std::runtime_error("No se pudo insertar el embedding de ejemplo.");
            }
        }
        const QueryEngine engine;
        const auto neighbors = engine.search(database, query, k);

        std::cout << "=== AetherIndex ===\n";
        std::cout << "Busqueda por fuerza bruta\n\n";
        std::cout << "Vector de consulta: ";
        for (float value : query) {
            std::cout << value << " ";
        }

        std::cout << "\n\nVecinos mas cercanos:\n";
        for (std::size_t i = 0; i < neighbors.size(); ++i) {
            std::cout << i + 1 << ". ID: " << neighbors[i].id
                      << " | Distancia^2: " << neighbors[i].distance << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
