// Vector deletes: tombstoned vectors never appear in any result, exact search equals exact
// search over the live set, compaction equals a fresh build over the live set, a deleted ID
// can be inserted again, and tombstones survive a snapshot.

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <random>
#include <unordered_set>
#include <vector>

#include "../hnsw_test_helpers.hpp"
#include "aether/ann/brute_force/brute_force_index.hpp"
#include "aether/core/prepared_query.hpp"
#include "aether/persistence/snapshot.hpp"
#include "aether/quantization/sq8.hpp"

namespace {

using namespace aetherindex;

void require_same(const std::vector<SearchResult>& a, const std::vector<SearchResult>& b,
                  const char* message) {
    require(a.size() == b.size(), message);
    for (std::size_t i = 0; i < a.size(); ++i)
        require(a[i].id == b[i].id && std::memcmp(&a[i].distance, &b[i].distance, 4) == 0,
                message);
}

struct Fixture {
    std::size_t dimension = 20;
    std::vector<std::vector<float>> rows = hnsw_test::embeddings(1500, 20, 51);
    std::vector<std::vector<float>> queries = hnsw_test::embeddings(40, 20, 52);
    std::unordered_set<VectorId> deleted;
    HnswConfig config = [] {
        HnswConfig c;
        c.m = 8;
        c.m_max_0 = 16;
        c.ef_construction = 60;
        c.random_seed = 5;
        return c;
    }();

    Fixture() {
        std::mt19937 random(9);
        while (deleted.size() < 400) deleted.insert(hnsw_test::fixture_id(random() % rows.size()));
    }
};

void deleted_vectors_never_returned() {
    Fixture f;
    for (auto metric : {DistanceKind::Cosine, DistanceKind::SquaredEuclidean}) {
        VectorStore store(f.dimension, metric);
        hnsw_test::populate(store, f.rows);
        HnswIndex index(store, f.config);
        for (auto id : store.ids()) index.insert(id);
        const Sq8Vectors codes(store);
        for (auto id : f.deleted) require(store.erase(id), "erase must find a live ID.");
        require(!store.erase(*f.deleted.begin()), "A second erase must report absence.");
        require(store.live_size() == f.rows.size() - f.deleted.size(), "Live size is wrong.");

        // Exact search over the live set, built from scratch.
        VectorStore live(f.dimension, metric);
        for (std::size_t i = 0; i < f.rows.size(); ++i)
            if (!f.deleted.contains(hnsw_test::fixture_id(i)))
                static_cast<void>(live.insert(hnsw_test::fixture_id(i), f.rows[i]));
        std::size_t found = 0, expected = 0;
        for (const auto& query : f.queries) {
            require_same(bruteForceSearch(store, query, 25), bruteForceSearch(live, query, 25),
                         "Exact search must equal exact search over the live set.");
            const PreparedQuery prepared(query, f.dimension, metric);
            const auto approximate = index.search(query, 10, 60);
            const auto quantized = index.search_quantized(prepared, codes, 10, 60, 10);
            const auto filtered = index.search_filtered(
                prepared, 10, 60, [](VectorId id) { return id % 2 == 0; });
            for (const auto* results : {&approximate, &quantized, &filtered})
                for (const auto& hit : *results)
                    require(!f.deleted.contains(hit.id), "A deleted vector was returned.");
            require(approximate.size() == 10, "HNSW must still fill k from live vectors.");
            const auto truth = bruteForceSearch(live, query, 10);
            std::unordered_set<VectorId> wanted;
            for (const auto& hit : truth) wanted.insert(hit.id);
            for (const auto& hit : approximate) found += wanted.contains(hit.id);
            expected += truth.size();
        }
        require(static_cast<double>(found) / static_cast<double>(expected) >= 0.9,
                "HNSW recall over the live set collapsed after deletes.");
    }
}

void compaction_equals_fresh_build() {
    Fixture f;
    VectorStore store(f.dimension, DistanceKind::Cosine);
    hnsw_test::populate(store, f.rows);
    for (auto id : f.deleted) store.erase(id);
    const auto compacted = store.compacted();
    HnswIndex rebuilt(*compacted, f.config);
    for (auto id : compacted->ids()) rebuilt.insert(id);

    VectorStore fresh_store(f.dimension, DistanceKind::Cosine);
    for (std::size_t i = 0; i < f.rows.size(); ++i)
        if (!f.deleted.contains(hnsw_test::fixture_id(i)))
            static_cast<void>(fresh_store.insert(hnsw_test::fixture_id(i), f.rows[i]));
    HnswIndex fresh(fresh_store, f.config);
    for (auto id : fresh_store.ids()) fresh.insert(id);

    require(compacted->size() == fresh_store.size() && compacted->deleted_count() == 0,
            "Compacted store size differs.");
    for (std::size_t i = 0; i < fresh_store.size(); ++i)
        require(compacted->ids()[i] == fresh_store.ids()[i] &&
                    std::memcmp(compacted->data_by_position(i), fresh_store.data_by_position(i),
                                fresh_store.padded_dimension() * sizeof(float)) == 0,
                "Compacted rows must be bit-identical to a fresh store.");
    for (const auto& query : f.queries)
        require_same(rebuilt.search(query, 10, 40), fresh.search(query, 10, 40),
                     "A graph rebuilt after compaction must equal a fresh build.");
}

void reinsert_and_snapshot() {
    Fixture f;
    VectorStore store(f.dimension, DistanceKind::SquaredEuclidean);
    hnsw_test::populate(store, f.rows);
    HnswIndex index(store, f.config);
    for (auto id : store.ids()) index.insert(id);
    const VectorId target = hnsw_test::fixture_id(3);
    require(store.erase(target), "erase failed.");
    require(!index.contains(target), "A deleted ID must not be reported as indexed.");
    // Insert a new version far from everything and query exactly there.
    std::vector<float> replacement(f.dimension, 50.0f);
    require(store.insert(target, replacement) == InsertStatus::Success,
            "A deleted ID must be insertable again.");
    index.insert(target);
    const auto hits = index.search(replacement, 3, 40);
    require(!hits.empty() && hits.front().id == target && hits.front().distance == 0.0f,
            "The new version must be found.");
    require(std::count_if(hits.begin(), hits.end(),
                          [&](const SearchResult& hit) { return hit.id == target; }) == 1,
            "The old version of an updated ID must not be returned.");

    for (auto id : f.deleted)
        if (id != target) store.erase(id);
    const auto dir = std::filesystem::temp_directory_path() / "aether_vector_delete_test";
    std::filesystem::create_directories(dir);
    snapshot::save(store, dir / "vectors.sec");
    snapshot::save(index, dir / "hnsw.sec");
    const auto loaded_store = snapshot::load_vector_store(dir / "vectors.sec", true);
    const auto loaded = snapshot::load_hnsw(dir / "hnsw.sec", *loaded_store, true);
    require(loaded_store->deleted_count() == store.deleted_count() &&
                loaded_store->live_size() == store.live_size() &&
                loaded_store->position_of(target) == store.position_of(target),
            "Tombstones were not restored.");
    for (const auto& query : f.queries)
        require_same(loaded->search(query, 10, 40), index.search(query, 10, 40),
                     "A loaded index with deletes must answer identically.");
    std::filesystem::remove_all(dir);
}

}  // namespace

int main() {
    try {
        deleted_vectors_never_returned();
        compaction_equals_fresh_build();
        reinsert_and_snapshot();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Vector delete tests passed.\n";
    return 0;
}
