#include "cli_parser.hpp"

#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <system_error>

namespace aetherindex::cli {
namespace {

template <typename Integer>
Integer parse_unsigned(std::string_view text, std::string_view option, bool allow_zero = false) {
    Integer value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || (!allow_zero && value == 0)) {
        throw std::invalid_argument("Invalid value for " + std::string(option) +
                                    ": expected a representable " +
                                    (allow_zero ? "unsigned" : "positive") + " integer.");
    }
    return value;
}

}  // namespace

CliOptions parse_arguments(std::span<const std::string_view> arguments) {
    CliOptions options;
    if (arguments.empty() || (arguments.size() == 1 && arguments[0] == "--help")) {
        options.show_help = true;
        return options;
    }
    if (arguments[0] != "search") {
        throw std::invalid_argument("Unknown command: " + std::string(arguments[0]) + ".");
    }
    bool have_dataset = false;
    bool have_query = false;
    bool have_algorithm = false;
    bool have_k = false;
    bool have_metric = false;
    bool have_m = false;
    bool have_m0 = false;
    bool have_ef_construction = false;
    bool have_ef_search = false;
    bool have_seed = false;
    bool have_selection = false;
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const auto name = arguments[i];
        if (name == "--help") {
            if (options.show_help) {
                throw std::invalid_argument("Repeated option: --help.");
            }
            options.show_help = true;
            continue;
        }
        bool* seen = nullptr;
        if (name == "--dataset") {
            seen = &have_dataset;
        } else if (name == "--query") {
            seen = &have_query;
        } else if (name == "--algorithm") {
            seen = &have_algorithm;
        } else if (name == "--k") {
            seen = &have_k;
        } else if (name == "--metric") {
            seen = &have_metric;
        } else if (name == "--m") {
            seen = &have_m;
        } else if (name == "--m0") {
            seen = &have_m0;
        } else if (name == "--ef-construction") {
            seen = &have_ef_construction;
        } else if (name == "--ef-search") {
            seen = &have_ef_search;
        } else if (name == "--seed") {
            seen = &have_seed;
        } else if (name == "--neighbor-selection") {
            seen = &have_selection;
        } else {
            throw std::invalid_argument("Unknown option: " + std::string(name) + ".");
        }
        if (*seen) {
            throw std::invalid_argument("Repeated option: " + std::string(name) + ".");
        }
        *seen = true;
        if (i + 1 == arguments.size() || arguments[i + 1].empty() ||
            arguments[i + 1].starts_with("--")) {
            throw std::invalid_argument("Missing value for " + std::string(name) + ".");
        }
        const auto value = arguments[++i];
        if (name == "--dataset") {
            options.search.dataset_path = std::string(value);
        } else if (name == "--query") {
            options.search.query_path = std::string(value);
        } else if (name == "--algorithm") {
            if (value == "brute-force") {
                options.search.algorithm = SearchAlgorithm::BruteForce;
            } else if (value == "hnsw") {
                options.search.algorithm = SearchAlgorithm::Hnsw;
            }  else if (value == "bm25") {
    options.search.algorithm = SearchAlgorithm::Bm25;
            } else {
                throw std::invalid_argument("Unknown algorithm: " + std::string(value) +
                                            ". Available algorithms: brute-force, hnsw, bm25.");
            }
        } else if (name == "--metric") {
            if (value == "cosine") {
                options.search.metric = DistanceKind::Cosine;
            } else if (value == "squared-euclidean") {
                options.search.metric = DistanceKind::SquaredEuclidean;
            } else {
                throw std::invalid_argument("Invalid --metric: use cosine or squared-euclidean.");
            }
        } else if (name == "--neighbor-selection") {
            if (value == "simple")
                options.search.hnsw_config.neighbor_selection = NeighborSelection::Simple;
            else if (value == "heuristic")
                options.search.hnsw_config.neighbor_selection = NeighborSelection::Heuristic;
            else
                throw std::invalid_argument(
                    "Invalid --neighbor-selection: use simple or heuristic.");
        } else if (name == "--k") {
            options.search.k = parse_unsigned<std::size_t>(value, name);
        } else if (name == "--seed") {
            options.search.hnsw_config.random_seed =
                parse_unsigned<std::uint64_t>(value, name, true);
        } else {
            const auto parameter = parse_unsigned<std::size_t>(value, name);
            auto& config = options.search.hnsw_config;
            if (name == "--m")
                config.m = parameter;
            else if (name == "--m0")
                config.m_max_0 = parameter;
            else if (name == "--ef-construction")
                config.ef_construction = parameter;
            else if (name == "--ef-search")
                config.ef_search = parameter;
        }
    }
    if (!options.show_help) {
        if (!have_dataset) {
            throw std::invalid_argument("Missing required option: --dataset.");
        }
        if (!have_query) {
            throw std::invalid_argument("Missing required option: --query.");
        }
        if (!have_algorithm) {
            throw std::invalid_argument("Missing required option: --algorithm.");
        }
        if (options.search.algorithm != SearchAlgorithm::Hnsw &&
            (have_m || have_m0 || have_ef_construction || have_ef_search || have_seed ||
             have_selection)) {
            throw std::invalid_argument("HNSW options require --algorithm hnsw.");
        }
        validate_search_request(options.search);
    }
    return options;
}

std::string_view help_text() {
    return "AetherIndex CLI\n\n"
           "Usage:\n"
           "  aetherindex search --dataset FILE --query FILE --algorithm NAME [options]\n"
           "  aetherindex --help\n\n"
           "Options:\n"
           "  --dataset FILE       Required: CSV rows ID,x1,...,xD\n"
           "  --query FILE         Required: one CSV row x1,...,xD (without an ID)\n"
           "  --algorithm NAME     Required: brute-force (exact) | hnsw (approximate)\n"
           "  --k INTEGER          Positive number of neighbors (default: 10)\n"
           "  --metric NAME        cosine (default) | squared-euclidean\n"
           "  --m INTEGER          HNSW upper-layer neighbor limit (default: 16; >= 2)\n"
           "  --m0 INTEGER         HNSW base-layer neighbor limit (default: 32; >= M)\n"
           "  --ef-construction N  HNSW construction search width (default: 100; >= M)\n"
           "  --ef-search N        HNSW query search width (default: 50; > 0)\n"
           "  --seed INTEGER       HNSW unsigned 64-bit seed, including zero (default: 42)\n"
           "  --neighbor-selection NAME  HNSW heuristic (default, Algorithm 4) | simple (Algorithm "
           "3)\n"
           "  --help               Show help (also: search --help)\n\n"
           "CSV: no headers or quotes; finite floats; unique unsigned 32-bit IDs.\n"
           "Spaces/tabs around fields and blank lines are allowed. All dimensions must match.\n"
           "Cosine stores unit vectors and rejects embedding L2 norms below 1e-7.\n"
           "Cosine queries must be nonzero; squared Euclidean permits zero vectors.\n"
           "Results are sorted by increasing distance; at most min(k, dataset size) are returned.\n"
           "Distances use nine decimal places; IDs stay integers in the bordered results table.\n"
           "Query time is reported in milliseconds and excludes CSV loading and output.\n"
           "HNSW graph construction is reported separately and excluded from query time.\n"
           "HNSW uses max(efSearch,k), capped by index size; sparse graphs may return fewer "
           "neighbors.\n"
           "HNSW options require --algorithm hnsw; its graph is rebuilt in each execution.\n"
           "Data is loaded and searched in one execution. There is no persistence.\n\n"
           "Exit codes: 0 success/help, 2 arguments, 3 input data, 4 "
           "search/internal error.\n";
}

}  // namespace aetherindex::cli
