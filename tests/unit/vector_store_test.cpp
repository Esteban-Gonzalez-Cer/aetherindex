#include "aether/storage/vector_store.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <limits>
#include <random>
#include <type_traits>
#include <vector>

#include "../test_helpers.hpp"
#include "aether/core/prepared_query.hpp"

using namespace aetherindex;

static_assert(std::is_same_v<VectorId, std::uint32_t>);
static_assert(std::is_same_v<EmbeddingValue, float>);
static_assert(!std::is_copy_constructible_v<VectorStore>);
static_assert(!std::is_move_constructible_v<VectorStore>);
static_assert(!std::is_copy_assignable_v<VectorStore>);
static_assert(!std::is_move_assignable_v<VectorStore>);
static_assert(std::is_const_v<std::span<const EmbeddingValue>::element_type>);

void checkBasicStorage() {
    requireInvalidArgument([] { VectorStore store(0); }, "Zero dimension must be rejected.");
    requireInvalidArgument([] { VectorStore store(3, static_cast<DistanceKind>(42)); },
                           "Unsupported store metrics must be rejected.");
    VectorStore store(3);
    require(store.size() == 0 && store.dimension() == 3 && store.ids().empty(),
            "A new store must be empty with the requested dimension.");
    require(store.distance_kind() == DistanceKind::Cosine, "Cosine must be the default metric.");
    require(!store.contains(20) && !store.find(20), "Missing IDs must be reported without a view.");

    std::array<float, 3> original = {1.0f, 0.0f, 0.0f};
    const std::array<float, 3> second = {0.0f, 1.0f, 0.0f};
    const std::array<float, 3> third = {0.0f, 0.0f, 1.0f};
    const VectorId maximum = std::numeric_limits<VectorId>::max();
    require(store.insert(0, original) == InsertStatus::Success, "ID zero must be supported.");
    original[0] = 99.0f;
    require((*store.find(0))[0] == 1.0f, "The store must own its embedding independently.");
    require(store.insert(20, second) == InsertStatus::Success, "Second insertion failed.");
    require(store.insert(maximum, third) == InsertStatus::Success, "Maximum uint32 ID must work.");
    require(store.size() == 3 && store.dimension() == 3, "Stored count or dimension changed.");
    require(!store.contains(12345) && !store.find(12345),
            "A missing ID in a populated store must return no view.");
    const auto ids = store.ids();
    require(ids[0] == 0 && ids[1] == 20 && ids[2] == maximum,
            "Public traversal must retain insertion order and original IDs.");
    const auto first_view = *store.find(0);
    const auto second_view = *store.find(20);
    const auto third_view = *store.find(maximum);
    require(first_view.size() == 3 && second_view.size() == 3 && third_view.size() == 3,
            "Views must have the fixed dimension.");
    require(second_view.data() == first_view.data() + store.padded_dimension() &&
                third_view.data() == second_view.data() + store.padded_dimension(),
            "Embedding storage must be contiguous with its declared padded stride.");
    require(store.find(20)->data() == second_view.data(), "Reading must not allocate a copy.");
    require(second_view[0] == 0.0f && second_view[1] == 1.0f && second_view[2] == 0.0f,
            "Stored components must be preserved.");
}

void checkRejections() {
    VectorStore store(3);
    const std::array<float, 3> valid = {1.0f, 2.0f, 3.0f};
    const std::array<float, 3> replacement = {4.0f, 5.0f, 6.0f};
    require(store.insert(10, valid) == InsertStatus::Success, "Setup insertion failed.");
    const std::vector<float> snapshot(store.find(10)->begin(), store.find(10)->end());
    require(store.insert(10, replacement) == InsertStatus::DuplicateId, "Duplicate ID accepted.");
    const std::array<float, 2> wrong_dimension = {1.0f, 2.0f};
    require(store.insert(40, wrong_dimension) == InsertStatus::DimensionMismatch,
            "Wrong dimension accepted.");
    require(store.insert(41, {}) == InsertStatus::DimensionMismatch, "Empty embedding accepted.");
    const std::array<float, 3> zero = {0.0f, -0.0f, 0.0f};
    require(store.insert(42, zero) == InsertStatus::InvalidEmbedding,
            "Cosine storage must reject zero vectors, including negative zero.");
    for (float invalid :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
          -std::numeric_limits<float>::infinity()}) {
        const std::array<float, 3> values = {1.0f, invalid, 3.0f};
        require(store.insert(50, values) == InsertStatus::InvalidEmbedding,
                "Nonfinite coordinates must be rejected.");
        require(store.size() == 1 && !store.contains(50), "Rejection altered the store.");
    }
    require(store.size() == 1 && !store.contains(40) && !store.contains(41) && !store.contains(42),
            "Rejected IDs must never enter the position map.");
    require(std::equal(store.find(10)->begin(), store.find(10)->end(), snapshot.begin()),
            "Rejected insertion must preserve the original embedding.");
    require(store.insert(50, valid) == InsertStatus::Success, "Store must remain usable.");

    VectorStore squared(3, DistanceKind::SquaredEuclidean);
    require(squared.insert(0, zero) == InsertStatus::Success, "Squared distance allows zeros.");
    const std::array<float, 3> invalid = {1.0f, std::numeric_limits<float>::infinity(), 0.0f};
    require(squared.insert(1, invalid) == InsertStatus::InvalidEmbedding,
            "Squared Euclidean stores must also reject nonfinite values.");
    const std::array<float, 3> tiny = {std::numeric_limits<float>::denorm_min(), 0.0f, 0.0f};
    require(store.insert(60, tiny) == InsertStatus::InvalidEmbedding,
            "Cosine inserts with norm below 1e-7 must be rejected.");
}

void checkAliasedInsertion() {
    VectorStore store(3);
    const std::array<float, 3> values = {0.25f, -2.0f, 4.0f};
    require(store.insert(0, values) == InsertStatus::Success, "Setup insertion failed.");
    const PreparedQuery expected(values, 3, DistanceKind::Cosine);
    for (VectorId id = 1; id <= 128; ++id) {
        // Acquire a fresh view for every call; never use it after insertion.
        require(store.insert(id, *store.find(0)) == InsertStatus::Success,
                "Insertion from an existing store view must survive reallocation.");
        const auto copied = *store.find(id);
        for (std::size_t i = 0; i < copied.size(); ++i) {
            require(std::abs(copied[i] - expected.values()[i]) < 1e-6f,
                    "Aliased insertion corrupted a normalized embedding.");
        }
    }
    require(store.size() == 129, "Aliased insertions lost IDs.");
}

void checkThousandEmbeddings() {
    constexpr std::size_t count = 1000;
    constexpr std::size_t dimension = 128;
    std::mt19937 random(42);
    std::vector<std::array<float, dimension>> expected(count);
    VectorStore store(dimension, DistanceKind::SquaredEuclidean);
    for (std::size_t i = 0; i < count; ++i) {
        for (float& value : expected[i]) {
            value = static_cast<float>(static_cast<int>(random() % 2001) - 1000) / 1000.0f;
        }
        expected[i][0] = 1.0f;  // Ensure every embedding is valid for cosine.
        const VectorId id = static_cast<VectorId>(i * 7 + 10);
        require(store.insert(id, expected[i]) == InsertStatus::Success,
                "Deterministic dataset insertion failed.");
    }
    require(store.size() == count, "The store must contain exactly 1,000 embeddings.");
    for (std::size_t i = 0; i < count; ++i) {
        const VectorId id = static_cast<VectorId>(i * 7 + 10);
        const auto recovered = store.find(id);
        require(store.ids()[i] == id && recovered.has_value(), "Dataset ID missing or reordered.");
        require(recovered->size() == dimension, "Recovered embedding dimension changed.");
        for (std::size_t component = 0; component < dimension; ++component) {
            require((*recovered)[component] == expected[i][component],
                    "Dataset retrieval changed an embedding component.");
        }
    }
    std::cout << "1000/1000 embeddings of dimension 128 recovered exactly (seed 42).\n";
}

int main() {
    try {
        checkBasicStorage();
        checkRejections();
        checkAliasedInsertion();
        checkThousandEmbeddings();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
