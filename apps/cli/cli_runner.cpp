#include "cli_runner.hpp"

#include <iostream>
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

// Agrega el prototipo o la definición completa arriba en cli_runner.cpp:
void print_ranked_documents(std::ostream& output,
                            const std::vector<aetherindex::RankedResult>& results,
                            const std::vector<std::string>& document_ids) {
    using Row = std::array<std::string, 3>;
    const Row headings{"Rank", "Document ID", "Score"};
    std::array<std::size_t, 3> widths{headings[0].size(), headings[1].size(), headings[2].size()};
    
    std::vector<Row> rows;
    rows.reserve(results.size());

    for (std::size_t i = 0; i < results.size(); ++i) {
        // Mapea el DocumentId numérico al ID original en string si está dentro de rango
        std::string doc_id = (results[i].document_id < document_ids.size()) 
            ? document_ids[results[i].document_id] 
            : std::to_string(results[i].document_id);

        // Formateo del puntaje (Score BM25) con precisión fija
        std::ostringstream score_ss;
        score_ss.imbue(std::locale::classic());
        score_ss << std::fixed << std::setprecision(6) << results[i].score;

        rows.push_back({std::to_string(i + 1), std::move(doc_id), score_ss.str()});

        // Actualización dinámica del ancho de columnas
        for (std::size_t column = 0; column < widths.size(); ++column) {
            widths[column] = std::max(widths[column], rows.back()[column].size());
        }
    }

    // Dibujado de bordes superiores e inferiores de la tabla
    const auto border = [&] {
        output << '+';
        for (const auto width : widths) {
            output << std::string(width + 2, '-') << '+';
        }
        output << '\n';
    };

    // Formateo de cada fila respetando alineación
    const auto print_row = [&](const Row& row, bool align_right) {
        output << '|';
        for (std::size_t column = 0; column < widths.size(); ++column) {
            const std::string padding(widths[column] - row[column].size(), ' ');
            output << ' ' << (align_right ? padding + row[column] : row[column] + padding) << " |";
        }
        output << '\n';
    };

    border();
    print_row(headings, false); // Encabezados alineados a la izquierda
    border();
    for (const auto& row : rows) {
        print_row(row, true);   // Datos alineados a la derecha
        border();
    }
}

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
                  << "Algorithm: ";

        // 1. Algoritmo y configuración
switch (options.search.algorithm) {
    case SearchAlgorithm::BruteForce:
        formatted << "BruteForce\n"
                  << "Distance metric: "
                  << (options.search.metric == DistanceKind::Cosine ? "Cosine" : "SquaredEuclidean");
        break;

    case SearchAlgorithm::Hnsw:
        formatted << "HNSW\n"
                  << "Distance metric: "
                  << (options.search.metric == DistanceKind::Cosine ? "Cosine" : "SquaredEuclidean");
        break;

    case SearchAlgorithm::Bm25:
        formatted << "BM25\n"
                  << "Scoring: lexical relevance";
        break;
}

// 2. Estadísticas generales
formatted << "\nDocuments indexed: "
          << (options.search.algorithm == SearchAlgorithm::Bm25
                  ? response.ranked_document_ids.size()
                  : response.vectors_indexed)
          << "\nRequested K: " << options.search.k 
          << "\nResults returned: "
          << (options.search.algorithm == SearchAlgorithm::Bm25
                  ? response.ranked_documents.size()
                  : response.neighbors.size());

// 3. Tiempos de ejecución
if (options.search.algorithm == SearchAlgorithm::Hnsw || 
    options.search.algorithm == SearchAlgorithm::Bm25) {
    formatted << "\nIndex build time (ms): " << std::fixed << std::setprecision(6)
              << response.index_build_time.count();
}

formatted << "\nQuery time (ms): " << std::fixed << std::setprecision(6)
          << response.query_time.count()
          << "\nQuery time scope: search only (excludes file loading)\n\n";

// 4. Salida de tabla ASCII formateada en el flujo 'formatted'
if (options.search.algorithm == SearchAlgorithm::Bm25) {
    print_ranked_documents(formatted, response.ranked_documents, response.ranked_document_ids);
} else {
    print_neighbors(formatted, response.neighbors);
}
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
