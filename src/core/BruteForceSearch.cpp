
#include "core/BruteForceSearch.hpp"
#include "distance/Distance.hpp"

#include <algorithm>
#include <stdexcept>

namespace aetherindex {

std::vector<SearchResult> bruteForceSearch(
    const std::vector<VectorEntry>& database,
    const std::vector<float>& query,
    std::size_t k
) {
    if (k == 0 || database.empty()) {
        return {};
    }

    // Validar las dimensiones antes de iniciar la búsqueda.
    for (const auto& entry : database) {
        if (entry.values.size() != query.size()) {
            throw std::invalid_argument(
                "La consulta y los vectores deben tener "
                "la misma dimension."
            );
        }
    }

    std::vector<SearchResult> results;
    results.reserve(database.size());

    // 1. Comparar la consulta contra TODOS los vectores.
    for (const auto& entry : database) {
        float distance = squaredEuclideanDistance(
            query,
            entry.values
        );

        results.push_back({
            entry.id,
            distance
        });
    }

    // 2. Ordenar de menor a mayor distancia.
    std::sort(
        results.begin(),
        results.end(),
        [](const SearchResult& a, const SearchResult& b) {
            return a.distance < b.distance;
        }
    );

    // 3. Devolver como máximo k vecinos.
    if (results.size() > k) {
        results.resize(k);
    }

    return results;
}

} // namespace aetherindex
