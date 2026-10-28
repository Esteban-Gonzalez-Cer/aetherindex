#include "aether/lexical/bm25.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

#include "aether/lexical/tokenizer.hpp"

namespace aetherindex::lexical {

void validate_bm25_config(const Bm25Config& config) {
    if (!std::isfinite(config.k1) || config.k1 <= 0.0 || !std::isfinite(config.b) ||
        config.b < 0.0 || config.b > 1.0) {
        throw std::invalid_argument("BM25 requires finite k1 > 0 and finite b in [0,1].");
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

double bm25_term_score(std::uint32_t tf, std::uint32_t document_length, double avgdl, double idf,
                       const Bm25Config& config) {
    validate_bm25_config(config);
    if (!std::isfinite(avgdl) || avgdl <= 0.0 || !std::isfinite(idf) || idf <= 0.0 ||
        tf > document_length) {
        throw std::invalid_argument("Invalid BM25 term statistics.");
    }
    if (tf == 0) return 0.0;
    const double frequency = tf;
    const double normalized =
        1.0 - config.b + (config.b == 0.0 ? 0.0 : (config.b * document_length) / avgdl);
    if (!std::isfinite(normalized)) throw std::overflow_error("BM25 length overflow.");
    // Algebraically identical scaling avoids inf/inf for large finite k1.
    const double factor =
        config.k1 >= 1.0
            ? frequency * (1.0 + 1.0 / config.k1) / (frequency / config.k1 + normalized)
            : frequency * (config.k1 + 1.0) / (frequency + config.k1 * normalized);
    const double score = idf * factor;
    if (!std::isfinite(score)) throw std::overflow_error("BM25 score overflow.");
    return score;
}

Bm25Index::Bm25Index(Bm25Config config) : config_(config) { validate_bm25_config(config_); }
void Bm25Index::add_document(DocumentId id, std::string_view text) {
    index_.add_document(id, text);
}
void Bm25Index::build() { index_.build(); }

std::vector<RankedResult> Bm25Index::search(std::string_view query, std::size_t k) const {
    if (!index_.is_built()) throw std::logic_error("BM25 search requires build().");
    if (k == 0) throw std::invalid_argument("BM25 k must be greater than zero.");
    auto terms = Tokenizer{}.tokenize(query);
    std::sort(terms.begin(), terms.end());
    terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
    std::unordered_map<DocumentId, double> scores;
    for (const auto& term : terms) {
        const auto list = index_.find(term);
        if (list.empty()) continue;
        const double idf = compute_idf(index_.document_count(), list.size());
        for (const auto& posting : list) {
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
    std::sort(results.begin(), results.end(), [](const RankedResult& a, const RankedResult& b) {
        return a.score != b.score ? a.score > b.score : a.document_id < b.document_id;
    });
    if (results.size() > k) results.resize(k);
    return results;
}

bool Bm25Index::is_built() const noexcept { return index_.is_built(); }
std::size_t Bm25Index::document_count() const noexcept { return index_.document_count(); }
double Bm25Index::average_document_length() const noexcept {
    return index_.average_document_length();
}
const InvertedIndex& Bm25Index::inverted_index() const noexcept { return index_; }
Bm25Config Bm25Index::config() const noexcept { return config_; }

}  // namespace aetherindex::lexical
