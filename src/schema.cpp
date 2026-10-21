#include "raybotic/scan/schema.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>

namespace raybotic::scan {

namespace detail {
const std::vector<std::pair<std::string_view, std::string_view>>& embedded_schemas();
}

namespace {

using Json = nlohmann::ordered_json;

bool valid_name(std::string_view name) {
    return !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_';
    });
}

std::string str(const Json& j, const char* key, const std::string& fallback = {}) {
    if (!j.contains(key)) return fallback;
    if (!j[key].is_string()) throw Error(std::string("'") + key + "' must be a string");
    return j[key].get<std::string>();
}

bool flag(const Json& j, const char* key) {
    if (!j.contains(key)) return false;
    if (!j[key].is_boolean()) throw Error(std::string("'") + key + "' must be true or false");
    return j[key].get<bool>();
}

AttributeSpec parse_attribute(const std::string& name, const Json& j) {
    if (!valid_name(name)) throw Error("invalid attribute name '" + name + "'");
    if (!j.is_object()) throw Error("attribute '" + name + "' must be an object");
    AttributeSpec a;
    a.name = name;
    a.dtype = dtype_from_string(str(j, "dtype"));
    a.array = flag(j, "array");
    a.required = flag(j, "required");
    a.units = str(j, "units");
    a.description = str(j, "description");
    return a;
}

FieldSpec parse_field(const std::string& name, const Json& j) {
    if (!valid_name(name)) throw Error("invalid field name '" + name + "'");
    if (name == "time") throw Error("'time' is reserved for the time axis");
    if (!j.is_object()) throw Error("field '" + name + "' must be an object");
    FieldSpec f;
    f.name = name;
    // "dtype": "uint16" or a list of allowed types, the first being the default.
    if (j.contains("dtype") && j["dtype"].is_array()) {
        for (const auto& d : j["dtype"]) {
            if (!d.is_string()) throw Error("field '" + name + "': dtype list must hold names");
            f.dtypes.push_back(dtype_from_string(d.get<std::string>()));
        }
        if (f.dtypes.empty()) throw Error("field '" + name + "': empty dtype list");
    } else {
        f.dtypes.push_back(dtype_from_string(str(j, "dtype")));
    }
    f.dtype = f.dtypes.front();
    for (auto d : f.dtypes) {
        if (d == DType::String) throw Error("field '" + name + "': strings are attributes only");
    }
    if (j.contains("shape")) {
        if (!j["shape"].is_array()) throw Error("field '" + name + "': shape must be a list");
        for (const auto& d : j["shape"]) {
            DimSpec dim;
            if (d.is_number_unsigned() && d.get<std::uint64_t>() > 0) {
                dim.size = d.get<std::uint64_t>();
            } else if (d.is_string() && valid_name(d.get<std::string>())) {
                dim.name = d.get<std::string>();
            } else {
                throw Error("field '" + name + "': a dimension is a positive size or a name");
            }
            f.shape.push_back(std::move(dim));
        }
    }
    f.optional = flag(j, "optional");
    f.units = str(j, "units");
    f.description = str(j, "description");
    return f;
}

SeriesSpec parse_series(const std::string& name, const Json& j) {
    if (!valid_name(name)) throw Error("invalid series name '" + name + "'");
    if (!j.is_object()) throw Error("series '" + name + "' must be an object");
    SeriesSpec s;
    s.name = name;
    const std::string storage = str(j, "storage", "inline");
    if (storage == "inline") {
        s.storage = Storage::Inline;
    } else if (storage == "segmented") {
        s.storage = Storage::Segmented;
    } else {
        throw Error("series '" + name + "': storage must be inline or segmented");
    }
    s.description = str(j, "description");
    if (!j.contains("fields") || !j["fields"].is_object() || j["fields"].empty()) {
        throw Error("series '" + name + "' needs at least one field");
    }
    for (const auto& [field_name, field] : j["fields"].items()) {
        s.fields.push_back(parse_field(field_name, field));
    }
    return s;
}

template <class T>
void merge_by_name(std::vector<T>& into, T item) {
    auto it = std::find_if(into.begin(), into.end(),
                           [&](const T& x) { return x.name == item.name; });
    if (it != into.end()) {
        *it = std::move(item);
    } else {
        into.push_back(std::move(item));
    }
}

Schema parse(std::string_view text, const SchemaRegistry* registry) {
    Json j;
    try {
        j = Json::parse(text);
    } catch (const nlohmann::json::exception& e) {
        throw Error(std::string("schema is not valid JSON: ") + e.what());
    }
    if (!j.is_object()) throw Error("schema must be a JSON object");

    Schema s;
    const std::string name = str(j, "schema");
    try {
        if (name.empty()) throw Error("'schema' (the name) is missing");
        if (!j.contains("version") || !j["version"].is_number_unsigned() ||
            j["version"].get<std::uint64_t>() == 0) {
            throw Error("'version' must be a positive integer");
        }

        if (j.contains("extends")) {
            if (registry == nullptr) throw Error("'extends' is not allowed here");
            s = registry->get(str(j, "extends"));
        }
        s.name = name;
        s.version = j["version"].get<std::uint32_t>();
        s.category = str(j, "category", s.category);
        s.description = str(j, "description");
        s.abstract = flag(j, "abstract");
        if (!valid_name(s.category)) throw Error("'category' is missing or invalid");

        if (j.contains("attributes")) {
            if (!j["attributes"].is_object()) throw Error("'attributes' must be an object");
            for (const auto& [attr_name, attr] : j["attributes"].items()) {
                merge_by_name(s.attributes, parse_attribute(attr_name, attr));
            }
        }
        if (j.contains("series")) {
            if (!j["series"].is_object()) throw Error("'series' must be an object");
            for (const auto& [series_name, series] : j["series"].items()) {
                merge_by_name(s.series, parse_series(series_name, series));
            }
        }
    } catch (const Error& e) {
        throw Error("schema " + (name.empty() ? std::string("?") : name) + ": " + e.what());
    }
    return s;
}

std::vector<std::filesystem::path> schema_path_from_env() {
    std::vector<std::filesystem::path> dirs;
#ifdef _WIN32
    constexpr char sep = ';';
    char* value = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&value, &len, "RBSCAN_SCHEMA_PATH") != 0 || value == nullptr) return dirs;
    std::string env(value);
    std::free(value);
#else
    constexpr char sep = ':';
    const char* value = std::getenv("RBSCAN_SCHEMA_PATH");
    if (value == nullptr) return dirs;
    std::string env(value);
#endif
    std::stringstream in(env);
    std::string item;
    while (std::getline(in, item, sep)) {
        if (!item.empty()) dirs.emplace_back(item);
    }
    return dirs;
}

}  // namespace

const FieldSpec* SeriesSpec::field(std::string_view field_name) const {
    for (const auto& f : fields) {
        if (f.name == field_name) return &f;
    }
    return nullptr;
}

std::string Schema::id() const { return name + "/" + std::to_string(version); }

const AttributeSpec* Schema::attribute(std::string_view attr_name) const {
    for (const auto& a : attributes) {
        if (a.name == attr_name) return &a;
    }
    return nullptr;
}

const SeriesSpec* Schema::find_series(std::string_view series_name) const {
    for (const auto& s : series) {
        if (s.name == series_name) return &s;
    }
    return nullptr;
}

const SeriesSpec& Schema::get_series(std::string_view series_name) const {
    if (const auto* s = find_series(series_name)) return *s;
    throw Error("schema " + id() + " has no series '" + std::string(series_name) + "'");
}

std::string Schema::to_json() const {
    Json j;
    j["schema"] = name;
    j["version"] = version;
    j["category"] = category;
    if (abstract) j["abstract"] = true;
    j["description"] = description;
    Json attrs = Json::object();
    for (const auto& a : attributes) {
        Json ja;
        ja["dtype"] = std::string(to_string(a.dtype));
        if (a.array) ja["array"] = true;
        if (a.required) ja["required"] = true;
        if (!a.units.empty()) ja["units"] = a.units;
        if (!a.description.empty()) ja["description"] = a.description;
        attrs[a.name] = ja;
    }
    j["attributes"] = attrs;
    Json series_json = Json::object();
    for (const auto& s : series) {
        Json js;
        js["storage"] = s.storage == Storage::Segmented ? "segmented" : "inline";
        if (!s.description.empty()) js["description"] = s.description;
        Json fields = Json::object();
        for (const auto& f : s.fields) {
            Json jf;
            if (f.dtypes.size() > 1) {
                Json list = Json::array();
                for (auto d : f.dtypes) list.push_back(std::string(to_string(d)));
                jf["dtype"] = list;
            } else {
                jf["dtype"] = std::string(to_string(f.dtype));
            }
            if (!f.shape.empty()) {
                Json shape = Json::array();
                for (const auto& d : f.shape) {
                    if (d.fixed()) {
                        shape.push_back(d.size);
                    } else {
                        shape.push_back(d.name);
                    }
                }
                jf["shape"] = shape;
            }
            if (f.optional) jf["optional"] = true;
            if (!f.units.empty()) jf["units"] = f.units;
            if (!f.description.empty()) jf["description"] = f.description;
            fields[f.name] = jf;
        }
        js["fields"] = fields;
        series_json[s.name] = js;
    }
    j["series"] = series_json;
    return j.dump(2);
}

Schema parse_schema(std::string_view json) { return parse(json, nullptr); }

struct SchemaRegistry::Impl {
    mutable std::mutex mutex;
    std::map<std::string, std::unique_ptr<Schema>, std::less<>> schemas;
};

SchemaRegistry::SchemaRegistry() : impl_(std::make_unique<Impl>()) {}
SchemaRegistry::~SchemaRegistry() = default;

SchemaRegistry& SchemaRegistry::global() {
    static SchemaRegistry* registry = [] {
        auto* r = new SchemaRegistry();
        // Built-in schemas may extend each other in any file order.
        std::vector<std::string_view> pending;
        for (const auto& [file, text] : detail::embedded_schemas()) pending.push_back(text);
        while (!pending.empty()) {
            std::vector<std::string_view> failed;
            std::string first_error;
            for (auto text : pending) {
                try {
                    r->add_json(text);
                } catch (const Error& e) {
                    failed.push_back(text);
                    if (first_error.empty()) first_error = e.what();
                }
            }
            if (failed.size() == pending.size()) {
                throw Error("built-in schemas are broken: " + first_error);
            }
            pending = std::move(failed);
        }
        for (const auto& dir : schema_path_from_env()) {
            if (std::filesystem::is_directory(dir)) r->add_directory(dir);
        }
        return r;
    }();
    return *registry;
}

const Schema& SchemaRegistry::add_json(std::string_view json) {
    Schema schema = parse(json, this);
    std::scoped_lock lock(impl_->mutex);
    const std::string id = schema.id();
    if (auto it = impl_->schemas.find(id); it != impl_->schemas.end()) {
        if (it->second->to_json() == schema.to_json()) return *it->second;
        throw Error("a different schema " + id + " is already registered");
    }
    auto& slot = impl_->schemas[id];
    slot = std::make_unique<Schema>(std::move(schema));
    return *slot;
}

void SchemaRegistry::add_directory(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    std::vector<std::pair<std::filesystem::path, std::string>> pending;
    for (const auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        pending.emplace_back(f, text.str());
    }
    while (!pending.empty()) {
        decltype(pending) failed;
        std::string first_error;
        for (auto& [file, text] : pending) {
            try {
                add_json(text);
            } catch (const Error& e) {
                if (first_error.empty()) first_error = file.string() + ": " + e.what();
                failed.emplace_back(file, std::move(text));
            }
        }
        if (failed.size() == pending.size()) throw Error(first_error);
        pending = std::move(failed);
    }
}

const Schema* SchemaRegistry::find(std::string_view id) const {
    std::scoped_lock lock(impl_->mutex);
    if (id.find('/') != std::string_view::npos) {
        auto it = impl_->schemas.find(id);
        return it == impl_->schemas.end() ? nullptr : it->second.get();
    }
    const Schema* best = nullptr;
    for (const auto& [key, schema] : impl_->schemas) {
        if (schema->name == id && (best == nullptr || schema->version > best->version)) {
            best = schema.get();
        }
    }
    return best;
}

const Schema& SchemaRegistry::get(std::string_view id) const {
    if (const auto* s = find(id)) return *s;
    throw Error("unknown schema '" + std::string(id) + "'");
}

std::vector<std::string> SchemaRegistry::ids() const {
    std::scoped_lock lock(impl_->mutex);
    std::vector<std::string> out;
    for (const auto& [key, schema] : impl_->schemas) out.push_back(key);
    return out;
}

}  // namespace raybotic::scan
