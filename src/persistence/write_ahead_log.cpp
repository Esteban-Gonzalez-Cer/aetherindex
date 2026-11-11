#include "aether/persistence/write_ahead_log.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

#include "aether/io/binary_io.hpp"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace aetherindex {
namespace {

constexpr std::array<char, 8> kMagic{'A', 'E', 'T', 'H', 'W', 'A', 'L', '1'};
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kHeaderBytes = 16;
constexpr std::size_t kRecordHeaderBytes = 4 + 4 + 8 + 1;  // length, crc, sequence, op.
constexpr std::uint32_t kMaxPayload = 1u << 31;

std::array<unsigned char, kHeaderBytes> header_bytes() {
    std::array<unsigned char, kHeaderBytes> header{};
    std::memcpy(header.data(), kMagic.data(), kMagic.size());
    io::put<std::uint32_t>(header.data() + 8, kVersion);
    io::put<std::uint32_t>(header.data() + 12, io::crc32c(header.data(), 12));
    return header;
}

}  // namespace

void crash_point(std::string_view point) noexcept {
    static const char* const target = std::getenv("AETHER_CRASH_AT");
    if (target && point == target) std::_Exit(86);
}

WriteAheadLog::Contents WriteAheadLog::read(const std::filesystem::path& path) {
    Contents contents;
    std::error_code error;
    if (!std::filesystem::exists(path, error)) return contents;
    std::string bytes;
    {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw io::IoError("Cannot read write-ahead log: " + path.string());
        bytes.assign(std::istreambuf_iterator<char>(input), {});
    }
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    const auto expected = header_bytes();
    // A header shorter than 16 bytes can only come from a crash while creating the log.
    if (bytes.size() < kHeaderBytes) {
        contents.torn = !bytes.empty();
        return contents;
    }
    if (std::memcmp(data, expected.data(), kHeaderBytes) != 0)
        throw io::IoError(path.string() + ": not an AetherIndex write-ahead log (bad header).");
    std::uint64_t offset = kHeaderBytes;
    std::uint64_t expected_sequence = 0;
    while (offset + kRecordHeaderBytes <= bytes.size()) {
        const std::uint32_t length = io::get<std::uint32_t>(data + offset);
        const std::uint32_t crc = io::get<std::uint32_t>(data + offset + 4);
        if (length > kMaxPayload || offset + kRecordHeaderBytes + length > bytes.size()) break;
        if (io::crc32c(data + offset + 8, 9 + length) != crc) break;
        Record record;
        record.sequence = io::get<std::uint64_t>(data + offset + 8);
        record.operation = data[offset + 16];
        if (expected_sequence != 0 && record.sequence != expected_sequence) break;
        record.payload.assign(bytes, offset + kRecordHeaderBytes, length);
        expected_sequence = record.sequence + 1;
        offset += kRecordHeaderBytes + length;
        contents.records.push_back(std::move(record));
    }
    contents.valid_bytes = offset;
    contents.torn = offset != bytes.size();
    return contents;
}

WriteAheadLog::WriteAheadLog(const std::filesystem::path& path, std::uint64_t valid_bytes,
                             std::uint64_t next_sequence, bool sync)
    : path_(path), next_sequence_(next_sequence), sync_(sync) {
#if !defined(_WIN32)
    std::error_code error;
    if (!std::filesystem::exists(path, error) || valid_bytes < kHeaderBytes) {
        // A fresh log: header written to a temporary file and renamed into place.
        const auto header = header_bytes();
        io::write_file_atomically(path, [&](std::ostream& output) {
            output.write(reinterpret_cast<const char*>(header.data()), header.size());
        });
        valid_bytes = kHeaderBytes;
    }
    descriptor_ = ::open(path.c_str(), O_WRONLY);
    if (descriptor_ < 0) throw io::IoError("Cannot open write-ahead log: " + path.string());
    // Drop a torn tail before anything is appended after it.
    if (::ftruncate(descriptor_, static_cast<off_t>(valid_bytes)) != 0 ||
        ::lseek(descriptor_, static_cast<off_t>(valid_bytes), SEEK_SET) < 0) {
        ::close(descriptor_);
        throw io::IoError("Cannot prepare write-ahead log: " + path.string());
    }
    if (sync_) io::sync_path(path);
#else
    static_cast<void>(valid_bytes);
    throw io::IoError("The write-ahead log requires a POSIX file system.");
#endif
}

WriteAheadLog::~WriteAheadLog() {
#if !defined(_WIN32)
    if (descriptor_ >= 0) ::close(descriptor_);
#endif
}

void WriteAheadLog::write_all(const void* data, std::size_t size) {
#if !defined(_WIN32)
    const auto* bytes = static_cast<const char*>(data);
    while (size > 0) {
        const auto written = ::write(descriptor_, bytes, size);
        if (written < 0) {
            if (errno == EINTR) continue;
            throw io::IoError("Cannot append to write-ahead log: " + path_.string());
        }
        bytes += written;
        size -= static_cast<std::size_t>(written);
    }
#else
    static_cast<void>(data);
    static_cast<void>(size);
#endif
}

std::uint64_t WriteAheadLog::append(std::uint8_t operation, std::string_view payload) {
    if (payload.size() > kMaxPayload)
        throw std::length_error("Write-ahead log record exceeds 2 GiB.");
    std::string record(kRecordHeaderBytes + payload.size(), '\0');
    auto* data = reinterpret_cast<unsigned char*>(record.data());
    io::put<std::uint32_t>(data, static_cast<std::uint32_t>(payload.size()));
    io::put<std::uint64_t>(data + 8, next_sequence_);
    data[16] = operation;
    std::memcpy(data + kRecordHeaderBytes, payload.data(), payload.size());
    io::put<std::uint32_t>(data + 4, io::crc32c(data + 8, 9 + payload.size()));
    if (std::getenv("AETHER_CRASH_AT")) {
        // Tests: stop with only part of the record on disk.
        static const bool torn =
            std::string_view(std::getenv("AETHER_CRASH_AT")) == "wal-torn-record";
        if (torn) {
            write_all(record.data(), record.size() / 2);
            crash_point("wal-torn-record");
        }
    }
    write_all(record.data(), record.size());
    crash_point("wal-before-sync");
    if (sync_) io::sync_path(path_);
    return next_sequence_++;
}

void WriteAheadLog::reset(std::uint64_t next_sequence) {
#if !defined(_WIN32)
    if (::ftruncate(descriptor_, static_cast<off_t>(kHeaderBytes)) != 0 ||
        ::lseek(descriptor_, static_cast<off_t>(kHeaderBytes), SEEK_SET) < 0)
        throw io::IoError("Cannot truncate write-ahead log: " + path_.string());
    if (sync_) io::sync_path(path_);
#endif
    next_sequence_ = next_sequence;
}

}  // namespace aetherindex
