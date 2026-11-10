#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aether/lexical/bm25.hpp"
#include "aether/lexical/lexical_searcher.hpp"

namespace aetherindex {
struct SnapshotAccess;
}  // namespace aetherindex

namespace aetherindex::lexical {

// A BM25 index that accepts additions and deletions, built from immutable segments (each
// a Bm25Index) in the manner of Lucene:
//
// - add() indexes a batch of documents as a new segment; the documents are searchable as
//   soon as it returns.
// - remove() tombstones a document; it is never returned again. Like Lucene, corpus
//   statistics (N, average length, document frequencies) keep counting a deleted document
//   until its segment is merged or compacted.
// - compact() merges everything into one segment without the deleted documents. The result
//   answers bit-identically to an index built from scratch over the live documents.
// - When there are more than kMaxSegments segments, the two smallest are merged.
//
// Scores always use corpus-wide statistics, summed over the segments, so every document
// scores exactly as the exhaustive scorer over the whole corpus would. With one segment and
// no deletions, search() is exactly Bm25Index::search (the measured fast path). Otherwise
// each segment runs MaxScore (term-at-a-time if configured, the exhaustive scorer for raw
// postings) with bounds computed at query time from each block's maximum term frequency
// and minimum document length, and the per-segment Top-K lists are merged.
class SegmentedBm25 final : public LexicalSearcher {
   public:
    static constexpr std::size_t kMaxSegments = 8;

    explicit SegmentedBm25(Bm25Config config = {});
    // Starts from an already built index as the only segment (shared, never modified).
    explicit SegmentedBm25(std::shared_ptr<const Bm25Index> base);

    // Indexes (DocumentId, text) pairs as one new segment. A DocumentId that is live, or
    // repeated in the batch, throws invalid_argument and leaves the index unchanged. An
    // empty batch does nothing.
    void add(std::span<const std::pair<DocumentId, std::string>> documents);
    // Tombstones a live document. Returns false if no live document has this ID.
    bool remove(DocumentId id);
    bool contains(DocumentId id) const;
    // Rewrites the index as a single segment of the live documents.
    void compact();

    // Requires k > 0 (invalid_argument).
    [[nodiscard]] std::vector<RankedResult> search(std::string_view query, std::size_t k) const;
    [[nodiscard]] std::vector<RankedResult> search(
        std::string_view query, std::size_t k,
        const std::function<bool(DocumentId)>& allow) const override;

    const Bm25Config& config() const noexcept { return config_; }
    std::size_t segment_count() const noexcept { return segments_.size(); }
    // Documents counted by the statistics (live and not yet expunged deleted ones).
    std::size_t corpus_size() const noexcept;
    std::size_t live_count() const noexcept { return corpus_size() - deleted_count(); }
    std::size_t deleted_count() const noexcept;
    double average_document_length() const noexcept;
    // The only segment, when there is exactly one and nothing is deleted.
    const Bm25Index* single_index() const noexcept;

   private:
    friend struct ::aetherindex::SnapshotAccess;

    struct Segment {
        std::shared_ptr<const Bm25Index> index;
        std::vector<std::uint64_t> deleted;  // Tombstone bit per ordinal (empty if none).
        std::size_t deleted_count = 0;
        // Corpus-wide scoring data, prepared once a second segment or a deletion appears.
        std::vector<DocumentId> ids;          // Ordinal -> DocumentId (ascending).
        std::vector<std::uint32_t> lengths;   // Ordinal -> document length.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> impacts;  // Per block.
        bool prepared = false;
    };

    static std::span<const DocumentId> ids_of(const Segment& segment);
    std::size_t ordinal_of(const Segment& segment, DocumentId id) const;  // npos if absent.
    static bool is_deleted(const Segment& segment, std::size_t ordinal) noexcept;
    void prepare(Segment& segment) const;
    void prepare_all();
    std::shared_ptr<const Bm25Index> merge(std::span<const Segment* const> segments) const;
    void merge_if_needed();

    Bm25Config config_;
    std::vector<Segment> segments_;
};

}  // namespace aetherindex::lexical
