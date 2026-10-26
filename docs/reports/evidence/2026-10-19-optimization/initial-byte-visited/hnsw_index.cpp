#include "aether/ann/hnsw/hnsw_index.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <stdexcept>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <immintrin.h>
#endif

namespace aetherindex {
namespace {

inline void prefetch_read(const float* pointer) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(pointer, 0, 3);
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_prefetch(reinterpret_cast<const char*>(pointer), _MM_HINT_T0);
#else
    (void)pointer;
#endif
}

}  // namespace

void validate_hnsw_config(const HnswConfig& config) {
    if (config.m < 2 || config.m >= std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("HNSW m must be at least 2 and fit in uint32_t.");
    }
    if (config.m_max_0 < config.m || config.m_max_0 >= std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("HNSW m_max_0 must be at least m and fit in uint32_t.");
    }
    if (config.ef_construction < config.m) {
        throw std::invalid_argument("HNSW ef_construction must be at least m.");
    }
    if (config.ef_search == 0) throw std::invalid_argument("HNSW ef_search must be positive.");
    if (config.max_level > 32)
        throw std::invalid_argument("HNSW max_level must be between 0 and 32.");
    if (config.neighbor_selection != NeighborSelection::Simple &&
        config.neighbor_selection != NeighborSelection::Heuristic) {
        throw std::invalid_argument("Unsupported HNSW neighbor selection.");
    }
}

bool HnswGraphValidation::valid() const noexcept {
    return usable && invalid_references == 0 && missing_embeddings == 0 && invalid_levels == 0 &&
           layer_violations == 0 && self_references == 0 && duplicate_edges == 0 &&
           degree_violations == 0 && entry_point_violations == 0 && max_level_violations == 0 &&
           insertion_count_violations == 0;
}

HnswIndex::HnswIndex(const VectorStore& store, HnswConfig config)
    : store_(store),
      config_(config),
      layer0_(0, config.m_max_0),
      upper_(0, config.m),
      random_engine_(config.random_seed),
      distance_(detail::distance_kernel(store.distance_kind())) {
    validate_hnsw_config(config_);
    reserve(store.size());
}

void HnswIndex::reserve(std::size_t count) {
    ensure_usable();
    if (count > std::numeric_limits<InternalId>::max()) {
        throw std::length_error("HNSW dense position capacity exceeds uint32_t.");
    }
    layer0_.reserve(count);
    levels_.reserve(count);
    upper_offsets_.reserve(count);
    // Stochastic headroom for promoted layers, not N dense upper matrices.
    const std::size_t expected = count / (config_.m - 1) + 1;
    const std::size_t upper_capacity = expected + expected / 5 + 32;
    upper_.reserve(upper_capacity);
}

void HnswIndex::ensure_usable() const {
    if (!usable_)
        throw std::logic_error("HNSW insertion failed after graph mutation; rebuild the index.");
}

std::size_t HnswIndex::random_level() {
    // Level sampling is preprocessing, not a distance accumulation.
    const double uniform = static_cast<double>((random_engine_() >> 12) + 1) / 4503599627370497.0;
    const double level = std::floor(-std::log(uniform) / std::log(static_cast<double>(config_.m)));
    return static_cast<std::size_t>(std::min(level, static_cast<double>(config_.max_level)));
}

std::size_t HnswIndex::connection_limit(std::size_t level) const noexcept {
    return level == 0 ? config_.m_max_0 : config_.m;
}

float HnswIndex::distance_to(const float* query, InternalId id) const {
    return distance_(query, store_.data_by_position(id), store_.padded_dimension());
}

std::span<const InternalId> HnswIndex::neighbors_internal(InternalId id,
                                                          std::size_t level) const noexcept {
    return level == 0
               ? layer0_.get_neighbors(id)
               : upper_.get_neighbors(static_cast<std::size_t>(upper_offsets_[id]) + level - 1);
}

void HnswIndex::set_neighbors(InternalId id, std::size_t level,
                              std::span<const InternalId> neighbors) {
    if (level == 0)
        layer0_.set_neighbors(id, neighbors);
    else
        upper_.set_neighbors(static_cast<std::size_t>(upper_offsets_[id]) + level - 1, neighbors);
}

std::vector<HnswIndex::Candidate> HnswIndex::search_layer(const float* query, InternalId entry,
                                                          std::size_t ef, std::size_t level) const {
    ef = std::min(ef, successful_insertions_ + 1);
    using MinHeap = std::priority_queue<Candidate, std::vector<Candidate>, std::greater<Candidate>>;
    std::vector<Candidate> candidate_buffer, result_buffer;
    candidate_buffer.reserve(ef);
    result_buffer.reserve(ef);
    MinHeap candidates(std::greater<Candidate>{}, std::move(candidate_buffer));
    std::priority_queue<Candidate> results(std::less<Candidate>{}, std::move(result_buffer));
    // One dense visited allocation per traversal, no per-visited-node heap or hash.
    std::vector<std::uint8_t> visited(levels_.size(), 0);
    const Candidate first{distance_to(query, entry), store_.ids()[entry], entry};
    visited[entry] = 1;
    candidates.push(first);
    results.push(first);

    while (!candidates.empty()) {
        const Candidate current = candidates.top();
        if (results.size() >= ef && current.distance > results.top().distance) break;
        candidates.pop();
        const auto adjacent = neighbors_internal(current.node, level);
        if (!adjacent.empty()) prefetch_read(store_.data_by_position(adjacent.front()));
        for (std::size_t i = 0; i < adjacent.size(); ++i) {
            if (i + 1 < adjacent.size()) prefetch_read(store_.data_by_position(adjacent[i + 1]));
            const InternalId neighbor = adjacent[i];
            if (visited[neighbor]) continue;
            visited[neighbor] = 1;
            const Candidate next{distance_to(query, neighbor), store_.ids()[neighbor], neighbor};
            if (results.size() < ef || next < results.top()) {
                candidates.push(next);
                results.push(next);
                if (results.size() > ef) results.pop();
            }
        }
    }
    std::vector<Candidate> ordered(results.size());
    for (std::size_t i = ordered.size(); i > 0; --i) {
        ordered[i - 1] = results.top();
        results.pop();
    }
    return ordered;
}

std::vector<InternalId> HnswIndex::select_neighbors(InternalId base,
                                                    std::vector<Candidate> candidates,
                                                    std::size_t limit, std::size_t level) const {
    // Extension uses a snapshot of C (one hop), as in Algorithm 4. Dense marks
    // deduplicate before scoring. No graph mutation occurs during selection.
    if (config_.neighbor_selection == NeighborSelection::Heuristic && config_.extend_candidates) {
        std::vector<std::uint8_t> seen(levels_.size(), 0);
        seen[base] = 1;
        for (const auto& candidate : candidates) seen[candidate.node] = 1;
        const std::size_t original_size = candidates.size();
        for (std::size_t i = 0; i < original_size; ++i) {
            for (InternalId neighbor : neighbors_internal(candidates[i].node, level)) {
                if (!seen[neighbor]) {
                    seen[neighbor] = 1;
                    candidates.push_back({distance_to(store_.data_by_position(base), neighbor),
                                          store_.ids()[neighbor], neighbor});
                }
            }
        }
    }
    std::sort(candidates.begin(), candidates.end());
    // Search and extension already produce unique nodes. Erase repeated nodes
    // defensively for pruning as well, without a persistent per-node container.
    candidates.erase(
        std::unique(candidates.begin(), candidates.end(),
                    [](const Candidate& a, const Candidate& b) { return a.node == b.node; }),
        candidates.end());
    std::vector<InternalId> selected;
    selected.reserve(std::min(limit, candidates.size()));
    std::vector<InternalId> discarded;
    if (config_.keep_pruned_connections) discarded.reserve(candidates.size());
    for (const Candidate& candidate : candidates) {
        if (candidate.node == base) continue;
        if (selected.size() == limit) break;
        bool keep = true;
        if (config_.neighbor_selection == NeighborSelection::Heuristic) {
            for (InternalId neighbor : selected) {
                if (distance_to(store_.data_by_position(candidate.node), neighbor) <
                    candidate.distance) {
                    keep = false;
                    break;
                }
            }
        }
        if (keep)
            selected.push_back(candidate.node);
        else if (config_.keep_pruned_connections)
            discarded.push_back(candidate.node);
    }
    for (InternalId node : discarded) {
        if (selected.size() == limit) break;
        selected.push_back(node);
    }
    return selected;
}

void HnswIndex::add_link(InternalId a, InternalId b, std::size_t level) {
    const auto adjacent = neighbors_internal(a, level);
    if (a == b || std::find(adjacent.begin(), adjacent.end(), b) != adjacent.end()) return;
    if (adjacent.size() < connection_limit(level)) {
        if (level == 0)
            layer0_.append_neighbor(a, b);
        else
            upper_.append_neighbor(static_cast<std::size_t>(upper_offsets_[a]) + level - 1, b);
        return;
    }
    // Score M_max + 1 in a temporary workspace. Never overflow a fixed slot.
    std::vector<Candidate> scored;
    scored.reserve(adjacent.size() + 1);
    const float* base = store_.data_by_position(a);
    for (InternalId node : adjacent)
        scored.push_back({distance_to(base, node), store_.ids()[node], node});
    scored.push_back({distance_to(base, b), store_.ids()[b], b});
    const auto selected = select_neighbors(a, std::move(scored), connection_limit(level), level);
    set_neighbors(a, level, selected);
}

void HnswIndex::insert(VectorId id) {
    ensure_usable();
    const auto position = store_.position_of(id);
    if (!position)
        throw std::invalid_argument("Insert the vector into VectorStore before indexing its ID.");
    if (contains(id)) throw std::invalid_argument("The vector ID is already indexed in HNSW.");
    if (*position >= std::numeric_limits<InternalId>::max())
        throw std::length_error("HNSW position overflow.");
    const InternalId node = static_cast<InternalId>(*position);
    const std::size_t new_level = random_level();
    const std::size_t old_upper_size = upper_.slots();
    if (old_upper_size > std::numeric_limits<std::uint32_t>::max() - new_level) {
        throw std::length_error("HNSW upper slot offset overflow.");
    }
    // Allocate before making this node active. A failed allocation leaves all
    // existing edges and active nodes valid; unused capacity may have grown.
    if (levels_.size() <= node) {
        layer0_.resize(static_cast<std::size_t>(node) + 1);
        upper_offsets_.resize(static_cast<std::size_t>(node) + 1, 0);
        levels_.resize(static_cast<std::size_t>(node) + 1, absent_level);
    }
    upper_.resize(old_upper_size + new_level);
    upper_offsets_[node] = static_cast<std::uint32_t>(old_upper_size);
    levels_[node] = static_cast<std::uint8_t>(new_level);
    try {
        if (!entry_point_) {
            entry_point_ = node;
            max_level_ = new_level;
            ++successful_insertions_;
            return;
        }
        const float* embedding = store_.data_by_position(node);
        InternalId current = *entry_point_;
        for (std::size_t level = max_level_; level > new_level; --level) {
            current = search_layer(embedding, current, 1, level).front().node;
        }
        std::size_t level = std::min(new_level, max_level_);
        for (;;) {
            const auto candidates =
                search_layer(embedding, current, config_.ef_construction, level);
            const auto selected =
                select_neighbors(node, candidates, connection_limit(level), level);
            set_neighbors(node, level, selected);
            for (InternalId neighbor : selected) add_link(neighbor, node, level);
            if (!candidates.empty()) current = candidates.front().node;
            if (level == 0) break;
            --level;
        }
        if (new_level > max_level_) {
            entry_point_ = node;
            max_level_ = new_level;
        }
        ++successful_insertions_;
    } catch (...) {
        usable_ = false;
        throw;
    }
}

std::vector<SearchResult> HnswIndex::search(std::span<const EmbeddingValue> query,
                                            std::size_t k) const {
    return search(query, k, config_.ef_search);
}

std::vector<SearchResult> HnswIndex::search(std::span<const EmbeddingValue> query, std::size_t k,
                                            std::size_t ef_search) const {
    return search_prepared(PreparedQuery(query, store_.dimension(), store_.distance_kind()), k,
                           ef_search);
}

std::vector<SearchResult> HnswIndex::search_prepared(const PreparedQuery& query, std::size_t k,
                                                     std::size_t ef_search) const {
    ensure_usable();
    if (k == 0 || ef_search == 0)
        throw std::invalid_argument("HNSW k and ef_search must be positive.");
    if (query.dimension() != store_.dimension() ||
        query.distance_kind() != store_.distance_kind()) {
        throw std::invalid_argument("Prepared query does not match HNSW VectorStore.");
    }
    if (!entry_point_) return {};
    InternalId current = *entry_point_;
    for (std::size_t level = max_level_; level > 0; --level) {
        current = search_layer(query.data(), current, 1, level).front().node;
    }
    const std::size_t ef = std::min(size(), std::max(k, ef_search));
    const auto candidates = search_layer(query.data(), current, ef, 0);
    std::vector<SearchResult> results;
    const std::size_t count = std::min(k, candidates.size());
    results.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        results.push_back({candidates[i].id, candidates[i].distance});
    return results;
}

std::size_t HnswIndex::size() const noexcept { return successful_insertions_; }

bool HnswIndex::contains(VectorId id) const {
    const auto position = store_.position_of(id);
    return position && *position < levels_.size() && levels_[*position] != absent_level;
}

const HnswConfig& HnswIndex::config() const noexcept { return config_; }

std::optional<VectorId> HnswIndex::entry_point() const noexcept {
    return entry_point_ ? std::optional<VectorId>(store_.ids()[*entry_point_]) : std::nullopt;
}

std::optional<std::size_t> HnswIndex::max_level() const noexcept {
    return entry_point_ ? std::optional<std::size_t>(max_level_) : std::nullopt;
}

std::optional<HnswNode> HnswIndex::find_node(VectorId id) const {
    const auto position = store_.position_of(id);
    if (!position || *position >= levels_.size() || levels_[*position] == absent_level)
        return std::nullopt;
    return HnswNode{id, static_cast<InternalId>(*position), levels_[*position]};
}

std::span<const InternalId> HnswIndex::neighbors(VectorId id, std::size_t level) const {
    const auto node = find_node(id);
    if (!node || node->max_level < level)
        throw std::out_of_range("HNSW node is absent from layer.");
    return neighbors_internal(node->internal_id, level);
}

VectorId HnswIndex::external_id(InternalId id) const {
    if (id >= levels_.size() || levels_[id] == absent_level)
        throw std::out_of_range("HNSW internal ID absent.");
    return store_.ids()[id];
}

GraphMemoryUsage HnswIndex::memory_usage() const noexcept {
    return {layer0_.allocated_bytes(),
            upper_.allocated_bytes(),
            levels_.capacity() * sizeof(std::uint8_t) +
                upper_offsets_.capacity() * sizeof(std::uint32_t),
            sizeof(*this),
            upper_.slots(),
            static_cast<std::size_t>(layer0_.capacity() > 0) + (upper_.capacity() > 0) +
                (levels_.capacity() > 0) + (upper_offsets_.capacity() > 0)};
}

HnswGraphValidation HnswIndex::validate_graph() const {
    HnswGraphValidation report;
    report.usable = usable_;
    std::size_t observed_max = 0;
    for (std::size_t position = 0; position < levels_.size(); ++position) {
        if (levels_[position] == absent_level) continue;
        ++report.nodes;
        if (position >= store_.size()) {
            ++report.missing_embeddings;
            continue;
        }
        if (levels_[position] > config_.max_level) {
            ++report.invalid_levels;
            continue;
        }
        observed_max = std::max(observed_max, static_cast<std::size_t>(levels_[position]));
        if (static_cast<std::size_t>(upper_offsets_[position]) + levels_[position] >
            upper_.slots()) {
            ++report.invalid_levels;
            continue;
        }
        for (std::size_t level = 0; level <= levels_[position]; ++level) {
            const auto adjacent = neighbors_internal(static_cast<InternalId>(position), level);
            if (adjacent.size() > connection_limit(level)) ++report.degree_violations;
            for (std::size_t i = 0; i < adjacent.size(); ++i) {
                const InternalId neighbor = adjacent[i];
                if (neighbor == position) ++report.self_references;
                if (std::find(adjacent.begin(), adjacent.begin() + i, neighbor) !=
                    adjacent.begin() + i) {
                    ++report.duplicate_edges;
                }
                if (neighbor >= levels_.size() || levels_[neighbor] == absent_level)
                    ++report.invalid_references;
                else if (levels_[neighbor] < level)
                    ++report.layer_violations;
            }
        }
    }
    if (!entry_point_) {
        if (report.nodes != 0) ++report.entry_point_violations;
    } else {
        if (*entry_point_ >= levels_.size() || levels_[*entry_point_] == absent_level ||
            levels_[*entry_point_] != max_level_)
            ++report.entry_point_violations;
        if (observed_max != max_level_) ++report.max_level_violations;
    }
    if (report.nodes != successful_insertions_) ++report.insertion_count_violations;
    return report;
}

}  // namespace aetherindex
