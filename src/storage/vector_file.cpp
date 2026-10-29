#include "aether/storage/vector_file.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace aetherindex {
namespace {

constexpr std::array<char, 8> kMagic{'A', 'E', 'T', 'H', 'V', 'E', 'C', '1'};
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kHeaderBytes = 64;

// CRC-32C (Castagnoli). ARMv8 CRC instructions when available, table otherwise.
std::uint32_t crc32c(const unsigned char* data, std::size_t size) {
    std::uint32_t crc = 0xFFFFFFFFu;
#if defined(__ARM_FEATURE_CRC32)
    for (; size >= 8; data += 8, size -= 8) {
        std::uint64_t word;
        std::memcpy(&word, data, 8);
        crc = __crc32cd(crc, word);
    }
    for (; size > 0; ++data, --size) crc = __crc32cb(crc, *data);
#else
    static const auto table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^ ((value & 1u) ? 0x82F63B78u : 0u);
            values[i] = value;
        }
        return values;
    }();
    for (; size > 0; ++data, --size) crc = table[(crc ^ *data) & 0xFFu] ^ (crc >> 8);
#endif
    return crc ^ 0xFFFFFFFFu;
}

template <class T>
void put(unsigned char* out, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i)
        out[i] = static_cast<unsigned char>(static_cast<std::uint64_t>(value) >> (8 * i));
}

template <class T>
T get(const unsigned char* in) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) value |= std::uint64_t{in[i]} << (8 * i);
    return static_cast<T>(value);
}

constexpr std::uint64_t align64(std::uint64_t value) { return (value + 63) & ~std::uint64_t{63}; }

void require_little_endian() {
    if constexpr (std::endian::native != std::endian::little)
        throw VectorFileError("The .avec format requires a little-endian host.");
}

// Read-only view of the whole file: mmap on POSIX, a heap copy otherwise.
class FileView {
   public:
    explicit FileView(const std::filesystem::path& path) : path_(path) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error))
            throw VectorFileError("Cannot open vector file (expected a regular file): " +
                                  path.string());
#if !defined(_WIN32)
        descriptor_ = ::open(path.c_str(), O_RDONLY);
        if (descriptor_ < 0) throw VectorFileError("Cannot open vector file: " + path.string());
        struct stat info{};
        if (::fstat(descriptor_, &info) != 0) {
            ::close(descriptor_);
            throw VectorFileError("Cannot stat vector file: " + path.string());
        }
        size_ = static_cast<std::size_t>(info.st_size);
        if (size_ > 0) {
            void* mapped = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, descriptor_, 0);
            if (mapped == MAP_FAILED) {
                ::close(descriptor_);
                throw VectorFileError("Cannot map vector file: " + path.string());
            }
            data_ = static_cast<const unsigned char*>(mapped);
        }
#else
        std::ifstream input(path, std::ios::binary);
        if (!input) throw VectorFileError("Cannot open vector file: " + path.string());
        buffer_.assign(std::istreambuf_iterator<char>(input), {});
        size_ = buffer_.size();
        data_ = reinterpret_cast<const unsigned char*>(buffer_.data());
#endif
    }
    FileView(const FileView&) = delete;
    FileView& operator=(const FileView&) = delete;
    ~FileView() {
#if !defined(_WIN32)
        if (data_) ::munmap(const_cast<unsigned char*>(data_), size_);
        if (descriptor_ >= 0) ::close(descriptor_);
#endif
    }
    const unsigned char* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }

   private:
    std::filesystem::path path_;
    const unsigned char* data_ = nullptr;
    std::size_t size_ = 0;
#if !defined(_WIN32)
    int descriptor_ = -1;
#else
    std::vector<char> buffer_;
#endif
};

}  // namespace

void write_vector_file(const std::filesystem::path& path, std::span<const VectorId> ids,
                       std::span<const EmbeddingValue> rows, std::size_t dimension) {
    require_little_endian();
    if (dimension == 0 || dimension > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("Vector file dimension must be in [1, 2^32).");
    if (ids.empty()) throw std::invalid_argument("A vector file needs at least one vector.");
    if (rows.size() / dimension != ids.size() || rows.size() % dimension != 0)
        throw std::invalid_argument("Vector file rows must hold count * dimension values.");
    for (float value : rows)
        if (!std::isfinite(value))
            throw std::invalid_argument("Vector file components must be finite.");

    const std::uint64_t count = ids.size();
    const std::uint64_t ids_offset = kHeaderBytes;
    const std::uint64_t rows_offset = align64(ids_offset + 4 * count);
    const std::uint64_t file_bytes = rows_offset + rows.size_bytes();
    std::array<unsigned char, kHeaderBytes> header{};
    std::memcpy(header.data(), kMagic.data(), kMagic.size());
    put<std::uint32_t>(header.data() + 8, kVersion);
    put<std::uint32_t>(header.data() + 12, static_cast<std::uint32_t>(dimension));
    put<std::uint64_t>(header.data() + 16, count);
    put<std::uint64_t>(header.data() + 24, ids_offset);
    put<std::uint64_t>(header.data() + 32, rows_offset);
    put<std::uint64_t>(header.data() + 40, file_bytes);
    put<std::uint32_t>(
        header.data() + 48,
        crc32c(reinterpret_cast<const unsigned char*>(ids.data()), ids.size_bytes()));
    put<std::uint32_t>(
        header.data() + 52,
        crc32c(reinterpret_cast<const unsigned char*>(rows.data()), rows.size_bytes()));
    put<std::uint32_t>(header.data() + 56, 0);
    put<std::uint32_t>(header.data() + 60, crc32c(header.data(), 60));

    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw VectorFileError("Cannot create vector file: " + temporary.string());
        const std::array<char, 64> padding{};
        output.write(reinterpret_cast<const char*>(header.data()), header.size());
        output.write(reinterpret_cast<const char*>(ids.data()),
                     static_cast<std::streamsize>(ids.size_bytes()));
        output.write(padding.data(),
                     static_cast<std::streamsize>(rows_offset - ids_offset - ids.size_bytes()));
        output.write(reinterpret_cast<const char*>(rows.data()),
                     static_cast<std::streamsize>(rows.size_bytes()));
        output.close();
        if (!output) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            throw VectorFileError("Cannot write vector file: " + temporary.string());
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        throw VectorFileError("Cannot replace vector file: " + path.string());
    }
}

void write_vector_file(const std::filesystem::path& path, const VectorStore& store) {
    if (!store.has_rows()) throw std::invalid_argument("The VectorStore rows were released.");
    const auto ids = store.ids();
    std::vector<EmbeddingValue> rows;
    rows.reserve(store.size() * store.dimension());
    for (std::size_t position = 0; position < store.size(); ++position) {
        const float* row = store.data_by_position(position);
        rows.insert(rows.end(), row, row + store.dimension());
    }
    write_vector_file(path, ids, rows, store.dimension());
}

std::unique_ptr<VectorStore> read_vector_file(const std::filesystem::path& path,
                                              DistanceKind metric) {
    require_little_endian();
    const FileView file(path);
    const auto fail = [&](const std::string& reason) {
        return VectorFileError(path.string() + ": " + reason);
    };
    const unsigned char* data = file.data();
    if (file.size() < kHeaderBytes) throw fail("truncated header.");
    if (std::memcmp(data, kMagic.data(), kMagic.size()) != 0)
        throw fail("not an AetherIndex vector file (bad magic).");
    if (get<std::uint32_t>(data + 60) != crc32c(data, 60)) throw fail("header checksum mismatch.");
    if (get<std::uint32_t>(data + 8) != kVersion) throw fail("unsupported version.");
    if (get<std::uint32_t>(data + 56) != 0) throw fail("unsupported flags.");
    const std::uint64_t dimension = get<std::uint32_t>(data + 12);
    const std::uint64_t count = get<std::uint64_t>(data + 16);
    const std::uint64_t ids_offset = get<std::uint64_t>(data + 24);
    const std::uint64_t rows_offset = get<std::uint64_t>(data + 32);
    const std::uint64_t file_bytes = get<std::uint64_t>(data + 40);
    if (dimension == 0 || count == 0) throw fail("empty dimension or vector count.");
    if (count > std::numeric_limits<VectorId>::max() ||
        count > (std::numeric_limits<std::uint64_t>::max() / 4) / dimension)
        throw fail("vector count overflow.");
    if (ids_offset != kHeaderBytes || rows_offset != align64(ids_offset + 4 * count) ||
        file_bytes != rows_offset + 4 * count * dimension || file_bytes != file.size())
        throw fail("inconsistent layout or truncated file.");
    const auto* id_bytes = data + ids_offset;
    const auto* row_bytes = data + rows_offset;
    if (get<std::uint32_t>(data + 48) != crc32c(id_bytes, 4 * count))
        throw fail("ID checksum mismatch.");
    if (get<std::uint32_t>(data + 52) != crc32c(row_bytes, 4 * count * dimension))
        throw fail("row checksum mismatch.");

    auto store = std::make_unique<VectorStore>(static_cast<std::size_t>(dimension), metric);
    store->reserve(static_cast<std::size_t>(count));
    // rows_offset is 64-byte aligned within a page-aligned mapping, so float access is aligned.
    const auto* rows = reinterpret_cast<const EmbeddingValue*>(row_bytes);
    for (std::uint64_t i = 0; i < count; ++i) {
        VectorId id;
        std::memcpy(&id, id_bytes + 4 * i, 4);
        const auto status = store->insert(id, {rows + i * dimension, dimension});
        if (status == InsertStatus::DuplicateId)
            throw fail("duplicate VectorId " + std::to_string(id) + " at row " + std::to_string(i));
        if (status != InsertStatus::Success)
            throw fail("invalid embedding at row " + std::to_string(i) +
                       " (non-finite or zero norm for cosine).");
    }
    return store;
}

}  // namespace aetherindex
