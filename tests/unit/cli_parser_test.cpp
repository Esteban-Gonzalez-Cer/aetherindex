#include "cli_parser.hpp"

#include <exception>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "../test_helpers.hpp"

using namespace aetherindex;
using namespace aetherindex::cli;

CliOptions parse(std::initializer_list<std::string_view> arguments) {
    return parse_arguments(std::span(arguments.begin(), arguments.size()));
}

void checkValidOptions() {
    require(parse({}).show_help, "No arguments must show help.");
    require(parse({"--help"}).show_help, "Global help must work.");
    require(parse({"search", "--help"}).show_help, "Search help must work without data files.");
    const auto defaults = parse({"search", "--dataset", "vectors.csv", "--query", "query.csv",
                                 "--algorithm", "brute-force"});
    require(!defaults.show_help && defaults.search.k == 10 &&
                defaults.search.metric == DistanceKind::Cosine &&
                defaults.search.algorithm == SearchAlgorithm::BruteForce,
            "Search defaults must be cosine and top-10.");
    require(
        defaults.search.dataset_path == "vectors.csv" && defaults.search.query_path == "query.csv",
        "The supplied paths must be preserved.");
    const auto explicit_options =
        parse({"search", "--k", "20", "--metric", "squared-euclidean", "--algorithm", "brute-force",
               "--query", "query with spaces.csv", "--dataset", "dataset with spaces.csv"});
    require(explicit_options.search.k == 20 &&
                explicit_options.search.metric == DistanceKind::SquaredEuclidean &&
                explicit_options.search.query_path == "query with spaces.csv" &&
                explicit_options.search.dataset_path == "dataset with spaces.csv",
            "Flag order, explicit metrics, k and quoted paths must be supported.");
    auto arguments = std::vector<std::string_view>{
        "search", "--dataset", "data", "--query", "query", "--algorithm", "brute-force", "--k", {}};
    const auto maximum = std::to_string(std::numeric_limits<std::size_t>::max());
    arguments.back() = maximum;
    require(parse_arguments(arguments).search.k == std::numeric_limits<std::size_t>::max(),
            "The full positive size_t range must be accepted and truncated by search.");
    const auto hnsw_defaults =
        parse({"search", "--dataset", "missing", "--query", "missing", "--algorithm", "hnsw"});
    require(hnsw_defaults.search.algorithm == SearchAlgorithm::Hnsw &&
                hnsw_defaults.search.hnsw_config.m == 16 &&
                hnsw_defaults.search.hnsw_config.m_max_0 == 32 &&
                hnsw_defaults.search.hnsw_config.ef_construction == 100 &&
                hnsw_defaults.search.hnsw_config.ef_search == 50 &&
                hnsw_defaults.search.hnsw_config.random_seed == 42,
            "HNSW must parse without reading files and retain the documented defaults.");
    const auto hnsw_explicit = parse({"search",      "--m",         "4",
                                      "--m0",        "8",           "--ef-construction",
                                      "20",          "--ef-search", "3",
                                      "--seed",      "0",           "--dataset",
                                      "data",        "--query",     "query",
                                      "--algorithm", "hnsw",        "--k",
                                      "10",          "--metric",    "squared-euclidean"});
    require(hnsw_explicit.search.hnsw_config.m == 4 &&
                hnsw_explicit.search.hnsw_config.m_max_0 == 8 &&
                hnsw_explicit.search.hnsw_config.ef_construction == 20 &&
                hnsw_explicit.search.hnsw_config.ef_search == 3 &&
                hnsw_explicit.search.hnsw_config.random_seed == 0 && hnsw_explicit.search.k == 10 &&
                hnsw_explicit.search.metric == DistanceKind::SquaredEuclidean,
            "HNSW flags, seed zero and efSearch<K must parse in arbitrary order.");
    arguments = {"search",      "--dataset", "data",   "--query", "query",
                 "--algorithm", "hnsw",      "--seed", {}};
    const auto maximum_seed = std::to_string(std::numeric_limits<std::uint64_t>::max());
    arguments.back() = maximum_seed;
    require(parse_arguments(arguments).search.hnsw_config.random_seed ==
                std::numeric_limits<std::uint64_t>::max(),
            "Seeds must accept the full unsigned 64-bit range.");
}

void checkInvalidOptions() {
    require(parse({"search", "--dataset", "d", "--query", "q", "--algorithm", "hnsw",
                   "--neighbor-selection", "simple"})
                    .search.hnsw_config.neighbor_selection == NeighborSelection::Simple,
            "The CLI must support the controlled Algorithm 3 comparison.");
    for (const auto algorithm : {"brute-force", "hnsw"}) {
        requireInvalidArgument(
            [&] {
                parse({"search", "--dataset", "d", "--query", "q", "--algorithm", algorithm,
                       "--neighbor-selection",
                       algorithm == std::string_view("hnsw") ? "unknown" : "heuristic"});
            },
            "Invalid or inapplicable selection options must be rejected.");
    }
    const std::vector<std::vector<std::string_view>> invalid = {
        {"index"},
        {"search"},
        {"search", "--dataset", "data"},
        {"search", "--query", "query", "--algorithm", "brute-force"},
        {"search", "--dataset", "data", "--query", "query"},
        {"search", "--dataset"},
        {"search", "--dataset", ""},
        {"search", "--dataset", "--query", "query"},
        {"search", "--unknown", "value"},
        {"search", "unexpected"},
        {"search", "--dataset=data"},
        {"search", "--dataset", "a", "--dataset", "b"},
        {"search", "--query", "a", "--query", "b"},
        {"search", "--algorithm", "brute-force", "--algorithm", "brute-force"},
        {"search", "--k", "1", "--k", "2"},
        {"search", "--metric", "cosine", "--metric", "cosine"},
        {"search", "--help", "--help"},
        {"search", "--algorithm", "xyz"},
        {"search", "--metric", "euclidean"},
    };
    for (const auto& arguments : invalid) {
        requireInvalidArgument([&] { parse_arguments(arguments); },
                               "Malformed arguments must be rejected.");
    }
    for (const auto value :
         {"0", "-1", "+1", "abc", "1.5", "1x", "1 2", "999999999999999999999999999999999999999"}) {
        requireInvalidArgument(
            [&] {
                parse({"search", "--dataset", "d", "--query", "q", "--algorithm", "brute-force",
                       "--k", value});
            },
            "k must be a complete, positive, representable unsigned integer.");
    }
    for (const auto flag : {"--m", "--m0", "--ef-construction", "--ef-search", "--seed"}) {
        const auto valid_value = flag == std::string_view("--seed") ? "0" : "32";
        requireInvalidArgument(
            [&] {
                parse({"search", "--dataset", "d", "--query", "q", "--algorithm", "brute-force",
                       flag, valid_value});
            },
            "HNSW-only flags must be rejected with brute force, even when numerically valid.");
        requireInvalidArgument(
            [&] {
                parse({"search", "--dataset", "d", "--query", "q", "--algorithm", "hnsw", flag,
                       valid_value, flag, valid_value});
            },
            "Repeated HNSW flags must be rejected.");
        for (const auto value : {"-1", "+1", "abc", "1.5", "1x", "1 2", "18446744073709551616"}) {
            requireInvalidArgument(
                [&] {
                    parse({"search", "--dataset", "d", "--query", "q", "--algorithm", "hnsw", flag,
                           value});
                },
                "HNSW numeric options must be complete unsigned integers in range.");
        }
        requireInvalidArgument(
            [&] {
                parse({"search", "--dataset", "d", "--query", "q", "--algorithm", "hnsw", flag});
            },
            "HNSW option values are required.");
    }
    for (const auto flag : {"--m", "--m0", "--ef-construction", "--ef-search"}) {
        requireInvalidArgument(
            [&] {
                parse(
                    {"search", "--dataset", "d", "--query", "q", "--algorithm", "hnsw", flag, "0"});
            },
            "HNSW sizes and widths must be positive.");
    }
    requireInvalidArgument(
        [&] {
            parse({"search", "--dataset", "d", "--query", "q", "--algorithm", "hnsw", "--m", "1"});
        },
        "M must be at least two.");
    requireInvalidArgument(
        [&] {
            parse(
                {"search", "--dataset", "d", "--query", "q", "--algorithm", "hnsw", "--m0", "15"});
        },
        "M0 must be at least M.");
    requireInvalidArgument(
        [&] {
            parse({"search", "--dataset", "d", "--query", "q", "--algorithm", "hnsw",
                   "--ef-construction", "15"});
        },
        "efConstruction must be at least M.");
}

void checkLexicalOptions() {
    const auto defaults = parse({"search", "--dataset", "corpus.jsonl", "--query", "queries.jsonl",
                                 "--query-id", "Q002", "--algorithm", "bm25"});
    require(defaults.search.mode == SearchMode::Lexical && defaults.search.query_id == "Q002" &&
                defaults.search.bm25_config.k1 == 1.2 && defaults.search.bm25_config.b == 0.75,
            "BM25 shorthand must select lexical mode and preserve query/configuration.");
    const auto literal = parse({"search", "--mode", "lexical", "--dataset", "corpus.jsonl",
                                "--query-text", "", "--bm25-k1", "2.5", "--bm25-b", "0"});
    require(literal.search.query_text && literal.search.query_text->empty() &&
                literal.search.bm25_config.k1 == 2.5 && literal.search.bm25_config.b == 0,
            "Lexical mode requires no vector algorithm and accepts empty queries and b=0.");
    const std::vector<std::vector<std::string_view>> invalid = {
        {"--mode", "vector"},
        {"--mode", "hybrid"},
        {"--metric", "cosine"},
        {"--m", "16"},
        {"--ef-search", "50"},
        {"--seed", "42"},
        {"--neighbor-selection", "heuristic"},
        {"--query", "queries.jsonl"},
        {"--query-id", "Q001"},
        {"--bm25-k1", "0"},
        {"--bm25-k1", "-1"},
        {"--bm25-b", "-0.1"},
        {"--bm25-b", "1.1"},
        {"--bm25-k1", "1", "--bm25-k1", "2"},
        {"--mode", "lexical", "--mode", "lexical"},
        {"--bm25-b", "0", "--bm25-b", "1"},
        {"--query-id", "Q1", "--query-id", "Q2"},
    };
    for (const auto& suffix : invalid) {
        std::vector<std::string_view> arguments = {
            "search", "--algorithm", "bm25", "--dataset", "missing.jsonl", "--query-text", "radio"};
        arguments.insert(arguments.end(), suffix.begin(), suffix.end());
        requireInvalidArgument(
            [&] { static_cast<void>(parse_arguments(arguments)); },
            "Inapplicable, conflicting or repeated lexical flags were accepted.");
    }
    for (const auto flag : {"--bm25-k1", "--bm25-b"}) {
        for (const auto value : {"nan", "inf", "-inf", "1junk", "1e999", "1,2", ""}) {
            requireInvalidArgument(
                [&] {
                    parse({"search", "--algorithm", "bm25", "--dataset", "missing.jsonl",
                           "--query-text", "radio", flag, value});
                },
                "Invalid/nonfinite BM25 numeric argument was accepted.");
        }
        requireInvalidArgument(
            [&] {
                parse({"search", "--algorithm", "brute-force", "--dataset", "data.csv", "--query",
                       "query.csv", flag, "1"});
            },
            "BM25-only flags must not be ignored by vector search.");
    }
    for (const auto algorithm : {"brute-force", "hnsw"}) {
        requireInvalidArgument(
            [&] {
                parse({"search", "--mode", "lexical", "--algorithm", algorithm, "--dataset",
                       "missing.jsonl", "--query-text", "radio"});
            },
            "A vector algorithm must not silently run as BM25.");
    }
    requireInvalidArgument(
        [&] {
            parse({"search", "--algorithm", "brute-force", "--dataset", "data.csv", "--query",
                   "query.csv", "--query-id", "Q1"});
        },
        "Query selectors require JSONL.");
}

void checkHybridOptions() {
    const auto defaults = parse({"search", "--mode", "hybrid", "--dataset", "corpus.jsonl",
                                 "--query", "queries.jsonl", "--query-id", "Q001"});
    require(defaults.search.mode == SearchMode::Hybrid &&
                defaults.search.algorithm == SearchAlgorithm::Hnsw &&
                !defaults.search.candidate_k && defaults.search.rrf_config.k_rrf == 60.0 &&
                effective_candidate_k(defaults.search) == 50,
            "Hybrid defaults must be HNSW + BM25 + RRF(k=60) with max(k,50) candidates.");
    const auto large_k = parse({"search", "--mode", "hybrid", "--dataset", "corpus.jsonl",
                                "--query", "queries.jsonl", "--k", "80"});
    require(effective_candidate_k(large_k.search) == 80, "Default candidates must cover k.");
    const auto custom = parse({"search", "--mode", "hybrid", "--algorithm", "hnsw", "--dataset",
                               "corpus.jsonl", "--query", "queries.jsonl", "--k", "5",
                               "--candidate-k", "20", "--rrf-k", "10.5", "--ef-search", "64",
                               "--bm25-k1", "1.5", "--bm25-b", "0.5", "--metric",
                               "squared-euclidean"});
    require(custom.search.candidate_k == 20 && custom.search.rrf_config.k_rrf == 10.5 &&
                custom.search.hnsw_config.ef_search == 64 && custom.search.bm25_config.k1 == 1.5 &&
                custom.search.bm25_config.b == 0.5 &&
                custom.search.metric == DistanceKind::SquaredEuclidean,
            "Hybrid must accept HNSW, BM25, metric and fusion parameters.");
    const std::vector<std::vector<std::string_view>> invalid = {
        {"--algorithm", "brute-force"},
        {"--algorithm", "bm25"},
        {"--candidate-k", "4", "--k", "5"},
        {"--candidate-k", "0"},
        {"--rrf-k", "0"},
        {"--rrf-k", "-1"},
        {"--rrf-k", "nan"},
        {"--rrf-k", "1", "--rrf-k", "2"},
        {"--query-text", "motor"},
    };
    for (const auto& suffix : invalid) {
        std::vector<std::string_view> arguments = {"search", "--mode", "hybrid", "--dataset",
                                                   "corpus.jsonl", "--query", "queries.jsonl"};
        arguments.insert(arguments.end(), suffix.begin(), suffix.end());
        requireInvalidArgument([&] { static_cast<void>(parse_arguments(arguments)); },
                               "Invalid or conflicting hybrid flags were accepted.");
    }
    for (const auto flag : {"--candidate-k", "--rrf-k"}) {
        requireInvalidArgument(
            [&] {
                parse({"search", "--algorithm", "hnsw", "--dataset", "data.csv", "--query",
                       "query.csv", flag, "10"});
            },
            "Fusion flags must not be ignored by vector search.");
        requireInvalidArgument(
            [&] {
                parse({"search", "--algorithm", "bm25", "--dataset", "corpus.jsonl",
                       "--query-text", "radio", flag, "10"});
            },
            "Fusion flags must not be ignored by lexical search.");
    }
    requireInvalidArgument(
        [&] {
            parse({"search", "--mode", "hybrid", "--dataset", "data.csv", "--query",
                   "query.csv"});
        },
        "Hybrid requires JSONL documents and queries.");
}

int main() {
    try {
        checkValidOptions();
        checkInvalidOptions();
        checkLexicalOptions();
        checkHybridOptions();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
