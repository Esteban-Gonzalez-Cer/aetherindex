#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>

#include "aether/storage/vector_store.hpp"

namespace aetherindex {

// Binary embedding file (.avec), version 1, little-endian:
//
//   offset  0  magic "AETHVEC1"          offset 32  rows_offset  u64 (64-byte aligned)
//   offset  8  version u32 (=1)           offset 40  file_bytes   u64
//   offset 12  dimension u32 (> 0)        offset 48  ids_crc32c   u32
//   offset 16  count u64 (> 0)            offset 52  rows_crc32c  u32
//   offset 24  ids_offset u64 (=64)       offset 56  flags u32 (=0)
//                                         offset 60  header_crc32c u32 (bytes 0..59)
//   ids_offset : VectorId ids[count] (uint32)
//   rows_offset: float rows[count][dimension] (IEEE-754 binary32, raw caller values)
//
// It replaces text parsing of embeddings only. It is not an index snapshot: graphs and
// lexical indexes are still built in memory by each process.
class VectorFileError : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};

// rows holds count*dimension raw components in id order. Writes to a temporary file
// and renames it, so a failed write never leaves a partial target. Throws
// invalid_argument for inconsistent arguments and VectorFileError on I/O failures.
void write_vector_file(const std::filesystem::path& path, std::span<const VectorId> ids,
                       std::span<const EmbeddingValue> rows, std::size_t dimension);

// Writes every row of an existing store in insertion order. Rows are written as stored,
// so a SquaredEuclidean store preserves raw values while a Cosine store writes unit rows.
void write_vector_file(const std::filesystem::path& path, const VectorStore& store);

// Validates header, sizes and checksums, then inserts each row into a new store with
// the requested metric (cosine normalizes on insert, as VectorStore always does).
// Duplicate IDs, non-finite or (for cosine) near-zero rows and any corruption throw
// VectorFileError naming the file and, where relevant, the row. Insertion order is the
// file order. The file is memory-mapped read-only where available.
std::unique_ptr<VectorStore> read_vector_file(const std::filesystem::path& path,
                                              DistanceKind metric);

}  // namespace aetherindex
