#include "aether/persistence/snapshot.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <locale>
#include <sstream>
#include <string>
#include <utility>
#include <unordered_set>
#include <vector>

#include "aether/core/vector_math.hpp"
#include "aether/io/binary_io.hpp"
#include "aether/lexical/segmented_bm25.hpp"
#include "aether/quantization/pq.hpp"
#include "aether/quantization/sq8.hpp"

namespace aetherindex {

using io::SectionReader;
using io::SectionWriter;
using snapshot::Section;

// The one place that reads and writes private component state. Each save_* writes the
// fields in a fixed order and each load_* reads them back in the same order, validating
// everything later code would use as an index or a size before it is used.
struct SnapshotAccess {
    template <class Body>
    static void write(const std::filesystem::path& path, Section kind, Body&& body) {
        io::write_file_atomically(path, [&](std::ostream& output) {
            SectionWriter writer(output, static_cast<std::uint32_t>(kind));
            body(writer);
            writer.finish();
        });
    }

    static std::uint32_t ids_checksum(std::span<const VectorId> ids) {
        return io::crc32c(ids.data(), ids.size_bytes());
    }

    // ----- VectorStore -----------------------------------------------------------------

    static void save_store(const VectorStore& store, SectionWriter& out) {
        out.u64(store.dimension_);
        out.u32(static_cast<std::uint32_t>(store.metric_));
        out.u8(store.rows_released_ ? 1 : 0);
        out.array<VectorId>(store.ids_);
        out.array<EmbeddingValue>(store.data_);
        out.u64(store.deleted_count_);
        out.array<std::uint64_t>(store.deleted_);
    }

    static std::unique_ptr<VectorStore> load_store(SectionReader& in) {
        const std::uint64_t dimension = in.u64();
        const std::uint32_t metric = in.u32();
        const bool released = in.u8() != 0;
        if (dimension == 0 || dimension > std::numeric_limits<std::uint32_t>::max())
            in.fail("invalid dimension");
        if (metric != static_cast<std::uint32_t>(DistanceKind::Cosine) &&
            metric != static_cast<std::uint32_t>(DistanceKind::SquaredEuclidean))
            in.fail("unknown distance metric");
        auto store = std::make_unique<VectorStore>(static_cast<std::size_t>(dimension),
                                                   static_cast<DistanceKind>(metric));
        const auto ids = in.array<VectorId>();
        const auto rows = in.array<EmbeddingValue>();
        const std::uint64_t deleted_count = in.u64();
        const auto deleted = in.array<std::uint64_t>();
        in.expect_end();
        if (released ? !rows.empty() : rows.size() != ids.size() * store->stride_)
            in.fail("row count does not match the ID count");
        if (deleted.size() > ids.size() / 64 + 1) in.fail("tombstone bitmap is too large");
        store->ids_.assign(ids.begin(), ids.end());
        store->deleted_.assign(deleted.begin(), deleted.end());
        store->deleted_count_ = static_cast<std::size_t>(deleted_count);
        std::uint64_t marked = 0;
        for (std::size_t word = 0; word < deleted.size(); ++word) {
            marked += static_cast<std::uint64_t>(std::popcount(deleted[word]));
            // Bits past the last position must be clear.
            const std::size_t base = word * 64;
            const std::uint64_t valid =
                base >= ids.size()       ? 0
                : ids.size() - base >= 64 ? ~std::uint64_t{0}
                                          : (std::uint64_t{1} << (ids.size() - base)) - 1;
            if ((deleted[word] & ~valid) != 0) in.fail("tombstone beyond the last position");
        }
        if (marked != deleted_count) in.fail("tombstone count does not match the bitmap");
        store->positions_.reserve(ids.size());
        for (std::size_t i = 0; i < ids.size(); ++i)
            if (!store->is_deleted(i) && !store->positions_.emplace(ids[i], i).second)
                in.fail("duplicate live VectorId");
        store->data_.assign(rows.begin(), rows.end());
        store->rows_released_ = released;
        return store;
    }

    // ----- HnswIndex -------------------------------------------------------------------

    static void save_layer(const FlatGraphLayer& layer, SectionWriter& out) {
        out.u64(layer.max_neighbors_);
        out.array<std::uint32_t>(layer.buffer_);
    }

    static void load_layer(FlatGraphLayer& layer, SectionReader& in) {
        if (in.u64() != layer.max_neighbors_) in.fail("graph degree does not match the config");
        const auto buffer = in.array<std::uint32_t>();
        if (buffer.size() % layer.stride_ != 0) in.fail("graph layer size is not a slot multiple");
        for (std::size_t slot = 0; slot < buffer.size(); slot += layer.stride_)
            if (buffer[slot] > layer.max_neighbors_) in.fail("graph degree exceeds M_max");
        layer.buffer_.assign(buffer.begin(), buffer.end());
    }

    static void save_hnsw(const HnswIndex& index, SectionWriter& out) {
        if (!index.usable_) throw std::logic_error("Cannot save an unusable HNSW index.");
        const auto& config = index.config_;
        out.u64(config.m);
        out.u64(config.m_max_0);
        out.u64(config.ef_construction);
        out.u64(config.ef_search);
        out.u64(config.random_seed);
        out.u64(config.max_level);
        out.u32(static_cast<std::uint32_t>(config.neighbor_selection));
        out.u8(config.extend_candidates ? 1 : 0);
        out.u8(config.keep_pruned_connections ? 1 : 0);
        out.u64(index.store_.size());
        out.u32(ids_checksum(index.store_.ids()));
        std::ostringstream engine;
        engine.imbue(std::locale::classic());
        engine << index.random_engine_;
        out.string(engine.str());
        out.u8(index.entry_point_ ? 1 : 0);
        out.u32(index.entry_point_.value_or(0));
        out.u64(index.max_level_);
        out.u64(index.successful_insertions_.load());
        out.array<std::uint8_t>(index.levels_);
        out.array<std::uint32_t>(index.upper_offsets_);
        save_layer(index.layer0_, out);
        save_layer(index.upper_, out);
    }

    static std::unique_ptr<HnswIndex> load_hnsw(SectionReader& in, const VectorStore& store,
                                                std::optional<std::size_t> ef_search) {
        HnswConfig config;
        config.m = in.u64();
        config.m_max_0 = in.u64();
        config.ef_construction = in.u64();
        config.ef_search = in.u64();
        config.random_seed = in.u64();
        config.max_level = in.u64();
        config.neighbor_selection = static_cast<NeighborSelection>(in.u32());
        config.extend_candidates = in.u8() != 0;
        config.keep_pruned_connections = in.u8() != 0;
        if (ef_search) config.ef_search = *ef_search;
        try {
            validate_hnsw_config(config);
        } catch (const std::invalid_argument& error) {
            in.fail(std::string("invalid HNSW config: ") + error.what());
        }
        if (in.u64() != store.size() || in.u32() != ids_checksum(store.ids()))
            in.fail("the HNSW graph was built on different vectors");
        auto index = std::make_unique<HnswIndex>(store, config);
        std::istringstream engine(in.string());
        engine.imbue(std::locale::classic());
        engine >> index->random_engine_;
        if (!engine) in.fail("invalid random engine state");
        const bool has_entry = in.u8() != 0;
        const std::uint32_t entry = in.u32();
        index->max_level_ = in.u64();
        const std::uint64_t insertions = in.u64();
        const auto levels = in.array<std::uint8_t>();
        const auto offsets = in.array<std::uint32_t>();
        load_layer(index->layer0_, in);
        load_layer(index->upper_, in);
        in.expect_end();

        // Bounds first: every later read (traversal, validate_graph) relies on them.
        if (levels.size() > store.size() || offsets.size() != levels.size() ||
            index->layer0_.slots() != levels.size())
            in.fail("graph node arrays do not match");
        for (std::size_t node = 0; node < levels.size(); ++node) {
            if (levels[node] == HnswIndex::absent_level) continue;
            if (levels[node] > config.max_level) in.fail("node level exceeds max_level");
            if (static_cast<std::uint64_t>(offsets[node]) + levels[node] > index->upper_.slots())
                in.fail("upper-layer offset out of range");
        }
        if (has_entry && (entry >= levels.size() || levels[entry] == HnswIndex::absent_level))
            in.fail("entry point is not a graph node");
        index->levels_.assign(levels.begin(), levels.end());
        index->upper_offsets_.assign(offsets.begin(), offsets.end());
        index->entry_point_ = has_entry ? std::optional<InternalId>(entry) : std::nullopt;
        index->successful_insertions_ = static_cast<std::size_t>(insertions);
        const auto report = index->validate_graph();
        if (!report.valid()) in.fail("graph structure is invalid");
        return index;
    }

    // ----- Bm25Index -------------------------------------------------------------------

    // Terms as one byte blob plus offsets, sorted so that equal indexes give equal bytes.
    template <class Map>
    static std::vector<std::string_view> sorted_terms(const Map& map) {
        std::vector<std::string_view> terms;
        terms.reserve(map.size());
        for (const auto& entry : map) terms.push_back(entry.first);
        std::sort(terms.begin(), terms.end());
        return terms;
    }

    static void save_terms(std::span<const std::string_view> terms, SectionWriter& out) {
        std::vector<std::uint64_t> offsets{0};
        std::string blob;
        for (auto term : terms) {
            blob += term;
            offsets.push_back(blob.size());
        }
        out.array<std::uint64_t>(offsets);
        out.array<char>(blob);
    }

    static std::vector<std::string> load_terms(SectionReader& in) {
        const auto offsets = in.array<std::uint64_t>();
        const auto blob = in.array<char>();
        if (offsets.empty() || offsets.front() != 0 || offsets.back() != blob.size())
            in.fail("term dictionary is inconsistent");
        std::vector<std::string> terms;
        terms.reserve(offsets.size() - 1);
        for (std::size_t i = 0; i + 1 < offsets.size(); ++i) {
            if (offsets[i + 1] < offsets[i]) in.fail("term dictionary is inconsistent");
            terms.emplace_back(blob.data() + offsets[i], offsets[i + 1] - offsets[i]);
        }
        return terms;
    }

    static void save_bm25(const lexical::Bm25Index& index, SectionWriter& out) {
        const auto& config = index.config_;
        const auto& inverted = index.index_;
        if (!inverted.ready_) throw std::logic_error("Cannot save a BM25 index before build().");
        out.f64(config.k1);
        out.f64(config.b);
        out.u32(static_cast<std::uint32_t>(config.strategy));
        out.u32(static_cast<std::uint32_t>(config.analyzer));
        out.u32(static_cast<std::uint32_t>(config.query_weighting));
        // Statistics and document lengths, by ascending DocumentId.
        out.u64(inverted.document_count_);
        out.u64(inverted.total_tokens_);
        out.f64(inverted.average_length_);
        std::vector<DocumentId> ids;
        ids.reserve(inverted.lengths_.size());
        for (const auto& [id, length] : inverted.lengths_) ids.push_back(id);
        std::sort(ids.begin(), ids.end());
        std::vector<std::uint32_t> lengths;
        lengths.reserve(ids.size());
        for (auto id : ids) lengths.push_back(inverted.lengths_.at(id));
        out.array<DocumentId>(ids);
        out.array<std::uint32_t>(lengths);
        out.u64(index.memory_.raw_bytes);
        out.u64(index.memory_.compressed_bytes);
        out.u64(index.memory_.postings);
        out.u64(index.memory_.blocks);
        out.u8(inverted.postings_released_ ? 1 : 0);
        if (!inverted.postings_released_) {
            // Exhaustive: raw postings per term, concatenated in term order.
            const auto terms = sorted_terms(inverted.postings_);
            std::vector<std::uint64_t> starts{0};
            std::vector<lexical::Posting> postings;
            for (auto term : terms) {
                const auto& list = inverted.postings_.find(term)->second;
                postings.insert(postings.end(), list.begin(), list.end());
                starts.push_back(postings.size());
            }
            save_terms(terms, out);
            out.array<std::uint64_t>(starts);
            out.array<lexical::Posting>(postings);
            return;
        }
        // Compressed layout.
        const auto terms = sorted_terms(index.layout_);
        std::vector<lexical::Bm25Index::TermLayout> layouts;
        layouts.reserve(terms.size());
        for (auto term : terms) layouts.push_back(index.layout_.find(term)->second);
        save_terms(terms, out);
        out.array<lexical::Bm25Index::TermLayout>(layouts);
        out.array<lexical::Bm25Index::Block>(index.blocks_);
        out.array<std::uint8_t>(index.postings_);
        out.array<DocumentId>(index.document_ids_);
        out.array<double>(index.length_norm_);
    }

    static std::unique_ptr<lexical::Bm25Index> load_bm25(SectionReader& in,
                                                         const snapshot::Bm25QueryOverrides& with) {
        using lexical::Bm25Strategy;
        lexical::Bm25Config config;
        config.k1 = in.f64();
        config.b = in.f64();
        config.strategy = static_cast<Bm25Strategy>(in.u32());
        config.analyzer = static_cast<lexical::Analyzer>(in.u32());
        config.query_weighting = static_cast<lexical::QueryTermWeighting>(in.u32());
        try {
            lexical::validate_bm25_config(config);
        } catch (const std::invalid_argument& error) {
            in.fail(std::string("invalid BM25 config: ") + error.what());
        }
        const bool saved_exhaustive = config.strategy == Bm25Strategy::Exhaustive;
        if (with.strategy) {
            if ((*with.strategy == Bm25Strategy::Exhaustive) != saved_exhaustive)
                throw std::invalid_argument(
                    saved_exhaustive
                        ? "This index keeps raw postings (exhaustive); compressed BM25 "
                          "strategies need an index built with one of them."
                        : "This index keeps compressed postings; the exhaustive BM25 strategy "
                          "needs an index built with --bm25-strategy exhaustive.");
            config.strategy = *with.strategy;
        }
        if (with.query_weighting) config.query_weighting = *with.query_weighting;
        auto index = std::make_unique<lexical::Bm25Index>(config);
        auto& inverted = index->index_;
        inverted.document_count_ = in.u64();
        inverted.total_tokens_ = in.u64();
        inverted.average_length_ = in.f64();
        const auto ids = in.array<DocumentId>();
        const auto lengths = in.array<std::uint32_t>();
        if (ids.size() != inverted.document_count_ || lengths.size() != ids.size())
            in.fail("document statistics do not match");
        inverted.lengths_.reserve(ids.size());
        for (std::size_t i = 0; i < ids.size(); ++i) {
            if (i > 0 && ids[i] <= ids[i - 1]) in.fail("document IDs are not ascending");
            inverted.lengths_.emplace(ids[i], lengths[i]);
        }
        index->memory_.raw_bytes = in.u64();
        index->memory_.compressed_bytes = in.u64();
        index->memory_.postings = in.u64();
        index->memory_.blocks = in.u64();
        const bool released = in.u8() != 0;
        if (released == saved_exhaustive) in.fail("posting format does not match the strategy");
        if (!released) {
            const auto terms = load_terms(in);
            const auto starts = in.array<std::uint64_t>();
            const auto postings = in.array<lexical::Posting>();
            if (starts.size() != terms.size() + 1 || starts.back() != postings.size())
                in.fail("posting lists are inconsistent");
            for (std::size_t t = 0; t < terms.size(); ++t) {
                if (starts[t + 1] < starts[t]) in.fail("posting lists are inconsistent");
                lexical::PostingList list(postings.begin() + starts[t], postings.begin() + starts[t + 1]);
                for (const auto& posting : list)
                    if (!inverted.lengths_.contains(posting.document_id))
                        in.fail("a posting names an unknown document");
                inverted.postings_.emplace(terms[t], std::move(list));
            }
        } else {
            const auto terms = load_terms(in);
            const auto layouts = in.array<lexical::Bm25Index::TermLayout>();
            const auto blocks = in.array<lexical::Bm25Index::Block>();
            const auto postings = in.array<std::uint8_t>();
            const auto document_ids = in.array<DocumentId>();
            const auto norms = in.array<double>();
            if (layouts.size() != terms.size() || document_ids.size() != ids.size() ||
                norms.size() != ids.size() || postings.size() < sizeof(std::uint64_t))
                in.fail("compressed layout sizes do not match");
            for (std::size_t t = 0; t < terms.size(); ++t)
                index->layout_.emplace(terms[t], layouts[t]);
            index->blocks_.assign(blocks.begin(), blocks.end());
            index->postings_.assign(postings.begin(), postings.end());
            index->document_ids_.assign(document_ids.begin(), document_ids.end());
            index->length_norm_.assign(norms.begin(), norms.end());
            inverted.postings_released_ = true;
            if (!index->layout_is_consistent()) in.fail("compressed postings are inconsistent");
        }
        inverted.ready_ = true;
        return index;
    }

    // ----- SegmentedBm25 ---------------------------------------------------------------

    static void save_segmented(const lexical::SegmentedBm25& index, SectionWriter& out) {
        const auto& config = index.config_;
        out.f64(config.k1);
        out.f64(config.b);
        out.u32(static_cast<std::uint32_t>(config.strategy));
        out.u32(static_cast<std::uint32_t>(config.analyzer));
        out.u32(static_cast<std::uint32_t>(config.query_weighting));
        out.u64(index.segments_.size());
        for (const auto& segment : index.segments_) {
            save_bm25(*segment.index, out);
            out.u64(segment.deleted_count);
            out.array<std::uint64_t>(segment.deleted);
        }
    }

    static std::unique_ptr<lexical::SegmentedBm25> load_segmented(
        SectionReader& in, const snapshot::Bm25QueryOverrides& with) {
        lexical::Bm25Config config;
        config.k1 = in.f64();
        config.b = in.f64();
        config.strategy = static_cast<lexical::Bm25Strategy>(in.u32());
        config.analyzer = static_cast<lexical::Analyzer>(in.u32());
        config.query_weighting = static_cast<lexical::QueryTermWeighting>(in.u32());
        try {
            lexical::validate_bm25_config(config);
        } catch (const std::invalid_argument& error) {
            in.fail(std::string("invalid BM25 config: ") + error.what());
        }
        if (with.strategy) config.strategy = *with.strategy;
        if (with.query_weighting) config.query_weighting = *with.query_weighting;
        auto index = std::make_unique<lexical::SegmentedBm25>(config);
        const std::uint64_t count = in.u64();
        if (count > in.payload_bytes()) in.fail("segment count exceeds the payload");
        bool global = count > 1;
        for (std::uint64_t i = 0; i < count; ++i) {
            std::shared_ptr<const lexical::Bm25Index> segment = load_bm25(in, with);
            const auto& loaded = segment->config_;
            if (loaded.k1 != config.k1 || loaded.b != config.b ||
                loaded.analyzer != config.analyzer)
                in.fail("segments disagree on the BM25 configuration");
            const std::uint64_t deleted_count = in.u64();
            auto deleted = in.vector<std::uint64_t>();
            const std::size_t documents = segment->document_count();
            if (!deleted.empty() && deleted.size() != (documents + 63) / 64)
                in.fail("segment tombstone bitmap has the wrong size");
            std::uint64_t marked = 0;
            for (std::size_t word = 0; word < deleted.size(); ++word) {
                marked += static_cast<std::uint64_t>(std::popcount(deleted[word]));
                const std::size_t base = word * 64;
                const std::uint64_t valid =
                    documents - base >= 64 ? ~std::uint64_t{0}
                                           : (std::uint64_t{1} << (documents - base)) - 1;
                if ((deleted[word] & ~valid) != 0) in.fail("tombstone beyond the last document");
            }
            if (marked != deleted_count || deleted_count > documents)
                in.fail("segment tombstone count does not match");
            global = global || deleted_count != 0;
            index->segments_.push_back(
                {std::move(segment), std::move(deleted), static_cast<std::size_t>(deleted_count),
                 {}, {}, {}, false});
        }
        in.expect_end();
        // Live DocumentIds must be unique across segments.
        std::unordered_set<DocumentId> live;
        for (const auto& segment : index->segments_) {
            const auto ids = segment.index->ordinal_ids();
            for (std::size_t ordinal = 0; ordinal < ids.size(); ++ordinal)
                if (!lexical::SegmentedBm25::is_deleted(segment, ordinal) &&
                    !live.insert(ids[ordinal]).second)
                    in.fail("a live document appears in two segments");
        }
        if (global) index->prepare_all();
        return index;
    }

    // ----- Quantized codes -------------------------------------------------------------

    static void save_sq8(const Sq8Vectors& codes, SectionWriter& out) {
        out.u64(codes.dimension_);
        out.u64(codes.stride_);
        out.u64(codes.rows_);
        out.u32(static_cast<std::uint32_t>(codes.metric_));
        out.array<float>(codes.min_);
        out.array<float>(codes.scale_);
        out.array<std::uint8_t>(codes.codes_);
        out.array<float>(codes.row_terms_);
    }

    static std::unique_ptr<QuantizedVectors> load_sq8(SectionReader& in) {
        std::unique_ptr<Sq8Vectors> codes(new Sq8Vectors());
        codes->dimension_ = in.u64();
        codes->stride_ = in.u64();
        codes->rows_ = in.u64();
        codes->metric_ = static_cast<DistanceKind>(in.u32());
        codes->min_ = in.vector<float>();
        codes->scale_ = in.vector<float>();
        const auto bytes = in.array<std::uint8_t>();
        codes->row_terms_ = in.vector<float>();
        in.expect_end();
        if (codes->dimension_ == 0 || codes->stride_ < codes->dimension_ ||
            codes->stride_ % 16 != 0 || codes->min_.size() != codes->dimension_ ||
            codes->scale_.size() != codes->dimension_ ||
            bytes.size() != codes->rows_ * codes->stride_ ||
            (codes->metric_ != DistanceKind::Cosine &&
             codes->metric_ != DistanceKind::SquaredEuclidean))
            in.fail("SQ8 sizes do not match");
        codes->codes_.assign(bytes.begin(), bytes.end());
        return codes;
    }

    static void save_pq(const PqVectors& codes, SectionWriter& out) {
        out.u64(codes.dimension_);
        out.u64(codes.rows_);
        out.u32(static_cast<std::uint32_t>(codes.metric_));
        out.u64(codes.subspaces_);
        out.u64(codes.sub_dimension_);
        out.u64(codes.centroids_);
        out.array<float>(codes.codebooks_);
        out.array<std::uint8_t>(codes.codes_);
    }

    static std::unique_ptr<QuantizedVectors> load_pq(SectionReader& in) {
        std::unique_ptr<PqVectors> codes(new PqVectors());
        codes->dimension_ = in.u64();
        codes->rows_ = in.u64();
        codes->metric_ = static_cast<DistanceKind>(in.u32());
        codes->subspaces_ = in.u64();
        codes->sub_dimension_ = in.u64();
        codes->centroids_ = in.u64();
        codes->codebooks_ = in.vector<float>();
        codes->codes_ = in.vector<std::uint8_t>();
        in.expect_end();
        if (codes->subspaces_ == 0 || codes->centroids_ == 0 || codes->centroids_ > 256 ||
            codes->subspaces_ * codes->sub_dimension_ != codes->dimension_ ||
            codes->codebooks_.size() !=
                codes->subspaces_ * codes->centroids_ * codes->sub_dimension_ ||
            codes->codes_.size() != codes->rows_ * codes->subspaces_ ||
            (codes->metric_ != DistanceKind::Cosine &&
             codes->metric_ != DistanceKind::SquaredEuclidean))
            in.fail("PQ sizes do not match");
        for (auto code : codes->codes_)
            if (code >= codes->centroids_) in.fail("PQ code out of range");
        return codes;
    }

    static void save_codes(const QuantizedVectors& codes, const std::filesystem::path& path) {
        if (const auto* sq8 = dynamic_cast<const Sq8Vectors*>(&codes))
            return write(path, Section::Sq8, [&](SectionWriter& out) { save_sq8(*sq8, out); });
        if (const auto* pq = dynamic_cast<const PqVectors*>(&codes))
            return write(path, Section::Pq, [&](SectionWriter& out) { save_pq(*pq, out); });
        throw std::invalid_argument("Unsupported quantized vectors for a snapshot.");
    }

    static std::unique_ptr<QuantizedVectors> load_codes(const std::filesystem::path& path,
                                                        bool verify) {
        const io::FileView file(path);
        const auto kind = file.size() >= io::kSectionHeaderBytes
                              ? io::get<std::uint32_t>(file.data() + 12)
                              : 0;
        if (kind == static_cast<std::uint32_t>(Section::Pq)) {
            SectionReader in(file, kind, verify);
            return load_pq(in);
        }
        SectionReader in(file, static_cast<std::uint32_t>(Section::Sq8), verify);
        return load_sq8(in);
    }
};

namespace snapshot {

void save(const VectorStore& store, const std::filesystem::path& path) {
    SnapshotAccess::write(path, Section::Vectors,
                          [&](SectionWriter& out) { SnapshotAccess::save_store(store, out); });
}

std::unique_ptr<VectorStore> load_vector_store(const std::filesystem::path& path,
                                               bool verify_checksums) {
    const io::FileView file(path);
    SectionReader in(file, static_cast<std::uint32_t>(Section::Vectors), verify_checksums);
    return SnapshotAccess::load_store(in);
}

void save(const HnswIndex& index, const std::filesystem::path& path) {
    SnapshotAccess::write(path, Section::Hnsw,
                          [&](SectionWriter& out) { SnapshotAccess::save_hnsw(index, out); });
}

std::unique_ptr<HnswIndex> load_hnsw(const std::filesystem::path& path, const VectorStore& store,
                                     bool verify_checksums, std::optional<std::size_t> ef_search) {
    const io::FileView file(path);
    SectionReader in(file, static_cast<std::uint32_t>(Section::Hnsw), verify_checksums);
    return SnapshotAccess::load_hnsw(in, store, ef_search);
}

void save(const lexical::Bm25Index& index, const std::filesystem::path& path) {
    SnapshotAccess::write(path, Section::Bm25,
                          [&](SectionWriter& out) { SnapshotAccess::save_bm25(index, out); });
}

std::unique_ptr<lexical::Bm25Index> load_bm25(const std::filesystem::path& path,
                                              bool verify_checksums,
                                              const Bm25QueryOverrides& overrides) {
    const io::FileView file(path);
    SectionReader in(file, static_cast<std::uint32_t>(Section::Bm25), verify_checksums);
    auto index = SnapshotAccess::load_bm25(in, overrides);
    in.expect_end();
    return index;
}

void save(const lexical::SegmentedBm25& index, const std::filesystem::path& path) {
    SnapshotAccess::write(path, Section::Bm25Segments,
                          [&](SectionWriter& out) { SnapshotAccess::save_segmented(index, out); });
}

std::unique_ptr<lexical::SegmentedBm25> load_segmented_bm25(const std::filesystem::path& path,
                                                            bool verify_checksums,
                                                            const Bm25QueryOverrides& overrides) {
    const io::FileView file(path);
    SectionReader in(file, static_cast<std::uint32_t>(Section::Bm25Segments), verify_checksums);
    return SnapshotAccess::load_segmented(in, overrides);
}

void save(const QuantizedVectors& codes, const std::filesystem::path& path) {
    SnapshotAccess::save_codes(codes, path);
}

std::unique_ptr<QuantizedVectors> load_quantized(const std::filesystem::path& path,
                                                 bool verify_checksums) {
    return SnapshotAccess::load_codes(path, verify_checksums);
}

}  // namespace snapshot
}  // namespace aetherindex
