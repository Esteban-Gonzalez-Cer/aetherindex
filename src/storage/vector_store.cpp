#include "aether/storage/vector_store.hpp"

#include <cmath>
#include <stdexcept>

#include "aether/core/vector_math.hpp"

namespace aetherindex {

VectorStore::VectorStore(std::size_t dimension, DistanceKind metric)
    : dimension_(dimension), stride_(detail::padded_dimension(dimension)), metric_(metric) {
    if (dimension == 0) {
        throw std::invalid_argument("La dimension debe ser mayor que cero.");
    }
    if (metric != DistanceKind::Cosine && metric != DistanceKind::SquaredEuclidean) {
        throw std::invalid_argument("Metrica de distancia no soportada.");
    }
}

InsertStatus VectorStore::insert(VectorId id, std::span<const EmbeddingValue> embedding) {
    if (rows_released_) throw std::logic_error("VectorStore rows were released.");
    if (embedding.size() != dimension_) {
        return InsertStatus::DimensionMismatch;
    }
    if (contains(id)) {
        return InsertStatus::DuplicateId;
    }

    std::optional<detail::Normalization> norm;
    if (metric_ == DistanceKind::Cosine) {
        norm = detail::normalization(embedding, 1e-7f);
        if (!norm) return InsertStatus::InvalidEmbedding;
    } else {
        for (float value : embedding) {
            if (!std::isfinite(value)) return InsertStatus::InvalidEmbedding;
        }
    }

    // Stage first: embedding may refer to data_ which insertion can reallocate.
    AlignedVector<float> pending(stride_, 0.0f);
    for (std::size_t i = 0; i < dimension_; ++i) {
        pending[i] = norm ? norm->apply(embedding[i]) : embedding[i];
    }
    append_row(id, pending.data());
    return InsertStatus::Success;
}

std::optional<std::span<const EmbeddingValue>> VectorStore::find(VectorId id) const {
    if (rows_released_) throw std::logic_error("VectorStore rows were released.");
    const auto position = position_of(id);
    if (!position) return std::nullopt;
    return std::span<const EmbeddingValue>(data_).subspan(*position * stride_, dimension_);
}

void VectorStore::materialize_positions() {
    if (!identity_) return;
    std::unordered_map<VectorId, std::size_t> positions;
    positions.reserve(ids_.size() + 1);
    for (std::size_t i = 0; i < ids_.size(); ++i)
        if (!is_deleted(i)) positions.emplace(ids_[i], i);
    positions_.swap(positions);
    identity_ = false;
}

void VectorStore::append_row(VectorId id, const float* row) {
    // Map first (it may allocate); the vectors after, rolled back on failure.
    const bool stays_identity = identity_ && id == ids_.size();
    if (!stays_identity) materialize_positions();
    const std::size_t old_data_size = data_.size();
    const std::size_t old_size = ids_.size();
    if (!stays_identity) positions_.emplace(id, old_size);
    try {
        data_.insert(data_.end(), row, row + stride_);
        ids_.push_back(id);
    } catch (...) {
        data_.resize(old_data_size);
        ids_.resize(old_size);
        if (!stays_identity) positions_.erase(id);
        throw;
    }
}

std::size_t VectorStore::size() const noexcept { return ids_.size(); }

std::size_t VectorStore::dimension() const noexcept { return dimension_; }

DistanceKind VectorStore::distance_kind() const noexcept { return metric_; }

bool VectorStore::contains(VectorId id) const { return position_of(id).has_value(); }

std::optional<std::size_t> VectorStore::position_of(VectorId id) const {
    if (identity_)
        return id < ids_.size() && !is_deleted(id) ? std::optional<std::size_t>(id) : std::nullopt;
    const auto it = positions_.find(id);
    return it == positions_.end() ? std::nullopt : std::optional<std::size_t>(it->second);
}

void VectorStore::release_rows() {
    AlignedVector<EmbeddingValue>().swap(data_);
    rows_released_ = true;
}

bool VectorStore::erase(VectorId id) {
    const auto found = position_of(id);
    if (!found) return false;
    const std::size_t position = *found;
    if (deleted_.size() <= position / 64) deleted_.resize(ids_.size() / 64 + 1, 0);
    deleted_[position / 64] |= std::uint64_t{1} << (position % 64);
    if (!identity_) positions_.erase(id);
    ++deleted_count_;
    return true;
}

InsertStatus VectorStore::insert_from(const VectorStore& source, std::size_t position,
                                      VectorId id) {
    if (rows_released_ || !source.has_rows())
        throw std::logic_error("VectorStore rows were released.");
    if (source.dimension_ != dimension_ || source.metric_ != metric_)
        return InsertStatus::DimensionMismatch;
    if (position >= source.size()) throw std::out_of_range("Source position is absent.");
    if (contains(id)) return InsertStatus::DuplicateId;
    // Stage first: source may be this store, and appending can reallocate data_.
    const AlignedVector<float> row(source.data_by_position(position),
                                   source.data_by_position(position) + stride_);
    append_row(id, row.data());
    return InsertStatus::Success;
}

std::unique_ptr<VectorStore> VectorStore::compacted() const {
    if (rows_released_) throw std::logic_error("VectorStore rows were released.");
    auto store = std::make_unique<VectorStore>(dimension_, metric_);
    store->reserve(live_size());
    for (std::size_t position = 0; position < ids_.size(); ++position) {
        if (is_deleted(position)) continue;
        store->append_row(ids_[position], data_by_position(position));
    }
    return store;
}

void VectorStore::reserve(std::size_t count) {
    if (rows_released_) throw std::logic_error("VectorStore rows were released.");
    if (count > data_.max_size() / stride_)
        throw std::length_error("VectorStore capacity overflow.");
    data_.reserve(count * stride_);
    ids_.reserve(count);
    if (!identity_) positions_.reserve(count);
}

}  // namespace aetherindex
