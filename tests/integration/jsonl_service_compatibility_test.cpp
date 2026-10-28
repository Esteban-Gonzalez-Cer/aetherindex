#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

#include "../test_helpers.hpp"
#include "aether/application/search_service.hpp"

using namespace aetherindex;

namespace {
void write(const std::filesystem::path& path, std::string_view text) {
    std::ofstream file(path, std::ios::binary);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file << text;
    file.close();
}

template <class Function>
void require_input_error(Function action) {
    try {
        action();
    } catch (const InputDataError&) {
        return;
    }
    throw std::runtime_error("Malformed/incomplete JSONL vector input was accepted.");
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "Pass a fixture output directory.");
        const std::filesystem::path folder(argv[1]);
        std::filesystem::create_directories(folder);
        SearchRequest request;
        request.dataset_path = folder / "corpus.jsonl";
        request.query_path = folder / "query.jsonl";
        request.k = 2;
        write(request.dataset_path,
              "{\"id\":\"alpha\",\"text\":\"motor\",\"vector\":[10,0]}\n"
              "{\"id\":\"beta\",\"text\":\"motor temperatura\",\"vector\":[1,1]}\n");
        write(request.query_path, "{\"id\":\"q\",\"query\":\"motor\",\"vector\":[1,0]}\n");
        const SearchService service;
        for (auto algorithm : {SearchAlgorithm::BruteForce, SearchAlgorithm::Hnsw}) {
            request.algorithm = algorithm;
            request.metric = DistanceKind::Cosine;
            const auto cosine = service.search(request);
            require(cosine.vectors_indexed == 2 && cosine.dimension == 2 &&
                        cosine.neighbors.size() == 2 && cosine.neighbors[0].id == 0,
                    "JSONL cosine path failed after API repair.");
            requireNear(cosine.neighbors[0].distance, 0, "JSONL cosine result incorrect.");
            request.metric = DistanceKind::SquaredEuclidean;
            const auto squared = service.search(request);
            require(squared.neighbors.size() == 2 && squared.neighbors[0].id == 1 &&
                        squared.neighbors[1].id == 0,
                    "JSONL store ignored the configured distance metric.");
            requireNear(squared.neighbors[0].distance, 1, "JSONL squared result incorrect.");
            requireNear(squared.neighbors[1].distance, 81, "JSONL squared magnitude was lost.");
        }
        write(request.query_path, "");
        require_input_error([&] { static_cast<void>(service.search(request)); });
        write(request.query_path, "{\"id\":\"q\",\"query\":\"motor\"}\n");
        require_input_error([&] { static_cast<void>(service.search(request)); });
        write(request.query_path, "{\"id\":\"q\",\"vector\":[1,0]}\n");
        write(request.dataset_path,
              "{\"document_id\":\"D001\",\"text\":\"motor\",\"embedding\":null}\n");
        require_input_error([&] { static_cast<void>(service.search(request)); });
        write(request.dataset_path,
              "{\"id\":\"a\",\"vector\":[1,0]}\n{\"id\":\"b\",\"vector\":[1,0,0]}\n");
        require_input_error([&] { static_cast<void>(service.search(request)); });
        write(request.dataset_path, "");
        require_input_error([&] { static_cast<void>(service.search(request)); });
        std::cout << "Existing JSONL vector path: both algorithms/metrics and missing-data guards "
                     "passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
