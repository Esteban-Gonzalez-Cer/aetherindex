#include "aether/lexical/segmented_bm25.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace aetherindex::lexical {
namespace {

constexpr std::size_t kAbsent = std::numeric_limits<std::size_t>::max();

bool better(const RankedResult& a, const RankedResult& b) {
    return a.score != b.score ? a.score > b.score : a.document_id < b.document_id;
}

}  // namespace

SegmentedBm25::SegmentedBm25(Bm25Config config) : config_(config) {
    validate_bm25_config(config_);
}

SegmentedBm25::SegmentedBm25(std::shared_ptr<const Bm25Index> base)
    : config_(base ? base->config() : Bm25Config{}) {
    if (!base || !base->is_built())
        throw std::invalid_argument("SegmentedBm25 needs a built Bm25Index.");
    segments_.push_back({std::move(base), {}, 0, {}, {}, {}, false});
}

std::span<const DocumentId> SegmentedBm25::ids_of(const Segment& segment) {
    if (segment.prepared) return segment.ids;
    return segment.index->document_ids_;  // Compressed layout (raw postings are prepared).
}

std::size_t SegmentedBm25::ordinal_of(const Segment& segment, DocumentId id) const {
    std::span<const DocumentId> ids;
    std::vector<DocumentId> raw;
    if (segment.prepared || !segment.index->index_.has_postings()) {
        ids = ids_of(segment);
    } else {
        raw = segment.index->ordinal_ids();
        ids = raw;
    }
    const auto found = std::lower_bound(ids.begin(), ids.end(), id);
    return found != ids.end() && *found == id ? static_cast<std::size_t>(found - ids.begin())
                                              : kAbsent;
}

bool SegmentedBm25::is_deleted(const Segment& segment, std::size_t ordinal) noexcept {
    return segment.deleted_count != 0 && ordinal / 64 < segment.deleted.size() &&
           ((segment.deleted[ordinal / 64] >> (ordinal % 64)) & 1u) != 0;
}

void SegmentedBm25::prepare(Segment& segment) const {
    if (segment.prepared) return;
    segment.ids = segment.index->ordinal_ids();
    segment.lengths = segment.index->ordinal_lengths(segment.ids);
    segment.impacts = segment.index->block_impacts(segment.lengths);
    segment.prepared = true;
}

void SegmentedBm25::prepare_all() {
    for (auto& segment : segments_) prepare(segment);
}

std::size_t SegmentedBm25::corpus_size() const noexcept {
    std::size_t total = 0;
    for (const auto& segment : segments_) total += segment.index->document_count();
    return total;
}

std::size_t SegmentedBm25::deleted_count() const noexcept {
    std::size_t total = 0;
    for (const auto& segment : segments_) total += segment.deleted_count;
    return total;
}

double SegmentedBm25::average_document_length() const noexcept {
    std::uint64_t tokens = 0;
    std::size_t documents = 0;
    for (const auto& segment : segments_) {
        tokens += segment.index->total_token_count();
        documents += segment.index->document_count();
    }
    return documents == 0 ? 0.0 : static_cast<double>(tokens) / static_cast<double>(documents);
}

const Bm25Index* SegmentedBm25::single_index() const noexcept {
    return segments_.size() == 1 && segments_.front().deleted_count == 0
               ? segments_.front().index.get()
               : nullptr;
}

bool SegmentedBm25::contains(DocumentId id) const {
    for (const auto& segment : segments_) {
        const auto ordinal = ordinal_of(segment, id);
        if (ordinal != kAbsent && !is_deleted(segment, ordinal)) return true;
    }
    return false;
}

void SegmentedBm25::add(std::span<const std::pair<DocumentId, std::string>> documents) {
    if (documents.empty()) return;
    std::unordered_set<DocumentId> batch;
    for (const auto& [id, text] : documents) {
        if (!batch.insert(id).second) throw std::invalid_argument("Duplicate DocumentId in batch.");
        if (contains(id)) throw std::invalid_argument("DocumentId is already indexed.");
    }
    auto index = std::make_shared<Bm25Index>(config_);
    for (const auto& [id, text] : documents) index->add_document(id, text);
    index->build();
    // Build everything that can throw before publishing the new segment.
    Segment segment{std::move(index), {}, 0, {}, {}, {}, false};
    prepare(segment);
    prepare_all();
    segments_.push_back(std::move(segment));
    merge_if_needed();
}

bool SegmentedBm25::remove(DocumentId id) {
    for (auto& segment : segments_) {
        const auto ordinal = ordinal_of(segment, id);
        if (ordinal == kAbsent || is_deleted(segment, ordinal)) continue;
        prepare_all();
        if (segment.deleted.empty())
            segment.deleted.assign((segment.index->document_count() + 63) / 64, 0);
        segment.deleted[ordinal / 64] |= std::uint64_t{1} << (ordinal % 64);
        ++segment.deleted_count;
        return true;
    }
    return false;
}

std::shared_ptr<const Bm25Index> SegmentedBm25::merge(
    std::span<const Segment* const> segments) const {
    std::unordered_map<std::string, PostingList, InvertedIndex::TermHash, std::equal_to<>> postings;
    std::unordered_map<DocumentId, std::uint32_t> lengths;
    std::uint64_t tokens = 0;
    for (const Segment* segment : segments) {
        const auto ids = segment->index->ordinal_ids();
        const auto segment_lengths = segment->index->ordinal_lengths(ids);
        for (std::size_t ordinal = 0; ordinal < ids.size(); ++ordinal) {
            if (is_deleted(*segment, ordinal)) continue;
            lengths.emplace(ids[ordinal], segment_lengths[ordinal]);
            tokens += segment_lengths[ordinal];
        }
        segment->index->for_each_posting_list(
            [&](std::string_view term, std::span<const Posting> list) {
                PostingList* target = nullptr;
                for (const auto& posting : list) {
                    if (segment->deleted_count != 0) {
                        const auto ordinal = static_cast<std::size_t>(
                            std::lower_bound(ids.begin(), ids.end(), posting.document_id) -
                            ids.begin());
                        if (is_deleted(*segment, ordinal)) continue;
                    }
                    if (!target) target = &postings[std::string(term)];
                    target->push_back(posting);
                }
            });
    }
    if (segments.size() > 1)
        for (auto& [term, list] : postings)
            std::sort(list.begin(), list.end(), [](const Posting& a, const Posting& b) {
                return a.document_id < b.document_id;
            });
    return Bm25Index::from_postings(config_, std::move(postings), std::move(lengths), tokens);
}

void SegmentedBm25::compact() {
    if (segments_.empty() || single_index()) return;
    std::vector<const Segment*> all;
    for (const auto& segment : segments_) all.push_back(&segment);
    auto merged = merge(all);
    segments_.clear();
    if (merged->document_count() != 0) segments_.push_back({std::move(merged), {}, 0, {}, {}, {}, false});
}

void SegmentedBm25::merge_if_needed() {
    while (segments_.size() > kMaxSegments) {
        // The two segments with the fewest live documents.
        std::vector<std::size_t> order(segments_.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        const auto live = [&](std::size_t i) {
            return segments_[i].index->document_count() - segments_[i].deleted_count;
        };
        std::partial_sort(order.begin(), order.begin() + 2, order.end(),
                          [&](std::size_t a, std::size_t b) { return live(a) < live(b); });
        const Segment* pair[] = {&segments_[order[0]], &segments_[order[1]]};
        Segment merged{merge(pair), {}, 0, {}, {}, {}, false};
        prepare(merged);
        const std::size_t first = std::min(order[0], order[1]);
        const std::size_t second = std::max(order[0], order[1]);
        segments_.erase(segments_.begin() + static_cast<std::ptrdiff_t>(second));
        segments_[first] = std::move(merged);
    }
}

std::vector<RankedResult> SegmentedBm25::search(std::string_view query, std::size_t k) const {
    return search(query, k, {});
}

std::vector<RankedResult> SegmentedBm25::search(std::string_view query, std::size_t k,
                                                const std::function<bool(DocumentId)>& allow) const {
    if (k == 0) throw std::invalid_argument("BM25 k must be greater than zero.");
    if (const Bm25Index* only = single_index()) return only->search(query, k, allow);
    if (segments_.empty()) return {};
    const auto terms = segments_.front().index->query_terms(query);
    std::size_t documents = 0;
    std::uint64_t tokens = 0;
    for (const auto& segment : segments_) {
        documents += segment.index->document_count();
        tokens += segment.index->total_token_count();
    }
    // Same expressions as a single index over the whole corpus.
    const double average = documents == 0 ? 0.0
                                          : static_cast<double>(tokens) /
                                                static_cast<double>(documents);
    std::vector<double> idf(terms.size(), 0.0);
    for (std::size_t i = 0; i < terms.size(); ++i) {
        std::size_t frequency = 0;
        for (const auto& segment : segments_)
            frequency += segment.index->term_document_frequency(terms[i].text);
        if (frequency != 0) idf[i] = compute_idf(documents, frequency);
    }
    std::vector<RankedResult> results;
    for (const auto& segment : segments_) {
        const Bm25Index::GlobalContext context{
            average, segment.lengths, segment.ids,
            segment.deleted_count ? segment.deleted.data() : nullptr, segment.impacts};
        auto partial = segment.index->search_global(terms, idf, k, allow, context);
        results.insert(results.end(), partial.begin(), partial.end());
    }
    std::sort(results.begin(), results.end(), better);
    if (results.size() > k) results.resize(k);
    return results;
}

}  // namespace aetherindex::lexical
