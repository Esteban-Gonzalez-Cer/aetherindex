#pragma once

#include <iostream>
#include <iosfwd>
#include <span>
#include <string_view>

namespace aetherindex::cli {

// Results/help go to output; diagnostics go to errors. Arguments exclude argv[0].
int run(std::span<const std::string_view> arguments, std::ostream& output, std::ostream& errors);

}  // namespace aetherindex::cli
