#include "cli_runner.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <iomanip>
#include <locale>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "aether/application/search_service.hpp"
#include "cli_parser.hpp"

namespace aetherindex::cli {

namespace {

void print_neighbors(std::ostream& output, const std::vector<SearchResult>& neighbors) {
    using Row = std::array<std::string, 3>;
    const Row headings{"Rank", "Vector ID", "Distance"};
    std::array<std::size_t, 3> widths{headings[0].size(), headings[1].size(), headings[2].size()};
    std::vector<Row> rows;
    rows.reserve(neighbors.size());
    for (std::size_t i = 0; i < neighbors.size(); ++i) {
        std::ostringstream distance;
        distance.imbue(std::locale::classic());
        distance << std::fixed << std::setprecision(9) << neighbors[i].distance;
        rows.push_back({std::to_string(i + 1), std::to_string(neighbors[i].id), distance.str()});
        for (std::size_t column = 0; column < widths.size(); ++column) {
            widths[column] = std::max(widths[column], rows.back()[column].size());
        }
    }
    const auto border = [&] {
        output << '+';
        for (const auto width : widths) {
            output << std::string(width + 2, '-') << '+';
        }
        output << '\n';
    };
    const auto print_row = [&](const Row& row, bool align_right) {
        output << '|';
        for (std::size_t column = 0; column < widths.size(); ++column) {
            const std::string padding(widths[column] - row[column].size(), ' ');
            output << ' ' << (align_right ? padding + row[column] : row[column] + padding) << " |";
        }
        output << '\n';
    };
    border();
    print_row(headings, false);
    border();
    for (const auto& row : rows) {
        print_row(row, true);
        border();
    }
}

}  // namespace

int run(std::span<const std::string_view> arguments, std::ostream& output, std::ostream& errors) {
    CliOptions options;
    try {
        options = parse_arguments(arguments);
    } catch (const std::invalid_argument& error) {
        errors << "Error: " << error.what() << "\nUse aetherindex --help for usage.\n";
        return 2;
    } catch (const std::exception& error) {
        errors << "Error: " << error.what() << '\n';
        return 4;
    }

    try {
        if (options.show_help) {
            output << help_text();
            return 0;
        }
        const SearchService service;
        const auto response = service.search(options.search);
        // Prepare the complete response only after validation and search succeed.
        std::ostringstream formatted;
        formatted.imbue(std::locale::classic());
        formatted << "AetherIndex Search\n\n"
                  << "Algorithm: "
                  << (options.search.algorithm == SearchAlgorithm::Hnsw ? "HNSW" : "BruteForce")
                  << '\n'
                  << "Distance metric: "
                  << (options.search.metric == DistanceKind::Cosine ? "Cosine" : "SquaredEuclidean")
                  << "\nVectors indexed: " << response.vectors_indexed
                  << "\nDimensions: " << response.dimension << "\nRequested K: " << options.search.k
                  << "\nResults returned: " << response.neighbors.size();
        if (options.search.algorithm == SearchAlgorithm::Hnsw) {
            const auto& config = options.search.hnsw_config;
            formatted << "\nHNSW configuration: M=" << config.m << " M0=" << config.m_max_0
                      << " efConstruction=" << config.ef_construction
                      << " efSearch=" << config.ef_search << " seed=" << config.random_seed
                      << "\nHNSW neighbor selection: "
                      << (config.neighbor_selection == NeighborSelection::Heuristic
                              ? "Heuristic (Algorithm 4)"
                              : "Simple (Algorithm 3)")
                      << "\nIndex build time (ms): " << std::fixed << std::setprecision(6)
                      << response.index_build_time.count()
                      << "\nIndex build time scope: graph construction only (excludes CSV loading)";
        }
        formatted << "\nQuery time (ms): " << std::fixed << std::setprecision(6)
                  << response.query_time.count()
                  << "\nQuery time scope: search only (excludes CSV loading)\n\n";
        print_neighbors(formatted, response.neighbors);
        formatted << "\nSearch completed successfully.\n";
        output << formatted.str();
        return 0;
    } catch (const InputDataError& error) {
        errors << "Error: " << error.what() << '\n';
        return 3;
    } catch (const std::exception& error) {
        errors << "Error during search: " << error.what() << '\n';
        return 4;
    }
}

}  // namespace aetherindex::cli
