// SegmentedBm25: additions, deletions, updates, merges and compaction must score every
// document exactly (bitwise) as an exhaustive BM25 index over the same corpus statistics,
// and never return a deleted document.

#include <cstring>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../test_helpers.hpp"
#include "aether/lexical/segmented_bm25.hpp"

namespace {

using namespace aetherindex;
using namespace aetherindex::lexical;

constexpr DocumentId kRetired = 3'000'000'000u;  // IDs for deleted versions in references.

const std::vector<std::string> kWords = {
    "motor", "bomba", "sensor", "falla", "presion", "valvula", "e47", "turbina", "aceite",
    "filtro", "temperatura", "vibracion", "rodamiento", "eje", "sello", "fuga", "corriente",
    "voltaje", "caudal", "nivel", "alarma", "reinicio", "manual", "turno", "planta"};

std::string random_text(std::mt19937& random) {
    std::string text;
    const int length = 1 + static_cast<int>(random() % 30);
    for (int w = 0; w < length; ++w) {
        // Skewed choice: low indexes are frequent, as in natural text.
        const auto a = random() % kWords.size(), b = random() % kWords.size();
        text += kWords[std::min(a, b)] + " ";
    }
    return text;
}

const std::vector<std::string> kQueries = {
    "motor bomba", "falla falla sensor", "e47", "aceite filtro turbina presion valvula",
    "planta turno manual reinicio alarma nivel caudal voltaje corriente fuga sello eje",
    "inexistente", "", "motor motor motor"};

void require_same(const std::vector<RankedResult>& actual,
                  const std::vector<RankedResult>& expected, const std::string& context) {
    if (actual.size() != expected.size())
        throw std::runtime_error(context + ": size " + std::to_string(actual.size()) + " vs " +
                                 std::to_string(expected.size()));
    for (std::size_t i = 0; i < actual.size(); ++i)
        if (actual[i].document_id != expected[i].document_id ||
            std::memcmp(&actual[i].score, &expected[i].score, sizeof(double)) != 0)
            throw std::runtime_error(context + ": rank " + std::to_string(i) + " differs");
}

// Every version ever indexed and not expunged, with its state.
struct Version {
    DocumentId id;
    std::string text;
    bool deleted;
};

// Exhaustive index over all versions (deleted ones under unused IDs, so they count in the
// statistics but can never be returned), searched with deleted versions excluded.
std::vector<RankedResult> reference(const Bm25Config& base, const std::vector<Version>& versions,
                                    std::string_view query, std::size_t k,
                                    const std::function<bool(DocumentId)>& allow) {
    Bm25Config config = base;
    config.strategy = Bm25Strategy::Exhaustive;
    Bm25Index index(config);
    DocumentId retired = kRetired;
    for (const auto& version : versions)
        index.add_document(version.deleted ? retired++ : version.id, version.text);
    index.build();
    return index.search(query, k, [&](DocumentId id) {
        return id < kRetired && (!allow || allow(id));
    });
}

void check(const SegmentedBm25& index, const std::vector<Version>& versions,
           const std::string& context) {
    const std::function<bool(DocumentId)> even = [](DocumentId id) { return id % 2 == 0; };
    for (const auto& query : kQueries)
        for (std::size_t k : {1, 3, 10, 1000}) {
            require_same(index.search(query, k), reference(index.config(), versions, query, k, {}),
                         context + " query '" + query + "' k=" + std::to_string(k));
            require_same(index.search(query, k, even),
                         reference(index.config(), versions, query, k, even),
                         context + " filtered query '" + query + "'");
        }
}

std::vector<Bm25Config> configs() {
    std::vector<Bm25Config> all;
    for (auto strategy : {Bm25Strategy::MaxScore, Bm25Strategy::TermAtATime,
                          Bm25Strategy::BlockMaxWand, Bm25Strategy::Exhaustive})
        for (auto weighting : {QueryTermWeighting::Frequency, QueryTermWeighting::Unique}) {
            Bm25Config config;
            config.strategy = strategy;
            config.query_weighting = weighting;
            config.k1 = 0.9;
            config.b = 0.4;
            all.push_back(config);
        }
    return all;
}

// Adds, deletes and updates across up to 8 segments (no automatic merge).
void adds_deletes_updates() {
    for (const auto& config : configs()) {
        std::mt19937 random(17);
        SegmentedBm25 index(config);
        std::vector<Version> versions;
        std::map<DocumentId, std::size_t> live;  // id -> version index
        DocumentId next = 10;
        const auto add_batch = [&](std::size_t size) {
            std::vector<std::pair<DocumentId, std::string>> batch;
            for (std::size_t i = 0; i < size; ++i) {
                batch.emplace_back(next, random_text(random));
                live[next] = versions.size();
                versions.push_back({next, batch.back().second, false});
                next += 1 + static_cast<DocumentId>(random() % 3);
            }
            index.add(batch);
        };
        add_batch(400);
        check(index, versions, "one segment");
        for (std::size_t size : {1, 7, 60, 150}) {
            add_batch(size);
            // Delete a few live documents.
            for (int d = 0; d < 15 && !live.empty(); ++d) {
                auto it = live.begin();
                std::advance(it, static_cast<long>(random() % live.size()));
                require(index.remove(it->first), "remove must find a live document");
                versions[it->second].deleted = true;
                live.erase(it);
            }
            // Update one: delete, then add a new version under the same ID.
            auto it = live.begin();
            std::advance(it, static_cast<long>(random() % live.size()));
            const DocumentId updated = it->first;
            require(index.remove(updated), "remove before update");
            versions[it->second].deleted = true;
            const std::vector<std::pair<DocumentId, std::string>> one = {
                {updated, "motor bomba falla actualizada " + random_text(random)}};
            index.add(one);
            it->second = versions.size();
            versions.push_back({updated, one.front().second, false});
            check(index, versions, "after batch of " + std::to_string(size));
        }
        require(index.segment_count() <= SegmentedBm25::kMaxSegments, "unexpected merge");
        require(!index.remove(999'999), "removing an absent document must fail");
        require(!index.contains(kRetired) && index.contains(live.begin()->first), "contains");
        bool rejected = false;
        try {
            const std::vector<std::pair<DocumentId, std::string>> duplicate = {
                {live.begin()->first, "x"}};
            index.add(duplicate);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "adding a live DocumentId must be rejected");

        // Compaction: bit-identical to a fresh index over the live documents.
        index.compact();
        require(index.segment_count() == 1 && index.deleted_count() == 0, "compaction shape");
        Bm25Index fresh(config);
        for (const auto& [id, position] : live) fresh.add_document(id, versions[position].text);
        fresh.build();
        for (const auto& query : kQueries)
            for (std::size_t k : {1, 10, 1000})
                require_same(index.search(query, k), fresh.search(query, k),
                             "compacted vs fresh build, query '" + query + "'");
    }
}

// Many small batches without deletions: automatic merges keep the segment count bounded
// and, with nothing deleted, never change the statistics.
void merges() {
    for (const auto& config : configs()) {
        std::mt19937 random(23);
        SegmentedBm25 index(config);
        std::vector<Version> versions;
        DocumentId next = 1;
        for (int batch = 0; batch < 30; ++batch) {
            std::vector<std::pair<DocumentId, std::string>> documents;
            for (std::size_t i = 0; i < 1 + random() % 40; ++i) {
                documents.emplace_back(next, random_text(random));
                versions.push_back({next++, documents.back().second, false});
            }
            index.add(documents);
            require(index.segment_count() <= SegmentedBm25::kMaxSegments, "too many segments");
        }
        check(index, versions, "after merges");
    }
}

void starts_from_a_built_index() {
    Bm25Config config;
    std::mt19937 random(31);
    auto base = std::make_shared<Bm25Index>(config);
    std::vector<Version> versions;
    for (DocumentId id = 0; id < 200; ++id) {
        versions.push_back({id, random_text(random), false});
        base->add_document(id, versions.back().text);
    }
    base->build();
    SegmentedBm25 index(base);
    require(index.single_index() == base.get(), "a fresh wrap must use the fast path");
    for (const auto& query : kQueries)
        require_same(index.search(query, 10), base->search(query, 10), "fast path");
    require(index.remove(5), "remove from the base segment");
    versions[5].deleted = true;
    require(index.single_index() == nullptr, "a deletion leaves the fast path");
    check(index, versions, "base with a deletion");
}

}  // namespace

int main() {
    try {
        adds_deletes_updates();
        merges();
        starts_from_a_built_index();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Segmented BM25 tests passed.\n";
    return 0;
}
