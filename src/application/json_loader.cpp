#include "aether/application/json_loader.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_set>

#include "aether/application/search_service.hpp"
#include "json_value.hpp"

namespace aetherindex::detail {
namespace {

using Object = JsonValue::Object;

std::ifstream open_file(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error))
        throw InputDataError("Cannot open input file (expected a readable regular file): " +
                             path.string());
    std::ifstream input(path);
    if (!input) throw InputDataError("Cannot open input file: " + path.string());
    return input;
}

template <class Consumer>
void read_records(const std::filesystem::path& path, Consumer consumer) {
    auto input = open_file(path);
    std::string line;
    std::size_t number = 0;
    while (std::getline(input, line)) {
        ++number;
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        const auto prefix = path.string() + ":line " + std::to_string(number) + ": ";
        try {
            const auto json = parse_json(line);
            const auto* object = std::get_if<Object>(&json.value);
            if (!object) throw std::invalid_argument("expected a JSON object.");
            consumer(*object);
        } catch (const std::invalid_argument& error) {
            throw InputDataError(prefix + error.what());
        }
    }
    if (input.bad() || (input.fail() && !input.eof()))
        throw InputDataError("Cannot read input file: " + path.string());
}

const JsonValue* field(const Object& object, std::string_view name) {
    const auto found = object.find(name);
    return found == object.end() ? nullptr : &found->second;
}

std::string text_field(const Object& object, std::string_view name, bool required = false) {
    const auto* item = field(object, name);
    if (!item && !required) return {};
    const auto* text = item ? std::get_if<std::string>(&item->value) : nullptr;
    if (!text) throw std::invalid_argument("field '" + std::string(name) + "' must be a string.");
    return *text;
}

std::optional<std::string> alias_text(const Object& object,
                                      std::initializer_list<std::string_view> names) {
    std::optional<std::string> result;
    for (auto name : names) {
        if (!field(object, name)) continue;
        auto next = text_field(object, name, true);
        if (result && *result != next) throw std::invalid_argument("conflicting text aliases.");
        result = std::move(next);
    }
    return result;
}

std::optional<DocumentId> numeric_id(std::string_view text) {
    DocumentId result = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size() || std::to_string(result) != text)
        return {};
    return result;
}

std::string identity(const Object& object, std::string_view primary, bool required) {
    std::optional<std::string> result;
    for (auto name : {primary, std::string_view("id")}) {
        const auto* item = field(object, name);
        if (!item) continue;
        std::string next;
        if (const auto* text = std::get_if<std::string>(&item->value))
            next = *text;
        else if (const auto* number = std::get_if<JsonNumber>(&item->value);
                 number && numeric_id(number->text))
            next = number->text;
        else
            throw std::invalid_argument("field '" + std::string(name) +
                                        "' must be a string or uint32 ID.");
        if (next.empty()) throw std::invalid_argument("empty record ID.");
        if (result && *result != next) throw std::invalid_argument("conflicting ID aliases.");
        result = std::move(next);
    }
    if (!result && required)
        throw std::invalid_argument("missing field '" + std::string(primary) + "'.");
    return result.value_or("");
}

std::vector<float> embedding(const Object& object) {
    const auto* item = field(object, "embedding");
    const auto* legacy = field(object, "vector");
    if (item && legacy)
        throw std::invalid_argument("use either 'embedding' or 'vector', not both.");
    if (!item) item = legacy;
    if (!item || std::holds_alternative<std::nullptr_t>(item->value)) return {};
    const auto* array = std::get_if<JsonValue::Array>(&item->value);
    if (!array) throw std::invalid_argument("embedding must be an array of finite floats.");
    std::vector<float> values;
    values.reserve(array->size());
    for (const auto& element : *array) {
        const auto* number = std::get_if<JsonNumber>(&element.value);
        if (!number) throw std::invalid_argument("embedding components must be finite floats.");
        float value = 0;
        const auto& text = number->text;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value))
            throw std::invalid_argument("embedding component is outside the finite float range.");
        values.push_back(value);
    }
    return values;
}

std::vector<std::string> relevant_ids(const Object& object) {
    const auto* item = field(object, "relevant_document_ids");
    if (!item || std::holds_alternative<std::nullptr_t>(item->value)) return {};
    const auto* array = std::get_if<JsonValue::Array>(&item->value);
    if (!array) throw std::invalid_argument("'relevant_document_ids' must be an array of IDs.");
    std::vector<std::string> ids;
    std::unordered_set<std::string> seen;
    for (const auto& element : *array) {
        std::string id;
        if (const auto* text = std::get_if<std::string>(&element.value))
            id = *text;
        else if (const auto* number = std::get_if<JsonNumber>(&element.value);
                 number && numeric_id(number->text))
            id = number->text;
        if (id.empty())
            throw std::invalid_argument(
                "'relevant_document_ids' requires nonempty string/uint32 IDs.");
        if (!seen.insert(id).second)
            throw std::invalid_argument("duplicate relevant document ID '" + id + "'.");
        ids.push_back(std::move(id));
    }
    return ids;
}

Metadata metadata_field(const Object& object) {
    const auto* item = field(object, "metadata");
    if (!item || std::holds_alternative<std::nullptr_t>(item->value)) return {};
    const auto* entries = std::get_if<Object>(&item->value);
    if (!entries) throw std::invalid_argument("'metadata' must be a JSON object.");
    Metadata metadata;
    for (const auto& [key, value] : *entries) {
        if (key.empty()) throw std::invalid_argument("metadata keys must be nonempty.");
        if (const auto* text = std::get_if<std::string>(&value.value))
            metadata.emplace_back(key, *text);
        else if (const auto* number = std::get_if<JsonNumber>(&value.value))
            metadata.emplace_back(key, number->text);
        else if (const auto* flag = std::get_if<bool>(&value.value))
            metadata.emplace_back(key, *flag ? "true" : "false");
        else if (!std::holds_alternative<std::nullptr_t>(value.value))
            throw std::invalid_argument("metadata values must be strings, numbers or booleans.");
    }
    return metadata;
}

// Opaque source IDs never become row numbers in the response. Reserve canonical
// numeric IDs first, then assign unused IDs in source-ID order, not file order.
void assign_document_ids(std::vector<DocumentRecord>& docs) {
    std::unordered_set<DocumentId> used;
    std::vector<std::size_t> opaque;
    for (std::size_t i = 0; i < docs.size(); ++i) {
        if (const auto id = numeric_id(docs[i].id)) {
            docs[i].document_id = *id;
            used.insert(*id);
        } else
            opaque.push_back(i);
    }
    std::sort(opaque.begin(), opaque.end(),
              [&](auto a, auto b) { return docs[a].id < docs[b].id; });
    std::uint64_t next = 0;
    for (auto position : opaque) {
        while (next <= std::numeric_limits<DocumentId>::max() &&
               used.contains(static_cast<DocumentId>(next)))
            ++next;
        if (next > std::numeric_limits<DocumentId>::max())
            throw InputDataError("Too many document identifiers.");
        docs[position].document_id = static_cast<DocumentId>(next++);
    }
}

std::vector<DocumentRecord> documents(const std::filesystem::path& path, bool read_vectors) {
    std::vector<DocumentRecord> docs;
    std::unordered_set<std::string> ids;
    read_records(path, [&](const Object& object) {
        DocumentRecord doc;
        doc.id = identity(object, "document_id", true);
        if (!ids.insert(doc.id).second)
            throw std::invalid_argument("duplicate document ID '" + doc.id + "'.");
        doc.title = text_field(object, "title");
        doc.metadata = metadata_field(object);
        doc.text = text_field(object, "text", !read_vectors);
        if (read_vectors) doc.vector = embedding(object);
        docs.push_back(std::move(doc));
    });
    if (docs.empty()) throw InputDataError("Dataset is empty: " + path.string());
    assign_document_ids(docs);
    return docs;
}

}  // namespace

std::vector<DocumentRecord> load_jsonl_documents(const std::filesystem::path& path) {
    return documents(path, false);
}

std::pair<std::shared_ptr<VectorStore>, std::vector<DocumentRecord>> load_jsonl_corpus(
    const std::string& path, DistanceKind metric) {
    auto docs = documents(path, true);
    std::shared_ptr<VectorStore> store;
    for (std::size_t i = 0; i < docs.size(); ++i) {
        const auto prefix = path + ":document '" + docs[i].id + "': ";
        if (docs[i].vector.empty())
            throw InputDataError(prefix +
                                 "vector search requires a nonempty 'embedding' or 'vector'.");
        if (!store) store = std::make_shared<VectorStore>(docs[i].vector.size(), metric);
        docs[i].vector_id = docs[i].document_id;
        if (store->insert(docs[i].vector_id, docs[i].vector) != InsertStatus::Success)
            throw InputDataError(prefix + "invalid vector dimension or embedding.");
    }
    return {std::move(store), std::move(docs)};
}

std::vector<QueryRecord> load_jsonl_queries(const std::string& path, bool read_vectors) {
    std::vector<QueryRecord> queries;
    std::unordered_set<std::string> ids;
    read_records(path, [&](const Object& object) {
        QueryRecord query;
        query.id = identity(object, "query_id", false);
        if (!query.id.empty() && !ids.insert(query.id).second)
            throw std::invalid_argument("duplicate query ID '" + query.id + "'.");
        auto text = alias_text(object, {"query_text", "query", "text"});
        query.has_text = text.has_value();
        if (text) query.text = std::move(*text);
        if (read_vectors) query.vector = embedding(object);
        // Relevance labels are kept for evaluators and intentionally ignored by retrieval.
        query.relevant_doc_ids = relevant_ids(object);
        queries.push_back(std::move(query));
    });
    return queries;
}

}  // namespace aetherindex::detail
