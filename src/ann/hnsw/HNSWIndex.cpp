
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
      efSearch_(std::max<std::size_t>(1, efSearch)),
      randomEngine_(std::random_device{}()) {
    if (M < 2) {
        throw std::invalid_argument(
            "M debe ser al menos 2 para generar niveles."
        );
    }
}

int HNSWIndex::randomLevel() {
    // La probabilidad de pertenecer a niveles altos disminuye
    // exponencialmente.
    double u = 0.0;

    do {
        u = std::generate_canonical<double, 53>(randomEngine_);
    } while (u <= 0.0);

    const double level =
        -std::log(u) / std::log(static_cast<double>(M_));

    // Evita niveles desproporcionados en casos extremos.
    return std::min(32, static_cast<int>(level));
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

    if (store_.distance_kind() == DistanceKind::SquaredEuclidean) {
        float sum = 0.0f;

        for (std::size_t i = 0; i < a.size(); ++i) {
            const float diff = a[i] - b[i];
            sum += diff * diff;
        }

        return sum;
    }

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

void HNSWIndex::pruneNeighbors(VectorId id, int level) {
    auto& neighbors = nodes_.at(id).neighbors.at(
        static_cast<std::size_t>(level)
    );

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
                *embedding, *store_.find(a)
            ) < distance(
                *embedding, *store_.find(b)
            );
        }
    );

    if (neighbors.size() > M_) {
        neighbors.resize(M_);
    }
}

void HNSWIndex::connect(
    VectorId a,
    VectorId b,
    int level
) {
    auto& neighborsA = nodes_.at(a).neighbors.at(
        static_cast<std::size_t>(level)
    );
    auto& neighborsB = nodes_.at(b).neighbors.at(
        static_cast<std::size_t>(level)
    );

    if (std::find(neighborsA.begin(), neighborsA.end(), b)
        == neighborsA.end()) {
        neighborsA.push_back(b);
    }

    if (std::find(neighborsB.begin(), neighborsB.end(), a)
        == neighborsB.end()) {
        neighborsB.push_back(a);
    }

    pruneNeighbors(a, level);
    pruneNeighbors(b, level);
}

std::vector<HNSWIndex::Candidate> HNSWIndex::searchLayer(
    std::span<const EmbeddingValue> query,
    const std::vector<VectorId>& entryPoints,
    std::size_t ef,
    int level
) const {
    if (ef == 0 || nodes_.empty() || level < 0) {
        return {};
    }

    using MinHeap = std::priority_queue<
        Candidate,
        std::vector<Candidate>,
        std::greater<Candidate>
    >;

    MinHeap candidates;
    std::priority_queue<Candidate> results;
    std::unordered_set<VectorId> visited;

    for (VectorId id : entryPoints) {
        const auto nodeIt = nodes_.find(id);

        if (nodeIt == nodes_.end() ||
            nodeIt->second.neighbors.size() <=
                static_cast<std::size_t>(level) ||
            !visited.insert(id).second) {
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

        if (results.size() >= ef &&
            current.first > results.top().first) {
            break;
        }

        candidates.pop();

        const auto& neighbors =
            nodes_.at(current.second).neighbors.at(
                static_cast<std::size_t>(level)
            );

        for (VectorId neighbor : neighbors) {
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

    std::vector<Candidate> ordered;
    ordered.reserve(results.size());

    while (!results.empty()) {
        ordered.push_back(results.top());
        results.pop();
    }

    std::sort(ordered.begin(), ordered.end());

    return ordered;
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

    const int newLevel = randomLevel();

    nodes_.emplace(id, HNSWNode{id, newLevel});

    if (!hasEntryPoint_) {
        entryPoint_ = id;
        maxLevel_ = newLevel;
        hasEntryPoint_ = true;
        return;
    }

    VectorId currentEntry = entryPoint_;

    // Primera fase: bajar por los niveles en los que
    // el nuevo nodo no participa.
    for (int level = maxLevel_; level > newLevel; --level) {
        const auto nearest = searchLayer(
            *embedding,
            {currentEntry},
            1,
            level
        );

        if (!nearest.empty()) {
            currentEntry = nearest.front().second;
        }
    }

    // Segunda fase: buscar vecinos y conectar en cada
    // nivel compartido entre el nuevo nodo y el grafo.
    const int lowestLevel = std::min(newLevel, maxLevel_);

    for (int level = lowestLevel; level >= 0; --level) {
        const auto candidates = searchLayer(
            *embedding,
            {currentEntry},
            efConstruction_,
            level
        );

        const std::size_t count =
            std::min(M_, candidates.size());

        for (std::size_t i = 0; i < count; ++i) {
            connect(id, candidates[i].second, level);
        }

        if (!candidates.empty()) {
            currentEntry = candidates.front().second;
        }
    }

    // Si el nodo nuevo alcanza un nivel superior,
    // se convierte en el nuevo punto de entrada.
    if (newLevel > maxLevel_) {
        entryPoint_ = id;
        maxLevel_ = newLevel;
    }
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

    VectorId currentEntry = entryPoint_;

    // Fase 1: búsqueda rápida en los niveles superiores.
    for (int level = maxLevel_; level > 0; --level) {
        const auto nearest = searchLayer(
            query,
            {currentEntry},
            1,
            level
        );

        if (!nearest.empty()) {
            currentEntry = nearest.front().second;
        }
    }

    // Fase 2: búsqueda más amplia en el nivel base.
    const std::size_t ef = std::max(k, efSearch_);

    const auto results = searchLayer(
        query,
        {currentEntry},
        ef,
        0
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
