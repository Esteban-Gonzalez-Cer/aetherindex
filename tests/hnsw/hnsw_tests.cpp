// Pruebas T4.1 - T4.6 de HNSW.
// Uso: aether_hnsw_tests            -> corre todas
//      aether_hnsw_tests T4.3       -> corre solo una

// Los headers estándar van ANTES del truco "private -> public" para no romperlos.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "aether/ann/brute_force/brute_force_index.hpp"
#include "aether/storage/vector_store.hpp"

// Permite inspeccionar nodes_, M_, maxLevel_ en T4.2 sin modificar HNSWIndex.h.
#define private public
#include "HNSWIndex.h"
#undef private

using namespace aetherindex;

// ----------------------------------------------------------------- utilidades
namespace {

struct TestFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond))                                                                  \
            throw TestFailure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                              "  CHECK fallo: " #cond);                               \
    } while (0)

// Generador determinista (mismo dataset en cualquier compilador).
class Rng {
   public:
    explicit Rng(std::uint32_t seed) : eng_(seed) {}
    float next() { return static_cast<float>(eng_() >> 8) / 16777216.0f; }  // [0,1)
   private:
    std::mt19937 eng_;
};

std::vector<EmbeddingValue> randomVector(Rng& rng, std::size_t dim) {
    std::vector<EmbeddingValue> v(dim);
    for (auto& x : v) x = rng.next() * 2.0f - 1.0f;  // [-1,1)
    return v;
}

// Llena el VectorStore con n vectores aleatorios (ids 0..n-1).
void fillStore(VectorStore& store, std::size_t n, std::uint32_t seed) {
    Rng rng(seed);
    for (std::size_t i = 0; i < n; ++i) {
        const auto v = randomVector(rng, store.dimension());
        CHECK(store.insert(static_cast<VectorId>(i), v) == InsertStatus::Success);
    }
}

void buildIndex(HNSWIndex& index, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) index.insert(static_cast<VectorId>(i));
}

constexpr std::size_t kDim = 32;
constexpr std::size_t kN = 2000;      // >= 1000
constexpr std::size_t kM = 16;
constexpr std::size_t kEfC = 200;
constexpr std::size_t kEfS = 100;

// ------------------------------------------------------------------- T4.1
void t4_1_basic_insert() {
    VectorStore store(3, DistanceKind::SquaredEuclidean);
    HNSWIndex index(store, 4, 50, 20);

    // Índice vacío
    std::printf("    Indice vacio creado: size = %zu\n", index.size());
    CHECK(index.size() == 0);

    // Un vector
    const std::vector<EmbeddingValue> v0{1.0f, 2.0f, 3.0f};
    CHECK(store.insert(0, v0) == InsertStatus::Success);
    index.insert(0);
    std::printf("    Insertado vector id=0: size = %zu\n", index.size());
    CHECK(index.size() == 1);
    CHECK(index.nodes_.contains(0));
    const auto found = index.search(v0, 1);
    CHECK(found.size() == 1 && found[0] == 0);
    std::printf("    Vector id=0 existe en el indice, search devuelve id=%u\n", found[0]);

    // Múltiples vectores
    std::size_t ok = 1;
    for (VectorId id = 1; id <= 20; ++id) {
        const std::vector<EmbeddingValue> v{static_cast<float>(id), static_cast<float>(id * 2),
                                            static_cast<float>(id % 5)};
        CHECK(store.insert(id, v) == InsertStatus::Success);
        index.insert(id);
        ++ok;
        CHECK(index.nodes_.contains(id));
    }
    std::printf("    Inserciones exitosas = %zu, size del indice = %zu\n", ok, index.size());
    CHECK(index.size() == ok);
    CHECK(index.size() == 21);

    // Inserciones fallidas NO deben contar
    bool threw = false;
    try { index.insert(5); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    std::printf("    Insertar id=5 duplicado: rechazado\n");
    threw = false;
    try { index.insert(999); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    std::printf("    Insertar id=999 inexistente en store: rechazado\n");
    std::printf("    size final = %zu (no cambio por los rechazos)\n", index.size());
    CHECK(index.size() == 21);
}

// ------------------------------------------------------------------- T4.2
void t4_2_graph_construction() {
    VectorStore store(kDim, DistanceKind::SquaredEuclidean);
    fillStore(store, kN, 42);
    HNSWIndex index(store, kM, kEfC, kEfS);
    buildIndex(index, kN);

    CHECK(index.size() == kN);
    CHECK(index.hasEntryPoint_);
    CHECK(index.nodes_.contains(index.entryPoint_));

    int highest = -1;
    std::size_t totalEdges = 0;

    for (const auto& [id, node] : index.nodes_) {
        CHECK(node.vector_id == id);
        CHECK(store.contains(id));

        // Nivel válido: 0..32 y no mayor que el nivel máximo del grafo
        CHECK(!node.neighbors.empty());
        const int level = static_cast<int>(node.neighbors.size()) - 1;
        CHECK(level >= 0 && level <= 32);
        CHECK(level <= index.maxLevel_);
        highest = std::max(highest, level);

        for (int l = 0; l <= level; ++l) {
            const auto& nb = node.neighbors[static_cast<std::size_t>(l)];

            // Límite del diseño: máximo M vecinos por nivel
            CHECK(nb.size() <= index.M_);

            std::unordered_set<VectorId> seen;
            for (VectorId n : nb) {
                // Apunta a un nodo existente (en el índice y en el store)
                const auto it = index.nodes_.find(n);
                CHECK(it != index.nodes_.end());
                CHECK(store.contains(n));
                CHECK(n != id);                         // sin auto-aristas
                CHECK(seen.insert(n).second);           // sin duplicados
                // El vecino debe existir en ese mismo nivel
                CHECK(it->second.neighbors.size() > static_cast<std::size_t>(l));
                ++totalEdges;
            }
        }
    }

    // El nivel máximo coincide con el del punto de entrada
    CHECK(highest == index.maxLevel_);
    CHECK(static_cast<int>(index.nodes_.at(index.entryPoint_).neighbors.size()) - 1 ==
          index.maxLevel_);
    CHECK(totalEdges > 0);

    std::printf("    nodos=%zu  maxLevel=%d  aristas=%zu\n", index.size(), index.maxLevel_,
                totalEdges);
}

// ------------------------------------------------------------------- T4.3
void t4_3_topk_basic() {
    VectorStore store(kDim, DistanceKind::SquaredEuclidean);
    fillStore(store, kN, 7);
    HNSWIndex index(store, kM, kEfC, kEfS);
    buildIndex(index, kN);
    std::printf("    Indice construido con n = %zu vectores\n", index.size());

    Rng rng(1234);
    const auto q = randomVector(rng, kDim);
    const auto res = index.search(q, 10);

    std::printf("    Consulta K=10 -> %zu resultados\n    IDs:", res.size());
    for (VectorId id : res) std::printf(" %u", id);
    std::printf("\n");

    CHECK(res.size() == 10);
    std::unordered_set<VectorId> uniq;
    for (VectorId id : res) {
        CHECK(id < kN);                 // ID válido (los ids son 0..kN-1)
        CHECK(store.contains(id));
        CHECK(uniq.insert(id).second);  // sin duplicados
    }
    CHECK(uniq.size() == 10);
    std::printf("    Todos los IDs validos (0..%zu) y sin duplicados (%zu unicos)\n", kN - 1,
                uniq.size());
}

// ------------------------------------------------------------------- T4.4
double runRecall(DistanceKind metric, std::uint32_t seed) {
    VectorStore store(kDim, metric);
    fillStore(store, kN, seed);
    HNSWIndex index(store, kM, kEfC, kEfS);
    buildIndex(index, kN);

    Rng rng(seed + 1000);
    constexpr std::size_t kQueries = 100;
    constexpr std::size_t K = 10;
    std::vector<double> recalls;

    for (std::size_t qi = 0; qi < kQueries; ++qi) {
        const auto q = randomVector(rng, kDim);
        const auto approx = index.search(q, K);
        const auto exact = bruteForceSearch(store, q, K);
        CHECK(approx.size() == K && exact.size() == K);

        std::unordered_set<VectorId> exactIds;
        for (const auto& r : exact) exactIds.insert(r.id);

        std::size_t hits = 0;
        for (VectorId id : approx) hits += exactIds.count(id);

        const double recall = static_cast<double>(hits) / static_cast<double>(K);
        recalls.push_back(recall);
        std::printf("    query %3zu  recall@10 = %.1f\n", qi, recall);
    }

    double sum = 0;
    for (double r : recalls) sum += r;
    return sum / static_cast<double>(recalls.size());
}

void t4_4_recall() {
    const double avgEuclid = runRecall(DistanceKind::SquaredEuclidean, 11);
    std::printf("    Recall@10 promedio (Euclidiana^2) = %.4f\n", avgEuclid);
    CHECK(avgEuclid >= 0.85);

    const double avgCos = runRecall(DistanceKind::Cosine, 22);
    std::printf("    Recall@10 promedio (Coseno)       = %.4f\n", avgCos);
    CHECK(avgCos >= 0.85);
}

// ------------------------------------------------------------------- T4.5
void t4_5_edge_cases() {
    const std::vector<EmbeddingValue> q{1.0f, 0.5f, -0.5f, 0.25f};

    // 0 elementos
    {
        VectorStore store(4, DistanceKind::SquaredEuclidean);
        HNSWIndex index(store, 4, 50, 20);
        CHECK(index.size() == 0);
        CHECK(index.search(q, 10).empty());
        CHECK(index.search(q, 1).empty());
        CHECK(index.search(q, 0).empty());
        std::printf("    [0 elementos] size=0, search K=10 -> 0 resultados, sin crash\n");
    }
    // 1 elemento
    {
        VectorStore store(4, DistanceKind::SquaredEuclidean);
        CHECK(store.insert(7, q) == InsertStatus::Success);
        HNSWIndex index(store, 4, 50, 20);
        index.insert(7);
        const auto r10 = index.search(q, 10);
        CHECK(r10.size() == 1 && r10[0] == 7);
        const auto r1 = index.search(q, 1);
        CHECK(r1.size() == 1 && r1[0] == 7);
        std::printf("    [1 elemento] K=10 -> %zu resultado (id=%u), K=1 -> id=%u\n", r10.size(),
                    r10[0], r1[0]);
    }
    // N < K  (5 elementos, K = 10)
    {
        VectorStore store(4, DistanceKind::SquaredEuclidean);
        fillStore(store, 5, 99);
        HNSWIndex index(store, 4, 50, 20);
        buildIndex(index, 5);
        const auto r = index.search(q, 10);
        CHECK(r.size() == 5);
        std::unordered_set<VectorId> u(r.begin(), r.end());
        CHECK(u.size() == 5);
        for (VectorId id : r) CHECK(id < 5);
        std::printf("    [N=5 < K=10] devuelve %zu resultados, IDs:", r.size());
        for (VectorId id : r) std::printf(" %u", id);
        std::printf(" (sin duplicados)\n");
    }
    // K = 1 sobre dataset grande
    {
        VectorStore store(kDim, DistanceKind::SquaredEuclidean);
        fillStore(store, 500, 5);
        HNSWIndex index(store, kM, kEfC, kEfS);
        buildIndex(index, 500);
        const auto v = store.find(123);
        CHECK(v.has_value());
        const auto r = index.search(*v, 1);
        CHECK(r.size() == 1 && r[0] == 123);
        std::printf("    [K=1, n=500] consulta = vector id=123 -> devuelve id=%u\n", r[0]);
    }
    // K = 0
    {
        VectorStore store(4, DistanceKind::SquaredEuclidean);
        fillStore(store, 10, 3);
        HNSWIndex index(store, 4, 50, 20);
        buildIndex(index, 10);
        CHECK(index.search(q, 0).empty());
        std::printf("    [K=0] devuelve 0 resultados, sin crash\n");
    }
    // Entradas inválidas -> excepción controlada, no crash
    {
        VectorStore store(4, DistanceKind::SquaredEuclidean);
        fillStore(store, 10, 3);
        HNSWIndex index(store, 4, 50, 20);
        buildIndex(index, 10);
        bool threw = false;
        try { index.search(std::vector<EmbeddingValue>{1.0f, 2.0f}, 3); }
        catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw);
        std::printf("    [dimension incorrecta] lanza invalid_argument (controlado)\n");
    }
    // M inválido
    {
        VectorStore store(4);
        bool threw = false;
        try { HNSWIndex bad(store, 1, 50, 20); } catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw);
        std::printf("    [M=1 invalido] lanza invalid_argument (controlado)\n");
    }
}

// ------------------------------------------------------------------- T4.6
void t4_6_stability() {
    VectorStore store(kDim, DistanceKind::SquaredEuclidean);
    fillStore(store, kN, 77);
    HNSWIndex index(store, kM, kEfC, kEfS);
    buildIndex(index, kN);
    CHECK(index.size() == kN);
    std::printf("    Indice construido con %zu vectores\n", index.size());

    Rng rng(555);
    int okQueries = 0;
    for (int i = 0; i < 100; ++i) {
        const auto q = randomVector(rng, kDim);
        const auto r = index.search(q, 10);
        CHECK(r.size() == 10);
        std::unordered_set<VectorId> u(r.begin(), r.end());
        CHECK(u.size() == 10);
        for (VectorId id : r) CHECK(store.contains(id));
        ++okQueries;
    }
    std::printf("    %d/100 consultas Top-10 ejecutadas sin crash ni errores\n", okQueries);
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"T4.1", t4_1_basic_insert},     {"T4.2", t4_2_graph_construction},
        {"T4.3", t4_3_topk_basic},       {"T4.4", t4_4_recall},
        {"T4.5", t4_5_edge_cases},       {"T4.6", t4_6_stability},
    };

    const std::string only = argc > 1 ? argv[1] : "";
    int failed = 0, ran = 0;

    for (const auto& [name, fn] : tests) {
        if (!only.empty() && only != name) continue;
        ++ran;
        std::printf("[ RUN  ] %s\n", name.c_str());
        try {
            fn();
            std::printf("[ PASS ] %s\n", name.c_str());
        } catch (const std::exception& e) {
            ++failed;
            std::printf("[ FAIL ] %s -> %s\n", name.c_str(), e.what());
        }
    }
    if (ran == 0) { std::printf("Prueba desconocida: %s\n", only.c_str()); return 2; }
    std::printf("\n%d/%d pruebas pasaron\n", ran - failed, ran);
    return failed == 0 ? 0 : 1;
}