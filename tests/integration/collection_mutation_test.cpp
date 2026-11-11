// Collection mutations end to end: after deletes, additions and replacements, a collection
// never returns deleted records, survives a save/open unchanged, and after compact()
// answers exactly (bitwise) like a collection built from scratch over the live records.

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../test_helpers.hpp"
#include "aether/application/collection.hpp"
#include "aether/application/json_loader.hpp"

namespace {

using namespace aetherindex;
namespace fs = std::filesystem;

fs::path work_dir;
constexpr std::size_t kDimension = 16;

struct Record {
    std::uint32_t id;
    std::string text;
    std::vector<float> embedding;
    std::string category;
};

const std::vector<std::string> kWords = {"motor", "bomba", "sensor", "falla", "presion",
                                         "valvula", "turbina", "aceite", "filtro", "eje",
                                         "sello", "fuga", "caudal", "nivel", "alarma"};

Record make_record(std::uint32_t id, std::mt19937& random) {
    Record record{id, "", std::vector<float>(kDimension), id % 3 == 0 ? "a" : "b"};
    for (int w = 0; w < 2 + static_cast<int>(random() % 15); ++w)
        record.text += kWords[std::min(random() % kWords.size(), random() % kWords.size())] + " ";
    for (auto& value : record.embedding)
        value = static_cast<float>(static_cast<int>(random() % 2001) - 1000) / 1000.0f;
    record.embedding[0] += 0.01f;  // Never all zero.
    return record;
}

void write_jsonl(const fs::path& path, const std::vector<Record>& records) {
    std::ofstream output(path);
    output.precision(9);
    for (const auto& record : records) {
        output << "{\"document_id\": \"" << record.id << "\", \"title\": \"\", \"text\": \""
               << record.text << "\", \"metadata\": {\"category\": \"" << record.category
               << "\"}, \"embedding\": [";
        for (std::size_t d = 0; d < record.embedding.size(); ++d)
            output << (d ? ", " : "") << record.embedding[d];
        output << "]}\n";
    }
}

void write_csv(const fs::path& path, const std::vector<Record>& records) {
    std::ofstream output(path);
    output.precision(9);
    for (const auto& record : records) {
        output << record.id;
        for (float value : record.embedding) output << ',' << value;
        output << '\n';
    }
}

void write_queries(const fs::path& path, std::mt19937& random) {
    std::ofstream output(path);
    output.precision(9);
    const std::vector<std::string> texts = {"motor bomba", "falla falla sensor", "aceite filtro",
                                            "nivel alarma caudal", "eje", "nada que buscar"};
    for (std::size_t q = 0; q < texts.size(); ++q) {
        output << "{\"query_id\": \"Q" << q << "\", \"query_text\": \"" << texts[q]
               << "\", \"embedding\": [";
        for (std::size_t d = 0; d < kDimension; ++d)
            output << (d ? ", " : "")
                   << static_cast<float>(static_cast<int>(random() % 2001) - 1000) / 1000.0f;
        output << "]}\n";
    }
}

SearchRequest base_request(SearchMode mode, const fs::path& dataset) {
    SearchRequest request;
    request.mode = mode;
    request.dataset_path = dataset;
    request.algorithm = SearchAlgorithm::Hnsw;
    request.hnsw_config.m = 8;
    request.hnsw_config.m_max_0 = 16;
    request.hnsw_config.ef_construction = 40;
    request.bm25_config.analyzer = lexical::Analyzer::AsciiTechV1;
    return request;
}

std::unique_ptr<Collection> build(const SearchRequest& request) {
    validate_build_request(request);
    auto collection = Collection::load(request);
    collection->build_indexes();
    return collection;
}

bool jsonl_dataset(const Collection& collection) {
    return collection.configuration().dataset_path.extension() == ".jsonl";
}

// One printable line per result, scores in hexadecimal so equality is bitwise.
std::string run(const Collection& collection, SearchMode mode, const fs::path& queries,
                const std::vector<FilterClause>& filters = {}) {
    SearchRequest request = collection.merged_request({});
    request.k = 12;
    request.filters = filters;
    const bool csv_query = mode == SearchMode::Vector && !jsonl_dataset(collection);
    request.all_queries = !csv_query;  // CSV vector queries are single queries.
    request.query_path = csv_query ? queries.parent_path() / "query.csv" : queries;
    validate_search_request(request);
    auto loaded = collection.load_queries(request);
    auto response = collection.search(request, loaded);
    if (csv_query) response.batch = {response};
    std::ostringstream out;
    out << std::hexfloat;
    for (const auto& query : response.batch) {
        out << query.selected_query_id << ':';
        for (const auto& hit : query.fused)
            out << ' ' << hit.source_id << '=' << hit.rrf_score << '/' << hit.dense_rank << '/'
                << hit.lexical_rank;
        for (const auto& hit : query.documents) out << ' ' << hit.source_id << '=' << hit.score;
        for (std::size_t i = 0; i < query.neighbors.size(); ++i)
            out << ' '
                << (query.neighbor_source_ids.empty() ? std::to_string(query.neighbors[i].id)
                                                      : query.neighbor_source_ids[i])
                << '=' << query.neighbors[i].distance;
        out << '\n';
    }
    return out.str();
}

void require_absent(const std::string& results, const std::set<std::uint32_t>& deleted) {
    std::istringstream lines(results);
    for (std::string token; lines >> token;) {
        const auto equals = token.find('=');
        if (equals == std::string::npos) continue;
        const auto id = std::stoul(token.substr(0, equals));
        require(!deleted.contains(static_cast<std::uint32_t>(id)), "A deleted record was returned.");
    }
}

void scenario(SearchMode mode, bool csv) {
    const auto dir = work_dir / (std::string(mode == SearchMode::Hybrid    ? "hybrid"
                                             : mode == SearchMode::Lexical ? "lexical"
                                                                           : "vector") +
                                 (csv ? "-csv" : ""));
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::mt19937 random(77);
    std::vector<Record> records;
    for (std::uint32_t id = 1; id <= 300; ++id) records.push_back(make_record(id, random));
    const auto extension = csv ? ".csv" : ".jsonl";
    const auto write = [&](const fs::path& path, const std::vector<Record>& rows) {
        csv ? write_csv(path, rows) : write_jsonl(path, rows);
    };
    write(dir / (std::string("initial") + extension), records);
    write_queries(dir / "queries.jsonl", random);
    {
        std::ofstream query(dir / "query.csv");
        query.precision(9);
        for (std::size_t d = 0; d < kDimension; ++d)
            query << (d ? "," : "") << (0.3f - 0.05f * static_cast<float>(d));
        query << '\n';
    }

    auto request = base_request(mode, dir / (std::string("initial") + extension));
    auto collection = build(request);

    // Deletes, then one batch with new records and replacements of existing ones.
    std::set<std::uint32_t> deleted;
    for (std::uint32_t id = 4; id <= 300; id += 9) deleted.insert(id);
    std::vector<std::string> ids;
    for (auto id : deleted) ids.push_back(std::to_string(id));
    require(collection->remove(ids) == ids.size(), "remove count");
    std::vector<Record> batch;
    for (std::uint32_t id = 301; id <= 330; ++id) batch.push_back(make_record(id, random));
    std::set<std::uint32_t> replaced = {10, 11, 12, 100};
    for (auto id : replaced) batch.push_back(make_record(id, random));
    write(dir / (std::string("batch") + extension), batch);
    bool rejected = false;
    try {
        collection->add(dir / (std::string("batch") + extension), {}, false);
    } catch (const InputDataError&) {
        rejected = true;
    }
    require(rejected, "Existing IDs need --replace.");
    const auto added = collection->add(dir / (std::string("batch") + extension), {}, true);
    require(added.added == 30 && added.replaced == replaced.size(), "add counts");

    const auto before = run(*collection, mode, dir / "queries.jsonl");
    require_absent(before, deleted);
    if (!csv)
        require_absent(run(*collection, mode, dir / "queries.jsonl",
                           {parse_equality_filter("category=a")}),
                       deleted);

    // Save and open: identical answers with deletes and several BM25 segments.
    collection->save(dir / "index");
    const auto reopened = Collection::open(dir / "index");
    require(run(*reopened, mode, dir / "queries.jsonl") == before,
            "A reopened collection with deletions must answer identically.");

    // Compaction equals a fresh build over the live records in insertion order.
    collection->compact();
    require(collection->deleted_count() == 0, "compaction left deleted records");
    std::vector<Record> live;
    for (const auto& record : records)
        if (!deleted.contains(record.id) && !replaced.contains(record.id)) live.push_back(record);
    live.insert(live.end(), batch.begin(), batch.end());
    write(dir / (std::string("live") + extension), live);
    const auto fresh = build(base_request(mode, dir / (std::string("live") + extension)));
    require(run(*collection, mode, dir / "queries.jsonl") ==
                run(*fresh, mode, dir / "queries.jsonl"),
            "A compacted collection must answer exactly like a fresh build.");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: aether_collection_mutation_tests WORK_DIR\n";
        return 2;
    }
    work_dir = argv[1];
    try {
        scenario(SearchMode::Hybrid, false);
        scenario(SearchMode::Lexical, false);
        scenario(SearchMode::Vector, false);
        scenario(SearchMode::Vector, true);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Collection mutation tests passed.\n";
    return 0;
}
