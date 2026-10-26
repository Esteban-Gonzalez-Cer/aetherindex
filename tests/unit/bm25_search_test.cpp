#include <algorithm>
#include <exception>
#include <iostream>
#include <limits>
#include <random>
#include <set>
#include <string>

#include "../bm25_test_helpers.hpp"

using namespace aetherindex;
using namespace aetherindex::lexical;

void check_contracts() {
    Bm25Index index;
    requireBm25Error<std::logic_error>([&] { return index.search("", 10); },
                                       "Unbuilt search accepted.");
    addBm25Reference(index);
    requireBm25Error<std::invalid_argument>([&] { index.add_document(101, "new"); },
                                            "Duplicate accepted.");
    require(index.document_count() == 3, "Duplicate changed N.");
    index.build();
    const auto both = index.search("motor e47", 10);
    require(both.size() == 2 && both[0].document_id == 101 && both[1].document_id == 102,
            "T6: score accumulation duplicated a document.");
    requireBm25Near(both[0].score, 0.5665797174 + 0.8142733421, "T6: sum of contributions failed.");
    requireSameRanking(index.search("e47 motor e47 motor", 10), both);
    requireSameRanking(index.search("motor e47 unknown", 10), both);
    requireSameRanking(index.search("E47 MOTOR", 10), both);
    for (std::string_view query : {"", " ", ";[]--!!", "unknown"}) {
        require(index.search(query, 10).empty(), "T8: empty/OOV query filled ranking.");
    }
    requireBm25Error<std::invalid_argument>([&] { return index.search("motor", 0); },
                                            "k=0 accepted.");
    requireBm25Error<std::invalid_argument>([&] { return index.search("", 0); },
                                            "Empty k=0 accepted.");
    requireBm25Error<std::invalid_argument>([&] { return index.search("\xFF", 10); },
                                            "Invalid query UTF-8 accepted.");
    requireBm25Error<std::logic_error>([&] { index.build(); }, "Second build accepted.");
    requireBm25Error<std::logic_error>([&] { index.add_document(999, "motor"); },
                                       "Post-build mutation accepted.");
    requireSameRanking(index.search("motor e47", std::numeric_limits<std::size_t>::max()), both);
    requireSameRanking(index.search("motor e47", 10), both);

    Bm25Index ties;
    for (DocumentId id : {std::numeric_limits<DocumentId>::max(), 427u, 7u, 0u}) {
        ties.add_document(id, "same term");
    }
    ties.build();
    const auto full = ties.search("same", 10);
    require(full.size() == 4 && full[0].document_id == 0 && full[1].document_id == 7 &&
                full[2].document_id == 427 &&
                full[3].document_id == std::numeric_limits<DocumentId>::max(),
            "T7: score ties must use document ID ascending.");
    for (std::size_t k : {1u, 2u, 10u}) {
        const auto top = ties.search("same", k);
        require(top.size() == std::min(k, full.size()), "T7: incorrect top-k size.");
        for (std::size_t i = 0; i < top.size(); ++i) {
            require(top[i].document_id == full[i].document_id && top[i].score == full[i].score,
                    "T7: top-k is not a prefix.");
        }
    }
}

// Independent oracle scans all original token arrays; it never uses the
// production tokenizer, postings, statistics or scoring functions.
void check_against_scan() {
    using Tokens = std::vector<std::string>;
    std::vector<Tokens> corpus(120);
    std::mt19937 random(42);
    Bm25Index index({1.7, 0.6});
    std::size_t total = 0;
    for (std::size_t d = 0; d < corpus.size(); ++d) {
        const auto length = random() % 30;
        std::string text;
        for (std::size_t i = 0; i < length; ++i) {
            const auto term = "term" + std::to_string(random() % 17);
            corpus[d].push_back(term);
            text += term + " ";
        }
        total += length;
        index.add_document(static_cast<DocumentId>(7 * d + 10), text);
    }
    index.build();
    const long double mean = static_cast<long double>(total) / corpus.size();
    for (std::size_t q = 0; q < 50; ++q) {
        std::set<std::string> terms;
        std::string query;
        for (std::size_t i = 0; i < 5; ++i) {
            auto term = "term" + std::to_string(random() % 22);
            terms.insert(term);
            query += term + " " + term + " ";
        }
        std::vector<RankedResult> expected;
        for (std::size_t d = 0; d < corpus.size(); ++d) {
            long double score = 0;
            bool matched = false;
            for (const auto& term : terms) {
                const auto tf = std::count(corpus[d].begin(), corpus[d].end(), term);
                if (tf == 0) continue;
                matched = true;
                std::size_t df = 0;
                for (const auto& other : corpus) {
                    if (std::find(other.begin(), other.end(), term) != other.end()) ++df;
                }
                const auto idf = std::log(1.0L + (corpus.size() - df + 0.5L) / (df + 0.5L));
                score += idf * tf * 2.7L / (tf + 1.7L * (0.4L + 0.6L * corpus[d].size() / mean));
            }
            if (matched)
                expected.push_back(
                    {static_cast<DocumentId>(7 * d + 10), static_cast<double>(score)});
        }
        std::sort(expected.begin(), expected.end(), [](const auto& a, const auto& b) {
            return a.score != b.score ? a.score > b.score : a.document_id < b.document_id;
        });
        if (expected.size() > 10) expected.resize(10);
        requireSameRanking(index.search(query, 10), expected);
    }
}

int main() {
    try {
        check_contracts();
        check_against_scan();
        std::cout
            << "T6-T10: accumulation, top-k, states and independent full-scan oracle passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
