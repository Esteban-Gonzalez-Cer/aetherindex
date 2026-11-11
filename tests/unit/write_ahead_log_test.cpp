// WriteAheadLog: records round-trip; reading stops at the first damaged record (truncation,
// checksum, sequence gap) and reports the torn tail; reopening for append discards it.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "../test_helpers.hpp"
#include "aether/io/binary_io.hpp"
#include "aether/persistence/write_ahead_log.hpp"

namespace {

using namespace aetherindex;
namespace fs = std::filesystem;

std::string bytes_of(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

void write_bytes(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const fs::path dir = argv[1];
    fs::remove_all(dir);
    fs::create_directories(dir);
    const auto path = dir / "wal.log";
    try {
        require(WriteAheadLog::read(path).records.empty(), "A missing log is empty.");
        {
            WriteAheadLog log(path, 0, 7, false);
            require(log.append(1, "first") == 7 && log.append(2, "") == 8 &&
                        log.append(3, std::string(1000, 'x')) == 9,
                    "Sequences must increase by one from next_sequence.");
        }
        auto contents = WriteAheadLog::read(path);
        require(contents.records.size() == 3 && !contents.torn &&
                    contents.records[0].sequence == 7 && contents.records[0].operation == 1 &&
                    contents.records[0].payload == "first" &&
                    contents.records[2].payload == std::string(1000, 'x'),
                "Records must round-trip.");
        const std::string intact = bytes_of(path);

        // Truncated tail: the last record is dropped and reported.
        write_bytes(path, intact.substr(0, intact.size() - 10));
        contents = WriteAheadLog::read(path);
        require(contents.records.size() == 2 && contents.torn, "A truncated record is a torn tail.");
        {
            WriteAheadLog log(path, contents.valid_bytes, 9, false);
            log.append(4, "after");
        }
        contents = WriteAheadLog::read(path);
        require(contents.records.size() == 3 && !contents.torn &&
                    contents.records[2].payload == "after",
                "Reopening must discard the torn tail before appending.");

        // A flipped byte in the middle record stops reading there.
        auto damaged = intact;
        damaged[16 + 17 + 5 + 17 - 1] ^= 1;  // Operation byte of the second record.
        write_bytes(path, damaged);
        contents = WriteAheadLog::read(path);
        require(contents.records.size() == 1 && contents.torn, "A checksum failure stops reading.");

        // A sequence gap stops reading: append under a wrong sequence by hand.
        write_bytes(path, intact);
        {
            WriteAheadLog log(path, intact.size(), 42, false);
            log.append(1, "gap");
        }
        contents = WriteAheadLog::read(path);
        require(contents.records.size() == 3 && contents.torn, "A sequence gap stops reading.");

        // reset empties the log; a short header (crash while creating) is an empty log.
        {
            WriteAheadLog log(path, WriteAheadLog::read(path).valid_bytes, 10, false);
            log.reset(100);
            log.append(1, "x");
        }
        contents = WriteAheadLog::read(path);
        require(contents.records.size() == 1 && contents.records[0].sequence == 100, "reset");
        write_bytes(path, intact.substr(0, 5));
        contents = WriteAheadLog::read(path);
        require(contents.records.empty() && contents.torn, "A partial header is an empty log.");

        // Something that is not a log is an error, not an empty log.
        write_bytes(path, std::string(64, 'z'));
        bool rejected = false;
        try {
            static_cast<void>(WriteAheadLog::read(path));
        } catch (const io::IoError&) {
            rejected = true;
        }
        require(rejected, "A foreign file must be rejected.");
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Write-ahead log tests passed.\n";
    return 0;
}
