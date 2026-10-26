#include "aether/application/jsonl_loader.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

// Nota: Si usas nlohmann/json en tu CMakeLists.txt, reemplaza el parser manual por nlohmann::json
namespace aetherindex {
namespace detail {

static std::vector<float> parse_float_array(const std::string& json_str, const std::string& key) {
    std::vector<float> result;
    size_t pos = json_str.find("\"" + key + "\"");
    if (pos == std::string::npos) return result;

    size_t start_arr = json_str.find('[', pos);
    size_t end_arr = json_str.find(']', start_arr);
    if (start_arr == std::string::npos || end_arr == std::string::npos) return result;

    std::string arr_str = json_str.substr(start_arr + 1, end_arr - start_arr - 1);
    std::stringstream ss(arr_str);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) {
            result.push_back(std::stof(item));
        }
    }
    return result;
}

static std::string parse_string_field(const std::string& json_str, const std::string& key) {
    size_t pos = json_str.find("\"" + key + "\"");
    if (pos == std::string::npos) return "";

    size_t start_val = json_str.find(':', pos) + 1;
    size_t start_quote = json_str.find('"', start_val);
    size_t end_quote = json_str.find('"', start_quote + 1);
    
    if (start_quote == std::string::npos || end_quote == std::string::npos) return "";
    return json_str.substr(start_quote + 1, end_quote - start_quote - 1);
}

std::pair<std::shared_ptr<VectorStore>, std::vector<DocumentRecord>> load_jsonl_corpus(
    const std::string& path) {
    
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Could not open JSONL corpus file: " + path);
    }

    auto store = std::make_shared<VectorStore>();
    std::vector<DocumentRecord> docs;
    std::string line;
    uint32_t internal_id = 0;

    while (std::getline(file, line)) {
        if (line.empty()) continue;

        DocumentRecord doc;
        doc.id = parse_string_field(line, "id");
        doc.text = parse_string_field(line, "text");
        doc.vector = parse_float_array(line, "vector");

        if (!doc.vector.empty()) {
            // Se inserta el embedding en el VectorStore usando un ID numérico interno
            store->add_vector(internal_id++, doc.vector);
        }
        docs.push_back(std::move(doc));
    }

    return {store, docs};
}

std::vector<QueryRecord> load_jsonl_queries(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Could not open JSONL queries file: " + path);
    }

    std::vector<QueryRecord> queries;
    std::string line;

    while (std::getline(file, line)) {
        if (line.empty()) continue;

        QueryRecord q;
        q.id = parse_string_field(line, "id");
        q.text = parse_string_field(line, "query") ;
        if (q.text.empty()) q.text = parse_string_field(line, "text");
        q.vector = parse_float_array(line, "vector");
        
        queries.push_back(std::move(q));
    }

    return queries;
}

}  // namespace detail
}  // namespace aetherindex