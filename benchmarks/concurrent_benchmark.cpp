// Concurrent reads and writes on a Collection (HNSW, vector mode).
//
//   aether_concurrent_benchmark WORK_DIR [vectors=200000] [dimension=64] [threads=10]
//
// 1. Lock cost: QPS of HnswIndex::search on `threads` threads, with and without taking a
//    shared std::shared_mutex around each search (what Collection::search adds).
// 2. Read QPS through Collection::search with no writer, then while one writer thread adds
//    batches of 1,000 vectors as fast as it can; reports the writer's insert throughput.
// Synthetic data (seeded): clustered Gaussian vectors, squared Euclidean.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <random>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "aether/ann/hnsw/hnsw_index.hpp"
#include "aether/application/collection.hpp"
#include "aether/application/json_loader.hpp"
#include "aether/storage/vector_file.hpp"

namespace {

using namespace aetherindex;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

std::vector<float> generate(std::size_t count, std::size_t dimension, std::uint64_t seed) {
    std::mt19937_64 random(seed);
    std::normal_distribution<float> noise(0.0f, 1.0f);
    std::vector<float> centers(64 * dimension);
    for (auto& value : centers) value = noise(random) * 10.0f;
    std::vector<float> rows(count * dimension);
    for (std::size_t i = 0; i < count; ++i) {
        const float* center = centers.data() + (random() % 64) * dimension;
        for (std::size_t d = 0; d < dimension; ++d)
            rows[i * dimension + d] = center[d] + noise(random);
    }
    return rows;
}

void write_avec(const fs::path& path, std::uint32_t first_id, const std::vector<float>& rows,
                std::size_t dimension) {
    std::vector<VectorId> ids(rows.size() / dimension);
    std::iota(ids.begin(), ids.end(), first_id);
    write_vector_file(path, ids, rows, dimension);
}

// Runs body(thread, iteration) on `threads` threads for `seconds`; returns calls/second.
template <class Body>
double throughput(std::size_t threads, double seconds, Body body) {
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> calls{0};
    std::vector<std::thread> workers;
    const auto start = Clock::now();
    for (std::size_t t = 0; t < threads; ++t)
        workers.emplace_back([&, t] {
            std::size_t local = 0;
            while (!stop.load(std::memory_order_relaxed)) body(t, local++);
            calls += local;
        });
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    stop = true;
    for (auto& worker : workers) worker.join();
    return static_cast<double>(calls.load()) /
           std::chrono::duration<double>(Clock::now() - start).count();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: aether_concurrent_benchmark WORK_DIR [vectors] [dimension] [threads]\n";
        return 2;
    }
    const fs::path dir = argv[1];
    const std::size_t count = argc > 2 ? std::stoul(argv[2]) : 200000;
    const std::size_t dimension = argc > 3 ? std::stoul(argv[3]) : 64;
    const std::size_t threads = argc > 4 ? std::stoul(argv[4]) : 10;
    fs::create_directories(dir);
    write_avec(dir / "base.avec", 0, generate(count, dimension, 1), dimension);
    const auto queries = generate(1000, dimension, 2);
    constexpr std::size_t kBatch = 1000, kBatches = 150;
    for (std::size_t b = 0; b < kBatches; ++b)
        write_avec(dir / ("batch" + std::to_string(b) + ".avec"),
                   static_cast<std::uint32_t>(count + b * kBatch),
                   generate(kBatch, dimension, 100 + b), dimension);

    SearchRequest build;
    build.algorithm = SearchAlgorithm::Hnsw;
    build.metric = DistanceKind::SquaredEuclidean;
    build.dataset_path = dir / "base.avec";
    build.hnsw_config.ef_construction = 100;
    build.hnsw_config.ef_search = 64;
    build.build_threads = threads;
    validate_build_request(build);
    auto collection = Collection::load(build);
    collection->build_indexes();
    std::cout << "Collection: " << count << " x " << dimension << ", " << threads
              << " threads, ef_search 64, k 10\n";

    // 1. The cost of the shared lock alone, on a plain HnswIndex.
    {
        auto store = read_vector_file(dir / "base.avec", DistanceKind::SquaredEuclidean);
        HnswIndex index(*store, build.hnsw_config);
        index.insert_parallel(store->ids(), threads);
        std::shared_mutex mutex;
        const auto query = [&](std::size_t i) {
            return std::span<const float>(queries.data() + (i % 1000) * dimension, dimension);
        };
        const double plain = throughput(threads, 3.0, [&](std::size_t t, std::size_t i) {
            static_cast<void>(index.search(query(t * 7919 + i), 10, 64));
        });
        const double locked = throughput(threads, 3.0, [&](std::size_t t, std::size_t i) {
            const std::shared_lock lock(mutex);
            static_cast<void>(index.search(query(t * 7919 + i), 10, 64));
        });
        index.begin_concurrent_reads();
        const double atomic_reads = throughput(threads, 3.0, [&](std::size_t t, std::size_t i) {
            static_cast<void>(index.search(query(t * 7919 + i), 10, 64));
        });
        index.end_concurrent_reads();
        std::cout << "HnswIndex::search QPS: " << plain << " plain, " << locked
                  << " with a shared lock (" << (locked / plain - 1.0) * 100.0 << "%), "
                  << atomic_reads << " with lock-free concurrent reads ("
                  << (atomic_reads / plain - 1.0) * 100.0 << "%)\n";
    }

    // 2. Collection reads without and with a concurrent writer.
    std::vector<QueryRecord> records(1000);
    for (std::size_t q = 0; q < 1000; ++q)
        records[q].vector.assign(queries.begin() + static_cast<std::ptrdiff_t>(q * dimension),
                                 queries.begin() + static_cast<std::ptrdiff_t>((q + 1) * dimension));
    SearchRequest request = collection->merged_request({});
    request.k = 10;
    const auto read = [&](std::size_t t, std::size_t i) {
        std::vector<QueryRecord> one = {records[(t * 7919 + i) % 1000]};
        static_cast<void>(collection->search(request, one));
    };
    const double idle = throughput(threads, 3.0, read);
    // Readers run exactly as long as the writer: QPS is measured only while it writes.
    std::atomic<bool> writing{true};
    std::atomic<std::size_t> reads{0};
    std::vector<std::thread> readers;
    for (std::size_t t = 0; t + 1 < threads; ++t)
        readers.emplace_back([&, t] {
            std::size_t local = 0;
            while (writing.load(std::memory_order_relaxed)) read(t, local++);
            reads += local;
        });
    std::size_t inserted = 0;
    const auto start = Clock::now();
    for (std::size_t b = 0; b < kBatches; ++b) {
        collection->add(dir / ("batch" + std::to_string(b) + ".avec"), {}, false);
        inserted += kBatch;
    }
    const double write_seconds = std::chrono::duration<double>(Clock::now() - start).count();
    writing = false;
    for (auto& reader : readers) reader.join();
    const double busy = static_cast<double>(reads.load()) / write_seconds;
    std::cout << "Collection::search QPS: " << idle << " with no writer (" << threads
              << " readers); " << busy << " with " << threads - 1
              << " readers while one writer inserts\n"
              << "Writer: " << inserted << " vectors in " << write_seconds << " s ("
              << static_cast<double>(inserted) / write_seconds << " inserts/s, one thread, "
              << "searches running)\n";
    return 0;
}
