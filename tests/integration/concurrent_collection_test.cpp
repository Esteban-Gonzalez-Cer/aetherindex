// Concurrent use of a Collection: reader threads search continuously (hybrid queries,
// some filtered) while one writer adds, deletes, replaces, saves and compacts. Run under
// ThreadSanitizer this checks the locking protocol and the lock-free HNSW reads. Every
// returned record must be one that existed; after the writer stops, the graph must be
// structurally valid and the compacted collection must answer exactly like a fresh build.

#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../test_helpers.hpp"
#include "aether/application/collection.hpp"
#include "aether/application/json_loader.hpp"

namespace {

using namespace aetherindex;
namespace fs = std::filesystem;

constexpr std::size_t kDimension = 16;
const std::vector<std::string> kWords = {"motor", "bomba", "sensor", "falla", "presion",
                                         "valvula", "turbina", "aceite", "filtro", "eje",
                                         "sello", "fuga", "caudal", "nivel", "alarma"};

struct Record {
    std::uint32_t id;
    std::string text;
    std::vector<float> embedding;
};

Record make_record(std::uint32_t id, std::mt19937& random) {
    Record record{id, "", std::vector<float>(kDimension)};
    for (int w = 0; w < 2 + static_cast<int>(random() % 12); ++w)
        record.text += kWords[std::min(random() % kWords.size(), random() % kWords.size())] + " ";
    for (auto& value : record.embedding)
        value = static_cast<float>(static_cast<int>(random() % 2001) - 1000) / 1000.0f;
    record.embedding[0] += 0.01f;
    return record;
}

void write_jsonl(const fs::path& path, const std::vector<Record>& records) {
    std::ofstream output(path);
    output.precision(9);
    for (const auto& record : records) {
        output << "{\"document_id\": \"" << record.id << "\", \"text\": \"" << record.text
               << "\", \"metadata\": {\"parity\": \"" << (record.id % 2 ? "odd" : "even")
               << "\"}, \"embedding\": [";
        for (std::size_t d = 0; d < kDimension; ++d)
            output << (d ? ", " : "") << record.embedding[d];
        output << "]}\n";
    }
}

std::unique_ptr<Collection> build(const fs::path& dataset) {
    SearchRequest request;
    request.mode = SearchMode::Hybrid;
    request.dataset_path = dataset;
    request.hnsw_config.m = 8;
    request.hnsw_config.m_max_0 = 16;
    request.hnsw_config.ef_construction = 40;
    validate_build_request(request);
    auto collection = Collection::load(request);
    collection->build_indexes();
    return collection;
}

SearchRequest query_request(const Collection& collection, const fs::path& queries,
                            bool filtered) {
    SearchRequest request = collection.merged_request({});
    request.query_path = queries;
    request.all_queries = true;
    request.k = 10;
    if (filtered) request.filters = {parse_equality_filter("parity=even")};
    validate_search_request(request);
    return request;
}

std::string answers(const Collection& collection, const fs::path& queries) {
    const auto request = query_request(collection, queries, false);
    auto loaded = collection.load_queries(request);
    const auto response = collection.search(request, loaded);
    std::ostringstream out;
    out << std::hexfloat;
    for (const auto& query : response.batch) {
        for (const auto& hit : query.fused)
            out << hit.source_id << '=' << hit.rrf_score << '/' << hit.dense_rank << '/'
                << hit.lexical_rank << ' ';
        out << '\n';
    }
    return out.str();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: aether_concurrent_collection_tests WORK_DIR\n";
        return 2;
    }
    const fs::path dir = argv[1];
    fs::remove_all(dir);
    fs::create_directories(dir);
    try {
        std::mt19937 random(5);
        std::vector<Record> records;
        for (std::uint32_t id = 1; id <= 1500; ++id) records.push_back(make_record(id, random));
        write_jsonl(dir / "initial.jsonl", records);
        {
            std::ofstream queries(dir / "queries.jsonl");
            queries.precision(9);
            for (int q = 0; q < 8; ++q) {
                queries << "{\"query_id\": \"Q" << q << "\", \"query_text\": \""
                        << kWords[q] << ' ' << kWords[q + 3] << "\", \"embedding\": [";
                for (std::size_t d = 0; d < kDimension; ++d)
                    queries << (d ? ", " : "")
                            << static_cast<float>(static_cast<int>(random() % 2001) - 1000) /
                                   1000.0f;
                queries << "]}\n";
            }
        }
        auto collection = build(dir / "initial.jsonl");

        // Every ID that ever exists, and the live ones in insertion (position) order.
        std::set<std::uint32_t> universe;
        std::vector<Record> live = records;
        for (const auto& record : records) universe.insert(record.id);
        std::vector<std::vector<Record>> batches;
        std::uint32_t next = 2001;
        for (int b = 0; b < 8; ++b) {
            std::vector<Record> batch;
            for (int i = 0; i < 60; ++i) batch.push_back(make_record(next++, random));
            for (const auto& record : batch) universe.insert(record.id);
            write_jsonl(dir / ("batch" + std::to_string(b) + ".jsonl"), batch);
            batches.push_back(std::move(batch));
        }

        std::atomic<bool> stop{false};
        std::atomic<std::size_t> searches{0};
        std::atomic<bool> failed{false};
        std::string failure;
        std::mutex failure_mutex;
        const auto reader = [&](int seed) {
            try {
                std::size_t round = 0;
                while (!stop.load()) {
                    const bool filtered = (round++ + static_cast<std::size_t>(seed)) % 3 == 0;
                    const auto request = query_request(*collection, dir / "queries.jsonl", filtered);
                    auto queries = collection->load_queries(request);
                    const auto response = collection->search(request, queries);
                    for (const auto& query : response.batch)
                        for (const auto& hit : query.fused) {
                            const auto id = static_cast<std::uint32_t>(std::stoul(hit.source_id));
                            if (!universe.contains(id) || !std::isfinite(hit.rrf_score))
                                throw std::runtime_error("unknown or invalid record returned");
                            if (filtered && id % 2 != 0)
                                throw std::runtime_error("the filter let an odd record through");
                        }
                    ++searches;
                }
            } catch (const std::exception& error) {
                const std::lock_guard lock(failure_mutex);
                failure = error.what();
                failed = true;
            }
        };
        std::vector<std::thread> readers;
        for (int t = 0; t < 4; ++t) readers.emplace_back(reader, t);

        // The writer.
        for (std::size_t b = 0; b < batches.size(); ++b) {
            collection->add(dir / ("batch" + std::to_string(b) + ".jsonl"), {}, false);
            live.insert(live.end(), batches[b].begin(), batches[b].end());
            std::vector<std::string> doomed;
            for (int d = 0; d < 25; ++d) {
                const auto position = random() % live.size();
                doomed.push_back(std::to_string(live[position].id));
                live.erase(live.begin() + static_cast<std::ptrdiff_t>(position));
            }
            collection->remove(doomed);
            if (b % 3 == 2) {  // Updates: new versions of three live records.
                std::vector<Record> updates;
                for (int u = 0; u < 3; ++u) {
                    const auto position = random() % live.size();
                    updates.push_back(make_record(live[position].id, random));
                    live.erase(live.begin() + static_cast<std::ptrdiff_t>(position));
                }
                write_jsonl(dir / "updates.jsonl", updates);
                collection->add(dir / "updates.jsonl", {}, true);
                live.insert(live.end(), updates.begin(), updates.end());
            }
            if (b == 3) collection->save(dir / "index");
            if (b == 5) collection->compact();
        }
        stop = true;
        for (auto& thread : readers) thread.join();
        if (failed) throw std::runtime_error("reader: " + failure);
        require(searches.load() >= 4, "the readers did not run");

        std::string report;
        require(collection->validate(report), "the graph is invalid after concurrent writes");
        collection->compact();
        write_jsonl(dir / "live.jsonl", live);
        const auto fresh = build(dir / "live.jsonl");
        require(answers(*collection, dir / "queries.jsonl") ==
                    answers(*fresh, dir / "queries.jsonl"),
                "after concurrent writes, compaction must equal a fresh build");
        std::cout << "Concurrent collection test passed (" << searches.load()
                  << " searches during writes).\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
