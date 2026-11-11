#include "aether/lexical/inverted_index.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

#include "aether/lexical/tokenizer.hpp"

namespace aetherindex::lexical {

void InvertedIndex::add_document(DocumentId id, std::string_view text) {
    if (ready_) throw std::logic_error("Cannot add documents after build().");
    if (documents_.contains(id)) throw std::invalid_argument("Duplicate DocumentId.");
    const auto tokens = tokenizer_.tokenize(text, stems_);
    if (tokens.size() > std::numeric_limits<std::uint32_t>::max() ||
        tokens.size() > std::numeric_limits<std::uint64_t>::max() - total_tokens_) {
        throw std::length_error("Document or corpus token count is too large.");
    }
    // Plan without mutating anything: ids for known terms, tentative ids for new ones.
    std::unordered_map<std::string_view, std::uint32_t> fresh;
    std::vector<std::string_view> fresh_order;
    std::vector<std::uint32_t> ids;
    ids.reserve(tokens.size());
    for (const auto& term : tokens) {
        if (const auto known = term_ids_.find(term); known != term_ids_.end()) {
            ids.push_back(known->second);
            continue;
        }
        const auto next = static_cast<std::uint32_t>(terms_.size() + fresh.size());
        const auto [entry, inserted] = fresh.try_emplace(term, next);
        if (inserted) fresh_order.push_back(term);
        ids.push_back(entry->second);
    }
    std::sort(ids.begin(), ids.end());
    DocumentTermData pending{static_cast<std::uint32_t>(tokens.size()), {}};
    for (std::size_t i = 0; i < ids.size();) {
        std::size_t j = i;
        while (j < ids.size() && ids[j] == ids[i]) ++j;
        pending.frequencies.emplace_back(ids[i], static_cast<std::uint32_t>(j - i));
        i = j;
    }
    // Commit. Capacity first, so the bookkeeping below cannot throw; the map nodes and the
    // document entry can, and are undone then.
    // Geometric growth: a reserve per document would reallocate (and rehash) every time.
    const std::size_t needed = terms_.size() + fresh_order.size();
    if (terms_.capacity() < needed) terms_.reserve(std::max(needed, 2 * terms_.capacity()));
    if (static_cast<double>(needed) >
        static_cast<double>(term_ids_.bucket_count()) * term_ids_.max_load_factor())
        term_ids_.reserve(std::max(needed, 2 * term_ids_.size()));
    std::vector<std::string_view> committed;
    committed.reserve(fresh_order.size());
    try {
        for (const auto term : fresh_order) {
            const auto entry = term_ids_.emplace(std::string(term), fresh.at(term)).first;
            committed.push_back(entry->first);
            terms_.push_back(entry->first);
        }
        documents_.emplace(id, std::move(pending));
    } catch (...) {
        for (const auto term : committed) term_ids_.erase(term_ids_.find(term));
        terms_.resize(terms_.size() - committed.size());
        throw;
    }
    // Nothing below throws.
    total_tokens_ += tokens.size();
    document_count_ = documents_.size();
    average_length_ = static_cast<double>(total_tokens_) / static_cast<double>(document_count_);
}

void InvertedIndex::build() { build({}, false); }

void InvertedIndex::build(const std::function<void(const InvertedIndex&)>& prepare,
                          bool release_postings) {
    if (ready_) throw std::logic_error("build() may only succeed once.");
    InvertedIndexData postings;
    std::unordered_map<DocumentId, std::uint32_t> lengths;
    lengths.reserve(documents_.size());
    // Exact-size lists by term id, filled in ascending DocumentId order (no sort needed).
    std::vector<std::uint32_t> counts(terms_.size(), 0);
    std::vector<DocumentId> order;
    order.reserve(documents_.size());
    for (const auto& [id, document] : documents_) {
        lengths.emplace(id, document.length);
        order.push_back(id);
        for (const auto& [term, frequency] : document.frequencies) ++counts[term];
    }
    std::sort(order.begin(), order.end());
    std::vector<PostingList> lists(terms_.size());
    for (std::size_t t = 0; t < lists.size(); ++t) lists[t].reserve(counts[t]);
    for (DocumentId id : order)
        for (const auto& [term, frequency] : documents_.find(id)->second.frequencies)
            lists[term].push_back({id, frequency});
    postings.reserve(terms_.size());
    for (std::size_t t = 0; t < lists.size(); ++t)
        if (!lists[t].empty()) postings.emplace(std::string(terms_[t]), std::move(lists[t]));
    const double avgdl = document_count_ == 0 ? 0.0
                                              : static_cast<double>(total_tokens_) /
                                                    static_cast<double>(document_count_);
    // Stage with nonthrowing swaps; undo them if prepare fails.
    postings_.swap(postings);
    lengths_.swap(lengths);
    const double collecting_average = std::exchange(average_length_, avgdl);
    ready_ = true;
    if (prepare) {
        try {
            prepare(*this);
        } catch (...) {
            ready_ = false;
            postings_.swap(postings);
            lengths_.swap(lengths);
            average_length_ = collecting_average;
            throw;
        }
    }
    documents_.clear();
    StemCache().swap(stems_);
    terms_.clear();
    term_ids_.clear();
    if (release_postings) {
        InvertedIndexData().swap(postings_);
        postings_released_ = true;
    }
}

void InvertedIndex::require_built() const {
    if (!ready_) throw std::logic_error("The inverted index requires build().");
    if (postings_released_)
        throw std::logic_error("Raw postings were released by a compressed BM25 strategy.");
}

std::vector<DocumentId> InvertedIndex::document_ids() const {
    if (!ready_) throw std::logic_error("The inverted index requires build().");
    std::vector<DocumentId> ids;
    ids.reserve(lengths_.size());
    for (const auto& [id, length] : lengths_) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    return ids;
}

void InvertedIndex::for_each_term(
    const std::function<void(std::string_view, std::span<const Posting>)>& visit) const {
    require_built();
    for (const auto& [term, list] : postings_) visit(term, list);
}

void InvertedIndex::release_postings() {
    if (!ready_) throw std::logic_error("The inverted index requires build().");
    InvertedIndexData().swap(postings_);
    postings_released_ = true;
}

std::span<const Posting> InvertedIndex::find(std::string_view term) const {
    require_built();
    const auto found = postings_.find(term);
    if (found == postings_.end()) return {};
    return found->second;
}

std::size_t InvertedIndex::document_frequency(std::string_view term) const {
    return find(term).size();
}

std::size_t InvertedIndex::vocabulary_size() const {
    require_built();
    return postings_.size();
}

bool InvertedIndex::is_built() const noexcept { return ready_; }
std::size_t InvertedIndex::document_count() const noexcept { return document_count_; }
std::uint64_t InvertedIndex::total_token_count() const noexcept { return total_tokens_; }
double InvertedIndex::average_document_length() const noexcept { return average_length_; }

std::optional<std::uint32_t> InvertedIndex::document_length(DocumentId id) const {
    if (!ready_) {
        const auto found = documents_.find(id);
        if (found == documents_.end()) return std::nullopt;
        return found->second.length;
    }
    const auto found = lengths_.find(id);
    if (found == lengths_.end()) return std::nullopt;
    return found->second;
}

}  // namespace aetherindex::lexical
