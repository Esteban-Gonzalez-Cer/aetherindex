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
    // Segment of a larger corpus: corpus-wide idf, and per-block bounds computed for the
    // corpus-wide statistics (block_bounds[i] belongs to this term's i-th block).
    Cursor(const Bm25Index& index, const TermLayout& term, std::size_t rank, double weight,
           double idf, const double* block_bounds, double term_bound)
        : blocks_(index.blocks_.data() + term.first_block),
          block_count_((term.df + kBlockSize - 1) / kBlockSize),
          postings_(index.postings_.data()),
          idf_(idf),
          max_score_(term.max_score),
          rank_(rank),
          weight_(weight),
          block_bounds_(block_bounds),
          term_bound_(term_bound) {
        decode(0);
    }

    std::uint32_t doc() const noexcept { return doc_; }
    std::uint32_t tf() const noexcept { return tfs_[position_]; }
    std::size_t rank() const noexcept { return rank_; }
    // Bounds include the query weight; rounding is monotonic, so weight * score never
    // exceeds weight * bound.
    double upper_bound() const noexcept {
        return weight_ * (block_bounds_ ? term_bound_ : static_cast<double>(max_score_));
    }
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
        return shallow_ < block_count_ ? weight_ * bound_of(shallow_) : 0.0;
    }
    // Largest weighted block bound over documents [first, last]; first >= doc().
    double range_max(std::uint32_t first, std::uint32_t last) {
        block_max(first);  // Positions shallow_ at the block holding first.
        double best = 0.0;
        for (std::size_t b = shallow_; b < block_count_; ++b) {
            best = std::max(best, bound_of(b));
            if (blocks_[b].last >= last) break;
        }
        return weight_ * best;
    }
    std::uint32_t block_last() const noexcept {
        return shallow_ < block_count_ ? blocks_[shallow_].last : kEnd;
    }

   private:
    double bound_of(std::size_t b) const noexcept {
        return block_bounds_ ? block_bounds_[b] : static_cast<double>(blocks_[b].max_score);
    }
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
    const double* block_bounds_ = nullptr;
    double term_bound_ = 0.0;
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

bool Bm25Index::layout_is_consistent() const {
    // Furthest byte one unpack call reads: whole 8-byte loads per value or group of 8.
    const auto read_end = [](std::uint64_t start, std::uint64_t count, unsigned bits) {
        if (bits == 0) return start;
        const std::uint64_t last = bits <= 8 ? (count - 1) / 8 * bits : (count - 1) * bits / 8;
        return start + last + sizeof(std::uint64_t);
    };
    std::array<std::uint32_t, kBlockSize> docs{}, tfs{};
    for (const auto& [term, layout] : layout_) {
        const std::uint64_t count = (std::uint64_t{layout.df} + kBlockSize - 1) / kBlockSize;
        if (layout.df == 0 || layout.df > document_ids_.size() ||
            std::uint64_t{layout.first_block} + count > blocks_.size())
            return false;
        std::uint64_t remaining = layout.df;
        std::uint32_t previous = 0;
        bool first = true;
        for (std::uint64_t b = layout.first_block; b < layout.first_block + count; ++b) {
            const Block& block = blocks_[b];
            const std::uint64_t expected = std::min<std::uint64_t>(remaining, kBlockSize);
            remaining -= expected;
            if (block.count != expected || block.doc_bits > 32 || block.tf_bits > 32) return false;
            const std::uint64_t tf_start =
                std::uint64_t{block.offset} + (expected * block.doc_bits + 7) / 8;
            if (read_end(block.offset, expected, block.doc_bits) > postings_.size() ||
                read_end(tf_start, expected, block.tf_bits) > postings_.size())
                return false;
            const std::uint8_t* in = postings_.data() + block.offset;
            kUnpack[block.doc_bits](in, docs.data(), block.count);
            kUnpack[block.tf_bits](postings_.data() + tf_start, tfs.data(), block.count);
            for (std::uint32_t i = 0; i < block.count; ++i) {
                const std::uint64_t current = std::uint64_t{previous} + docs[i];
                if (current >= document_ids_.size() || (!first && current <= previous) ||
                    tfs[i] == std::numeric_limits<std::uint32_t>::max())
                    return false;
                previous = static_cast<std::uint32_t>(current);
                first = false;
            }
            if (block.last != previous) return false;
        }
    }
    return true;
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
    std::vector<Cursor> cursors;
    cursors.reserve(terms.size());
    for (std::size_t rank = 0; rank < terms.size(); ++rank) {
        const auto found = layout_.find(terms[rank].text);
        if (found != layout_.end())
            cursors.emplace_back(*this, found->second, rank, terms[rank].weight);
    }
    return taat_impl(
        cursors, k, allow, [this](std::uint32_t d) { return length_norm_[d]; },
        [](std::uint32_t) { return false; });
}

// norm(d): BM25 length normalization of ordinal d; skip(d): tombstoned ordinal. Cursors
// are in term (rank) order, so contributions add in the same order as the exhaustive path.
template <class Norm, class Skip>
std::vector<RankedResult> Bm25Index::taat_impl(std::vector<Cursor>& cursors, std::size_t k,
                                               const Allow& allow, const Norm& norm,
                                               const Skip& skip) const {
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
        for (const auto& source : cursors) {
            // A local copy: the compiler can then keep the cursor's state out of memory
            // that the scratch writes below might alias (measured 7% on FiQA).
            Cursor cursor = source;
            for (; cursor.doc() != kEnd; cursor.next()) {
                const std::uint32_t d = cursor.doc();
                if (!touched_flag[d]) {
                    touched.push_back(d);
                    touched_flag[d] = 1;
                }
                const double contribution =
                    cursor.weight() *
                    detail::bm25_score_from_norm(cursor.tf(), norm(d), cursor.idf(), config_);
                scores[d] += contribution;
            }
        }
        for (std::uint32_t d : touched) {
            const double score = scores[d];
            scores[d] = 0.0;
            touched_flag[d] = 0;
            if (skip(d) || (allow && !allow(document_ids_[d]))) continue;
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
    std::vector<Cursor> cursors;
    cursors.reserve(terms.size());
    for (std::size_t rank = 0; rank < terms.size(); ++rank) {
        const auto found = layout_.find(terms[rank].text);
        if (found != layout_.end())
            cursors.emplace_back(*this, found->second, rank, terms[rank].weight);
    }
    return maxscore_impl(
        cursors, k, allow, [this](std::uint32_t d) { return length_norm_[d]; },
        [](std::uint32_t) { return false; });
}

template <class Norm, class Skip>
std::vector<RankedResult> Bm25Index::maxscore_impl(std::vector<Cursor>& cursors, std::size_t k,
                                                   const Allow& allow, const Norm& norm,
                                                   const Skip& skip) const {
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
    const auto contribution = [&](const Cursor& cursor, std::uint32_t d) {
        return cursor.weight() *
               detail::bm25_score_from_norm(cursor.tf(), norm(d), cursor.idf(), config_);
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
                    if (skip(d) || (allow && !allow(document_ids_[d]))) continue;
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

// ----- Compact on-disk form -------------------------------------------------------------

namespace {

void put_varint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(value | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

class CompactReader {
   public:
    explicit CompactReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    std::uint64_t varint() {
        std::uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            const std::uint8_t byte = take(1)[0];
            value |= std::uint64_t{byte & 0x7Fu} << shift;
            if ((byte & 0x80) == 0) return value;
        }
        fail("varint overflow");
    }
    std::span<const std::uint8_t> take(std::size_t count) {
        if (count > bytes_.size() - offset_) fail("truncated data");
        const auto view = bytes_.subspan(offset_, count);
        offset_ += count;
        return view;
    }
    bool done() const noexcept { return offset_ == bytes_.size(); }
    [[noreturn]] static void fail(const std::string& reason) {
        throw std::runtime_error("compact BM25 index: " + reason + ".");
    }

   private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0;
};

// Unpacks `count` values of `bits` bits from `in` (no padding needed past its end).
std::vector<std::uint32_t> unpack_values(std::span<const std::uint8_t> in, std::size_t count,
                                         unsigned bits) {
    std::vector<std::uint8_t> padded(in.begin(), in.end());
    padded.resize(padded.size() + sizeof(std::uint64_t), 0);
    std::vector<std::uint32_t> values((count + 7) / 8 * 8);
    kUnpack[bits](padded.data(), values.data(), count);
    values.resize(count);
    return values;
}

}  // namespace

std::vector<std::uint8_t> Bm25Index::write_compact() const {
    if (!index_.is_built() || index_.has_postings())
        throw std::logic_error("write_compact requires a compressed, built index.");
    std::vector<std::uint8_t> out;
    const std::size_t n = document_ids_.size();
    put_varint(out, n);
    put_varint(out, index_.total_token_count());
    bool dense = true;
    for (std::size_t i = 0; i < n && dense; ++i) dense = document_ids_[i] == i;
    out.push_back(dense ? 1 : 0);
    if (!dense)
        for (std::size_t i = 0; i < n; ++i)
            put_varint(out, document_ids_[i] - (i == 0 ? 0 : document_ids_[i - 1]));
    const auto lengths = ordinal_lengths(document_ids_);
    std::uint32_t widest = 0;
    for (auto length : lengths) widest = std::max(widest, length);
    const auto bits = static_cast<unsigned>(std::bit_width(widest));
    out.push_back(static_cast<std::uint8_t>(bits));
    pack(out, lengths.data(), lengths.size(), bits);

    std::vector<std::string_view> terms;
    terms.reserve(layout_.size());
    for (const auto& [term, layout] : layout_) terms.push_back(term);
    std::sort(terms.begin(), terms.end());
    put_varint(out, terms.size());
    std::string_view previous;
    for (const auto term : terms) {
        std::size_t shared = 0;
        while (shared < std::min(term.size(), previous.size()) && term[shared] == previous[shared])
            ++shared;
        put_varint(out, shared);
        put_varint(out, term.size() - shared);
        out.insert(out.end(), term.begin() + static_cast<std::ptrdiff_t>(shared), term.end());
        previous = term;
        const TermLayout& layout = layout_.find(term)->second;
        put_varint(out, layout.df);
        const std::size_t count = (layout.df + kBlockSize - 1) / kBlockSize;
        for (std::size_t b = layout.first_block; b < layout.first_block + count; ++b) {
            const Block& block = blocks_[b];
            out.push_back(block.doc_bits);
            out.push_back(block.tf_bits);
            const std::size_t size = (block.count * block.doc_bits + 7) / 8 +
                                     (block.count * block.tf_bits + 7) / 8;
            out.insert(out.end(), postings_.begin() + block.offset,
                       postings_.begin() + block.offset + static_cast<std::ptrdiff_t>(size));
        }
    }
    return out;
}

std::unique_ptr<Bm25Index> Bm25Index::read_compact(Bm25Config config,
                                                   std::span<const std::uint8_t> bytes) {
    if (config.strategy == Bm25Strategy::Exhaustive)
        CompactReader::fail("the exhaustive strategy keeps raw postings");
    CompactReader in(bytes);
    auto index = std::make_unique<Bm25Index>(config);
    auto& inverted = index->index_;
    const std::uint64_t n = in.varint();
    const std::uint64_t total = in.varint();
    if (n > std::numeric_limits<std::uint32_t>::max()) CompactReader::fail("too many documents");
    const bool dense = in.take(1)[0] != 0;
    std::vector<DocumentId> ids(static_cast<std::size_t>(n));
    std::uint64_t id = 0;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        id = dense ? i : (i == 0 ? 0 : id) + in.varint();
        if (id > std::numeric_limits<DocumentId>::max() || (i > 0 && !dense && id <= ids[i - 1]))
            CompactReader::fail("document IDs are not ascending");
        ids[i] = static_cast<DocumentId>(id);
    }
    const unsigned bits = in.take(1)[0];
    if (bits > 32) CompactReader::fail("invalid length width");
    const auto lengths = unpack_values(in.take((ids.size() * bits + 7) / 8), ids.size(), bits);
    std::uint64_t sum = 0;
    for (auto length : lengths) sum += length;
    if (sum != total) CompactReader::fail("document lengths do not add up to the token count");
    inverted.document_count_ = ids.size();
    inverted.total_tokens_ = total;
    // The expression InvertedIndex::build uses.
    inverted.average_length_ =
        ids.empty() ? 0.0 : static_cast<double>(total) / static_cast<double>(ids.size());
    inverted.lengths_.reserve(ids.size());
    index->length_norm_.resize(ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        inverted.lengths_.emplace(ids[i], lengths[i]);
        index->length_norm_[i] =
            detail::bm25_length_norm(lengths[i], inverted.average_length_, index->config_);
        if (!std::isfinite(index->length_norm_[i]))
            throw std::overflow_error("BM25 length overflow.");
    }
    index->document_ids_ = std::move(ids);

    const std::uint64_t vocabulary = in.varint();
    if (vocabulary > bytes.size()) CompactReader::fail("vocabulary exceeds the data");
    std::string term;
    std::size_t postings = 0;
    auto& packed = index->postings_;
    for (std::uint64_t t = 0; t < vocabulary; ++t) {
        const std::uint64_t shared = in.varint();
        const std::uint64_t suffix = in.varint();
        if (shared > term.size()) CompactReader::fail("invalid term prefix");
        const auto tail = in.take(static_cast<std::size_t>(suffix));
        term.resize(static_cast<std::size_t>(shared));
        term.append(tail.begin(), tail.end());
        const std::uint64_t df = in.varint();
        if (df == 0 || df > n) CompactReader::fail("invalid document frequency");
        TermLayout layout{static_cast<std::uint32_t>(df),
                          static_cast<std::uint32_t>(index->blocks_.size()), 0.0f};
        const double idf = compute_idf(static_cast<std::size_t>(n), static_cast<std::size_t>(df));
        std::uint64_t remaining = df;
        std::uint32_t previous = 0;
        bool first = true;
        while (remaining > 0) {
            const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(remaining, kBlockSize));
            remaining -= count;
            const auto widths = in.take(2);
            Block block{0, static_cast<std::uint32_t>(packed.size()), 0.0f,
                        static_cast<std::uint16_t>(count), widths[0], widths[1]};
            if (block.doc_bits > 32 || block.tf_bits > 32) CompactReader::fail("invalid bit width");
            const std::size_t doc_bytes = (count * block.doc_bits + 7) / 8;
            const auto data = in.take(doc_bytes + (count * block.tf_bits + 7) / 8);
            if (packed.size() > std::numeric_limits<std::uint32_t>::max() - data.size())
                CompactReader::fail("postings exceed 4 GiB");
            packed.insert(packed.end(), data.begin(), data.end());
            const auto deltas = unpack_values(data.first(doc_bytes), count, block.doc_bits);
            const auto frequencies =
                unpack_values(data.subspan(doc_bytes), count, block.tf_bits);
            double block_max = 0;
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::uint64_t ordinal = std::uint64_t{previous} + deltas[i];
                if (ordinal >= n || (!first && ordinal <= previous))
                    CompactReader::fail("posting ordinals out of order");
                previous = static_cast<std::uint32_t>(ordinal);
                first = false;
                const std::uint64_t tf = std::uint64_t{frequencies[i]} + 1;
                if (tf > lengths[previous]) CompactReader::fail("term frequency exceeds the length");
                const double score = detail::bm25_score_from_norm(
                    static_cast<std::uint32_t>(tf), index->length_norm_[previous], idf,
                    index->config_);
                if (!std::isfinite(score)) throw std::overflow_error("BM25 score overflow.");
                block_max = std::max(block_max, score);
            }
            // As build_layout: the float bound is rounded up to stay a safe bound.
            block.max_score = static_cast<float>(block_max);
            if (static_cast<double>(block.max_score) < block_max)
                block.max_score =
                    std::nextafter(block.max_score, std::numeric_limits<float>::infinity());
            block.last = previous;
            layout.max_score = std::max(layout.max_score, block.max_score);
            index->blocks_.push_back(block);
            postings += count;
        }
        if (!index->layout_.emplace(term, layout).second) CompactReader::fail("duplicate term");
    }
    if (!in.done()) CompactReader::fail("unexpected trailing bytes");
    packed.resize(packed.size() + sizeof(std::uint64_t), 0);
    packed.shrink_to_fit();
    index->blocks_.shrink_to_fit();
    // The accounting build() reports.
    auto& memory = index->memory_;
    memory.postings = postings;
    memory.raw_bytes = postings * sizeof(Posting) + vocabulary * sizeof(PostingList);
    memory.blocks = index->blocks_.size();
    memory.compressed_bytes = packed.size() + index->blocks_.size() * sizeof(Block) +
                              index->layout_.size() * sizeof(TermLayout) +
                              index->length_norm_.size() * sizeof(double) +
                              index->document_ids_.size() * sizeof(DocumentId);
    inverted.postings_released_ = true;
    inverted.ready_ = true;
    return index;
}

// ----- Segment support -------------------------------------------------------------------

std::size_t Bm25Index::term_document_frequency(std::string_view term) const {
    if (index_.has_postings()) return index_.find(term).size();
    const auto found = layout_.find(term);
    return found == layout_.end() ? 0 : found->second.df;
}

std::vector<DocumentId> Bm25Index::ordinal_ids() const {
    return index_.has_postings() ? index_.document_ids() : document_ids_;
}

std::vector<std::uint32_t> Bm25Index::ordinal_lengths(std::span<const DocumentId> ids) const {
    std::vector<std::uint32_t> lengths;
    lengths.reserve(ids.size());
    for (DocumentId id : ids) lengths.push_back(index_.document_length(id).value());
    return lengths;
}

std::vector<std::pair<std::uint32_t, std::uint32_t>> Bm25Index::block_impacts(
    std::span<const std::uint32_t> lengths) const {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> impacts(blocks_.size());
    std::array<std::uint32_t, kBlockSize> docs{}, tfs{};
    for (const auto& [term, layout] : layout_) {
        const std::size_t count = (layout.df + kBlockSize - 1) / kBlockSize;
        std::uint32_t previous = 0;
        for (std::size_t b = layout.first_block; b < layout.first_block + count; ++b) {
            const Block& block = blocks_[b];
            const std::uint8_t* in = postings_.data() + block.offset;
            kUnpack[block.doc_bits](in, docs.data(), block.count);
            kUnpack[block.tf_bits](in + (block.count * block.doc_bits + 7) / 8, tfs.data(),
                                   block.count);
            std::uint32_t max_tf = 0, min_length = std::numeric_limits<std::uint32_t>::max();
            for (std::uint32_t i = 0; i < block.count; ++i) {
                previous += docs[i];
                max_tf = std::max(max_tf, tfs[i] + 1);
                min_length = std::min(min_length, lengths[previous]);
            }
            impacts[b] = {max_tf, min_length};
        }
    }
    return impacts;
}

void Bm25Index::for_each_posting_list(
    const std::function<void(std::string_view, std::span<const Posting>)>& visit) const {
    if (index_.has_postings()) {
        index_.for_each_term(visit);
        return;
    }
    std::array<std::uint32_t, kBlockSize> docs{}, tfs{};
    std::vector<Posting> list;
    for (const auto& [term, layout] : layout_) {
        list.clear();
        const std::size_t count = (layout.df + kBlockSize - 1) / kBlockSize;
        std::uint32_t previous = 0;
        for (std::size_t b = layout.first_block; b < layout.first_block + count; ++b) {
            const Block& block = blocks_[b];
            const std::uint8_t* in = postings_.data() + block.offset;
            kUnpack[block.doc_bits](in, docs.data(), block.count);
            kUnpack[block.tf_bits](in + (block.count * block.doc_bits + 7) / 8, tfs.data(),
                                   block.count);
            for (std::uint32_t i = 0; i < block.count; ++i) {
                previous += docs[i];
                list.push_back({document_ids_[previous], tfs[i] + 1});
            }
        }
        visit(term, list);
    }
}

std::unique_ptr<Bm25Index> Bm25Index::from_postings(
    Bm25Config config,
    std::unordered_map<std::string, PostingList, InvertedIndex::TermHash, std::equal_to<>> postings,
    std::unordered_map<DocumentId, std::uint32_t> lengths, std::uint64_t total_tokens) {
    auto index = std::make_unique<Bm25Index>(config);
    auto& inverted = index->index_;
    inverted.postings_ = std::move(postings);
    inverted.lengths_ = std::move(lengths);
    inverted.document_count_ = inverted.lengths_.size();
    inverted.total_tokens_ = total_tokens;
    // The same expression InvertedIndex::build uses, so statistics are bit-identical.
    inverted.average_length_ = inverted.document_count_ == 0
                                   ? 0.0
                                   : static_cast<double>(total_tokens) /
                                         static_cast<double>(inverted.document_count_);
    inverted.ready_ = true;
    index->finish_build();
    return index;
}

void Bm25Index::finish_build() {
    const bool compressed = config_.strategy != Bm25Strategy::Exhaustive;
    PostingMemory memory{};
    std::size_t postings = 0, vocabulary = 0;
    index_.for_each_term([&](std::string_view, std::span<const Posting> list) {
        postings += list.size();
        ++vocabulary;
    });
    memory.postings = postings;
    memory.raw_bytes = postings * sizeof(Posting) + vocabulary * sizeof(PostingList);
    if (compressed) {
        build_layout(index_, memory);
        index_.release_postings();
    }
    memory_ = memory;
}

std::vector<RankedResult> Bm25Index::search_global(const std::vector<QueryTerm>& terms,
                                                   std::span<const double> idf, std::size_t k,
                                                   const Allow& allow,
                                                   const GlobalContext& context) const {
    const auto skip = [&](std::uint32_t d) {
        return context.deleted && ((context.deleted[d / 64] >> (d % 64)) & 1u) != 0;
    };
    if (index_.has_postings()) {
        // Raw postings: the exhaustive scorer itself, with corpus-wide statistics.
        std::unordered_map<DocumentId, double> scores;
        for (std::size_t rank = 0; rank < terms.size(); ++rank) {
            const auto list = index_.find(terms[rank].text);
            for (const auto& posting : list) {
                const auto ordinal = static_cast<std::uint32_t>(
                    std::lower_bound(context.ids.begin(), context.ids.end(), posting.document_id) -
                    context.ids.begin());
                if (skip(ordinal) || (allow && !allow(posting.document_id))) continue;
                const double contribution =
                    terms[rank].weight *
                    bm25_term_score(posting.term_frequency, context.lengths[ordinal],
                                    context.average_length, idf[rank], config_);
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
    // Compressed: bounds from each block's (max tf, min length) under the corpus-wide
    // statistics. A block's documents have tf <= max tf and length >= min length, and the
    // score grows with tf and shrinks with length, so the bound is never below a score.
    std::vector<std::vector<double>> bounds(terms.size());
    std::vector<Cursor> cursors;
    cursors.reserve(terms.size());
    for (std::size_t rank = 0; rank < terms.size(); ++rank) {
        const auto found = layout_.find(terms[rank].text);
        if (found == layout_.end()) continue;
        const TermLayout& layout = found->second;
        auto& term_bounds = bounds[rank];
        term_bounds.resize((layout.df + kBlockSize - 1) / kBlockSize);
        double term_bound = 0.0;
        for (std::size_t i = 0; i < term_bounds.size(); ++i) {
            const auto [max_tf, min_length] = context.impacts[layout.first_block + i];
            term_bounds[i] = detail::bm25_score_from_norm(
                max_tf, detail::bm25_length_norm(min_length, context.average_length, config_),
                idf[rank], config_);
            term_bound = std::max(term_bound, term_bounds[i]);
        }
        cursors.emplace_back(*this, layout, rank, terms[rank].weight, idf[rank],
                             term_bounds.data(), term_bound);
    }
    const auto norm = [&](std::uint32_t d) {
        return detail::bm25_length_norm(context.lengths[d], context.average_length, config_);
    };
    if (config_.strategy == Bm25Strategy::TermAtATime)
        return taat_impl(cursors, k, allow, norm, skip);
    return maxscore_impl(cursors, k, allow, norm, skip);
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
