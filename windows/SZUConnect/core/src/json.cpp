#include "szu/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace szu::json {

Value Value::boolean(bool value) { Value v; v.type_ = Type::boolean; v.bool_ = value; return v; }
Value Value::number(double value) { Value v; v.type_ = Type::number; v.number_ = value; return v; }
Value Value::string(std::string value) { Value v; v.type_ = Type::string; v.string_ = std::move(value); return v; }
Value Value::array() { Value v; v.type_ = Type::array; return v; }
Value Value::object() { Value v; v.type_ = Type::object; return v; }

void Value::push(Value value) {
    if (type_ == Type::null) type_ = Type::array;
    if (type_ == Type::array) items_.push_back(std::move(value));
}

const Value* Value::find(std::string_view key) const {
    if (type_ != Type::object) return nullptr;
    for (std::size_t i = keys_.size(); i-- > 0;) {
        if (keys_[i] == key) return &items_[i];
    }
    return nullptr;
}

Value* Value::find(std::string_view key) {
    return const_cast<Value*>(static_cast<const Value*>(this)->find(key));
}

void Value::set(std::string key, Value value) {
    if (type_ == Type::null) type_ = Type::object;
    if (type_ != Type::object) return;
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) { items_[i] = std::move(value); return; }
    }
    keys_.push_back(std::move(key));
    items_.push_back(std::move(value));
}

std::optional<std::string> Value::string_at(std::string_view key) const {
    const Value* v = find(key);
    if (!v || !v->is_string()) return std::nullopt;
    return v->as_string();
}
std::optional<double> Value::number_at(std::string_view key) const {
    const Value* v = find(key);
    if (!v || !v->is_number()) return std::nullopt;
    return v->as_number();
}
std::optional<bool> Value::bool_at(std::string_view key) const {
    const Value* v = find(key);
    if (!v || !v->is_bool()) return std::nullopt;
    return v->as_bool();
}

bool valid_utf8(std::string_view text) {
    const auto* s = reinterpret_cast<const unsigned char*>(text.data());
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        const unsigned char c = s[i];
        if (c < 0x80) { ++i; continue; }
        std::size_t len;
        unsigned int cp;
        if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1Fu; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0Fu; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07u; }
        else return false;
        if (i + len > n) return false;
        for (std::size_t k = 1; k < len; ++k) {
            if ((s[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (s[i + k] & 0x3Fu);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF))) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        i += len;
    }
    return true;
}

namespace {

void append_utf8(std::string& out, unsigned int cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

class Parser {
public:
    Parser(std::string_view text, const ParseOptions& options) : text_(text), options_(options) {}

    std::optional<Value> run() {
        skip_ws();
        Value value;
        if (!parse_value(value, 0)) return std::nullopt;
        skip_ws();
        if (pos_ != text_.size()) return std::nullopt;
        return value;
    }

private:
    std::string_view text_;
    const ParseOptions& options_;
    std::size_t pos_ = 0;

    bool eof() const { return pos_ >= text_.size(); }
    char peek() const { return text_[pos_]; }
    void skip_ws() {
        while (!eof() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) ++pos_;
    }
    bool literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }
    static bool digit(char c) { return c >= '0' && c <= '9'; }

    bool parse_value(Value& out, std::size_t depth) {
        if (depth > options_.max_depth || eof()) return false;
        const char c = peek();
        if (c == '{') return parse_object(out, depth + 1);
        if (c == '[') return parse_array(out, depth + 1);
        if (c == '"') {
            std::string s;
            if (!parse_string(s)) return false;
            out = Value::string(std::move(s));
            return true;
        }
        if (c == 't') { if (!literal("true")) return false; out = Value::boolean(true); return true; }
        if (c == 'f') { if (!literal("false")) return false; out = Value::boolean(false); return true; }
        if (c == 'n') { if (!literal("null")) return false; out = Value(); return true; }
        return parse_number(out);
    }

    bool parse_object(Value& out, std::size_t depth) {
        out = Value::object();
        ++pos_;
        skip_ws();
        if (!eof() && peek() == '}') { ++pos_; return true; }
        while (true) {
            skip_ws();
            if (eof() || peek() != '"') return false;
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (eof() || peek() != ':') return false;
            ++pos_;
            skip_ws();
            Value value;
            if (!parse_value(value, depth)) return false;
            out.set(std::move(key), std::move(value));
            skip_ws();
            if (eof()) return false;
            if (peek() == ',') { ++pos_; continue; }
            if (peek() == '}') { ++pos_; return true; }
            return false;
        }
    }

    bool parse_array(Value& out, std::size_t depth) {
        out = Value::array();
        ++pos_;
        skip_ws();
        if (!eof() && peek() == ']') { ++pos_; return true; }
        while (true) {
            skip_ws();
            Value value;
            if (!parse_value(value, depth)) return false;
            out.push(std::move(value));
            skip_ws();
            if (eof()) return false;
            if (peek() == ',') { ++pos_; continue; }
            if (peek() == ']') { ++pos_; return true; }
            return false;
        }
    }

    bool hex4(unsigned int& value) {
        if (pos_ + 4 > text_.size()) return false;
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<unsigned int>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned int>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned int>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool parse_string(std::string& out) {
        ++pos_;
        while (!eof()) {
            const unsigned char c = static_cast<unsigned char>(text_[pos_++]);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') { out.push_back(static_cast<char>(c)); continue; }
            if (eof()) return false;
            const char e = text_[pos_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned int cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        unsigned int low;
                        if (pos_ + 2 > text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') return false;
                        pos_ += 2;
                        if (!hex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return false;
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool parse_number(Value& out) {
        const std::size_t start = pos_;
        if (!eof() && peek() == '-') ++pos_;
        if (eof()) return false;
        if (peek() == '0') ++pos_;
        else if (peek() >= '1' && peek() <= '9') { while (!eof() && digit(peek())) ++pos_; }
        else return false;
        if (!eof() && peek() == '.') {
            ++pos_;
            if (eof() || !digit(peek())) return false;
            while (!eof() && digit(peek())) ++pos_;
        }
        if (!eof() && (peek() == 'e' || peek() == 'E')) {
            ++pos_;
            if (!eof() && (peek() == '+' || peek() == '-')) ++pos_;
            if (eof() || !digit(peek())) return false;
            while (!eof() && digit(peek())) ++pos_;
        }
        const std::string token(text_.substr(start, pos_ - start));
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(value)) return false;
        out = Value::number(value);
        return true;
    }
};

void serialize_into(std::string& out, const Value& value, bool pretty, int indent) {
    auto newline = [&](int level) {
        if (!pretty) return;
        out.push_back('\n');
        out.append(static_cast<std::size_t>(level) * 2, ' ');
    };
    switch (value.type()) {
        case Value::Type::null: out += "null"; break;
        case Value::Type::boolean: out += value.as_bool() ? "true" : "false"; break;
        case Value::Type::number: {
            const double d = value.as_number();
            char buffer[64];
            if (std::floor(d) == d && std::fabs(d) < 9.0e15) std::snprintf(buffer, sizeof buffer, "%.0f", d);
            else std::snprintf(buffer, sizeof buffer, "%.17g", d);
            out += buffer;
            break;
        }
        case Value::Type::string: out += quote(value.as_string()); break;
        case Value::Type::array: {
            out.push_back('[');
            for (std::size_t i = 0; i < value.items().size(); ++i) {
                if (i) out.push_back(',');
                newline(indent + 1);
                serialize_into(out, value.items()[i], pretty, indent + 1);
            }
            if (!value.items().empty()) newline(indent);
            out.push_back(']');
            break;
        }
        case Value::Type::object: {
            out.push_back('{');
            for (std::size_t i = 0; i < value.keys().size(); ++i) {
                if (i) out.push_back(',');
                newline(indent + 1);
                out += quote(value.keys()[i]);
                out += pretty ? ": " : ":";
                serialize_into(out, value.items()[i], pretty, indent + 1);
            }
            if (!value.keys().empty()) newline(indent);
            out.push_back('}');
            break;
        }
    }
}

}  // namespace

std::optional<Value> parse(std::string_view text, const ParseOptions& options) {
    if (text.size() > options.max_bytes || !valid_utf8(text)) return std::nullopt;
    Parser parser(text, options);
    return parser.run();
}

std::string quote(std::string_view utf8) {
    std::string out;
    out.reserve(utf8.size() + 2);
    out.push_back('"');
    for (const unsigned char c : utf8) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
                    out += buffer;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
    return out;
}

std::string serialize(const Value& value, bool pretty) {
    std::string out;
    serialize_into(out, value, pretty, 0);
    return out;
}

}  // namespace szu::json
