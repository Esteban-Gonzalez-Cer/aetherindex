#include "aether/io/binary_io.hpp"

#include <algorithm>
#include <iterator>
#include <system_error>

#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#elif defined(__SSE4_2__)
#include <nmmintrin.h>
#endif
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace aetherindex::io {
namespace {

constexpr std::array<char, 8> kSectionMagic{'A', 'E', 'T', 'H', 'S', 'E', 'C', '1'};
constexpr std::uint32_t kSectionVersion = 1;

}  // namespace

std::uint32_t crc32c(const void* data, std::size_t size, std::uint32_t previous) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::uint32_t crc = ~previous;
#if defined(__ARM_FEATURE_CRC32)
    for (; size >= 8; bytes += 8, size -= 8) {
        std::uint64_t word;
        std::memcpy(&word, bytes, 8);
        crc = __crc32cd(crc, word);
    }
    for (; size > 0; ++bytes, --size) crc = __crc32cb(crc, *bytes);
#elif defined(__SSE4_2__)
    std::uint64_t wide = crc;
    for (; size >= 8; bytes += 8, size -= 8) {
        std::uint64_t word;
        std::memcpy(&word, bytes, 8);
        wide = _mm_crc32_u64(wide, word);
    }
    crc = static_cast<std::uint32_t>(wide);
    for (; size > 0; ++bytes, --size) crc = _mm_crc32_u8(crc, *bytes);
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
    for (; size > 0; ++bytes, --size) crc = table[(crc ^ *bytes) & 0xFFu] ^ (crc >> 8);
#endif
    return ~crc;
}

FileView::FileView(const std::filesystem::path& path) : path_(path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error))
        throw IoError("Cannot open file (expected a regular file): " + path.string());
#if !defined(_WIN32)
    descriptor_ = ::open(path.c_str(), O_RDONLY);
    if (descriptor_ < 0) throw IoError("Cannot open file: " + path.string());
    struct stat info{};
    if (::fstat(descriptor_, &info) != 0) {
        ::close(descriptor_);
        throw IoError("Cannot stat file: " + path.string());
    }
    size_ = static_cast<std::size_t>(info.st_size);
    if (size_ > 0) {
        void* mapped = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, descriptor_, 0);
        if (mapped == MAP_FAILED) {
            ::close(descriptor_);
            throw IoError("Cannot map file: " + path.string());
        }
        data_ = static_cast<const unsigned char*>(mapped);
    }
#else
    std::ifstream input(path, std::ios::binary);
    if (!input) throw IoError("Cannot open file: " + path.string());
    buffer_.assign(std::istreambuf_iterator<char>(input), {});
    size_ = buffer_.size();
    data_ = reinterpret_cast<const unsigned char*>(buffer_.data());
#endif
}

FileView::~FileView() {
#if !defined(_WIN32)
    if (data_) ::munmap(const_cast<unsigned char*>(data_), size_);
    if (descriptor_ >= 0) ::close(descriptor_);
#endif
}

void sync_path(const std::filesystem::path& path) {
#if !defined(_WIN32)
    const int descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0) throw IoError("Cannot open for sync: " + path.string());
#if defined(__APPLE__)
    // F_FULLFSYNC is unsupported on some file systems; fall back to fsync there.
    int result = ::fcntl(descriptor, F_FULLFSYNC);
    if (result != 0) result = ::fsync(descriptor);
#else
    const int result = ::fsync(descriptor);
#endif
    ::close(descriptor);
    if (result != 0) throw IoError("Cannot sync to storage: " + path.string());
#else
    static_cast<void>(path);
#endif
}

SectionWriter::SectionWriter(std::ostream& output, std::uint32_t kind)
    : output_(output), kind_(kind) {
    const std::array<char, kSectionHeaderBytes> placeholder{};
    output_.write(placeholder.data(), placeholder.size());
}

void SectionWriter::bytes(const void* data, std::size_t size) {
    if (finished_) throw std::logic_error("SectionWriter is finished.");
    output_.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    crc_ = crc32c(data, size, crc_);
    offset_ += size;
}

void SectionWriter::pad_to_64() {
    static constexpr std::array<char, 64> zeros{};
    bytes(zeros.data(), static_cast<std::size_t>(align64(offset_) - offset_));
}

void SectionWriter::string(std::string_view text) {
    u64(text.size());
    bytes(text.data(), text.size());
}

void SectionWriter::finish() {
    if (finished_) throw std::logic_error("SectionWriter::finish() called twice.");
    std::array<unsigned char, kSectionHeaderBytes> header{};
    std::memcpy(header.data(), kSectionMagic.data(), kSectionMagic.size());
    put<std::uint32_t>(header.data() + 8, kSectionVersion);
    put<std::uint32_t>(header.data() + 12, kind_);
    put<std::uint64_t>(header.data() + 16, kSectionHeaderBytes);
    put<std::uint64_t>(header.data() + 24, offset_ - kSectionHeaderBytes);
    put<std::uint32_t>(header.data() + 32, crc_);
    put<std::uint32_t>(header.data() + 36, 0);
    put<std::uint32_t>(header.data() + 60, crc32c(header.data(), 60));
    const auto end = output_.tellp();
    output_.seekp(end - static_cast<std::streamoff>(offset_));
    output_.write(reinterpret_cast<const char*>(header.data()), header.size());
    output_.seekp(end);
    finished_ = true;
}

SectionReader::SectionReader(const FileView& file, std::uint32_t kind, bool verify_payload)
    : file_(file) {
    const unsigned char* data = file.data();
    if (file.size() < kSectionHeaderBytes) fail("truncated header");
    if (std::memcmp(data, kSectionMagic.data(), kSectionMagic.size()) != 0)
        fail("not an AetherIndex section file (bad magic)");
    if (get<std::uint32_t>(data + 60) != crc32c(data, 60)) fail("header checksum mismatch");
    if (get<std::uint32_t>(data + 8) != kSectionVersion) fail("unsupported section version");
    if (get<std::uint32_t>(data + 12) != kind) fail("unexpected section kind");
    if (get<std::uint32_t>(data + 36) != 0) fail("unsupported flags");
    const auto payload_offset = get<std::uint64_t>(data + 16);
    const auto payload_bytes = get<std::uint64_t>(data + 24);
    if (payload_offset != kSectionHeaderBytes || payload_bytes != file.size() - kSectionHeaderBytes)
        fail("inconsistent size or truncated file");
    end_ = kSectionHeaderBytes + payload_bytes;
    if (verify_payload &&
        get<std::uint32_t>(data + 32) != crc32c(data + kSectionHeaderBytes, payload_bytes))
        fail("payload checksum mismatch");
}

std::string SectionReader::string() {
    const std::uint64_t size = u64();
    if (size > end_ - std::min(offset_, end_)) fail("string exceeds the payload");
    std::string text(reinterpret_cast<const char*>(file_.data() + offset_),
                     static_cast<std::size_t>(size));
    offset_ += size;
    return text;
}

void SectionReader::expect_end() const {
    if (offset_ != end_) fail("unexpected trailing payload bytes");
}

void SectionReader::fail(const std::string& reason) const {
    throw IoError(file_.path().string() + ": " + reason + ".");
}

}  // namespace aetherindex::io
