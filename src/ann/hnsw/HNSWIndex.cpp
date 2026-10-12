
#include "HNSWIndex.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>
#include <stdexcept>
#include <unordered_set>

namespace aetherindex {

HNSWIndex::HNSWIndex(
    const VectorStore& store,
    std::size_t M,
    std::size_t efConstruction,
    std::size_t efSearch
)
    : store_(store),
      M_(M),
      efConstruction_(std::max(M, efConstruction)),
      efSearch_(std::max<std::size_t>(1, efSearch)) {
    if (M == 0) {
        throw std::invalid_argument("M debe ser mayor que cero.");
    }
}

float HNSWIndex::distance(
    std::span<const EmbeddingValue> a,
    std::span<const EmbeddingValue> b
) const {
    if (a.size() != b.size()) {
        throw std::invalid_argument(
            "Los vectores tienen dimensiones diferentes."
        );
    }

    float sum = 0.0f;

    if (store_.distance_kind() == DistanceKind::SquaredEuclidean) {
        for (std::size_t i = 0; i < a.size(); ++i) {
            const float diff = a[i] - b[i];
            sum += diff * diff;
        }

        return sum;
    }

    // Distancia coseno: 1 - similitud coseno.
    float dot = 0.0f;
    float normA = 0.0f;
    float normB = 0.0f;

    for (std::size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        normA += a[i] * a[i];
        normB += b[i] * b[i];
    }

    if (normA == 0.0f || normB == 0.0f) {
        throw std::invalid_argument(
            "La distancia coseno no admite vectores cero."
        );
    }

    const float similarity =
        dot / std::sqrt(normA * normB);

    return 1.0f - std::clamp(similarity, -1.0f, 1.0f);
}

void HNSWIndex::connect(VectorId a, VectorId b) {
    auto& neighborsA = nodes_.at(a).neighbors;
    auto& neighborsB = nodes_.at(b).neighbors;

    if (std::find(neighborsA.begin(), neighborsA.end(), b)
        == neighborsA.end()) {
        neighborsA.push_back(b);
    }

    if (std::find(neighborsB.begin(), neighborsB.end(), a)
        == neighborsB.end()) {
        neighborsB.push_back(a);
    }

    pruneNeighbors(a);
    pruneNeighbors(b);
}

void HNSWIndex::pruneNeighbors(VectorId id) {
    auto& neighbors = nodes_.at(id).neighbors;
    const auto embedding = store_.find(id);

    if (!embedding) {
        throw std::logic_error(
            "El nodo no existe en VectorStore."
        );
    }

    std::sort(
        neighbors.begin(),
        neighbors.end(),
        [&](VectorId a, VectorId b) {
            return distance(
                *embedding,
                *store_.find(a)
            ) < distance(
                *embedding,
                *store_.find(b)
            );
        }
    );

    if (neighbors.size() > M_) {
        neighbors.resize(M_);
    }
}

void HNSWIndex::insert(VectorId id) {
    const auto embedding = store_.find(id);

    if (!embedding) {
        throw std::invalid_argument(
            "El vector debe existir en VectorStore antes de indexarlo."
        );
    }

    if (nodes_.contains(id)) {
        throw std::invalid_argument(
            "El vector ya fue indexado en HNSW."
        );
    }

    nodes_.emplace(id, HNSWNode{id});

    if (!hasEntryPoint_) {
        entryPoint_ = id;
        hasEntryPoint_ = true;
        return;
    }

    const auto candidates = searchLayer(
        *embedding,
        {entryPoint_},
        efConstruction_
    );

    const std::size_t count = std::min(M_, candidates.size());

    for (std::size_t i = 0; i < count; ++i) {
        connect(id, candidates[i].second);
    }
}

std::vector<HNSWIndex::Candidate> HNSWIndex::searchLayer(
    std::span<const EmbeddingValue> query,
    const std::vector<VectorId>& entryPoints,
    std::size_t ef
) const {
    if (ef == 0 || nodes_.empty()) {
        return {};
    }

    // C: min-heap. El candidato más cercano sale primero.
    std::priority_queue<
        Candidate,
        std::vector<Candidate>,
        std::greater<Candidate>
    > candidates;

    // W: max-heap. El resultado más lejano sale primero.
    std::priority_queue<Candidate> results;

    std::unordered_set<VectorId> visited;

    for (VectorId id : entryPoints) {
        if (!nodes_.contains(id) || !visited.insert(id).second) {
            continue;
        }

        const auto embedding = store_.find(id);

        if (!embedding) {
            throw std::logic_error(
                "El vector indexado no existe en VectorStore."
            );
        }

        const float d = distance(query, *embedding);

        candidates.emplace(d, id);
        results.emplace(d, id);

        if (results.size() > ef) {
            results.pop();
        }
    }

    while (!candidates.empty()) {
        const Candidate current = candidates.top();

        // Si ya tenemos ef resultados mejores, podemos detenernos.
        if (results.size() >= ef &&
            current.first > results.top().first) {
            break;
        }

        candidates.pop();

        for (VectorId neighbor :
             nodes_.at(current.second).neighbors) {
            if (!visited.insert(neighbor).second) {
                continue;
            }

            const auto embedding = store_.find(neighbor);

            if (!embedding) {
                throw std::logic_error(
                    "Un vecino no existe en VectorStore."
                );
            }

            const float d = distance(query, *embedding);

            if (results.size() < ef ||
                d < results.top().first) {
                candidates.emplace(d, neighbor);
                results.emplace(d, neighbor);

                if (results.size() > ef) {
                    results.pop();
                }
            }
        }
    }

    // Devolver los candidatos del más cercano al más lejano.
    std::vector<Candidate> ordered;
    ordered.reserve(results.size());

    while (!results.empty()) {
        ordered.push_back(results.top());
        results.pop();
    }

    std::sort(ordered.begin(), ordered.end());

    return ordered;
}

std::vector<VectorId> HNSWIndex::search(
    std::span<const EmbeddingValue> query,
    std::size_t k
) const {
    if (k == 0 || !hasEntryPoint_) {
        return {};
    }

    if (query.size() != store_.dimension()) {
        throw std::invalid_argument(
            "La consulta tiene una dimension incorrecta."
        );
    }

    // Validar los valores antes de usarlos en las colas.
    bool nonzero = false;

    for (EmbeddingValue value : query) {
        if (!std::isfinite(value)) {
            throw std::invalid_argument(
                "La consulta contiene valores no finitos."
            );
        }

        nonzero = nonzero || value != 0.0f;
    }

    if (store_.distance_kind() == DistanceKind::Cosine &&
        !nonzero) {
        throw std::invalid_argument(
            "La consulta coseno no puede ser un vector cero."
        );
    }

    const std::size_t ef = std::max(k, efSearch_);

    const auto results = searchLayer(
        query,
        {entryPoint_},
        ef
    );

    std::vector<VectorId> ids;
    const std::size_t count = std::min(k, results.size());

    ids.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        ids.push_back(results[i].second);
    }

    return ids;
}

std::size_t HNSWIndex::size() const noexcept {
    return nodes_.size();
}

}  // namespace aetherindex
