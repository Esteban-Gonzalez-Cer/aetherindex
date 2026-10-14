#include <array>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <random>
#include <stdexcept>
#include <string>

#include "aether/core/types.hpp"

// Synthetic fixtures only: no model, downloads or production embedding generation.
int main(int argc, char* argv[]) {
    try {
        if (argc != 2) {
            throw std::invalid_argument("Usage: aether_cli_fixture_generator OUTPUT_DIRECTORY");
        }
        const std::filesystem::path root(argv[1]);
        std::filesystem::create_directories(root);
        std::ofstream dataset(root / "vectors.csv");
        dataset.exceptions(std::ios::failbit | std::ios::badbit);
        dataset.imbue(std::locale::classic());
        dataset << std::setprecision(std::numeric_limits<float>::max_digits10);
        std::mt19937 random(42);
        for (std::size_t row = 0; row < 1000; ++row) {
            std::array<float, 128> embedding{};
            for (auto& component : embedding) {
                // mt19937 output and this integer mapping are reproducible across standard
                // libraries.
                component = (static_cast<int>(random() % 2001) - 1000) / 1000.0f;
            }
            embedding[0] = 1.0f;
            embedding[1] = static_cast<float>(row) / 1000.0f;
            const auto id = static_cast<aetherindex::VectorId>(7 * row + 10);
            dataset << id;
            for (const auto value : embedding) {
                dataset << ',' << value;
            }
            dataset << '\n';
            if (row % 97 == 0 && row / 97 < 10) {
                std::ofstream query(root / ("query_" + std::to_string(row / 97) + ".csv"));
                query.exceptions(std::ios::failbit | std::ios::badbit);
                query.imbue(std::locale::classic());
                query << std::setprecision(std::numeric_limits<float>::max_digits10);
                for (std::size_t column = 0; column < embedding.size(); ++column) {
                    if (column != 0) {
                        query << ',';
                    }
                    query << embedding[column];
                }
                query << '\n';
                query.close();
            }
        }
        dataset.close();
        std::cout << "Generated 1000 embeddings of dimension 128 and 10 queries (seed 42).\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
