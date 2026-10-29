// Phase 1: binary .avec embeddings — round trip, metric handling and corruption.
#include "aether/storage/vector_file.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

#include "../test_helpers.hpp"

using namespace aetherindex;

namespace {

std::vector<unsigned char> bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

void store_bytes(const std::filesystem::path& path, const std::vector<unsigned char>& data) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(data.data()),
                 static_cast<std::streamsize>(data.size()));
}

template <class Function>
void require_file_error(Function action, const char* message) {
    try {
        action();
    } catch (const VectorFileError&) {
        return;
    }
    throw std::runtime_error(message);
}

// Recompute the IEEE CRC-32C the writer uses, so tests can forge files whose only
// defect is the one under test (e.g. NaN or a duplicate ID with valid checksums).
std::uint32_t crc32c(const unsigned char* data, std::size_t size) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1u) ? 0x82F63B78u : 0u);
    }
    return crc ^ 0xFFFFFFFFu;
}

void put32(std::vector<unsigned char>& data, std::size_t offset, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) data[offset + i] = static_cast<unsigned char>(value >> (8 * i));
}

std::uint64_t get64(const std::vector<unsigned char>& data, std::size_t offset) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value |= std::uint64_t{data[offset + i]} << (8 * i);
    return value;
}

void reseal(std::vector<unsigned char>& data, std::size_t dimension, std::size_t count) {
    const auto rows = get64(data, 32);
    put32(data, 48, crc32c(data.data() + 64, 4 * count));
    put32(data, 52, crc32c(data.data() + rows, 4 * count * dimension));
    put32(data, 60, crc32c(data.data(), 60));
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "Pass a scratch directory.");
        const std::filesystem::path folder(argv[1]);
        std::filesystem::create_directories(folder);
        const auto file = folder / "vectors.avec";

        // Random raw rows, sparse/maximal IDs and a dimension that is not a SIMD multiple.
        constexpr std::size_t dimension = 37, count = 257;
        std::mt19937_64 engine(7);
        std::vector<VectorId> ids;
        std::vector<float> rows;
        for (std::size_t i = 0; i < count; ++i) {
            ids.push_back(i + 1 == count ? std::numeric_limits<VectorId>::max()
                                         : static_cast<VectorId>(7 * i + 10));
            for (std::size_t d = 0; d < dimension; ++d)
                rows.push_back(
                    static_cast<float>(static_cast<std::int64_t>(engine() % 20001) - 10000) /
                    37.0f);
        }
        write_vector_file(file, ids, rows, dimension);
        require(
            std::filesystem::exists(file) && !std::filesystem::exists(folder / "vectors.avec.tmp"),
            "Writer left a temporary file or no target.");

        // Squared Euclidean keeps raw values: bitwise round trip in file order.
        const auto raw = read_vector_file(file, DistanceKind::SquaredEuclidean);
        require(raw->size() == count && raw->dimension() == dimension, "Round-trip shape lost.");
        for (std::size_t i = 0; i < count; ++i) {
            require(raw->ids()[i] == ids[i], "Insertion order or ID changed.");
            require(std::memcmp(raw->data_by_position(i), rows.data() + i * dimension,
                                dimension * sizeof(float)) == 0,
                    "Round trip is not bitwise identical.");
        }
        // Cosine reading normalizes exactly like inserting the same rows directly.
        const auto cosine = read_vector_file(file, DistanceKind::Cosine);
        VectorStore direct(dimension, DistanceKind::Cosine);
        for (std::size_t i = 0; i < count; ++i)
            require(direct.insert(ids[i], {rows.data() + i * dimension, dimension}) ==
                        InsertStatus::Success,
                    "Direct insert failed.");
        for (std::size_t i = 0; i < count; ++i)
            require(std::memcmp(cosine->data_by_position(i), direct.data_by_position(i),
                                cosine->padded_dimension() * sizeof(float)) == 0,
                    "Cosine read differs from direct insertion.");
        // Writing a store reproduces the same file.
        write_vector_file(folder / "copy.avec", *raw);
        require(bytes(folder / "copy.avec") == bytes(file), "Store export is not byte-identical.");

        // Writer argument validation.
        requireInvalidArgument([&] { write_vector_file(folder / "x.avec", ids, rows, 0); },
                               "Zero dimension accepted.");
        requireInvalidArgument([&] { write_vector_file(folder / "x.avec", {}, {}, 4); },
                               "Empty file accepted.");
        requireInvalidArgument(
            [&] {
                write_vector_file(folder / "x.avec", ids, {rows.data(), rows.size() - 1},
                                  dimension);
            },
            "Row count mismatch accepted.");
        auto nan_rows = rows;
        nan_rows[5] = std::numeric_limits<float>::quiet_NaN();
        requireInvalidArgument(
            [&] { write_vector_file(folder / "x.avec", ids, nan_rows, dimension); },
            "Non-finite component written.");

        // Corruption: every defect is detected before any row is used.
        const auto original = bytes(file);
        const auto corrupt = folder / "corrupt.avec";
        const auto expect_rejected = [&](std::vector<unsigned char> data, const char* message) {
            store_bytes(corrupt, data);
            require_file_error([&] { read_vector_file(corrupt, DistanceKind::Cosine); }, message);
        };
        auto data = original;
        data[0] = 'X';
        expect_rejected(data, "Bad magic accepted.");
        data = original;
        data[9] ^= 1;
        expect_rejected(data, "Header bit flip accepted.");
        data = original;
        put32(data, 8, 2);
        put32(data, 60, crc32c(data.data(), 60));
        expect_rejected(data, "Unsupported version accepted.");
        data = original;
        data.back() ^= 0x40;
        expect_rejected(data, "Row bit flip accepted.");
        data = original;
        data[70] ^= 1;
        expect_rejected(data, "ID bit flip accepted.");
        data = original;
        data.resize(data.size() - 4);
        expect_rejected(data, "Truncated file accepted.");
        data = original;
        data.push_back(0);
        expect_rejected(data, "Trailing bytes accepted.");
        expect_rejected(std::vector<unsigned char>(original.begin(), original.begin() + 10),
                        "Truncated header accepted.");
        // Valid checksums but invalid content.
        data = original;
        std::memcpy(data.data() + 64 + 4, data.data() + 64, 4);  // ids[1] = ids[0]
        reseal(data, dimension, count);
        expect_rejected(data, "Duplicate ID accepted.");
        data = original;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        std::memcpy(data.data() + get64(data, 32), &nan, 4);
        reseal(data, dimension, count);
        expect_rejected(data, "NaN component accepted.");
        // A zero row is valid for squared Euclidean and rejected for cosine.
        std::vector<float> zero_rows(2 * 3, 0.0f);
        zero_rows[3] = 1.0f;
        write_vector_file(folder / "zero.avec", std::vector<VectorId>{1, 2}, zero_rows, 3);
        require(read_vector_file(folder / "zero.avec", DistanceKind::SquaredEuclidean)->size() == 2,
                "Zero row rejected for squared Euclidean.");
        require_file_error([&] { read_vector_file(folder / "zero.avec", DistanceKind::Cosine); },
                           "Zero row accepted for cosine.");
        require_file_error([&] { read_vector_file(folder / "absent.avec", DistanceKind::Cosine); },
                           "Missing file accepted.");
        require_file_error([&] { read_vector_file(folder, DistanceKind::Cosine); },
                           "Directory accepted as a vector file.");
        std::cout << "Vector file round trip and corruption checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
