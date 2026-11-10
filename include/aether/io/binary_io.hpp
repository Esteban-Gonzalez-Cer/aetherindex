#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace aetherindex::io {

static_assert(std::endian::native == std::endian::little,
              "AetherIndex binary files are little-endian and are read without byte swapping.");

// Any failure to read, write or validate a binary file. Messages name the file.
class IoError : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};

// CRC-32C (Castagnoli). Pass a previous result as `previous` to extend it over more bytes:
// crc32c(b, n2, crc32c(a, n1)) == crc32c of a followed by b. Hardware instructions on
// ARMv8 (CRC extension) and x86 (SSE4.2), a table otherwise.
std::uint32_t crc32c(const void* data, std::size_t size, std::uint32_t previous = 0) noexcept;

template <class T>
void put(unsigned char* out, T value) noexcept {
    static_assert(std::is_integral_v<T>);
    for (std::size_t i = 0; i < sizeof(T); ++i)
        out[i] = static_cast<unsigned char>(static_cast<std::uint64_t>(value) >> (8 * i));
}

template <class T>
T get(const unsigned char* in) noexcept {
    static_assert(std::is_integral_v<T>);
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) value |= std::uint64_t{in[i]} << (8 * i);
    return static_cast<T>(value);
}

constexpr std::uint64_t align64(std::uint64_t value) noexcept {
    return (value + 63) & ~std::uint64_t{63};
}

// Read-only view of a whole regular file: memory-mapped on POSIX (page aligned), a heap
// copy elsewhere. Throws IoError when the file cannot be opened or mapped.
class FileView {
   public:
    explicit FileView(const std::filesystem::path& path);
    FileView(const FileView&) = delete;
    FileView& operator=(const FileView&) = delete;
    ~FileView();

    const unsigned char* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    const std::filesystem::path& path() const noexcept { return path_; }

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

// Flushes a file's data, or a directory's entries, to stable storage (F_FULLFSYNC on
// Apple platforms, where fsync alone does not flush the drive cache). No-op on Windows.
void sync_path(const std::filesystem::path& path);

// Atomically replaces `path` with a file written by `write`: the bytes go to a temporary
// sibling, which is synced, renamed over `path`, and the parent directory is synced. A
// failure (including an exception from `write`) removes the temporary file and leaves
// any existing `path` untouched.
template <class Write>
void write_file_atomically(const std::filesystem::path& path, Write&& write) {
    auto temporary = path;
    temporary += ".tmp";
    try {
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) throw IoError("Cannot create file: " + temporary.string());
            write(output);
            output.close();
            if (!output) throw IoError("Cannot write file: " + temporary.string());
        }
        sync_path(temporary);
        std::filesystem::rename(temporary, path);
        sync_path(path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path());
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

// Section files: a 64-byte header followed by a payload of fields.
//
//   offset  0  magic "AETHSEC1"            offset 32  payload_crc32c u32
//   offset  8  format version u32 (= 1)   offset 36  flags u32 (= 0)
//   offset 12  kind u32 (caller-defined)  offset 40  reserved, zero (20 bytes)
//   offset 16  payload_offset u64 (= 64)  offset 60  header_crc32c u32 (bytes 0..59)
//   offset 24  payload_bytes u64
//
// Payload fields are little-endian scalars, length-prefixed strings, and arrays: a u64
// element count, zero padding to the next 64-byte file offset, then the raw elements.
// Arrays are therefore aligned in a memory-mapped file and can be used in place.
inline constexpr std::size_t kSectionHeaderBytes = 64;

class SectionWriter {
   public:
    // Writes into `output` from its current position, which must be offset 0 of the file.
    SectionWriter(std::ostream& output, std::uint32_t kind);
    void u8(std::uint8_t value) { scalar(value); }
    void u32(std::uint32_t value) { scalar(value); }
    void u64(std::uint64_t value) { scalar(value); }
    void f64(double value) { scalar(std::bit_cast<std::uint64_t>(value)); }
    void f32(float value) { scalar(std::bit_cast<std::uint32_t>(value)); }
    void string(std::string_view text);
    template <class T>
    void array(std::span<const T> values) {
        static_assert(std::is_trivially_copyable_v<T>);
        u64(values.size());
        pad_to_64();
        bytes(values.data(), values.size_bytes());
    }
    // Writes the header (payload size and checksum); call once, after the last field.
    void finish();

   private:
    template <class T>
    void scalar(T value) {
        unsigned char buffer[sizeof(T)];
        put<T>(buffer, value);
        bytes(buffer, sizeof buffer);
    }
    void bytes(const void* data, std::size_t size);
    void pad_to_64();

    std::ostream& output_;
    std::uint32_t kind_;
    std::uint64_t offset_ = kSectionHeaderBytes;
    std::uint32_t crc_ = 0;
    bool finished_ = false;
};

class SectionReader {
   public:
    // Validates the header and kind. With verify_payload, also checks the payload CRC
    // (one pass over the bytes); without it, only structural bounds are checked.
    SectionReader(const FileView& file, std::uint32_t kind, bool verify_payload);
    std::uint8_t u8() { return scalar<std::uint8_t>(); }
    std::uint32_t u32() { return scalar<std::uint32_t>(); }
    std::uint64_t u64() { return scalar<std::uint64_t>(); }
    double f64() { return std::bit_cast<double>(scalar<std::uint64_t>()); }
    float f32() { return std::bit_cast<float>(scalar<std::uint32_t>()); }
    std::string string();
    // A view into the file; valid while the FileView lives.
    template <class T>
    std::span<const T> array() {
        static_assert(std::is_trivially_copyable_v<T>);
        const std::uint64_t count = u64();
        offset_ = align64(offset_);
        if (count > (end_ - std::min(offset_, end_)) / sizeof(T))
            fail("array exceeds the payload");
        const auto* start = reinterpret_cast<const T*>(file_.data() + offset_);
        offset_ += count * sizeof(T);
        return {start, static_cast<std::size_t>(count)};
    }
    template <class T>
    std::vector<T> vector() {
        const auto view = array<T>();
        return {view.begin(), view.end()};
    }
    // Throws unless every payload byte was consumed (a format or version mismatch).
    void expect_end() const;
    [[noreturn]] void fail(const std::string& reason) const;
    std::uint64_t payload_bytes() const noexcept { return end_ - kSectionHeaderBytes; }

   private:
    template <class T>
    T scalar() {
        if (sizeof(T) > end_ - std::min(offset_, end_)) fail("truncated payload");
        const T value = get<T>(file_.data() + offset_);
        offset_ += sizeof(T);
        return value;
    }

    const FileView& file_;
    std::uint64_t offset_ = kSectionHeaderBytes;
    std::uint64_t end_ = kSectionHeaderBytes;
};

}  // namespace aetherindex::io
