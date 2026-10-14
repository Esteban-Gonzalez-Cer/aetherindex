#include "aether/ann/hnsw/hnsw_index.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>
#include <stdexcept>
#include <unordered_set>

#include "aether/distance/distance.hpp"

namespace aetherindex {

void validate_hnsw_config(const HnswConfig& config) {
    if (config.m < 2) {
        throw std::invalid_argument("HNSW m must be at least 2.");
    }
    if (config.m_max_0 < config.m) {
        throw std::invalid_argument("HNSW m_max_0 must be at least m.");
    }
    if (config.ef_construction < config.m) {
        throw std::invalid_argument("HNSW ef_construction must be at least m.");
    }
    if (config.ef_search == 0) {
        throw std::invalid_argument("HNSW ef_search must be positive.");
    }
    if (config.max_level > 32) {
        throw std::invalid_argument("HNSW max_level must be between 0 and 32.");
    }
}

bool HnswGraphValidation::valid() const noexcept {
    return usable && invalid_references == 0 && missing_embeddings == 0 && invalid_levels == 0 &&
           layer_violations == 0 && self_references == 0 && duplicate_edges == 0 &&
           degree_violations == 0 && entry_point_violations == 0 && max_level_violations == 0 &&
           insertion_count_violations == 0;
}

HnswIndex::HnswIndex(const VectorStore& store, HnswConfig config)
    : store_(store), config_(config), random_engine_(config.random_seed) {
    validate_hnsw_config(config_);
}

void HnswIndex::ensure_usable() const {
    if (!usable_) {
        throw std::logic_error("HNSW insertion failed after graph mutation; rebuild the index.");
    }
}

std::size_t HnswIndex::random_level() {
    // Map 52 random bits to (0, 1), using exactly representable integers.
    // This avoids implementation-specific uniform_real_distribution behavior.
    const double uniform = static_cast<double>((random_engine_() >> 12) + 1) / 4503599627370497.0;
    const double level = std::floor(-std::log(uniform) / std::log(static_cast<double>(config_.m)));
    return static_cast<std::size_t>(std::min(level, static_cast<double>(config_.max_level)));
}

std::size_t HnswIndex::connection_limit(std::size_t level) const noexcept {
    return level == 0 ? config_.m_max_0 : config_.m;
}

float HnswIndex::distance_to(std::span<const EmbeddingValue> query, VectorId id) const {
    const auto embedding = store_.find(id);
    if (!embedding) {
        throw std::logic_error("An HNSW node has no embedding in VectorStore.");
    }
    return compute_distance(query, *embedding, store_.distance_kind());
}

std::vector<HnswIndex::Candidate> HnswIndex::search_layer(std::span<const EmbeddingValue> query,
                                                          const std::vector<VectorId>& entry_points,
                                                          std::size_t ef, std::size_t level) const {
    if (nodes_.empty() || ef == 0) {
        return {};
    }
    ef = std::min(ef, nodes_.size());
    using MinHeap = std::priority_queue<Candidate, std::vector<Candidate>, std::greater<Candidate>>;
    MinHeap candidates;
    std::priority_queue<Candidate> results;
    std::unordered_set<VectorId> visited;

    for (VectorId id : entry_points) {
        const auto node = nodes_.find(id);
        if (node == nodes_.end() || node->second.neighbors.size() <= level) {
            throw std::logic_error("HNSW entry point is absent from the search layer.");
        }
        if (!visited.insert(id).second) {
            continue;
        }
        const Candidate candidate{distance_to(query, id), id};
        candidates.push(candidate);
        results.push(candidate);
        if (results.size() > ef) {
            results.pop();
        }
    }

    while (!candidates.empty()) {
        const Candidate current = candidates.top();
        if (results.size() >= ef && current.first > results.top().first) {
            break;
        }
        candidates.pop();

        for (VectorId neighbor : nodes_.at(current.second).neighbors.at(level)) {
            if (!visited.insert(neighbor).second) {
                continue;
            }
            const Candidate candidate{distance_to(query, neighbor), neighbor};
            if (results.size() < ef || candidate < results.top()) {
                candidates.push(candidate);
                results.push(candidate);
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

std::vector<VectorId> HnswIndex::select_neighbors(VectorId id, std::vector<Candidate> candidates,
                                                  std::size_t limit) const {
    // Sprint 1 policy: nearest candidates first, with ID as the tie breaker.
    // This is intentionally separate from traversal and insertion mechanics.
    std::sort(candidates.begin(), candidates.end());
    std::vector<VectorId> selected;
    selected.reserve(std::min(limit, candidates.size()));
    for (const Candidate& candidate : candidates) {
        if (candidate.second != id &&
            std::find(selected.begin(), selected.end(), candidate.second) == selected.end()) {
            selected.push_back(candidate.second);
            if (selected.size() == limit) {
                break;
            }
        }
    }
    return selected;
}

void HnswIndex::prune_neighbors(VectorId id, std::size_t level) {
    auto& neighbors = nodes_.at(id).neighbors.at(level);
    if (neighbors.size() <= connection_limit(level)) {
        return;
    }
    const auto embedding = store_.find(id);
    if (!embedding) {
        throw std::logic_error("An HNSW node has no embedding in VectorStore.");
    }
    std::vector<Candidate> scored;
    scored.reserve(neighbors.size());
    for (VectorId neighbor : neighbors) {
        scored.emplace_back(distance_to(*embedding, neighbor), neighbor);
    }
    neighbors = select_neighbors(id, std::move(scored), connection_limit(level));
}

void HnswIndex::connect(VectorId a, VectorId b, std::size_t level) {
    if (a == b) {
        return;
    }
    auto& neighbors_a = nodes_.at(a).neighbors.at(level);
    auto& neighbors_b = nodes_.at(b).neighbors.at(level);
    if (std::find(neighbors_a.begin(), neighbors_a.end(), b) == neighbors_a.end()) {
        neighbors_a.push_back(b);
    }
    if (std::find(neighbors_b.begin(), neighbors_b.end(), a) == neighbors_b.end()) {
        neighbors_b.push_back(a);
    }
    // Pruning can leave a one-way edge, as allowed by the Sprint 1 contract.
    prune_neighbors(a, level);
    prune_neighbors(b, level);
}

void HnswIndex::insert(VectorId id) {
    ensure_usable();
    const auto embedding = store_.find(id);
    if (!embedding) {
        throw std::invalid_argument("Insert the vector into VectorStore before indexing its ID.");
    }
    if (nodes_.contains(id)) {
        throw std::invalid_argument("The vector ID is already indexed in HNSW.");
    }

    const std::size_t new_level = random_level();
    HnswNode new_node{id, new_level};
    bool mutation_started = false;
    try {
        nodes_.emplace(id, std::move(new_node));
        mutation_started = true;
        if (!entry_point_) {
            entry_point_ = id;
            max_level_ = new_level;
            ++successful_insertions_;
            return;
        }

        VectorId current_entry = *entry_point_;
        for (std::size_t level = max_level_; level > new_level; --level) {
            const auto nearest = search_layer(*embedding, {current_entry}, 1, level);
            if (!nearest.empty()) {
                current_entry = nearest.front().second;
            }
        }

        std::size_t level = std::min(new_level, max_level_);
        for (;;) {
            const auto candidates =
                search_layer(*embedding, {current_entry}, config_.ef_construction, level);
            const auto selected = select_neighbors(id, candidates, connection_limit(level));
            for (VectorId neighbor : selected) {
                connect(id, neighbor, level);
            }
            if (!candidates.empty()) {
                current_entry = candidates.front().second;
            }
            if (level == 0) {
                break;
            }
            --level;
        }

        if (new_level > max_level_) {
            entry_point_ = id;
            max_level_ = new_level;
        }
        ++successful_insertions_;
    } catch (...) {
        if (mutation_started) {
            usable_ = false;
        }
        throw;
    }
}

std::vector<SearchResult> HnswIndex::search(std::span<const EmbeddingValue> query,
                                            std::size_t k) const {
    return search(query, k, config_.ef_search);
}

std::vector<SearchResult> HnswIndex::search(std::span<const EmbeddingValue> query, std::size_t k,
                                            std::size_t ef_search) const {
    ensure_usable();
    if (k == 0 || ef_search == 0) {
        throw std::invalid_argument("HNSW k and ef_search must be positive.");
    }
    if (query.size() != store_.dimension()) {
        throw std::invalid_argument("The HNSW query dimension does not match VectorStore.");
    }
    // Validate every numeric input through the shared Distance contract, even
    // when there are no indexed nodes. Self-distance cannot overflow squared L2.
    compute_distance(query, query, store_.distance_kind());
    if (!entry_point_) {
        return {};
    }

    VectorId current_entry = *entry_point_;
    for (std::size_t level = max_level_; level > 0; --level) {
        const auto nearest = search_layer(query, {current_entry}, 1, level);
        if (!nearest.empty()) {
            current_entry = nearest.front().second;
        }
    }
    const std::size_t ef = std::min(nodes_.size(), std::max(k, ef_search));
    const auto candidates = search_layer(query, {current_entry}, ef, 0);
    std::vector<SearchResult> results;
    const std::size_t count = std::min(k, candidates.size());
    results.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        results.push_back({candidates[i].second, candidates[i].first});
    }
    return results;
}

std::size_t HnswIndex::size() const noexcept { return nodes_.size(); }

bool HnswIndex::contains(VectorId id) const { return nodes_.contains(id); }

const HnswConfig& HnswIndex::config() const noexcept { return config_; }

std::optional<VectorId> HnswIndex::entry_point() const noexcept { return entry_point_; }

std::optional<std::size_t> HnswIndex::max_level() const noexcept {
    return entry_point_ ? std::optional<std::size_t>(max_level_) : std::nullopt;
}

const HnswNode* HnswIndex::find_node(VectorId id) const {
    const auto node = nodes_.find(id);
    return node == nodes_.end() ? nullptr : &node->second;
}

HnswGraphValidation HnswIndex::validate_graph() const {
    HnswGraphValidation report;
    report.nodes = nodes_.size();
    report.usable = usable_;
    std::size_t observed_max_level = 0;
    for (const auto& [id, node] : nodes_) {
        if (node.id != id) {
            ++report.invalid_references;
        }
        if (!store_.contains(id)) {
            ++report.missing_embeddings;
        }
        if (node.max_level > config_.max_level || node.neighbors.size() != node.max_level + 1) {
            ++report.invalid_levels;
        }
        observed_max_level = std::max(observed_max_level, node.max_level);
        for (std::size_t level = 0; level < node.neighbors.size(); ++level) {
            if (level > node.max_level) {
                ++report.layer_violations;
            }
            const auto& neighbors = node.neighbors[level];
            if (neighbors.size() > connection_limit(level)) {
                ++report.degree_violations;
            }
            std::unordered_set<VectorId> seen;
            for (VectorId neighbor : neighbors) {
                if (neighbor == id) {
                    ++report.self_references;
                }
                if (!seen.insert(neighbor).second) {
                    ++report.duplicate_edges;
                }
                const auto target = nodes_.find(neighbor);
                if (target == nodes_.end()) {
                    ++report.invalid_references;
                } else if (target->second.max_level < level ||
                           target->second.neighbors.size() <= level) {
                    ++report.layer_violations;
                }
            }
        }
    }

    if (nodes_.empty()) {
        if (entry_point_) {
            ++report.entry_point_violations;
        }
    } else {
        const HnswNode* entry = entry_point_ ? find_node(*entry_point_) : nullptr;
        if (entry == nullptr || entry->max_level != max_level_) {
            ++report.entry_point_violations;
        }
        if (observed_max_level != max_level_) {
            ++report.max_level_violations;
        }
    }
    if (nodes_.size() != successful_insertions_) {
        ++report.insertion_count_violations;
    }
    return report;
}

}  // namespace aetherindex
