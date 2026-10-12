
#include "../src/ann/hnsw/HNSWIndex.h"

#include <iostream>
#include <vector>

int main() {
    HNSWIndex index(2);

    index.insert(0, {0.0f, 0.0f});
    index.insert(1, {1.0f, 1.0f});
    index.insert(2, {5.0f, 5.0f});
    index.insert(3, {1.1f, 1.0f});
    index.insert(4, {8.0f, 8.0f});

    std::vector<float> query = {1.0f, 0.9f};

    auto results = index.search(query, 3);

    std::cout << "Vectores indexados: "
              << index.size() << '\n';

    std::cout << "Resultados mas cercanos:\n";

    for (std::size_t id : results) {
        std::cout << "ID: " << id << '\n';
    }

    return 0;
}
