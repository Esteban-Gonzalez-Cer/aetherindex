// Component snapshots: a loaded component must answer exactly like the saved one, keep
// evolving exactly like it (graph RNG state), save deterministically, and reject damaged
// or mismatched files before using them.

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "aether/core/prepared_query.hpp"
#include "aether/io/binary_io.hpp"
#include "aether/persistence/snapshot.hpp"
#include "aether/quantization/pq.hpp"
#include "aether/quantization/sq8.hpp"
#include "../hnsw_test_helpers.hpp"

namespace {

using namespace aetherindex;
namespace fs = std::filesystem;

fs::path work_dir;

std::string bytes_of(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

void write_bytes(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

template <class Function>
void require_io_error(Function function, const char* message) {
    try {
        function();
    } catch (const io::IoError&) {
        return;
    }
    throw std::runtime_error(message);
}

void require_same_results(const std::vector<SearchResult>& a, const std::vector<SearchResult>& b,
                          const char* message) {
    require(a.size() == b.size(), message);
    for (std::size_t i = 0; i < a.size(); ++i)
        require(a[i].id == b[i].id &&
                    std::memcmp(&a[i].distance, &b[i].distance, sizeof(float)) == 0,
                message);
}

void require_same_ranking(const std::vector<RankedResult>& a, const std::vector<RankedResult>& b,
                          const char* message) {
    require(a.size() == b.size(), message);
    for (std::size_t i = 0; i < a.size(); ++i)
        require(a[i].document_id == b[i].document_id &&
                    std::memcmp(&a[i].score, &b[i].score, sizeof(double)) == 0,
                message);
}

std::unique_ptr<VectorStore> make_store(std::size_t count, std::size_t dimension,
                                        DistanceKind metric, std::uint32_t seed) {
    auto store = std::make_unique<VectorStore>(dimension, metric);
    hnsw_test::populate(*store, hnsw_test::embeddings(count, dimension, seed));
    return store;
}

void vector_store_round_trip() {
    for (auto metric : {DistanceKind::Cosine, DistanceKind::SquaredEuclidean}) {
        const auto store = make_store(300, 37, metric, 11);
        const auto path = work_dir / "vectors.sec";
        snapshot::save(*store, path);
        const auto loaded = snapshot::load_vector_store(path, true);
        require(loaded->size() == store->size() && loaded->dimension() == store->dimension() &&
                    loaded->distance_kind() == metric &&
                    loaded->padded_dimension() == store->padded_dimension(),
                "Loaded store shape differs.");
        for (std::size_t i = 0; i < store->size(); ++i) {
            require(loaded->ids()[i] == store->ids()[i], "Loaded store ID differs.");
            require(loaded->position_of(store->ids()[i]) == i, "Loaded position differs.");
            require(std::memcmp(loaded->data_by_position(i), store->data_by_position(i),
                                store->padded_dimension() * sizeof(float)) == 0,
                    "Loaded row differs.");
        }
        snapshot::save(*loaded, work_dir / "vectors-again.sec");
        require(bytes_of(path) == bytes_of(work_dir / "vectors-again.sec"),
                "Saving a loaded store must reproduce the same bytes.");
    }
}

void hnsw_round_trip() {
    const std::size_t dimension = 24;
    auto store = make_store(1500, dimension, DistanceKind::Cosine, 21);
    HnswConfig config;
    config.m = 8;
    config.m_max_0 = 16;
    config.ef_construction = 60;
    config.random_seed = 99;
    HnswIndex index(*store, config);
    // Index the first 1,000 vectors; the rest test that insertion continues identically.
    for (std::size_t i = 0; i < 1000; ++i) index.insert(store->ids()[i]);

    snapshot::save(*store, work_dir / "hnsw-vectors.sec");
    snapshot::save(index, work_dir / "hnsw.sec");
    auto loaded_store = snapshot::load_vector_store(work_dir / "hnsw-vectors.sec", true);
    auto loaded = snapshot::load_hnsw(work_dir / "hnsw.sec", *loaded_store, true);
    require(loaded->validate_graph().valid(), "Loaded graph is invalid.");
    require(loaded->size() == index.size() && loaded->entry_point() == index.entry_point() &&
                loaded->max_level() == index.max_level(),
            "Loaded graph summary differs.");

    const auto queries = hnsw_test::embeddings(60, dimension, 5);
    for (const auto& query : queries)
        require_same_results(loaded->search(query, 10, 40), index.search(query, 10, 40),
                             "Loaded HNSW results differ.");

    for (std::size_t i = 1000; i < store->size(); ++i) {
        index.insert(store->ids()[i]);
        loaded->insert(loaded_store->ids()[i]);
    }
    for (std::size_t i = 0; i < store->size(); ++i) {
        const auto id = store->ids()[i];
        const auto node = index.find_node(id);
        require(node && loaded->find_node(id) && loaded->find_node(id)->max_level == node->max_level,
                "Levels diverge after insertions on a loaded graph.");
        for (std::size_t level = 0; level <= node->max_level; ++level) {
            const auto a = index.neighbors(id, level);
            const auto b = loaded->neighbors(id, level);
            require(std::equal(a.begin(), a.end(), b.begin(), b.end()),
                    "Topology diverges after insertions on a loaded graph.");
        }
    }

    // ef_search override and mismatched stores.
    const auto overridden = snapshot::load_hnsw(work_dir / "hnsw.sec", *loaded_store, false, 7);
    require(overridden->config().ef_search == 7, "ef_search override was ignored.");
    const auto other = make_store(1500, dimension, DistanceKind::Cosine, 22);
    auto shifted = std::make_unique<VectorStore>(dimension, DistanceKind::Cosine);
    for (std::size_t i = 0; i < 1500; ++i)
        static_cast<void>(shifted->insert(static_cast<VectorId>(i + 1),
                                          *other->find(other->ids()[i])));
    require_io_error([&] { snapshot::load_hnsw(work_dir / "hnsw.sec", *shifted, true); },
                     "A graph loaded on different vector IDs must be rejected.");
}

void bm25_round_trip() {
    using lexical::Bm25Strategy;
    std::vector<std::string> texts;
    std::mt19937 random(3);
    const std::vector<std::string> words = {"motor", "bomba", "sensor", "falla", "presion",
                                            "valvula", "e47", "turbina", "aceite", "filtro",
                                            "temperatura", "vibracion", "rodamiento"};
    for (int d = 0; d < 700; ++d) {
        std::string text;
        const int length = 1 + static_cast<int>(random() % 40);
        for (int w = 0; w < length; ++w) text += words[random() % words.size()] + " ";
        texts.push_back(text);
    }
    const std::vector<std::string> queries = {"motor bomba", "falla falla sensor", "e47",
                                              "aceite filtro turbina presion valvula",
                                              "inexistente", ""};
    for (auto strategy : {Bm25Strategy::Exhaustive, Bm25Strategy::TermAtATime, Bm25Strategy::Wand,
                          Bm25Strategy::BlockMaxWand, Bm25Strategy::MaxScore}) {
        lexical::Bm25Config config;
        config.strategy = strategy;
        config.k1 = 0.9;
        config.b = 0.4;
        lexical::Bm25Index index(config);
        for (std::size_t d = 0; d < texts.size(); ++d)
            index.add_document(static_cast<DocumentId>(d * 3 + 5), texts[d]);
        index.build();
        const auto path = work_dir / "bm25.sec";
        snapshot::save(index, path);
        const auto loaded = snapshot::load_bm25(path, true);
        require(loaded->document_count() == index.document_count() &&
                    loaded->average_document_length() == index.average_document_length(),
                "Loaded BM25 statistics differ.");
        for (const auto& query : queries)
            for (std::size_t k : {1, 5, 50, 1000})
                require_same_ranking(loaded->search(query, k), index.search(query, k),
                                     "Loaded BM25 results differ.");
        snapshot::save(*loaded, work_dir / "bm25-again.sec");
        require(bytes_of(path) == bytes_of(work_dir / "bm25-again.sec"),
                "Saving a loaded BM25 index must reproduce the same bytes.");
        if (strategy == Bm25Strategy::Exhaustive) {
            bool rejected = false;
            try {
                snapshot::load_bm25(path, true, {Bm25Strategy::MaxScore, std::nullopt});
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            require(rejected, "A compressed strategy needs an index with compressed postings.");
        } else {
            const auto other = snapshot::load_bm25(
                path, false, {Bm25Strategy::Wand, lexical::QueryTermWeighting::Unique});
            require(other->config().strategy == Bm25Strategy::Wand &&
                        other->config().query_weighting == lexical::QueryTermWeighting::Unique,
                    "BM25 query overrides were ignored.");
            lexical::Bm25Config unique = config;
            unique.query_weighting = lexical::QueryTermWeighting::Unique;
            lexical::Bm25Index reference(unique);
            for (std::size_t d = 0; d < texts.size(); ++d)
                reference.add_document(static_cast<DocumentId>(d * 3 + 5), texts[d]);
            reference.build();
            for (const auto& query : queries)
                require_same_ranking(other->search(query, 10), reference.search(query, 10),
                                     "Overridden BM25 results differ from a fresh index.");
        }
    }
}

void segmented_bm25_round_trip() {
    for (auto strategy : {lexical::Bm25Strategy::MaxScore, lexical::Bm25Strategy::Exhaustive}) {
        lexical::Bm25Config config;
        config.strategy = strategy;
        lexical::SegmentedBm25 index(config);
        std::mt19937 random(13);
        const std::vector<std::string> words = {"motor", "bomba", "sensor", "falla", "e47",
                                                "turbina", "aceite", "filtro", "eje"};
        DocumentId next = 1;
        for (int batch = 0; batch < 5; ++batch) {
            std::vector<std::pair<DocumentId, std::string>> documents;
            for (int d = 0; d < 80; ++d) {
                std::string text;
                for (int w = 0; w < 1 + static_cast<int>(random() % 12); ++w)
                    text += words[random() % words.size()] + " ";
                documents.emplace_back(next++, text);
            }
            index.add(documents);
        }
        for (DocumentId id = 3; id < next; id += 7) index.remove(id);
        const auto path = work_dir / "segments.sec";
        snapshot::save(index, path);
        const auto loaded = snapshot::load_segmented_bm25(path, true);
        require(loaded->segment_count() == index.segment_count() &&
                    loaded->deleted_count() == index.deleted_count(),
                "Loaded segment structure differs.");
        for (const char* query : {"motor bomba", "e47 e47 falla", "eje", "nada"})
            for (std::size_t k : {1, 10, 500})
                require_same_ranking(loaded->search(query, k), index.search(query, k),
                                     "Loaded segmented BM25 results differ.");
        snapshot::save(*loaded, work_dir / "segments-again.sec");
        require(bytes_of(path) == bytes_of(work_dir / "segments-again.sec"),
                "Saving a loaded segmented index must reproduce the same bytes.");
    }
}

void quantized_round_trip() {
    const std::size_t dimension = 32;
    auto store = make_store(800, dimension, DistanceKind::SquaredEuclidean, 31);
    HnswIndex index(*store, {});
    for (auto id : store->ids()) index.insert(id);
    const Sq8Vectors sq8(*store);
    PqConfig pq_config;
    pq_config.subspaces = 8;
    const PqVectors pq(*store, pq_config);
    const auto queries = hnsw_test::embeddings(30, dimension, 8);
    for (const QuantizedVectors* codes : {static_cast<const QuantizedVectors*>(&sq8),
                                          static_cast<const QuantizedVectors*>(&pq)}) {
        const auto path = work_dir / "codes.sec";
        snapshot::save(*codes, path);
        const auto loaded = snapshot::load_quantized(path, true);
        require(std::string(loaded->name()) == codes->name() && loaded->size() == codes->size(),
                "Loaded codes differ.");
        for (const auto& query : queries) {
            const PreparedQuery prepared(query, dimension, DistanceKind::SquaredEuclidean);
            for (std::size_t rerank : {0, 20})
                require_same_results(index.search_quantized(prepared, *loaded, 10, 40, rerank),
                                     index.search_quantized(prepared, *codes, 10, 40, rerank),
                                     "Loaded quantized search differs.");
        }
    }
}

void damaged_files_are_rejected() {
    const auto store = make_store(200, 16, DistanceKind::Cosine, 41);
    const auto path = work_dir / "damaged.sec";
    snapshot::save(*store, path);
    const auto original = bytes_of(path);

    auto flipped = original;
    flipped[original.size() - 5] ^= 0x40;
    write_bytes(path, flipped);
    require_io_error([&] { snapshot::load_vector_store(path, true); },
                     "A flipped payload byte must fail checksum verification.");

    write_bytes(path, original.substr(0, original.size() - 64));
    require_io_error([&] { snapshot::load_vector_store(path, false); },
                     "A truncated file must be rejected.");

    auto header = original;
    header[20] ^= 1;  // Payload offset, covered by the header checksum.
    write_bytes(path, header);
    require_io_error([&] { snapshot::load_vector_store(path, false); },
                     "A damaged header must be rejected.");

    write_bytes(path, original);
    require_io_error([&] { snapshot::load_bm25(path, false); },
                     "A section of another kind must be rejected.");
    require_io_error([&] { snapshot::load_vector_store(work_dir / "missing.sec", false); },
                     "A missing file must be rejected.");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: aether_snapshot_tests WORK_DIR\n";
        return 2;
    }
    work_dir = argv[1];
    fs::create_directories(work_dir);
    try {
        vector_store_round_trip();
        hnsw_round_trip();
        bm25_round_trip();
        segmented_bm25_round_trip();
        quantized_round_trip();
        damaged_files_are_rejected();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Snapshot tests passed.\n";
    return 0;
}
