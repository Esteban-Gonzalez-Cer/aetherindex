#include "aether/lexical/bm25.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

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
        case Bm25Strategy::MaxScore:
            break;
        default:
            throw std::invalid_argument("Unsupported BM25 strategy.");
    }
    if (config.analyzer != Analyzer::AsciiTechV1 && config.analyzer != Analyzer::EnglishV1) {
        throw std::invalid_argument("Unsupported BM25 analyzer.");
    }
    if (config.query_weighting != QueryTermWeighting::Unique &&
        config.query_weighting != QueryTermWeighting::Frequency) {
        throw std::invalid_argument("Unsupported BM25 query term weighting.");
    }
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

// An empty document gets 1 - b, the same value as the formula whenever avgdl > 0; this
// also covers a corpus of only empty documents (avgdl = 0), whose norms are never used.
double bm25_length_norm(std::uint32_t document_length, double avgdl, const Bm25Config& config) {
    return 1.0 - config.b +
           (config.b == 0.0 || document_length == 0 ? 0.0
                                                    : (config.b * document_length) / avgdl);
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

static_assert(std::endian::native == std::endian::little,
              "Bit-packed postings are read with little-endian 8-byte loads.");

// Appends count values of `bits` bits each, least significant bit first.
void pack(std::vector<std::uint8_t>& out, const std::uint32_t* values, std::size_t count,
          unsigned bits) {
    if (bits == 0) return;
    const std::size_t start = out.size();
    out.resize(start + (count * bits + 7) / 8, 0);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t value = values[i];
        for (unsigned done = 0; done < bits;) {
            const std::size_t position = i * bits + done;
            out[start + position / 8] |=
                static_cast<std::uint8_t>((value >> done) << (position % 8));
            done += 8 - static_cast<unsigned>(position % 8);
        }
    }
}

// Width-specialized unpacking, no branches. Up to 8 bits, eight values fill exactly Bits
// bytes, so each group of eight comes from one unaligned 8-byte load (and `out` receives up
// to 7 extra values past `count`, so it must hold a multiple of 8); wider values use one
// load each. The postings buffer is padded, so loads past the last value stay inside it.
template <unsigned Bits>
void unpack_fixed(const std::uint8_t* in, std::uint32_t* out, std::size_t count) {
    if constexpr (Bits == 0) {
        std::fill_n(out, count, 0u);
    } else if constexpr (Bits <= 8) {
        constexpr std::uint64_t mask = (std::uint64_t{1} << Bits) - 1;
        for (std::size_t group = 0; group < count; group += 8) {
            std::uint64_t word;
            std::memcpy(&word, in + group / 8 * Bits, sizeof word);
            for (unsigned j = 0; j < 8; ++j)
                out[group + j] = static_cast<std::uint32_t>((word >> (j * Bits)) & mask);
        }
    } else {
        constexpr std::uint64_t mask = (std::uint64_t{1} << Bits) - 1;
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t position = i * Bits;
            std::uint64_t word;
            std::memcpy(&word, in + position / 8, sizeof word);
            out[i] = static_cast<std::uint32_t>((word >> (position % 8)) & mask);
        }
    }
}

using Unpack = void (*)(const std::uint8_t*, std::uint32_t*, std::size_t);
template <std::size_t... Bits>
constexpr std::array<Unpack, sizeof...(Bits)> unpack_table(std::index_sequence<Bits...>) {
    return {&unpack_fixed<Bits>...};
}
constexpr auto kUnpack = unpack_table(std::make_index_sequence<33>{});

bool better(const RankedResult& a, const RankedResult& b) {
    return a.score != b.score ? a.score > b.score : a.document_id < b.document_id;
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

// Distinct terms in lexicographic order, so every strategy sums contributions in the same
// order. A weight of 1 leaves each contribution bitwise unchanged.
std::vector<Bm25Index::QueryTerm> Bm25Index::query_terms(std::string_view query) const {
    auto tokens = Tokenizer(config_.analyzer).tokenize(query);
    std::sort(tokens.begin(), tokens.end());
    std::vector<QueryTerm> terms;
    for (auto& token : tokens) {
        if (!terms.empty() && terms.back().text == token) {
            if (config_.query_weighting == QueryTermWeighting::Frequency)
                terms.back().weight += 1.0;
        } else {
            terms.push_back({std::move(token), 1.0});
        }
    }
    return terms;
}

// Document-at-a-time cursor over one compressed posting list. The "shallow" block
// pointer serves block-max lookups without decoding; it only moves forward.
class Bm25Index::Cursor {
   public:
    Cursor(const Bm25Index& index, const TermLayout& term, std::size_t rank, double weight)
        : blocks_(index.blocks_.data() + term.first_block),
          block_count_((term.df + kBlockSize - 1) / kBlockSize),
          postings_(index.postings_.data()),
          idf_(compute_idf(index.document_ids_.size(), term.df)),
          max_score_(term.max_score),
          rank_(rank),
          weight_(weight) {
        decode(0);
    }

    std::uint32_t doc() const noexcept { return doc_; }
    std::uint32_t tf() const noexcept { return tfs_[position_]; }
    std::size_t rank() const noexcept { return rank_; }
    // Bounds include the query weight; rounding is monotonic, so weight * score never
    // exceeds weight * bound.
    double upper_bound() const noexcept { return weight_ * static_cast<double>(max_score_); }
    double idf() const noexcept { return idf_; }
    double weight() const noexcept { return weight_; }

    void next() {
        if (++position_ < count_) {
            doc_ = docs_[position_];
        } else if (block_ + 1 < block_count_) {
            decode(block_ + 1);
        } else {
            doc_ = kEnd;
        }
    }
    void next_geq(std::uint32_t target) {
        if (target <= doc_) return;
        std::size_t b = block_;
        while (b < block_count_ && blocks_[b].last < target) ++b;
        if (b == block_count_) {
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
        if (shallow_ > block_ && blocks_[shallow_ - 1].last >= target) shallow_ = block_;
        while (shallow_ < block_count_ && blocks_[shallow_].last < target) ++shallow_;
        return shallow_ < block_count_ ? weight_ * static_cast<double>(blocks_[shallow_].max_score)
                                       : 0.0;
    }
    // Largest weighted block bound over documents [first, last]; first >= doc().
    double range_max(std::uint32_t first, std::uint32_t last) {
        block_max(first);  // Positions shallow_ at the block holding first.
        double best = 0.0;
        for (std::size_t b = shallow_; b < block_count_; ++b) {
            best = std::max(best, static_cast<double>(blocks_[b].max_score));
            if (blocks_[b].last >= last) break;
        }
        return weight_ * best;
    }
    std::uint32_t block_last() const noexcept {
        return shallow_ < block_count_ ? blocks_[shallow_].last : kEnd;
    }

   private:
    void decode(std::size_t b) {
        block_ = b;
        const Block& block = blocks_[b];
        const std::uint8_t* in = postings_ + block.offset;
        kUnpack[block.doc_bits](in, docs_.data(), block.count);
        kUnpack[block.tf_bits](in + (block.count * block.doc_bits + 7) / 8, tfs_.data(),
                               block.count);
        std::uint32_t previous = b == 0 ? 0 : blocks_[b - 1].last;
        for (std::uint32_t i = 0; i < block.count; ++i) {
            previous += docs_[i];
            docs_[i] = previous;
            ++tfs_[i];
        }
        count_ = block.count;
        position_ = 0;
        doc_ = docs_[0];
        shallow_ = std::max(shallow_, b);
    }

    const Block* blocks_;
    std::size_t block_count_;
    const std::uint8_t* postings_;
    double idf_;
    float max_score_;
    std::size_t rank_;
    double weight_;
    std::size_t block_ = 0, shallow_ = 0, position_ = 0, count_ = 0;
    std::uint32_t doc_ = kEnd;
    std::array<std::uint32_t, kBlockSize> docs_{}, tfs_{};
};

Bm25Index::Bm25Index(Bm25Config config) : config_(config), index_(config.analyzer) {
    validate_bm25_config(config_);
}
void Bm25Index::add_document(DocumentId id, std::string_view text) {
    index_.add_document(id, text);
}

void Bm25Index::build() {
    const bool compressed = config_.strategy != Bm25Strategy::Exhaustive;
    PostingMemory memory{};
    // Everything that can throw runs before the inverted index publishes, so a failed
    // build leaves this index collecting and it may be retried.
    index_.build(
        [&](const InvertedIndex& built) {
            std::size_t postings = 0, vocabulary = 0;
            built.for_each_term([&](std::string_view, std::span<const Posting> list) {
                postings += list.size();
                ++vocabulary;
            });
            memory.postings = postings;
            memory.raw_bytes = postings * sizeof(Posting) + vocabulary * sizeof(PostingList);
            if (compressed) build_layout(built, memory);
        },
        compressed);
    memory_ = memory;
}

// Compressed blocks over dense ordinals assigned in ascending DocumentId order, so
// ordinal order equals the DocumentId tie order of the ranking. Built in locals and
// installed with nonthrowing swaps, so a failure leaves the members untouched.
void Bm25Index::build_layout(const InvertedIndex& built, PostingMemory& memory) {
    std::vector<DocumentId> document_ids = built.document_ids();
    std::unordered_map<DocumentId, std::uint32_t> ordinal;
    ordinal.reserve(document_ids.size());
    std::vector<double> length_norm(document_ids.size());
    const double avgdl = built.average_document_length();
    for (std::size_t i = 0; i < document_ids.size(); ++i) {
        ordinal.emplace(document_ids[i], static_cast<std::uint32_t>(i));
        length_norm[i] =
            detail::bm25_length_norm(*built.document_length(document_ids[i]), avgdl, config_);
        if (!std::isfinite(length_norm[i])) throw std::overflow_error("BM25 length overflow.");
    }
    const std::size_t n = built.document_count();
    std::unordered_map<std::string, TermLayout, TermHash, std::equal_to<>> layouts;
    std::vector<Block> blocks;
    std::vector<std::uint8_t> postings;
    std::array<std::uint32_t, kBlockSize> deltas{}, tfs{};
    built.for_each_term([&](std::string_view term, std::span<const Posting> list) {
        TermLayout layout{static_cast<std::uint32_t>(list.size()),
                          static_cast<std::uint32_t>(blocks.size()), 0.0f};
        const double idf = compute_idf(n, list.size());
        std::uint32_t previous = 0;
        for (std::size_t start = 0; start < list.size(); start += kBlockSize) {
            const std::size_t count = std::min(list.size() - start, kBlockSize);
            if (postings.size() > std::numeric_limits<std::uint32_t>::max())
                throw std::length_error("BM25 postings exceed 4 GiB.");
            Block block{0, static_cast<std::uint32_t>(postings.size()), 0.0f,
                        static_cast<std::uint16_t>(count), 0, 0};
            double block_max = 0;
            std::uint32_t widest_delta = 0, widest_tf = 0;
            for (std::size_t i = 0; i < count; ++i) {
                const std::uint32_t current = ordinal.at(list[start + i].document_id);
                const std::uint32_t tf = list[start + i].term_frequency;
                deltas[i] = current - previous;
                tfs[i] = tf - 1;
                widest_delta = std::max(widest_delta, deltas[i]);
                widest_tf = std::max(widest_tf, tfs[i]);
                previous = current;
                const double score =
                    detail::bm25_score_from_norm(tf, length_norm[current], idf, config_);
                if (!std::isfinite(score)) throw std::overflow_error("BM25 score overflow.");
                block_max = std::max(block_max, score);
            }
            block.doc_bits = static_cast<std::uint8_t>(std::bit_width(widest_delta));
            block.tf_bits = static_cast<std::uint8_t>(std::bit_width(widest_tf));
            pack(postings, deltas.data(), count, block.doc_bits);
            pack(postings, tfs.data(), count, block.tf_bits);
            // Round the float bound up so it never falls below the exact double maximum.
            block.max_score = static_cast<float>(block_max);
            if (static_cast<double>(block.max_score) < block_max)
                block.max_score =
                    std::nextafter(block.max_score, std::numeric_limits<float>::infinity());
            block.last = previous;
            layout.max_score = std::max(layout.max_score, block.max_score);
            blocks.push_back(block);
        }
        layouts.emplace(std::string(term), layout);
    });
    postings.resize(postings.size() + sizeof(std::uint64_t), 0);
    postings.shrink_to_fit();
    blocks.shrink_to_fit();
    memory.blocks = blocks.size();
    memory.compressed_bytes = postings.size() + blocks.size() * sizeof(Block) +
                              layouts.size() * sizeof(TermLayout) +
                              length_norm.size() * sizeof(double) +
                              document_ids.size() * sizeof(DocumentId);
    layout_.swap(layouts);
    blocks_.swap(blocks);
    postings_.swap(postings);
    document_ids_.swap(document_ids);
    length_norm_.swap(length_norm);
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
        case Bm25Strategy::MaxScore:
            return search_maxscore(terms, k, allow);
    }
    throw std::logic_error("Unsupported BM25 strategy.");
}

std::vector<RankedResult> Bm25Index::search_exhaustive(const std::vector<QueryTerm>& terms,
                                                       std::size_t k, const Allow& allow) const {
    std::unordered_map<DocumentId, double> scores;
    for (const auto& term : terms) {
        const auto list = index_.find(term.text);
        if (list.empty()) continue;
        const double idf = compute_idf(index_.document_count(), list.size());
        for (const auto& posting : list) {
            if (allow && !allow(posting.document_id)) continue;
            // Separate statements keep the weighted product from being fused into the sum
            // (FMA), so every strategy adds identically rounded contributions.
            const double contribution =
                term.weight * bm25_term_score(posting.term_frequency,
                                              index_.document_length(posting.document_id).value(),
                                              index_.average_document_length(), idf, config_);
            auto& score = scores[posting.document_id];
            score += contribution;
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
std::vector<RankedResult> Bm25Index::search_term_at_a_time(const std::vector<QueryTerm>& terms,
                                                           std::size_t k,
                                                           const Allow& allow) const {
    // Per-thread scratch; every search leaves it clean, also when something throws (each
    // dirty slot is in `touched`, appended before its score changes).
    thread_local std::vector<double> scores;
    thread_local std::vector<std::uint8_t> touched_flag;
    thread_local std::vector<std::uint32_t> touched;
    if (scores.size() < document_ids_.size() || touched_flag.size() < document_ids_.size()) {
        scores.resize(document_ids_.size(), 0.0);
        touched_flag.resize(document_ids_.size(), 0);
    }
    touched.clear();
    TopK top(k);
    try {
        for (const auto& term : terms) {
            const auto found = layout_.find(term.text);
            if (found == layout_.end()) continue;
            Cursor cursor(*this, found->second, 0, term.weight);
            for (; cursor.doc() != kEnd; cursor.next()) {
                const std::uint32_t d = cursor.doc();
                if (!touched_flag[d]) {
                    touched.push_back(d);
                    touched_flag[d] = 1;
                }
                const double contribution =
                    term.weight * detail::bm25_score_from_norm(cursor.tf(), length_norm_[d],
                                                               cursor.idf(), config_);
                scores[d] += contribution;
            }
        }
        for (std::uint32_t d : touched) {
            const double score = scores[d];
            scores[d] = 0.0;
            touched_flag[d] = 0;
            if (allow && !allow(document_ids_[d])) continue;
            if (!std::isfinite(score))
                throw std::overflow_error("BM25 accumulated score overflow.");
            top.offer({document_ids_[d], score});
        }
    } catch (...) {
        for (std::uint32_t d : touched) {
            scores[d] = 0.0;
            touched_flag[d] = 0;
        }
        throw;
    }
    return std::move(top).sorted();
}

// Rank-safe WAND / Block-Max WAND (Broder et al. 2003; Ding and Suel 2011). A document is
// skipped only when a (slack-inflated) bound is strictly below the K-th score, so
// documents tying the threshold are still scored and the DocumentId tie rule holds.
// A filter only removes candidates, so it can never raise the threshold incorrectly;
// excluded documents are skipped without being offered.
std::vector<RankedResult> Bm25Index::search_wand(const std::vector<QueryTerm>& terms,
                                                 std::size_t k, bool block_max,
                                                 const Allow& allow) const {
    std::vector<Cursor> cursors;
    for (std::size_t rank = 0; rank < terms.size(); ++rank) {
        const auto found = layout_.find(terms[rank].text);
        if (found != layout_.end())
            cursors.emplace_back(*this, found->second, rank, terms[rank].weight);
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
                    order[i]->weight() *
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

// Rank-safe MaxScore (Turtle and Flood 1995) over windows of document ordinals, as in
// Lucene's MaxScoreBulkScorer. For each window, every list's bound is the largest block
// bound overlapping it; the longest prefix of lists (by ascending bound) whose summed
// bounds stay (slack-inflated) strictly below the K-th score is non-essential, since a
// document matching only those lists cannot enter the Top-K. Essential lists are
// accumulated term-at-a-time into the window; each touched document then probes the
// non-essential lists from the largest bound down only while it can still reach the
// threshold. Partial sums (any order) drive pruning only: survivors are scored by summing
// their contributions in term (rank) order, so scores are bitwise identical to the other
// strategies, and documents tying the threshold are always scored.
std::vector<RankedResult> Bm25Index::search_maxscore(const std::vector<QueryTerm>& terms,
                                                     std::size_t k, const Allow& allow) const {
    constexpr std::uint32_t kWindow = 4096;
    constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();
    struct Entry {
        std::uint32_t next;  // Previous entry of the same document, or kNone.
        std::uint32_t rank;
        double value;
    };
    // Per-thread scratch. Every search leaves it clean (sums 0, heads kNone, no bits set),
    // also when an allocation or the allow callback throws mid-window.
    thread_local std::vector<double> partial;
    thread_local std::vector<std::uint32_t> head;
    thread_local std::vector<std::uint64_t> touched;  // One bit per window slot.
    thread_local std::vector<Entry> pool;
    if (partial.size() < kWindow || head.size() < kWindow || touched.size() < kWindow / 64) {
        partial.assign(kWindow, 0.0);
        head.assign(kWindow, kNone);
        touched.assign(kWindow / 64, 0);
    }
    std::vector<Cursor> cursors;
    cursors.reserve(terms.size());
    for (std::size_t rank = 0; rank < terms.size(); ++rank) {
        const auto found = layout_.find(terms[rank].text);
        if (found != layout_.end())
            cursors.emplace_back(*this, found->second, rank, terms[rank].weight);
    }
    const auto contribution = [&](const Cursor& cursor, std::uint32_t d) {
        return cursor.weight() *
               detail::bm25_score_from_norm(cursor.tf(), length_norm_[d], cursor.idf(), config_);
    };
    TopK top(k);
    std::vector<std::pair<double, Cursor*>> lists;  // (window bound, cursor), ascending.
    std::vector<double> prefix;                     // Summed window bounds of lists[0..i].
    std::vector<std::pair<std::size_t, double>> contributions;
    try {
        std::uint32_t lo = kEnd;
        for (const auto& cursor : cursors) lo = std::min(lo, cursor.doc());
        while (lo != kEnd) {
            const std::uint32_t hi = lo >= kEnd - kWindow ? kEnd : lo + kWindow;  // Exclusive.
            lists.clear();
            for (auto& cursor : cursors)
                if (cursor.doc() < hi)
                    lists.emplace_back(cursor.range_max(cursor.doc(), hi - 1), &cursor);
            std::sort(lists.begin(), lists.end(), [](const auto& a, const auto& b) {
                return a.first != b.first ? a.first < b.first : a.second->rank() < b.second->rank();
            });
            prefix.resize(lists.size());
            for (std::size_t i = 0; i < lists.size(); ++i)
                prefix[i] = (i == 0 ? 0.0 : prefix[i - 1]) + lists[i].first;
            std::size_t essential = 0;
            if (top.full())
                while (essential < lists.size() &&
                       prefix[essential] * kBoundSlack < top.threshold())
                    ++essential;

            pool.clear();
            for (std::size_t i = essential; i < lists.size(); ++i) {
                Cursor& cursor = *lists[i].second;
                for (; cursor.doc() < hi; cursor.next()) {
                    const std::uint32_t slot = cursor.doc() - lo;
                    const double value = contribution(cursor, cursor.doc());
                    touched[slot / 64] |= std::uint64_t{1} << (slot % 64);
                    pool.push_back({head[slot], static_cast<std::uint32_t>(cursor.rank()), value});
                    head[slot] = static_cast<std::uint32_t>(pool.size() - 1);
                    partial[slot] += value;
                }
            }
            // Touched slots in ascending order, as the non-essential cursors only move forward.
            for (std::uint32_t word = 0; word < kWindow / 64; ++word) {
                for (std::uint64_t bits = std::exchange(touched[word], 0); bits != 0;
                     bits &= bits - 1) {
                    const std::uint32_t slot =
                        word * 64 + static_cast<std::uint32_t>(std::countr_zero(bits));
                    const std::uint32_t d = lo + slot;
                    double sum = partial[slot];
                    std::uint32_t chain = head[slot];
                    partial[slot] = 0.0;
                    head[slot] = kNone;
                    if (allow && !allow(document_ids_[d])) continue;
                    contributions.clear();
                    bool pruned = false;
                    for (std::size_t i = essential; i-- > 0;) {
                        if (top.full() && (sum + prefix[i]) * kBoundSlack < top.threshold()) {
                            pruned = true;
                            break;
                        }
                        Cursor& cursor = *lists[i].second;
                        cursor.next_geq(d);
                        if (cursor.doc() != d) continue;
                        const double value = contribution(cursor, d);
                        contributions.emplace_back(cursor.rank(), value);
                        sum += value;
                    }
                    if (pruned || (top.full() && sum * kBoundSlack < top.threshold())) continue;
                    for (; chain != kNone; chain = pool[chain].next)
                        contributions.emplace_back(pool[chain].rank, pool[chain].value);
                    std::sort(contributions.begin(), contributions.end());
                    double score = 0.0;
                    for (const auto& [rank, value] : contributions) score += value;
                    if (!std::isfinite(score))
                        throw std::overflow_error("BM25 accumulated score overflow.");
                    top.offer({document_ids_[d], score});
                }
            }
            lo = kEnd;
            for (auto& cursor : cursors) {
                cursor.next_geq(hi);
                lo = std::min(lo, cursor.doc());
            }
        }
    } catch (...) {
        std::fill(partial.begin(), partial.end(), 0.0);
        std::fill(head.begin(), head.end(), kNone);
        std::fill(touched.begin(), touched.end(), std::uint64_t{0});
        throw;
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
