#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "aether/storage/vector_store.hpp"

namespace aetherindex::detail {

// Stream one row at a time into the sole owning store. Failures discard the entire load.
std::unique_ptr<VectorStore> load_csv_dataset(const std::filesystem::path& path,
                                              DistanceKind metric);
std::vector<EmbeddingValue> load_csv_query(const std::filesystem::path& path, std::size_t dimension,
                                           DistanceKind metric);

}  // namespace aetherindex::detail
