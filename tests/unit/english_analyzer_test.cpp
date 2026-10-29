// english-v1 analyzer: Porter stemmer, possessives, stopwords, word joining, and BM25
// strategy equivalence under the analyzer. Also query term weighting (unique/frequency).
#include <exception>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "../bm25_test_helpers.hpp"
#include "aether/lexical/bm25.hpp"
#include "aether/lexical/tokenizer.hpp"

using namespace aetherindex;
using namespace aetherindex::lexical;

namespace {

// Reference pairs from Porter (1980), checked against the full algorithm.
void check_porter() {
    const std::pair<const char*, const char*> pairs[] = {
        {"caresses", "caress"},     {"ponies", "poni"},         {"ties", "ti"},
        {"caress", "caress"},       {"cats", "cat"},            {"feed", "feed"},
        {"agreed", "agre"},         {"plastered", "plaster"},   {"bled", "bled"},
        {"motoring", "motor"},      {"sing", "sing"},           {"conflated", "conflat"},
        {"troubled", "troubl"},     {"sized", "size"},          {"hopping", "hop"},
        {"tanned", "tan"},          {"falling", "fall"},        {"hissing", "hiss"},
        {"fizzed", "fizz"},         {"failing", "fail"},        {"filing", "file"},
        {"happy", "happi"},         {"sky", "sky"},             {"relational", "relat"},
        {"conditional", "condit"},  {"rational", "ration"},     {"valenci", "valenc"},
        {"digitizer", "digit"},     {"conformabli", "conform"}, {"radicalli", "radic"},
        {"differentli", "differ"},  {"vileli", "vile"},         {"analogousli", "analog"},
        {"vietnamization", "vietnam"}, {"predication", "predic"}, {"operator", "oper"},
        {"feudalism", "feudal"},    {"decisiveness", "decis"},  {"hopefulness", "hope"},
        {"callousness", "callous"}, {"formaliti", "formal"},    {"sensitiviti", "sensit"},
        {"sensibiliti", "sensibl"}, {"triplicate", "triplic"},  {"formative", "form"},
        {"formalize", "formal"},    {"electriciti", "electr"},  {"electrical", "electr"},
        {"hopeful", "hope"},        {"goodness", "good"},       {"revival", "reviv"},
        {"allowance", "allow"},     {"inference", "infer"},     {"airliner", "airlin"},
        {"gyroscopic", "gyroscop"}, {"adjustable", "adjust"},   {"defensible", "defens"},
        {"irritant", "irrit"},      {"replacement", "replac"},  {"adjustment", "adjust"},
        {"dependent", "depend"},    {"adoption", "adopt"},      {"homologou", "homolog"},
        {"communism", "commun"},    {"activate", "activ"},      {"angulariti", "angular"},
        {"homologous", "homolog"},  {"effective", "effect"},    {"bowdlerize", "bowdler"},
        {"probate", "probat"},      {"rate", "rate"},           {"cease", "ceas"},
        {"controll", "control"},    {"roll", "roll"},           {"generalizations", "gener"},
        {"oscillators", "oscil"},   {"ies", "i"},               {"is", "is"},
        {"archaeology", "archaeolog"}, {"possibly", "possibl"}, {"covid19", "covid19"}};
    for (const auto& [word, stem] : pairs) {
        const auto actual = porter_stem(word);
        const auto message =
            std::string("Porter stem of ") + word + " is " + actual + ", expected " + stem + ".";
        require(actual == stem, message.c_str());
    }
}

void check_tokens() {
    const Tokenizer english(Analyzer::EnglishV1);
    const auto check = [&](std::string_view input, std::vector<std::string> expected) {
        const auto actual = english.tokenize(input);
        std::string shown;
        for (const auto& token : actual) shown += "[" + token + "]";
        const auto message = "english-v1 tokens for '" + std::string(input) + "': " + shown;
        require(actual == expected, message.c_str());
    };
    check("The Cats are running into the gardens", {"cat", "run", "garden"});
    check("John's dog's bones", {"john", "dog", "bone"});
    check("John\xE2\x80\x99s book", {"john", "book"});
    check("don't stop", {"don't", "stop"});
    check("COVID-19 T-cell state-of-the-art", {"covid", "19", "t", "cell", "state", "art"});
    check("HTTP_404 C++ x+y", {"http_404", "c", "x", "y"});
    check("U.S.A. e.g. version 3.14 costs 1,000 units; 2;3", {"u.s.a", "e.g", "version", "3.14",
                                                              "cost", "1,000", "unit", "2;3"});
    check("ratio 1:2 a:b end.", {"ratio", "1", "2", "a:b", "end"});
    check("A AN THE and Not NO", {});
    check("a1.5 x.1", {"a1.5", "x", "1"});
    // Symbols split like Lucene's StandardTokenizer; Latin-1 capitals are lowercased.
    check("2\u00B10.5 10\u00D73 m\u00B2 CD4\u207A \u00B5m \u00C5NGSTR\u00D6M 0\u00B705",
          {"2", "0.5", "10", "3", "m", "cd4", "\u00B5m", "\u00E5ngstr\u00F6m", "0", "05"});
    check("", {});
    check(" ... '' -- __ ", {});
    // Stemming runs after stopword removal and on every token.
    check("connections connected connecting connection", {"connect", "connect", "connect",
                                                          "connect"});
    requireBm25Error<std::invalid_argument>([&] { return english.tokenize("\xC3\x28"); },
                                            "Invalid UTF-8 accepted.");
    require(std::string(analyzer_name(Analyzer::EnglishV1)) == "english-v1" &&
                std::string(analyzer_name(Analyzer::AsciiTechV1)) == "ascii-tech-v1",
            "Analyzer names changed.");
    // The default analyzer is unchanged.
    require(Tokenizer{}.tokenize("The Cats C++ COVID-19") ==
                std::vector<std::string>{"the", "cats", "c++", "covid-19"},
            "Default analyzer changed.");
}

// Every strategy returns the same ranking and bitwise scores under english-v1, and
// queries go through the same analysis as documents.
void check_bm25() {
    std::mt19937_64 engine(21);
    const char* words[] = {"connect", "connected", "connections", "running", "runs", "the",
                           "gardens", "garden's", "cell", "cells", "protein", "proteins",
                           "binding", "binds", "is", "a", "studies", "studying"};
    std::vector<std::string> docs(1500);
    for (auto& doc : docs)
        for (std::size_t t = 0, n = 3 + engine() % 30; t < n; ++t)
            doc += std::string(" ") + words[engine() % std::size(words)];
    Bm25Index reference({1.2, 0.75, Bm25Strategy::Exhaustive, Analyzer::EnglishV1});
    for (std::size_t i = 0; i < docs.size(); ++i)
        reference.add_document(static_cast<DocumentId>(i), docs[i]);
    reference.build();
    require(reference.inverted_index().document_frequency("connected") == 0 &&
                reference.inverted_index().document_frequency("connect") > 0 &&
                reference.inverted_index().document_frequency("the") == 0,
            "Documents were not analyzed with english-v1.");
    const auto stemmed = reference.search("connections", 10);
    const auto plain = reference.search("connect", 10);
    require(!stemmed.empty() && stemmed.size() == plain.size(), "Query not stemmed.");
    for (std::size_t i = 0; i < stemmed.size(); ++i)
        require(stemmed[i].document_id == plain[i].document_id &&
                    stemmed[i].score == plain[i].score,
                "Stemmed query differs from its stem.");
    require(reference.search("the a is", 10).empty(), "Stopword-only query matched.");
    for (auto strategy :
         {Bm25Strategy::TermAtATime, Bm25Strategy::Wand, Bm25Strategy::BlockMaxWand}) {
        Bm25Index variant({1.2, 0.75, strategy, Analyzer::EnglishV1});
        for (std::size_t i = 0; i < docs.size(); ++i)
            variant.add_document(static_cast<DocumentId>(i), docs[i]);
        variant.build();
        for (std::size_t q = 0; q < 200; ++q) {
            const std::string query = std::string(words[engine() % std::size(words)]) + " " +
                                      words[engine() % std::size(words)];
            const auto expected = reference.search(query, 10);
            const auto actual = variant.search(query, 10);
            require(actual.size() == expected.size(), "Strategy result count differs.");
            for (std::size_t i = 0; i < actual.size(); ++i)
                require(actual[i].document_id == expected[i].document_id &&
                            actual[i].score == expected[i].score,
                        "Strategy differs under english-v1.");
        }
    }
    Bm25Config bad;
    bad.analyzer = static_cast<Analyzer>(7);
    requireBm25Error<std::invalid_argument>([&] { return Bm25Index(bad).document_count(); },
                                            "Unknown analyzer accepted.");
}

// Frequency weighting multiplies each distinct term's contribution by its query count;
// Unique (the default) ignores repetitions. Every strategy agrees bitwise under both.
void check_query_weighting() {
    std::mt19937_64 engine(34);
    const char* words[] = {"cell", "protein", "binding", "garden", "graph", "vector", "index",
                           "query", "score", "block"};
    std::vector<std::string> docs(2000);
    for (auto& doc : docs)
        for (std::size_t t = 0, n = 2 + engine() % 40; t < n; ++t)
            doc += std::string(" ") + words[engine() % 3 == 0 ? engine() % 3 : engine() % 10];
    const auto build = [&](Bm25Strategy strategy, QueryTermWeighting weighting) {
        Bm25Config config{0.9, 0.4, strategy, Analyzer::EnglishV1};
        config.query_weighting = weighting;
        auto index = std::make_unique<Bm25Index>(config);
        for (std::size_t i = 0; i < docs.size(); ++i)
            index->add_document(static_cast<DocumentId>(i), docs[i]);
        index->build();
        return index;
    };
    const auto same = [](const std::vector<RankedResult>& a, const std::vector<RankedResult>& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (a[i].document_id != b[i].document_id || a[i].score != b[i].score) return false;
        return true;
    };
    require(Bm25Config{}.query_weighting == QueryTermWeighting::Unique,
            "Default query weighting changed.");
    const auto unique = build(Bm25Strategy::Exhaustive, QueryTermWeighting::Unique);
    const auto frequency = build(Bm25Strategy::Exhaustive, QueryTermWeighting::Frequency);
    require(same(unique->search("cells cell protein", 50), unique->search("cell protein", 50)),
            "Unique weighting counted a repeated term.");
    require(same(frequency->search("cell protein", 50), unique->search("cell protein", 50)),
            "Frequency weighting changed a query without repetitions.");

    // Exact expected score: terms summed in lexicographic order, "cell" weighted 2.
    std::unordered_map<DocumentId, double> cell, protein;
    for (const auto& r : unique->search("cell", docs.size())) cell[r.document_id] = r.score;
    for (const auto& r : unique->search("protein", docs.size())) protein[r.document_id] = r.score;
    const auto weighted = frequency->search("cells protein cell", docs.size());
    require(!weighted.empty(), "Weighted query returned nothing.");
    for (const auto& r : weighted) {
        const double expected = 2.0 * cell[r.document_id] + protein[r.document_id];
        require(r.score == expected, "Frequency weighting score differs from 2*cell + protein.");
    }
    require(!same(weighted, unique->search("cells protein cell", docs.size())),
            "Frequency weighting had no effect.");

    // Long queries with many repetitions exercise the weighted WAND/BMW bounds.
    std::vector<std::string> queries;
    for (std::size_t q = 0; q < 150; ++q) {
        std::string query;
        for (std::size_t t = 0, n = 1 + engine() % 25; t < n; ++t)
            query += std::string(" ") + words[engine() % std::size(words)];
        queries.push_back(query);
    }
    for (auto weighting : {QueryTermWeighting::Unique, QueryTermWeighting::Frequency}) {
        const auto reference = build(Bm25Strategy::Exhaustive, weighting);
        for (auto strategy :
             {Bm25Strategy::TermAtATime, Bm25Strategy::Wand, Bm25Strategy::BlockMaxWand}) {
            const auto variant = build(strategy, weighting);
            for (const auto& query : queries)
                for (std::size_t k : {1, 10, 100})
                    require(same(variant->search(query, k), reference->search(query, k)),
                            "Strategy differs under query term weighting.");
        }
    }
    Bm25Config bad;
    bad.query_weighting = static_cast<QueryTermWeighting>(5);
    requireBm25Error<std::invalid_argument>([&] { return Bm25Index(bad).document_count(); },
                                            "Unknown query weighting accepted.");
}

}  // namespace

int main() {
    try {
        check_porter();
        check_tokens();
        check_bm25();
        check_query_weighting();
        std::cout << "english-v1 analyzer checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
