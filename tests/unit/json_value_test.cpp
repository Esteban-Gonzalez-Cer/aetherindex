#include "../../src/application/json_value.hpp"

#include <exception>
#include <iostream>
#include <string>

#include "../test_helpers.hpp"

using namespace aetherindex::detail;

int main() {
    try {
        const auto parsed = parse_json(
            R"({"text":"motor \"E47\"\nC:\\logs\t\u00e1\ud83d\ude80","metadata":{"valid":true,"other":[null,false,1e999]}})");
        const auto& object = std::get<JsonValue::Object>(parsed.value);
        require(std::get<std::string>(object.at("text").value) == "motor \"E47\"\nC:\\logs\tá🚀",
                "JSON escapes, Unicode or supplementary characters were corrupted.");
        for (const auto text : {"0", "-0", "-0.1", "1.25", "1e+2", "-2E-3", "1e999"})
            require(std::get<JsonNumber>(parse_json(text).value).text == text,
                    "JSON number syntax was not preserved.");
        require(std::get<std::string>(parse_json("\"árbol🚀\"").value) == "árbol🚀",
                "Valid UTF-8 must be preserved.");
        require(std::get<std::string>(parse_json(R"("\u0000")").value).size() == 1,
                "Escaped NUL is a valid JSON string character.");
        for (const auto text : {"",
                                "NaN",
                                "Infinity",
                                "-Infinity",
                                "1oops",
                                "01",
                                "+1",
                                "1.",
                                ".1",
                                "1e",
                                "1e+",
                                "--1",
                                "truefalse",
                                "{}{}",
                                "[1,]",
                                "{\"a\":1,}",
                                "{\"a\":1,\"a\":2}",
                                "{\"a\":{\"b\":1,\"b\":2}}",
                                "\"unterminated",
                                "\"raw\nline\"",
                                R"("\q")",
                                R"("\ud800")",
                                R"("\udc00")",
                                R"("\ud800\u0041")",
                                "{x:1}",
                                "[null false]"}) {
            requireInvalidArgument([&] { static_cast<void>(parse_json(text)); },
                                   "Malformed JSON must not be accepted as a partial value.");
        }
        for (const auto& bytes :
             {std::string("\xc0\xaf", 2), std::string("\xe0\x80\xaf", 3),
              std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4),
              std::string("\xc2", 1), std::string(1, '\0')}) {
            requireInvalidArgument([&] { static_cast<void>(parse_json('"' + bytes + '"')); },
                                   "Invalid UTF-8 or a raw control was accepted.");
        }
        static_cast<void>(parse_json(std::string(64, '[') + "0" + std::string(64, ']')));
        requireInvalidArgument(
            [&] {
                static_cast<void>(parse_json(std::string(65, '[') + "0" + std::string(65, ']')));
            },
            "The 64-container boundary must be enforced exactly.");
        const std::string deep = std::string(1000, '[') + "0" + std::string(1000, ']');
        requireInvalidArgument([&] { static_cast<void>(parse_json(deep)); },
                               "Deep metadata must fail without overflowing the stack.");
        std::cout
            << "JSON grammar, complete consumption, UTF-8, escapes, duplicates and depth passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
