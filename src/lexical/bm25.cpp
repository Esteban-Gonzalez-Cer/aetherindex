#include "aether/lexical/bm25.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>

#include "aether/lexical/tokenizer.hpp"

namespace aetherindex::lexical {

void validate_bm25_config(const Bm25Config& config) {
    if (!std::isfinite(config.k1) || config.k1 <= 0.0 || !std::isfinite(config.b) ||
        config.b < 0.0 || config.b > 1.0) {
        throw std::invalid_argument("BM25 requires finite k1 > 0 and finite b in [0,1].");
    }
    switch (config.strategy) {
        case Bm25Strategy::Exhaustive:
        case Bm25Strategy::TermAtATime:
        case Bm25Strategy::Wand:
        case Bm25Strategy::BlockMaxWand:
            return;
    }
    throw std::invalid_argument("Unsupported BM25 strategy.");
}

double compute_idf(std::size_t document_count, std::size_t document_frequency) {
    if (document_count == 0 || document_frequency == 0 || document_frequency > document_count) {
        throw std::invalid_argument("IDF requires 0 < DF <= N.");
    }
    // Subtract integers before conversion to preserve N-DF when N is very large.
    const double rare = static_cast<double>(document_count - document_frequency) + 0.5;
    return std::log1p(rare / (static_cast<double>(document_frequency) + 0.5));
}

namespace detail {

double bm25_length_norm(std::uint32_t document_length, double avgdl, const Bm25Config& config) {
    return 1.0 - config.b + (config.b == 0.0 ? 0.0 : (config.b * document_length) / avgdl);
}

double bm25_score_from_norm(std::uint32_t tf, double normalized, double idf,
                            const Bm25Config& config) {
    if (tf == 0) return 0.0;
    const double frequency = tf;
    // Algebraically identical scaling avoids inf/inf for large finite k1.
    const double factor =
        config.k1 >= 1.0
            ? frequency * (1.0 + 1.0 / config.k1) / (frequency / config.k1 + normalized)
            : frequency * (config.k1 + 1.0) / (frequency + config.k1 * normalized);
    return idf * factor;
}

}  // namespace detail

double bm25_term_score(std::uint32_t tf, std::uint32_t document_length, double avgdl, double idf,
                       const Bm25Config& config) {
    validate_bm25_config(config);
    if (!std::isfinite(avgdl) || avgdl <= 0.0 || !std::isfinite(idf) || idf <= 0.0 ||
        tf > document_length) {
        throw std::invalid_argument("Invalid BM25 term statistics.");
    }
    if (tf == 0) return 0.0;
    const double normalized = detail::bm25_length_norm(document_length, avgdl, config);
    if (!std::isfinite(normalized)) throw std::overflow_error("BM25 length overflow.");
    const double score = detail::bm25_score_from_norm(tf, normalized, idf, config);
    if (!std::isfinite(score)) throw std::overflow_error("BM25 score overflow.");
    return score;
}

namespace {

constexpr std::size_t kBlockSize = 128;
constexpr std::uint32_t kEnd = std::numeric_limits<std::uint32_t>::max();
// Conservative inflation of summed upper bounds: summation order differs from the exact
// per-document order, so rounding must never prune a document that belongs to the Top-K.
constexpr double kBoundSlack = 1.0 + 1e-9;

void put_varint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(value | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

std::uint64_t get_varint(const std::uint8_t*& in) {
    std::uint64_t value = 0;
    for (int shift = 0;; shift += 7) {
        const std::uint8_t byte = *in++;
        value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
        if (!(byte & 0x80)) return value;
    }
}

bool better(const RankedResult& a, const RankedResult& b) {
    return a.score != b.score ? a.score > b.score : a.document_id < b.document_id;
}

std::vector<std::string> query_terms(std::string_view query) {
    auto terms = Tokenizer{}.tokenize(query);
    std::sort(terms.begin(), terms.end());
    terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
    return terms;
}

// Bounded Top-K under the ranking order; top() is the current worst kept result.
class TopK {
   public:
    explicit TopK(std::size_t k) : k_(k) { heap_.reserve(std::min<std::size_t>(k, 4096)); }
    bool full() const noexcept { return heap_.size() == k_; }
    double threshold() const noexcept { return heap_.front().score; }
    void offer(const RankedResult& candidate) {
        const auto worse_on_top = [](const RankedResult& a, const RankedResult& b) {
            return better(a, b);
        };
        if (!full()) {
            heap_.push_back(candidate);
            std::push_heap(heap_.begin(), heap_.end(), worse_on_top);
        } else if (better(candidate, heap_.front())) {
            std::pop_heap(heap_.begin(), heap_.end(), worse_on_top);
            heap_.back() = candidate;
            std::push_heap(heap_.begin(), heap_.end(), worse_on_top);
        }
    }
    std::vector<RankedResult> sorted() && {
        std::sort(heap_.begin(), heap_.end(), better);
        return std::move(heap_);
    }

   private:
    std::size_t k_;
    std::vector<RankedResult> heap_;
};

}  // namespace

// Document-at-a-time cursor over one compressed posting list. The "shallow" block
// pointer serves block-max lookups without decoding; it only moves forward.
class Bm25Index::Cursor {
   public:
    Cursor(const TermLayout& term, std::size_t rank) : term_(&term), rank_(rank) { decode(0); }

    std::uint32_t doc() const noexcept { return doc_; }
    std::uint32_t tf() const noexcept { return tfs_[position_]; }
    std::size_t rank() const noexcept { return rank_; }
    double upper_bound() const noexcept { return term_->max_score; }
    double idf() const noexcept { return term_->idf; }

    void next() {
        if (++position_ < count_) {
            doc_ = docs_[position_];
        } else if (block_ + 1 < term_->blocks.size()) {
            decode(block_ + 1);
        } else {
            doc_ = kEnd;
        }
    }
    void next_geq(std::uint32_t target) {
        if (target <= doc_) return;
        std::size_t b = block_;
        while (b < term_->blocks.size() && term_->blocks[b].last < target) ++b;
        if (b == term_->blocks.size()) {
            doc_ = kEnd;
            return;
        }
        if (b != block_) decode(b);
        while (docs_[position_] < target) ++position_;
        doc_ = docs_[position_];
    }
    // Upper bound of the block that would contain `target` (0 past the list end).
    double block_max(std::uint32_t target) {
        // Never answer with a later block's bound: re-anchor if target precedes shallow_.
        if (shallow_ > block_ && term_->blocks[shallow_ - 1].last >= target) shallow_ = block_;
        while (shallow_ < term_->blocks.size() && term_->blocks[shallow_].last < target) ++shallow_;
        return shallow_ < term_->blocks.size()
                   ? static_cast<double>(term_->blocks[shallow_].max_score)
                   : 0.0;
    }
    std::uint32_t block_last() const noexcept {
        return shallow_ < term_->blocks.size() ? term_->blocks[shallow_].last : kEnd;
    }

   private:
    void decode(std::size_t b) {
        block_ = b;
        const Block& block = term_->blocks[b];
        const std::uint8_t* in = term_->bytes.data() + block.offset;
        std::uint32_t previous = b == 0 ? 0 : term_->blocks[b - 1].last;
        for (std::uint32_t i = 0; i < block.count; ++i) {
            const std::uint64_t packed = get_varint(in);
            previous += static_cast<std::uint32_t>(packed >> 1);
            docs_[i] = previous;
            tfs_[i] = (packed & 1) ? 1 : static_cast<std::uint32_t>(get_varint(in));
        }
        count_ = block.count;
        position_ = 0;
        doc_ = docs_[0];
        shallow_ = std::max(shallow_, b);
    }

    const TermLayout* term_;
    std::size_t rank_;
    std::size_t block_ = 0, shallow_ = 0, position_ = 0, count_ = 0;
    std::uint32_t doc_ = kEnd;
    std::array<std::uint32_t, kBlockSize> docs_{}, tfs_{};
};

Bm25Index::Bm25Index(Bm25Config config) : config_(config) { validate_bm25_config(config_); }
void Bm25Index::add_document(DocumentId id, std::string_view text) {
    index_.add_document(id, text);
}

void Bm25Index::build() {
    index_.build();
    std::size_t postings = 0, vocabulary = 0;
    index_.for_each_term([&](std::string_view, std::span<const Posting> list) {
        postings += list.size();
        ++vocabulary;
    });
    memory_.postings = postings;
    memory_.raw_bytes = postings * sizeof(Posting) + vocabulary * sizeof(PostingList);
    if (config_.strategy != Bm25Strategy::Exhaustive) {
        build_layout();
        index_.release_postings();
    }
}

// Compressed blocks over dense ordinals assigned in ascending DocumentId order, so
// ordinal order equals the DocumentId tie order of the ranking.
void Bm25Index::build_layout() {
    document_ids_ = index_.document_ids();
    std::unordered_map<DocumentId, std::uint32_t> ordinal;
    ordinal.reserve(document_ids_.size());
    length_norm_.resize(document_ids_.size());
    const double avgdl = index_.average_document_length();
    for (std::size_t i = 0; i < document_ids_.size(); ++i) {
        ordinal.emplace(document_ids_[i], static_cast<std::uint32_t>(i));
        length_norm_[i] =
            detail::bm25_length_norm(*index_.document_length(document_ids_[i]), avgdl, config_);
        if (!std::isfinite(length_norm_[i])) throw std::overflow_error("BM25 length overflow.");
    }
    const std::size_t n = index_.document_count();
    index_.for_each_term([&](std::string_view term, std::span<const Posting> list) {
        TermLayout layout;
        layout.idf = compute_idf(n, list.size());
        std::uint32_t previous = 0;
        for (std::size_t start = 0; start < list.size(); start += kBlockSize) {
            const std::size_t end = std::min(list.size(), start + kBlockSize);
            Block block{0, static_cast<std::uint32_t>(layout.bytes.size()),
                        static_cast<std::uint32_t>(end - start), 0.0f};
            double block_max = 0;
            for (std::size_t i = start; i < end; ++i) {
                const std::uint32_t current = ordinal.at(list[i].document_id);
                const std::uint32_t tf = list[i].term_frequency;
                put_varint(layout.bytes,
                           (static_cast<std::uint64_t>(current - previous) << 1) | (tf == 1));
                if (tf != 1) put_varint(layout.bytes, tf);
                previous = current;
                const double score =
                    detail::bm25_score_from_norm(tf, length_norm_[current], layout.idf, config_);
                if (!std::isfinite(score)) throw std::overflow_error("BM25 score overflow.");
                block_max = std::max(block_max, score);
            }
            // Round the float bound up so it never falls below the exact double maximum.
            block.max_score = static_cast<float>(block_max);
            if (static_cast<double>(block.max_score) < block_max)
                block.max_score =
                    std::nextafter(block.max_score, std::numeric_limits<float>::infinity());
            block.last = previous;
            layout.max_score = std::max(layout.max_score, block_max);
            layout.blocks.push_back(block);
        }
        layout.bytes.shrink_to_fit();
        memory_.blocks += layout.blocks.size();
        memory_.compressed_bytes +=
            layout.bytes.size() + layout.blocks.size() * sizeof(Block) + sizeof(TermLayout);
        layout_.emplace(std::string(term), std::move(layout));
    });
    memory_.compressed_bytes +=
        length_norm_.size() * sizeof(double) + document_ids_.size() * sizeof(DocumentId);
}

std::vector<RankedResult> Bm25Index::search(std::string_view query, std::size_t k) const {
    return search(query, k, {});
}

std::vector<RankedResult> Bm25Index::search(std::string_view query, std::size_t k,
                                            const Allow& allow) const {
    if (!index_.is_built()) throw std::logic_error("BM25 search requires build().");
    if (k == 0) throw std::invalid_argument("BM25 k must be greater than zero.");
    const auto terms = query_terms(query);
    switch (config_.strategy) {
        case Bm25Strategy::Exhaustive:
            return search_exhaustive(terms, k, allow);
        case Bm25Strategy::TermAtATime:
            return search_term_at_a_time(terms, k, allow);
        case Bm25Strategy::Wand:
            return search_wand(terms, k, false, allow);
        case Bm25Strategy::BlockMaxWand:
            return search_wand(terms, k, true, allow);
    }
    throw std::logic_error("Unsupported BM25 strategy.");
}

std::vector<RankedResult> Bm25Index::search_exhaustive(const std::vector<std::string>& terms,
                                                       std::size_t k, const Allow& allow) const {
    std::unordered_map<DocumentId, double> scores;
    for (const auto& term : terms) {
        const auto list = index_.find(term);
        if (list.empty()) continue;
        const double idf = compute_idf(index_.document_count(), list.size());
        for (const auto& posting : list) {
            if (allow && !allow(posting.document_id)) continue;
            auto& score = scores[posting.document_id];
            score += bm25_term_score(posting.term_frequency,
                                     index_.document_length(posting.document_id).value(),
                                     index_.average_document_length(), idf, config_);
            if (!std::isfinite(score))
                throw std::overflow_error("BM25 accumulated score overflow.");
        }
    }
    std::vector<RankedResult> results;
    results.reserve(scores.size());
    for (const auto& [id, score] : scores) results.push_back({id, score});
    std::sort(results.begin(), results.end(), better);
    if (results.size() > k) results.resize(k);
    return results;
}

// Dense per-thread accumulator; terms are added in the same lexicographic order as the
// exhaustive path, so each document's floating-point sum is identical.
std::vector<RankedResult> Bm25Index::search_term_at_a_time(const std::vector<std::string>& terms,
                                                           std::size_t k,
                                                           const Allow& allow) const {
    thread_local std::vector<double> scores;
    thread_local std::vector<std::uint8_t> touched_flag;
    thread_local std::vector<std::uint32_t> touched;
    if (scores.size() < document_ids_.size()) {
        scores.resize(document_ids_.size(), 0.0);
        touched_flag.resize(document_ids_.size(), 0);
    }
    touched.clear();
    for (const auto& term : terms) {
        const auto found = layout_.find(term);
        if (found == layout_.end()) continue;
        Cursor cursor(found->second, 0);
        for (; cursor.doc() != kEnd; cursor.next()) {
            const std::uint32_t d = cursor.doc();
            if (!touched_flag[d]) {
                touched_flag[d] = 1;
                touched.push_back(d);
            }
            scores[d] +=
                detail::bm25_score_from_norm(cursor.tf(), length_norm_[d], cursor.idf(), config_);
        }
    }
    TopK top(k);
    for (std::uint32_t d : touched) {
        const double score = scores[d];
        scores[d] = 0.0;
        touched_flag[d] = 0;
        if (allow && !allow(document_ids_[d])) continue;
        if (!std::isfinite(score)) throw std::overflow_error("BM25 accumulated score overflow.");
        top.offer({document_ids_[d], score});
    }
    return std::move(top).sorted();
}

// Rank-safe WAND / Block-Max WAND (Broder et al. 2003; Ding and Suel 2011). A document is
// skipped only when a (slack-inflated) bound is strictly below the K-th score, so
// documents tying the threshold are still scored and the DocumentId tie rule holds.
// A filter only removes candidates, so it can never raise the threshold incorrectly;
// excluded documents are skipped without being offered.
std::vector<RankedResult> Bm25Index::search_wand(const std::vector<std::string>& terms,
                                                 std::size_t k, bool block_max,
                                                 const Allow& allow) const {
    std::vector<Cursor> cursors;
    for (std::size_t rank = 0; rank < terms.size(); ++rank) {
        const auto found = layout_.find(terms[rank]);
        if (found != layout_.end()) cursors.emplace_back(found->second, rank);
    }
    std::vector<Cursor*> order;
    for (auto& cursor : cursors) order.push_back(&cursor);
    const auto by_doc = [](const Cursor* a, const Cursor* b) { return a->doc() < b->doc(); };
    TopK top(k);
    std::vector<std::pair<std::size_t, double>> contributions;
    for (;;) {
        std::sort(order.begin(), order.end(), by_doc);
        // Pivot: first cursor whose cumulative upper bound can reach the threshold.
        double bound = 0;
        std::size_t pivot = order.size();
        for (std::size_t i = 0; i < order.size() && order[i]->doc() != kEnd; ++i) {
            bound += order[i]->upper_bound();
            if (!top.full() || bound * kBoundSlack >= top.threshold()) {
                pivot = i;
                break;
            }
        }
        if (pivot == order.size()) break;
        const std::uint32_t pivot_doc = order[pivot]->doc();
        while (pivot + 1 < order.size() && order[pivot + 1]->doc() == pivot_doc) ++pivot;

        if (block_max && top.full()) {
            double block_bound = 0;
            for (std::size_t i = 0; i <= pivot; ++i) block_bound += order[i]->block_max(pivot_doc);
            if (block_bound * kBoundSlack < top.threshold()) {
                // Skip past the shallowest block end, but not beyond the next list's doc.
                std::uint32_t next = kEnd;
                for (std::size_t i = 0; i <= pivot; ++i)
                    next = std::min(
                        next, order[i]->block_last() == kEnd ? kEnd : order[i]->block_last() + 1);
                if (pivot + 1 < order.size()) next = std::min(next, order[pivot + 1]->doc());
                if (next <= pivot_doc) next = pivot_doc + 1;
                std::size_t widest = 0;
                for (std::size_t i = 1; i <= pivot; ++i)
                    if (order[i]->upper_bound() > order[widest]->upper_bound()) widest = i;
                order[widest]->next_geq(next);
                continue;
            }
        }

        if (order[0]->doc() == pivot_doc && allow && !allow(document_ids_[pivot_doc])) {
            for (std::size_t i = 0; i <= pivot; ++i) order[i]->next();
        } else if (order[0]->doc() == pivot_doc) {
            // Every list containing pivot_doc is at it; sum in term (rank) order.
            contributions.clear();
            for (std::size_t i = 0; i <= pivot; ++i)
                contributions.emplace_back(
                    order[i]->rank(),
                    detail::bm25_score_from_norm(order[i]->tf(), length_norm_[pivot_doc],
                                                 order[i]->idf(), config_));
            std::sort(contributions.begin(), contributions.end());
            double score = 0.0;
            for (const auto& [rank, value] : contributions) score += value;
            if (!std::isfinite(score))
                throw std::overflow_error("BM25 accumulated score overflow.");
            top.offer({document_ids_[pivot_doc], score});
            for (std::size_t i = 0; i <= pivot; ++i) order[i]->next();
        } else {
            // Advance a lagging list (the one with the largest bound) to the pivot.
            std::size_t widest = 0;
            for (std::size_t i = 1; i < pivot && order[i]->doc() != pivot_doc; ++i)
                if (order[i]->upper_bound() > order[widest]->upper_bound()) widest = i;
            order[widest]->next_geq(pivot_doc);
        }
    }
    return std::move(top).sorted();
}

bool Bm25Index::is_built() const noexcept { return index_.is_built(); }
std::size_t Bm25Index::document_count() const noexcept { return index_.document_count(); }
double Bm25Index::average_document_length() const noexcept {
    return index_.average_document_length();
}
const InvertedIndex& Bm25Index::inverted_index() const noexcept { return index_; }
Bm25Config Bm25Index::config() const noexcept { return config_; }
PostingMemory Bm25Index::posting_memory() const noexcept { return memory_; }

}  // namespace aetherindex::lexical
