#include <exception>
#include <iostream>
#include <string_view>
#include <vector>

#include "cli_runner.hpp"

int main(int argc, char* argv[]) {
    try {
        std::vector<std::string_view> arguments;
        for (int i = 1; i < argc; ++i) {
            arguments.emplace_back(argv[i]);
        }
        return aetherindex::cli::run(arguments, std::cout, std::cerr);
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 4;
    }
}
