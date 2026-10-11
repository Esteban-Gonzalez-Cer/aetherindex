
#include "storage/VectorEntry.hpp"
#include "core/BruteForceSearch.hpp"

#include <iostream>
#include <vector>
#include <exception>

int main() {
    using namespace aetherindex;

    // 1. Crear una base de datos de prueba.
    std::vector<VectorEntry> database = {
        {1, {1.0f, 0.0f, 3.0f}},
        {2, {0.9f, 0.1f, 4.0f}},
        {3, {0.0f, 1.0f, 10.0f}},
        {4, {-1.0f, 0.0f, 3.5f}}
    };

    // 2. Definir el vector de consulta.
    std::vector<float> query = {
        1.2f, 2.2f, 3.2f
    };

    // 3. Solicitar los 3 vecinos más cercanos.
    std::size_t k = 3;

    try {
        auto neighbors = bruteForceSearch(
            database,
            query,
            k
        );

        // 4. Mostrar los resultados.
        std::cout << "=== AetherIndex ===\n";
        std::cout << "Busqueda por fuerza bruta\n\n";

        std::cout << "Vector de consulta: ";

        for (float value : query) {
            std::cout << value << " ";
        }

        std::cout << "\n\nVecinos mas cercanos:\n";

        for (std::size_t i = 0; i < neighbors.size(); ++i) {
            std::cout
                << i + 1
                << ". ID: " << neighbors[i].id
                << " | Distancia^2: "
                << neighbors[i].distance
                << '\n';
        }
    }
    catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
