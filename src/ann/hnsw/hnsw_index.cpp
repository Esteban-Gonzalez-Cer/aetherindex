#include "aether/ann/hnsw/hnsw_index.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <stdexcept>
#include <unordered_set>

#include "aether/core/parallel.hpp"
#include "aether/core/prefetch.hpp"

namespace aetherindex {
namespace {

// Epoch-stamped visited marks reused by every traversal on this thread: preparing a
// search is O(1) instead of allocating and clearing an N-bit map per layer visit.
class VisitedTable {
   public:
    void prepare(std::size_t nodes) {
        if (stamps_.size() < nodes) stamps_.resize(nodes, 0);
        if (++epoch_ == 0) {
            std::fill(stamps_.begin(), stamps_.end(), 0);
            epoch_ = 1;
        }
    }
    // Returns whether id was already visited in this epoch, marking it visited.
    bool test_and_set(std::uint32_t id) noexcept {
        if (stamps_[id] == epoch_) return true;
        stamps_[id] = epoch_;
        return false;
    }

   private:
    std::vector<std::uint32_t> stamps_;
    std::uint32_t epoch_ = 0;
};

thread_local VisitedTable visited_table;
thread_local std::vector<InternalId> adjacency_copy;
thread_local std::vector<InternalId> unvisited_neighbors;

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

template <bool Atomic>
void HnswIndex::store_neighbors(InternalId id, std::size_t level,
                                std::span<const InternalId> neighbors) {
    if constexpr (!Atomic) {
        set_neighbors(id, level, neighbors);
    } else if (level == 0) {
        layer0_.set_neighbors_atomic(id, neighbors);
    } else {
        upper_.set_neighbors_atomic(static_cast<std::size_t>(upper_offsets_[id]) + level - 1,
                                    neighbors);
    }
}

void HnswIndex::set_neighbors(InternalId id, std::size_t level,
                              std::span<const InternalId> neighbors) {
    if (level == 0)
        layer0_.set_neighbors(id, neighbors);
    else
        upper_.set_neighbors(static_cast<std::size_t>(upper_offsets_[id]) + level - 1, neighbors);
}

std::mutex& HnswIndex::stripe(InternalId id) const noexcept {
    return build_stripes_[id & (stripe_count - 1)];
}

void HnswIndex::prefetch_row(InternalId id) const noexcept {
    prefetch_bytes(store_.data_by_position(id), store_.padded_dimension() * sizeof(float));
}

void HnswIndex::require_rows() const {
    if (!store_.has_rows())
        throw std::logic_error("This HNSW operation needs the FP32 rows; they were released.");
}

template <HnswIndex::Access Mode>
std::vector<HnswIndex::Candidate> HnswIndex::search_layer(const float* query, InternalId entry,
                                                          std::size_t ef, std::size_t level) const {
    return search_layer_with<Mode>([&](InternalId id) { return distance_to(query, id); },
                                         [&](InternalId id) { prefetch_row(id); }, entry, ef,
                                         level);
}

// Frozen: the query path with no concurrent writer (plain loads). Writer: insertion threads
// copy each adjacency list under its stripe lock. Reader: searches that may overlap
// insert_prepared copy each list with atomic loads, lock-free.
// prefetch(id) brings in whatever distance(id) reads: FP32 rows or quantized codes.
template <HnswIndex::Access Mode, class Distance, class Prefetch, class Admit>
std::vector<HnswIndex::Candidate> HnswIndex::search_layer_with(const Distance& distance,
                                                               const Prefetch& prefetch,
                                                               InternalId entry, std::size_t ef,
                                                               std::size_t level,
                                                               const Admit& admit) const {
    ef = std::min(ef, successful_insertions_ + 1);
    using MinHeap = std::priority_queue<Candidate, std::vector<Candidate>, std::greater<Candidate>>;
    std::vector<Candidate> candidate_buffer, result_buffer;
    candidate_buffer.reserve(ef);
    result_buffer.reserve(ef);
    MinHeap candidates(std::greater<Candidate>{}, std::move(candidate_buffer));
    std::priority_queue<Candidate> results(std::less<Candidate>{}, std::move(result_buffer));
    // Thread-local workspace, not graph topology; reset by bumping its epoch.
    auto& visited = visited_table;
    visited.prepare(levels_.size());
    const auto mark_visited = [&](InternalId id) { return visited.test_and_set(id); };
    const Candidate first{distance(entry), store_.ids()[entry], entry};
    mark_visited(entry);
    candidates.push(first);
    if (admit(entry)) results.push(first);

    while (!candidates.empty()) {
        const Candidate current = candidates.top();
        if (results.size() >= ef && current.distance > results.top().distance) break;
        candidates.pop();
        std::span<const InternalId> adjacent;
        if constexpr (Mode == Access::Writer) {
            const std::lock_guard lock(stripe(current.node));
            const auto shared = neighbors_internal(current.node, level);
            adjacency_copy.assign(shared.begin(), shared.end());
            adjacent = adjacency_copy;
        } else if constexpr (Mode == Access::Reader) {
            const FlatGraphLayer& layer = level == 0 ? layer0_ : upper_;
            const std::size_t slot =
                level == 0 ? current.node
                           : static_cast<std::size_t>(upper_offsets_[current.node]) + level - 1;
            adjacency_copy.resize(layer.max_neighbors());
            adjacent = {adjacency_copy.data(), layer.copy_neighbors_atomic(slot, adjacency_copy.data())};
        } else {
            adjacent = neighbors_internal(current.node, level);
        }
        // Two passes: the first drops visited neighbors and prefetches the vectors and IDs
        // of the rest, so their cache misses overlap; the second scores and inserts them in
        // adjacency order, so every decision (and the result) matches a one-pass loop.
        const VectorId* ids = store_.ids().data();
        auto& fresh = unvisited_neighbors;
        fresh.clear();
        for (const InternalId neighbor : adjacent) {
            if (mark_visited(neighbor)) continue;
            fresh.push_back(neighbor);
            prefetch(neighbor);
            prefetch_read(ids + neighbor);
        }
        for (const InternalId neighbor : fresh) {
            const Candidate next{distance(neighbor), ids[neighbor], neighbor};
            if (results.size() < ef || next < results.top()) {
                candidates.push(next);
                if (admit(neighbor)) {
                    results.push(next);
                    if (results.size() > ef) results.pop();
                }
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

template <bool Atomic>
void HnswIndex::add_link(InternalId a, InternalId b, std::size_t level) {
    const auto adjacent = neighbors_internal(a, level);
    if (a == b || std::find(adjacent.begin(), adjacent.end(), b) != adjacent.end()) return;
    if (adjacent.size() < connection_limit(level)) {
        FlatGraphLayer& layer = level == 0 ? layer0_ : upper_;
        const std::size_t slot =
            level == 0 ? a : static_cast<std::size_t>(upper_offsets_[a]) + level - 1;
        if constexpr (Atomic)
            layer.append_neighbor_atomic(slot, b);
        else
            layer.append_neighbor(slot, b);
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
    store_neighbors<Atomic>(a, level, selected);
}

void HnswIndex::insert(VectorId id) {
    ensure_usable();
    require_rows();
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
        const Top current_top = top();
        if (!current_top.present) {
            set_top(node, new_level);
            ++successful_insertions_;
            return;
        }
        const float* embedding = store_.data_by_position(node);
        InternalId current = current_top.entry;
        for (std::size_t level = current_top.level; level > new_level; --level) {
            current = search_layer<Access::Frozen>(embedding, current, 1, level).front().node;
        }
        std::size_t level = std::min(new_level, current_top.level);
        for (;;) {
            const auto candidates =
                search_layer<Access::Frozen>(embedding, current, config_.ef_construction, level);
            const auto selected =
                select_neighbors(node, candidates, connection_limit(level), level);
            set_neighbors(node, level, selected);
            for (InternalId neighbor : selected) add_link<false>(neighbor, node, level);
            if (!candidates.empty()) current = candidates.front().node;
            if (level == 0) break;
            --level;
        }
        if (new_level > current_top.level) set_top(node, new_level);
        ++successful_insertions_;
    } catch (...) {
        usable_ = false;
        throw;
    }
}

void HnswIndex::add_link_concurrent(InternalId a, InternalId b, std::size_t level) {
    const std::lock_guard lock(stripe(a));
    add_link<true>(a, b, level);
}

// One node of a parallel build. Only a node that raises the top level keeps the entry
// lock for its whole insertion (as in hnswlib); others snapshot the entry and release.
// The node becomes reachable only through the backlinks of the final phase, so every
// level's adjacency is complete before another thread can traverse from it, and no
// link another thread adds to it can be overwritten.
void HnswIndex::insert_concurrent(InternalId node, std::size_t new_level) {
    std::unique_lock entry_lock(entry_mutex_);
    const Top start = this->top();
    const std::size_t top = start.level;
    if (new_level <= top) entry_lock.unlock();
    const float* embedding = store_.data_by_position(node);
    InternalId current = start.entry;
    for (std::size_t level = top; level > new_level; --level)
        current = search_layer<Access::Writer>(embedding, current, 1, level).front().node;
    // Phase 1: neighbor selection for every level, top-down (no graph mutation).
    const std::size_t first_level = std::min(new_level, top);
    std::vector<std::vector<InternalId>> selected(first_level + 1);
    for (std::size_t level = first_level;; --level) {
        const auto candidates =
            search_layer<Access::Writer>(embedding, current, config_.ef_construction, level);
        selected[level] = select_neighbors(node, candidates, connection_limit(level), level);
        if (!candidates.empty()) current = candidates.front().node;
        if (level == 0) break;
    }
    // Phase 2: the node's own lists, merged with any link that already reached it.
    {
        const std::lock_guard lock(stripe(node));
        for (std::size_t level = 0; level <= first_level; ++level) {
            const auto existing = neighbors_internal(node, level);
            if (existing.empty()) {
                store_neighbors<true>(node, level, selected[level]);
                continue;
            }
            std::vector<Candidate> pool;
            for (InternalId id : existing)
                pool.push_back({distance_to(embedding, id), store_.ids()[id], id});
            for (InternalId id : selected[level])
                if (std::find(existing.begin(), existing.end(), id) == existing.end())
                    pool.push_back({distance_to(embedding, id), store_.ids()[id], id});
            store_neighbors<true>(
                node, level, select_neighbors(node, std::move(pool), connection_limit(level), level));
        }
    }
    // Phase 3: publish the node through backlinks.
    for (std::size_t level = 0; level <= first_level; ++level)
        for (InternalId neighbor : selected[level]) add_link_concurrent(neighbor, node, level);
    successful_insertions_.fetch_add(1, std::memory_order_relaxed);
    if (new_level > top) set_top(node, new_level);  // Still under entry_lock.
}

void HnswIndex::insert_parallel(std::span<const VectorId> ids, std::size_t threads) {
    ensure_usable();
    require_rows();
    if (threads <= 1) {
        for (VectorId id : ids) insert(id);
        return;
    }
    prepare_insert(ids);
    insert_prepared(threads);
}

void HnswIndex::prepare_insert(std::span<const VectorId> ids) {
    ensure_usable();
    require_rows();
    if (config_.extend_candidates)
        throw std::invalid_argument("Parallel HNSW construction requires extend_candidates=false.");
    if (!pending_nodes_.empty()) throw std::logic_error("An HNSW insertion is already prepared.");
    // Validate every ID before mutating anything.
    std::vector<InternalId> nodes;
    nodes.reserve(ids.size());
    std::unordered_set<VectorId> batch;
    batch.reserve(ids.size());
    for (VectorId id : ids) {
        const auto position = store_.position_of(id);
        if (!position)
            throw std::invalid_argument(
                "Insert the vector into VectorStore before indexing its ID.");
        if (contains(id) || !batch.insert(id).second)
            throw std::invalid_argument("The vector ID is already indexed in HNSW.");
        if (*position >= std::numeric_limits<InternalId>::max())
            throw std::length_error("HNSW position overflow.");
        nodes.push_back(static_cast<InternalId>(*position));
    }
    if (nodes.empty()) return;
    // Deterministic levels in input order, then all storage before any thread starts,
    // so no buffer is reallocated while others read it.
    std::vector<std::uint8_t> new_levels(nodes.size());
    std::size_t upper_total = upper_.slots();
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        new_levels[i] = static_cast<std::uint8_t>(random_level());
        if (upper_total > std::numeric_limits<std::uint32_t>::max() - new_levels[i])
            throw std::length_error("HNSW upper slot offset overflow.");
        upper_total += new_levels[i];
    }
    const InternalId highest = *std::max_element(nodes.begin(), nodes.end());
    auto stripes = build_stripes_ ? nullptr : std::make_unique<std::mutex[]>(stripe_count);
    try {
        if (levels_.size() <= highest) {
            layer0_.resize(static_cast<std::size_t>(highest) + 1);
            upper_offsets_.resize(static_cast<std::size_t>(highest) + 1, 0);
            levels_.resize(static_cast<std::size_t>(highest) + 1, absent_level);
        }
        std::size_t offset = upper_.slots();
        upper_.resize(upper_total);
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            upper_offsets_[nodes[i]] = static_cast<std::uint32_t>(offset);
            levels_[nodes[i]] = new_levels[i];
            offset += new_levels[i];
        }
        std::size_t first = 0;
        if (!top().present) {
            set_top(nodes[0], new_levels[0]);
            ++successful_insertions_;
            first = 1;
        }
        if (stripes) build_stripes_ = std::move(stripes);
        pending_nodes_.assign(nodes.begin() + static_cast<std::ptrdiff_t>(first), nodes.end());
        pending_levels_.assign(new_levels.begin() + static_cast<std::ptrdiff_t>(first),
                               new_levels.end());
    } catch (...) {
        usable_ = false;
        throw;
    }
}

void HnswIndex::insert_prepared(std::size_t threads) {
    ensure_usable();
    try {
        parallel_for(pending_nodes_.size(), std::max<std::size_t>(threads, 1), [&](std::size_t i) {
            insert_concurrent(pending_nodes_[i], pending_levels_[i]);
        });
    } catch (...) {
        pending_nodes_.clear();
        pending_levels_.clear();
        build_stripes_.reset();
        usable_ = false;
        throw;
    }
    pending_nodes_.clear();
    pending_levels_.clear();
    build_stripes_.reset();
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
    require_rows();
    if (k == 0 || ef_search == 0)
        throw std::invalid_argument("HNSW k and ef_search must be positive.");
    if (query.dimension() != store_.dimension() ||
        query.distance_kind() != store_.distance_kind()) {
        throw std::invalid_argument("Prepared query does not match HNSW VectorStore.");
    }
    // Tombstoned vectors still route the traversal but never enter the results, and a
    // search that may overlap an insertion needs atomic loads: both take the general path.
    if (store_.deleted_count() != 0 || concurrent_reads_.load(std::memory_order_acquire))
        return search_filtered(query, k, ef_search, {});
    const Top start = top();
    if (!start.present) return {};
    InternalId current = start.entry;
    for (std::size_t level = start.level; level > 0; --level) {
        current = search_layer<Access::Frozen>(query.data(), current, 1, level).front().node;
    }
    const std::size_t ef = std::min(size(), std::max(k, ef_search));
    const auto candidates = search_layer<Access::Frozen>(query.data(), current, ef, 0);
    std::vector<SearchResult> results;
    const std::size_t count = std::min(k, candidates.size());
    results.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        results.push_back({candidates[i].id, candidates[i].distance});
    return results;
}

// Shared tail of the quantized and filtered searches: greedy descent without filtering,
// filtered layer-0 search, optional exact FP32 re-rank of the best `rerank` candidates.
template <HnswIndex::Access Mode, class Distance, class Prefetch, class Admit>
std::vector<SearchResult> HnswIndex::search_with(const PreparedQuery& query,
                                                 const Distance& distance,
                                                 const Prefetch& prefetch, const Admit& admit,
                                                 std::size_t k, std::size_t ef_search,
                                                 std::size_t rerank) const {
    const Top start = top();
    if (!start.present) return {};
    InternalId current = start.entry;
    for (std::size_t level = start.level; level > 0; --level)
        current = search_layer_with<Mode>(distance, prefetch, current, 1, level).front().node;
    const std::size_t ef = std::min(size(), std::max(k, ef_search));
    auto candidates = search_layer_with<Mode>(distance, prefetch, current, ef, 0, admit);
    if (rerank != 0) {
        candidates.resize(std::min(candidates.size(), rerank));
        for (auto& candidate : candidates)
            candidate.distance = distance_to(query.data(), candidate.node);
        std::sort(candidates.begin(), candidates.end());
    }
    std::vector<SearchResult> results;
    const std::size_t count = std::min(k, candidates.size());
    results.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        results.push_back({candidates[i].id, candidates[i].distance});
    return results;
}

std::vector<SearchResult> HnswIndex::search_quantized(const PreparedQuery& query,
                                                      const QuantizedVectors& codes, std::size_t k,
                                                      std::size_t ef_search,
                                                      std::size_t rerank) const {
    return search_filtered(query, k, ef_search, {}, &codes, rerank);
}

std::vector<SearchResult> HnswIndex::search_filtered(const PreparedQuery& query, std::size_t k,
                                                     std::size_t ef_search,
                                                     const std::function<bool(VectorId)>& allow,
                                                     const QuantizedVectors* codes,
                                                     std::size_t rerank) const {
    ensure_usable();
    if (k == 0 || ef_search == 0)
        throw std::invalid_argument("HNSW k and ef_search must be positive.");
    if (query.dimension() != store_.dimension() || query.distance_kind() != store_.distance_kind())
        throw std::invalid_argument("Prepared query does not match HNSW VectorStore.");
    if (codes && (codes->size() != store_.size() || codes->dimension() != store_.dimension() ||
                  codes->metric() != store_.distance_kind()))
        throw std::invalid_argument("Quantized codes do not match the HNSW VectorStore.");
    if (!codes && rerank != 0) throw std::invalid_argument("HNSW rerank requires quantized codes.");
    if (rerank != 0 && rerank < k)
        throw std::invalid_argument("HNSW rerank must be 0 or at least k.");
    if (!codes || rerank != 0) require_rows();
    const auto admit_all = [](InternalId) { return true; };
    // Tombstoned positions are routed through but never admitted.
    const auto admit_live = [&](InternalId id) { return !store_.is_deleted(id); };
    const auto admit = [&](InternalId id) {
        return !store_.is_deleted(id) && allow(store_.ids()[id]);
    };
    const bool deletions = store_.deleted_count() != 0;
    const bool concurrent = concurrent_reads_.load(std::memory_order_acquire);
    // Every (access mode, admission) combination over one distance function.
    const auto dispatch = [&](const auto& distance, const auto& prefetch, std::size_t depth) {
        const auto run = [&](const auto& rule) {
            return concurrent ? search_with<Access::Reader>(query, distance, prefetch, rule, k,
                                                            ef_search, depth)
                              : search_with<Access::Frozen>(query, distance, prefetch, rule, k,
                                                            ef_search, depth);
        };
        if (allow) return run(admit);
        return deletions ? run(admit_live) : run(admit_all);
    };
    if (codes) {
        const auto approximate = codes->prepare(query);
        const auto distance = [&](InternalId id) { return approximate->distance(id); };
        const auto prefetch = [&](InternalId id) { approximate->prefetch(id); };
        return dispatch(distance, prefetch, rerank);
    }
    const auto distance = [&](InternalId id) { return distance_to(query.data(), id); };
    const auto prefetch = [&](InternalId id) { prefetch_row(id); };
    return dispatch(distance, prefetch, 0);
}

std::size_t HnswIndex::size() const noexcept { return successful_insertions_; }

bool HnswIndex::contains(VectorId id) const {
    const auto position = store_.position_of(id);
    return position && *position < levels_.size() && levels_[*position] != absent_level;
}

const HnswConfig& HnswIndex::config() const noexcept { return config_; }

std::optional<VectorId> HnswIndex::entry_point() const noexcept {
    const Top current = top();
    return current.present ? std::optional<VectorId>(store_.ids()[current.entry]) : std::nullopt;
}

std::optional<std::size_t> HnswIndex::max_level() const noexcept {
    const Top current = top();
    return current.present ? std::optional<std::size_t>(current.level) : std::nullopt;
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

bool HnswIndex::references_are_valid() const noexcept {
    std::size_t nodes = 0, observed_max = 0;
    for (std::size_t position = 0; position < levels_.size(); ++position) {
        if (levels_[position] == absent_level) continue;
        ++nodes;
        if (position >= store_.size()) return false;
        observed_max = std::max(observed_max, static_cast<std::size_t>(levels_[position]));
        for (std::size_t level = 0; level <= levels_[position]; ++level)
            for (const InternalId neighbor : neighbors_internal(static_cast<InternalId>(position), level))
                if (neighbor >= levels_.size() || levels_[neighbor] == absent_level ||
                    levels_[neighbor] < level)
                    return false;
    }
    const Top current = top();
    if (!current.present) return nodes == 0 && successful_insertions_ == 0;
    return current.entry < levels_.size() && levels_[current.entry] == current.level &&
           observed_max == current.level && nodes == successful_insertions_;
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
    const Top current = top();
    if (!current.present) {
        if (report.nodes != 0) ++report.entry_point_violations;
    } else {
        if (current.entry >= levels_.size() || levels_[current.entry] == absent_level ||
            levels_[current.entry] != current.level)
            ++report.entry_point_violations;
        if (observed_max != current.level) ++report.max_level_violations;
    }
    if (report.nodes != successful_insertions_) ++report.insertion_count_violations;
    return report;
}

}  // namespace aetherindex
