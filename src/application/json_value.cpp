#include "json_value.hpp"

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace aetherindex::detail {
namespace {

class Parser {
   public:
    explicit Parser(std::string_view text) : text_(text) {}

    JsonValue parse() {
        auto result = value(0);
        whitespace();
        if (position_ != text_.size()) fail("unexpected trailing content");
        return result;
    }

   private:
    std::string_view text_;
    std::size_t position_ = 0;

    [[noreturn]] void fail(std::string_view message) const {
        throw std::invalid_argument("JSON column " + std::to_string(position_ + 1) + ": " +
                                    std::string(message) + ".");
    }
    char peek() const { return position_ < text_.size() ? text_[position_] : '\0'; }
    char take() {
        if (position_ == text_.size()) fail("unexpected end of input");
        return text_[position_++];
    }
    bool consume(char c) {
        if (peek() != c || position_ == text_.size()) return false;
        ++position_;
        return true;
    }
    void expect(char c) {
        if (!consume(c)) fail(std::string("expected '") + c + "'");
    }
    void whitespace() {
        while (position_ < text_.size() &&
               (peek() == ' ' || peek() == '\t' || peek() == '\r' || peek() == '\n'))
            ++position_;
    }
    static bool digit(char c) { return c >= '0' && c <= '9'; }
    void digits() {
        if (!digit(peek())) fail("expected a digit");
        while (digit(peek())) ++position_;
    }
    std::uint32_t hex4() {
        std::uint32_t code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = take();
            code <<= 4;
            if (c >= '0' && c <= '9')
                code += c - '0';
            else if (c >= 'a' && c <= 'f')
                code += c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                code += c - 'A' + 10;
            else
                fail("invalid Unicode escape");
        }
        return code;
    }
    static void append_codepoint(std::string& result, std::uint32_t code) {
        if (code <= 0x7f)
            result += static_cast<char>(code);
        else if (code <= 0x7ff) {
            result += static_cast<char>(0xc0 | (code >> 6));
            result += static_cast<char>(0x80 | (code & 0x3f));
        } else if (code <= 0xffff) {
            result += static_cast<char>(0xe0 | (code >> 12));
            result += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
            result += static_cast<char>(0x80 | (code & 0x3f));
        } else {
            result += static_cast<char>(0xf0 | (code >> 18));
            result += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
            result += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
            result += static_cast<char>(0x80 | (code & 0x3f));
        }
    }
    void raw_utf8(std::string& result, unsigned char first) {
        const auto start = position_ - 1;
        unsigned count = 0;
        std::uint32_t code = 0, minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            count = 1;
            code = first & 0x1f;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            count = 2;
            code = first & 0x0f;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            count = 3;
            code = first & 0x07;
            minimum = 0x10000;
        } else
            fail("invalid UTF-8 lead byte");
        for (unsigned i = 0; i < count; ++i) {
            const auto next = static_cast<unsigned char>(take());
            if ((next & 0xc0) != 0x80) fail("invalid UTF-8 continuation");
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
            fail("invalid UTF-8 code point");
        result.append(text_.substr(start, position_ - start));
    }
    std::string string() {
        expect('"');
        std::string result;
        for (;;) {
            const auto c = static_cast<unsigned char>(take());
            if (c == '"') return result;
            if (c < 0x20) fail("unescaped control in string");
            if (c >= 0x80) {
                raw_utf8(result, c);
                continue;
            }
            if (c != '\\') {
                result += static_cast<char>(c);
                continue;
            }
            switch (take()) {
                case '"':
                    result += '"';
                    break;
                case '\\':
                    result += '\\';
                    break;
                case '/':
                    result += '/';
                    break;
                case 'b':
                    result += '\b';
                    break;
                case 'f':
                    result += '\f';
                    break;
                case 'n':
                    result += '\n';
                    break;
                case 'r':
                    result += '\r';
                    break;
                case 't':
                    result += '\t';
                    break;
                case 'u': {
                    auto code = hex4();
                    if (code >= 0xd800 && code <= 0xdbff) {
                        expect('\\');
                        expect('u');
                        const auto low = hex4();
                        if (low < 0xdc00 || low > 0xdfff) fail("missing low surrogate");
                        code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                    } else if (code >= 0xdc00 && code <= 0xdfff)
                        fail("unpaired low surrogate");
                    append_codepoint(result, code);
                    break;
                }
                default:
                    fail("unknown string escape");
            }
        }
    }
    JsonValue number() {
        const auto start = position_;
        consume('-');
        if (!consume('0')) digits();
        if (consume('.')) digits();
        if (consume('e') || consume('E')) {
            if (!consume('+')) consume('-');
            digits();
        }
        return {JsonNumber{std::string(text_.substr(start, position_ - start))}};
    }
    JsonValue literal(std::string_view name, JsonValue result) {
        if (text_.substr(position_, name.size()) != name) fail("invalid literal");
        position_ += name.size();
        return result;
    }
    JsonValue value(unsigned depth) {
        whitespace();
        if (depth >= 64 && (peek() == '[' || peek() == '{')) fail("nesting exceeds 64 levels");
        if (peek() == '"') return {string()};
        if (peek() == '-' || digit(peek())) return number();
        if (peek() == 'n') return literal("null", {nullptr});
        if (peek() == 't') return literal("true", {true});
        if (peek() == 'f') return literal("false", {false});
        if (consume('[')) {
            JsonValue::Array items;
            whitespace();
            if (consume(']')) return {std::move(items)};
            for (;;) {
                items.push_back(value(depth + 1));
                whitespace();
                if (consume(']')) return {std::move(items)};
                expect(',');
            }
        }
        if (consume('{')) {
            JsonValue::Object members;
            whitespace();
            if (consume('}')) return {std::move(members)};
            for (;;) {
                whitespace();
                auto key = string();
                whitespace();
                expect(':');
                auto item = value(depth + 1);
                if (!members.emplace(std::move(key), std::move(item)).second)
                    fail("duplicate object key");
                whitespace();
                if (consume('}')) return {std::move(members)};
                expect(',');
            }
        }
        fail("expected a JSON value");
    }
};

}  // namespace

JsonValue parse_json(std::string_view input) { return Parser(input).parse(); }

}  // namespace aetherindex::detail
