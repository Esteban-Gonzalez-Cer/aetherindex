#include "aether/lexical/tokenizer.hpp"

#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "../bm25_test_helpers.hpp"

using namespace aetherindex::lexical;

int main() {
    try {
        const Tokenizer tokenizer;
        const auto check = [&](std::string_view input, std::vector<std::string> expected) {
            require(tokenizer.tokenize(input) == expected, "Unexpected tokenizer output.");
            require(tokenizer.tokenize(input) == expected, "Tokenization is not reproducible.");
        };
        check("MOTOR Industrial", {"motor", "industrial"});
        check("Error E47", {"error", "e47"});
        check("motor motor", {"motor", "motor"});
        check("HTTP_404 RTX-5090 C++ C++17", {"http_404", "rtx-5090", "c++", "c++17"});
        check("", {});
        check(" \t\r\n !!! -- __ +++", {});
        check("(motor), [e47]; /temperatura/.", {"motor", "e47", "temperatura"});
        check("_HTTP_404_ -RTX-5090- x+y foo--bar",
              {"http_404", "rtx-5090", "x", "y", "foo", "bar"});
        check("El controlador presenta Error E47",
              {"el", "controlador", "presenta", "error", "e47"});
        check("ÁRBOL árbol acción", {"Árbol", "árbol", "acción"});
        check("cafe\xCC\x81 café", {"cafe\xCC\x81", "café"});  // No NFC equivalence.
        check(
            "motor\xC2\xA0"
            "bomba—control，e47",
            {"motor", "bomba", "control", "e47"});
        check("温度 センサー 😀", {"温度", "センサー", "😀"});
        check(std::string("motor\0e47", 9), {"motor", "e47"});
        check("\xEF\xBB\xBFmotor", {"motor"});

        // RFC 3629: isolated continuation, invalid leader, overlongs,
        // surrogates, >U+10FFFF, incomplete sequence, invalid continuation.
        for (const std::string invalid : {"\x80", "\xFF", "\xC0\xAF", "\xC1\xBF", "\xE0\x80\xAF",
                                          "\xED\xA0\x80", "\xF0\x80\x80\xAF", "\xF4\x90\x80\x80",
                                          "\xF5\x80\x80\x80", "\xC2", "\xE2\x82", "\xC2X"}) {
            requireBm25Error<std::invalid_argument>([&] { return tokenizer.tokenize(invalid); },
                                                    "Malformed UTF-8 was accepted.");
        }
        std::vector<std::string> owned;
        {
            std::string input = "TEMPORARY e47";
            owned = tokenizer.tokenize(input);
            input.assign(4096, 'x');
        }
        require(owned == std::vector<std::string>{"temporary", "e47"}, "Tokens do not own text.");
        std::cout << "T1: ASCII, technical identifiers, UTF-8 validity and ownership passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
