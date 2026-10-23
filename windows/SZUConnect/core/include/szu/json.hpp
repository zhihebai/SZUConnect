// Strict, bounded JSON (RFC 8259) for portal responses, fixtures and local records.
// No exceptions escape parse(); malformed input yields std::nullopt.
#pragma once
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace szu::json {

class Value {
public:
    enum class Type { null, boolean, number, string, array, object };

    Value() = default;
    static Value boolean(bool value);
    static Value number(double value);
    static Value string(std::string value);
    static Value array();
    static Value object();

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::null; }
    bool is_bool() const { return type_ == Type::boolean; }
    bool is_number() const { return type_ == Type::number; }
    bool is_string() const { return type_ == Type::string; }
    bool is_array() const { return type_ == Type::array; }
    bool is_object() const { return type_ == Type::object; }

    bool as_bool() const { return bool_; }
    double as_number() const { return number_; }
    const std::string& as_string() const { return string_; }

    const std::vector<Value>& items() const { return items_; }
    std::vector<Value>& items() { return items_; }
    void push(Value value);
    std::size_t size() const { return type_ == Type::object ? keys_.size() : items_.size(); }

    // Objects keep insertion order; a repeated key resolves to its last value (like NSDictionary).
    const std::vector<std::string>& keys() const { return keys_; }
    const Value* find(std::string_view key) const;
    Value* find(std::string_view key);
    void set(std::string key, Value value);

    std::optional<std::string> string_at(std::string_view key) const;
    std::optional<double> number_at(std::string_view key) const;
    std::optional<bool> bool_at(std::string_view key) const;

private:
    Type type_ = Type::null;
    bool bool_ = false;
    double number_ = 0;
    std::string string_;
    std::vector<std::string> keys_;
    std::vector<Value> items_;
};

struct ParseOptions {
    std::size_t max_depth = 64;
    std::size_t max_bytes = 4 * 1024 * 1024;
};

std::optional<Value> parse(std::string_view text, const ParseOptions& options = {});
std::string serialize(const Value& value, bool pretty = false);
// JSON string literal like Python json.dumps(ensure_ascii=False) / Swift JSONSerialization
// (.fragmentsAllowed, .withoutEscapingSlashes) for printable input.
std::string quote(std::string_view utf8);
bool valid_utf8(std::string_view text);

}  // namespace szu::json
