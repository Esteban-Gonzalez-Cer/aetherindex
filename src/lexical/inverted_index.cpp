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
    const auto tokens = Tokenizer{}.tokenize(text);
    if (tokens.size() > std::numeric_limits<std::uint32_t>::max() ||
        tokens.size() > std::numeric_limits<std::uint64_t>::max() - total_tokens_) {
        throw std::length_error("Document or corpus token count is too large.");
    }
    DocumentTermData pending{static_cast<std::uint32_t>(tokens.size()), {}};
    for (const auto& term : tokens) ++pending.frequencies[term];
    // All other mutations are nonthrowing after the single map insertion.
    documents_.emplace(id, std::move(pending));
    total_tokens_ += tokens.size();
    document_count_ = documents_.size();
    average_length_ = static_cast<double>(total_tokens_) / static_cast<double>(document_count_);
}

void InvertedIndex::build() {
    if (ready_) throw std::logic_error("build() may only succeed once.");
    InvertedIndexData postings;
    std::unordered_map<DocumentId, std::uint32_t> lengths;
    lengths.reserve(documents_.size());
    for (const auto& [id, document] : documents_) {
        lengths.emplace(id, document.length);
        for (const auto& [term, frequency] : document.frequencies) {
            postings[term].push_back({id, frequency});
        }
    }
    for (auto& [term, list] : postings) {
        std::sort(list.begin(), list.end(),
                  [](const Posting& a, const Posting& b) { return a.document_id < b.document_id; });
    }
    const double avgdl = document_count_ == 0 ? 0.0
                                              : static_cast<double>(total_tokens_) /
                                                    static_cast<double>(document_count_);
    postings_.swap(postings);
    lengths_.swap(lengths);
    average_length_ = avgdl;
    documents_.clear();
    ready_ = true;
}

void InvertedIndex::require_built() const {
    if (!ready_) throw std::logic_error("The inverted index requires build().");
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
