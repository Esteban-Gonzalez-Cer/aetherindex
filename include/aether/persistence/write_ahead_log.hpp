#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace aetherindex {

// Fault injection for crash tests: when the environment variable AETHER_CRASH_AT names
// `point`, the process ends immediately (std::_Exit(86)), as a crash would, without
// flushing or unwinding. The variable is read once. Never set it outside tests.
void crash_point(std::string_view point) noexcept;

// An append-only operation log, the redo log of a saved index.
//
//   header  : magic "AETHWAL1" (8), version u32 (= 1), header_crc32c u32 (bytes 0..11)
//   record  : payload_bytes u32, crc32c u32 (of the bytes that follow it), sequence u64,
//             operation u8, payload
//
// Sequences increase by one per record. Reading stops at the first record that is
// truncated, fails its checksum or breaks the sequence: a crash can only damage the end of
// the log, so everything before that point is intact and everything after it was never
// acknowledged.
class WriteAheadLog {
   public:
    struct Record {
        std::uint64_t sequence;
        std::uint8_t operation;
        std::string payload;
    };
    struct Contents {
        std::vector<Record> records;
        std::uint64_t valid_bytes = 0;  // Header plus the intact records.
        bool torn = false;              // Bytes past valid_bytes were discarded.
    };

    // Reads a log. A missing file is an empty log. A damaged header throws io::IoError.
    static Contents read(const std::filesystem::path& path);

    // Opens for appending: creates the file (synced) if absent, otherwise truncates it to
    // valid_bytes, discarding a torn tail. `next_sequence` is the sequence of the next
    // record. With sync, every append is flushed to stable storage before it returns
    // (F_FULLFSYNC on Apple platforms); without it, appends survive a process crash but
    // not necessarily a power loss.
    WriteAheadLog(const std::filesystem::path& path, std::uint64_t valid_bytes,
                  std::uint64_t next_sequence, bool sync);
    ~WriteAheadLog();
    WriteAheadLog(const WriteAheadLog&) = delete;
    WriteAheadLog& operator=(const WriteAheadLog&) = delete;

    // Appends one record and returns its sequence. Durable when it returns (see sync).
    std::uint64_t append(std::uint8_t operation, std::string_view payload);
    // Empties the log (after a checkpoint); the next record gets `next_sequence`.
    void reset(std::uint64_t next_sequence);

    std::uint64_t next_sequence() const noexcept { return next_sequence_; }
    const std::filesystem::path& path() const noexcept { return path_; }

   private:
    void write_all(const void* data, std::size_t size);

    std::filesystem::path path_;
    int descriptor_ = -1;
    std::uint64_t next_sequence_;
    bool sync_;
};

}  // namespace aetherindex
