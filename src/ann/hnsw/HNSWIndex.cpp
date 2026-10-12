
#include "HNSWIndex.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <stdexcept>
#include <utility>

HNSWIndex::HNSWIndex(std::size_t M)
    : M(M),
      entryPoint(0),
      hasEntryPoint(false) {
    if (M == 0) {
        throw std::invalid_argument("M debe ser mayor que cero");
    }
}

float HNSWIndex::distance(
    const std::vector<float>& a,
    const std::vector<float>& b
) const {
    if (a.size() != b.size()) {
        throw std::invalid_argument(
            "Los vectores tienen dimensiones diferentes"
        );
    }

    float sum = 0.0f;

    for (std::size_t i = 0; i < a.size(); ++i) {
        const float diff = a[i] - b[i];
        sum += diff * diff;
    }

    return std::sqrt(sum);
}

void HNSWIndex::connect(
    std::size_t a,
    std::size_t b
) {
    auto& neighborsA = nodes.at(a).neighbors;
    auto& neighborsB = nodes.at(b).neighbors;

    if (std::find(
            neighborsA.begin(),
            neighborsA.end(),
            b
        ) == neighborsA.end()) {
        neighborsA.push_back(b);
    }

    if (std::find(
            neighborsB.begin(),
            neighborsB.end(),
            a
        ) == neighborsB.end()) {
        neighborsB.push_back(a);
    }

    pruneNeighbors(a);
    pruneNeighbors(b);
}

void HNSWIndex::pruneNeighbors(std::size_t id) {
    auto& neighbors = nodes.at(id).neighbors;

    if (neighbors.size() <= M) {
        return;
    }

    const auto& currentVector = vectors.at(id);

    std::sort(
        neighbors.begin(),
        neighbors.end(),
        [&](std::size_t a, std::size_t b) {
            return distance(
                currentVector,
                vectors.at(a)
            ) < distance(
                currentVector,
                vectors.at(b)
            );
        }
    );

    neighbors.resize(M);
}

void HNSWIndex::insert(
    std::size_t id,
    const std::vector<float>& vector
) {
    if (vectors.count(id) > 0) {
        throw std::invalid_argument(
            "El identificador ya existe"
        );
    }

    if (!vectors.empty() &&
        vector.size() != vectors.begin()->second.size()) {
        throw std::invalid_argument(
            "Los vectores tienen dimensiones diferentes"
        );
    }

    vectors.emplace(id, vector);
    nodes.emplace(id, HNSWNode{id});

    if (!hasEntryPoint) {
        entryPoint = id;
        hasEntryPoint = true;
        return;
    }

    // Prototipo: comparar con todos los nodos anteriores
    // para seleccionar los M vecinos más cercanos.
    std::vector<std::pair<float, std::size_t>> candidates;

    for (const auto& [otherId, otherVector] : vectors) {
        if (otherId == id) {
            continue;
        }

        candidates.emplace_back(
            distance(vector, otherVector),
            otherId
        );
    }

    const std::size_t count = std::min(M, candidates.size());

    if (count == 0) {
        return;
    }

    std::partial_sort(
        candidates.begin(),
        candidates.begin() + count,
        candidates.end()
    );

    for (std::size_t i = 0; i < count; ++i) {
        connect(id, candidates[i].second);
    }
}

std::vector<std::size_t> HNSWIndex::search(
    const std::vector<float>& query,
    std::size_t k
) const {
    if (k == 0 || !hasEntryPoint) {
        return {};
    }

    if (query.size() != vectors.at(entryPoint).size()) {
        throw std::invalid_argument(
            "La consulta tiene una dimension incorrecta"
        );
    }

    // Distancia y nodo.
    using Candidate = std::pair<float, std::size_t>;

    // Menor distancia primero.
    std::priority_queue<
        Candidate,
        std::vector<Candidate>,
        std::greater<Candidate>
    > candidates;

    std::unordered_map<std::size_t, bool> visited;

    candidates.emplace(
        distance(query, vectors.at(entryPoint)),
        entryPoint
    );

    visited[entryPoint] = true;

    std::vector<Candidate> results;

    while (!candidates.empty()) {
        const auto [currentDistance, currentId] =
            candidates.top();

        candidates.pop();

        results.emplace_back(currentDistance, currentId);

        for (std::size_t neighbor :
             nodes.at(currentId).neighbors) {
            if (visited.count(neighbor)) {
                continue;
            }

            visited[neighbor] = true;

            candidates.emplace(
                distance(query, vectors.at(neighbor)),
                neighbor
            );
        }
    }

    std::sort(results.begin(), results.end());

    const std::size_t count = std::min(k, results.size());

    std::vector<std::size_t> ids;
    ids.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        ids.push_back(results[i].second);
    }

    return ids;
}

std::size_t HNSWIndex::size() const {
    return nodes.size();
}
