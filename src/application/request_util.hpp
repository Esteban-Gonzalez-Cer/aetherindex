#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace aetherindex::detail {

inline std::string lower_extension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    for (auto& c : extension)
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return extension;
}

inline bool jsonl_path(const std::filesystem::path& path) {
    return lower_extension(path) == ".jsonl";
}
inline bool avec_path(const std::filesystem::path& path) {
    return lower_extension(path) == ".avec";
}

// A JSON string literal with the escapes JSON requires.
inline std::string json_string(std::string_view text) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out += hex[c >> 4];
                    out += hex[c & 0xf];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "\"";
}

}  // namespace aetherindex::detail
