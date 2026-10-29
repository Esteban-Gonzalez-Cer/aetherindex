#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace aetherindex::detail {

// Application-private JSON representation. Numbers retain their syntax until a
// schema field needs conversion, so unrelated metadata cannot overflow a float.
struct JsonNumber {
    std::string text;
};

struct JsonValue {
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue, std::less<>>;
    std::variant<std::nullptr_t, bool, JsonNumber, std::string, Array, Object> value;
};

// Exactly one JSON value, valid UTF-8/escapes, unique object keys, <=64 levels.
// Throws invalid_argument with a column; the file loader adds path and line.
JsonValue parse_json(std::string_view input);

}  // namespace aetherindex::detail
