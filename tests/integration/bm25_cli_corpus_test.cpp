#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <set>

#include "../test_helpers.hpp"
#include "aether/application/json_loader.hpp"
#include "aether/application/search_service.hpp"
#include "aether/lexical/tokenizer.hpp"

using namespace aetherindex;

int main(int argc, char** argv) {
    try {
        require(argc == 3, "Pass corpus and output directories.");
        const std::filesystem::path corpus(argv[1]), output(argv[2]);
        std::filesystem::create_directories(output);
        const auto docs = detail::load_jsonl_documents(corpus / "corpus.jsonl");
        const auto queries = detail::load_jsonl_queries((corpus / "queries.jsonl").string(), false);
        require(docs.size() == 30 && queries.size() == 24,
                "Unexpected version of checked-in corpus.");
        struct Counts {
            std::map<std::string, std::size_t> tf;
            std::size_t length = 0;
        };
        std::vector<Counts> counts;
        std::map<std::string, std::size_t> df;
        std::size_t total = 0;
        for (const auto& doc : docs) {
            Counts item;
            for (const auto& term : lexical::Tokenizer{}.tokenize(doc.title + "\n" + doc.text)) {
                ++item.tf[term];
                ++item.length;
            }
            for (const auto& [term, frequency] : item.tf) {
                static_cast<void>(frequency);
                ++df[term];
            }
            total += item.length;
            counts.push_back(std::move(item));
        }
        const double avgdl = static_cast<double>(total) / docs.size();
        std::ofstream report(output / "scores.tsv");
        report.exceptions(std::ios::failbit | std::ios::badbit);
        report.imbue(std::locale::classic());
        report << std::setprecision(17)
               << "query_id\trank\tdocument_id\texpected\tactual\tabs_error\n";
        double max_error = 0;
        const SearchService service;
        for (const auto& query : queries) {
            const auto tokens = lexical::Tokenizer{}.tokenize(query.text);
            // Default Frequency weighting: a term counts once per query occurrence.
            std::map<std::string, int> terms;
            for (const auto& token : tokens) ++terms[token];
            std::vector<DocumentSearchResult> expected;
            for (std::size_t i = 0; i < docs.size(); ++i) {
                double score = 0;
                bool candidate = false;
                for (const auto& [term, occurrences] : terms) {
                    const auto tf = counts[i].tf.find(term);
                    if (tf == counts[i].tf.end()) continue;
                    candidate = true;
                    const double frequency = tf->second;
                    const double idf =
                        std::log(1.0 + (docs.size() - df.at(term) + 0.5) / (df.at(term) + 0.5));
                    score += occurrences * (idf * frequency * 2.2 /
                                            (frequency + 1.2 * (0.25 + 0.75 * counts[i].length /
                                                                           avgdl)));
                }
                if (candidate) expected.push_back({docs[i].document_id, docs[i].id, score});
            }
            SearchRequest request{corpus / "corpus.jsonl", corpus / "queries.jsonl"};
            request.mode = SearchMode::Lexical;
            request.query_id = query.id;
            request.k = std::numeric_limits<std::size_t>::max();
            const auto actual = service.search(request);
            require(actual.documents.size() == expected.size(),
                    "Missing or padded corpus candidates.");
            std::set<DocumentId> returned;
            for (std::size_t i = 0; i < actual.documents.size(); ++i) {
                const auto& row = actual.documents[i];
                require(returned.insert(row.id).second, "Duplicate document in corpus response.");
                const auto found = std::find_if(expected.begin(), expected.end(),
                                                [&](const auto& e) { return e.id == row.id; });
                require(found != expected.end() && found->source_id == row.source_id,
                        "Document identity was lost.");
                const double error = std::abs(row.score - found->score);
                require(error <= 1e-12, "Independent corpus score reference disagrees.");
                max_error = std::max(max_error, error);
                if (i)
                    require(actual.documents[i - 1].score > row.score ||
                                (actual.documents[i - 1].score == row.score &&
                                 actual.documents[i - 1].id < row.id),
                            "Invalid corpus score/tie order.");
                report << query.id << '\t' << i + 1 << '\t' << row.source_id << '\t' << found->score
                       << '\t' << row.score << '\t' << error << '\n';
            }
            request.k = 10;
            const auto top = service.search(request);
            require(top.documents.size() == std::min(std::size_t{10}, expected.size()),
                    "Top-K count incorrect.");
            for (std::size_t i = 0; i < top.documents.size(); ++i)
                require(top.documents[i].source_id == actual.documents[i].source_id &&
                            top.documents[i].score == actual.documents[i].score,
                        "Top-K is not a prefix of the full ranking.");
        }
        report.close();
        std::ofstream summary(output / "summary.json");
        summary.exceptions(std::ios::failbit | std::ios::badbit);
        summary.imbue(std::locale::classic());
        summary << std::setprecision(17)
                << "{\"documents\":30,\"queries\":24,\"passed_queries\":24,\"absolute_tolerance\":"
                   "1e-12,\"maximum_score_error\":"
                << max_error
                << ",\"purpose\":\"wiring and arithmetic; no relevance target asserted\"}\n";
        summary.close();
        std::cout << "24/24 corpus queries matched an independent scoring calculation; max "
                     "absolute error "
                  << max_error << ".\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
