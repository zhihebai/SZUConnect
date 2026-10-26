#include "szu/portal_wire.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <map>
#include <set>
#include <utility>

namespace szu::wire {
namespace {

std::string ascii_lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool contains(std::string_view haystack, std::string_view needle) { return haystack.find(needle) != std::string_view::npos; }

bool is_regex_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

std::string trim_ascii(std::string_view text) {
    std::size_t b = 0, e = text.size();
    while (b < e && is_regex_space(text[b])) ++b;
    while (e > b && is_regex_space(text[e - 1])) --e;
    return std::string(text.substr(b, e - b));
}

}  // namespace

namespace {
struct ConfigToken {
    enum class Kind { word, string, punctuation } kind;
    std::string text;
    char punctuation = 0;
    bool escaped = false;
    bool word(std::string_view value) const { return kind == Kind::word && text == value; }
    bool punct(char value) const { return kind == Kind::punctuation && punctuation == value; }
};

bool config_word_byte(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') || c == '_' || c == '$' || c >= 128;
}

bool config_digit(char c) { return c >= '0' && c <= '9'; }

int config_hex(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
        c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

std::optional<unsigned> config_unicode_escape(std::string_view script, std::size_t& index) {
    const bool braced = index < script.size() && script[index] == '{';
    if (braced) ++index;
    unsigned code = 0; int digits = 0;
    while (index < script.size() && (braced ? script[index] != '}' : digits < 4)) {
        const int value = config_hex(script[index++]);
        if (value < 0 || ++digits > 6) return std::nullopt;
        code = code * 16 + static_cast<unsigned>(value);
    }
    if ((!braced && digits != 4) || (braced && (!digits || index == script.size() || script[index++] != '}')) || code > 0x10ffff)
        return std::nullopt;
    return code;
}

std::optional<std::vector<ConfigToken>> config_tokens(std::string_view script) {
    if (script.size() > 65536) return std::nullopt;
    std::vector<ConfigToken> tokens;
    std::size_t i = 0;
    const auto unicode_line_end = [&](std::size_t offset) {
        return offset + 2 < script.size() && static_cast<unsigned char>(script[offset]) == 0xe2 &&
            static_cast<unsigned char>(script[offset + 1]) == 0x80 &&
            (static_cast<unsigned char>(script[offset + 2]) == 0xa8 || static_cast<unsigned char>(script[offset + 2]) == 0xa9);
    };
    const auto unicode_space_length = [&](std::size_t offset) -> std::size_t {
        const auto byte = [&](std::size_t index) { return static_cast<unsigned char>(script[index]); };
        if (offset + 1 < script.size() && byte(offset) == 0xc2 && byte(offset + 1) == 0xa0) return 2;
        if (offset + 2 >= script.size()) return 0;
        const auto first = byte(offset), second = byte(offset + 1), third = byte(offset + 2);
        if ((first == 0xe1 && second == 0x9a && third == 0x80) ||
            (first == 0xe2 && second == 0x80 && ((third >= 0x80 && third <= 0x8a) || third == 0xa8 || third == 0xa9 || third == 0xaf)) ||
            (first == 0xe2 && second == 0x81 && third == 0x9f) ||
            (first == 0xe3 && second == 0x80 && third == 0x80) ||
            (first == 0xef && second == 0xbb && third == 0xbf)) return 3;
        return 0;
    };
    while (i < script.size()) {
        const char c = script[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') { ++i; continue; }
        if (const auto length = unicode_space_length(i)) { i += length; continue; }
        if (c == '/' && i + 1 < script.size()) {
            if (script[i + 1] == '/') {
                i += 2;
                while (i < script.size() && script[i] != '\r' && script[i] != '\n' && !unicode_line_end(i)) ++i;
                continue;
            }
            if (script[i + 1] == '*') {
                const auto end = script.find("*/", i + 2);
                if (end == std::string_view::npos) return std::nullopt;
                i = end + 2; continue;
            }
        }
        // Regex and division require JavaScript context; only actual comments are supported.
        if (c == '/') return std::nullopt;
        if (c == '\'' || c == '"') {
            const auto start = ++i;
            bool escaped = false;
            while (i < script.size() && script[i] != c) {
                if (static_cast<unsigned char>(script[i]) < 0x20) return std::nullopt;
                if (script[i] == '\\') {
                    escaped = true;
                    if (++i == script.size() || static_cast<unsigned char>(script[i]) < 0x20) return std::nullopt;
                    const char escape = script[i++];
                    if (escape == 'u') {
                        if (!config_unicode_escape(script, i)) return std::nullopt;
                    } else if (escape == 'x') {
                        if (i + 1 >= script.size() || config_hex(script[i]) < 0 || config_hex(script[i + 1]) < 0) return std::nullopt;
                        i += 2;
                    } else if (escape == '0' && i < script.size() && config_digit(script[i])) return std::nullopt;
                } else ++i;
            }
            if (i == script.size()) return std::nullopt;
            tokens.push_back({ConfigToken::Kind::string, std::string(script.substr(start, i - start)), 0, escaped});
            ++i;
        } else if (config_digit(c)) {
            // Trivia must not splice separate tokens into a decimal literal.
            const auto start = i;
            while (i < script.size() && config_digit(script[i])) ++i;
            if (i + 1 < script.size() && script[i] == '.' && config_digit(script[i + 1])) {
                ++i;
                while (i < script.size() && config_digit(script[i])) ++i;
            }
            tokens.push_back({ConfigToken::Kind::word, std::string(script.substr(start, i - start)), 0, false});
        } else if (config_word_byte(static_cast<unsigned char>(c)) || c == '\\') {
            std::string word; bool escaped = false;
            while (i < script.size()) {
                if (unicode_space_length(i)) break;
                if (config_word_byte(static_cast<unsigned char>(script[i]))) { word.push_back(script[i++]); continue; }
                if (script[i] != '\\') break;
                escaped = true;
                if (++i == script.size() || script[i++] != 'u') return std::nullopt;
                const auto code = config_unicode_escape(script, i);
                if (!code || (*code >= 0xd800 && *code <= 0xdfff)) return std::nullopt;
                if (*code < 128) {
                    if (!config_word_byte(static_cast<unsigned char>(*code))) return std::nullopt;
                    word.push_back(static_cast<char>(*code));
                } else word.push_back('?'); // A non-ASCII identifier cannot name the ASCII CONFIG binding.
            }
            tokens.push_back({ConfigToken::Kind::word, std::move(word), 0, escaped});
        } else {
            if (c == '`') return std::nullopt;
            tokens.push_back({ConfigToken::Kind::punctuation, {}, c}); ++i;
        }
        if (tokens.size() > 16384) return std::nullopt;
    }
    return tokens;
}

bool config_script_attributes(std::string_view attributes) {
    const auto text = trim_ascii(attributes);
    if (text.empty()) return true;
    const auto lower = ascii_lower(text);
    if (!lower.starts_with("type")) return false;
    std::size_t i = 4;
    while (i < lower.size() && is_regex_space(lower[i])) ++i;
    if (i == lower.size() || lower[i++] != '=') return false;
    while (i < lower.size() && is_regex_space(lower[i])) ++i;
    if (i == lower.size() || (lower[i] != '\'' && lower[i] != '"')) return false;
    const char quote = lower[i++]; const auto start = i;
    while (i < lower.size() && lower[i] != quote) ++i;
    if (i == lower.size()) return false;
    const auto type = std::string_view(lower).substr(start, i - start);
    if (type != "text/javascript" && type != "application/javascript" &&
        type != "text/ecmascript" && type != "application/ecmascript") return false;
    ++i;
    while (i < lower.size() && is_regex_space(lower[i])) ++i;
    return i == lower.size();
}

std::optional<std::ptrdiff_t> config_global_receiver(const std::vector<ConfigToken>& tokens, std::ptrdiff_t end, bool& exhausted, int depth = 0) {
    if (end < 0) return std::nullopt;
    if (depth > 32) { exhausted = true; return std::nullopt; }
    const auto token_at = [&](std::ptrdiff_t index) -> const ConfigToken& { return tokens[static_cast<std::size_t>(index)]; };
    if (end >= 1 && token_at(end).punct('.') && token_at(end - 1).punct('?'))
        return config_global_receiver(tokens, end - 2, exhausted, depth + 1);
    if (token_at(end).kind == ConfigToken::Kind::word) {
        const auto& token = token_at(end);
        if (token.word("window") || token.word("globalThis") || token.word("self") || token.word("this")) return end;
        if (end >= 2 && token_at(end - 1).punct('.')) {
            const auto receiver = token_at(end - 2).punct('?') ? end - 3 : end - 2;
            return config_global_receiver(tokens, receiver, exhausted, depth + 1);
        }
    }
    if (token_at(end).punct(')') || token_at(end).punct(']')) {
        const char close = token_at(end).punct(')') ? ')' : ']';
        const char open = close == ')' ? '(' : '[';
        int nesting = 1;
        auto start = end - 1;
        for (; start >= 0; --start) {
            if (token_at(start).punct(close)) ++nesting;
            if (token_at(start).punct(open)) --nesting;
            if (!nesting) break;
        }
        if (start < 0) return std::nullopt;
        if (close == ')') {
            const auto root = config_global_receiver(tokens, end - 1, exhausted, depth + 1);
            if (root && *root == start + 1) return start;
        } else return config_global_receiver(tokens, start - 1, exhausted, depth + 1);
    }
    return std::nullopt;
}

bool config_computed_reference(const std::vector<ConfigToken>& tokens) {
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (!tokens[i].punct('[') || !i) continue;
        auto receiver = static_cast<std::ptrdiff_t>(i) - 1;
        if (receiver >= 1 && tokens[static_cast<std::size_t>(receiver)].punct('.') && tokens[static_cast<std::size_t>(receiver - 1)].punct('?'))
            receiver -= 2;
        if (receiver < 0) continue;
        const auto& preceding = tokens[static_cast<std::size_t>(receiver)];
        if ((preceding.kind != ConfigToken::Kind::word && preceding.kind != ConfigToken::Kind::string &&
            !preceding.punct(')') && !preceding.punct(']')) || preceding.word("return") || preceding.word("throw") || preceding.word("yield")) continue;
        std::string property;
        std::size_t j = i + 1;
        bool known_property = false;
        for (; j < tokens.size() && j - i <= 32; ) {
            if (tokens[j].kind != ConfigToken::Kind::string) break;
            // An escaped property key has no unambiguous plain identifier here.
            if (tokens[j].escaped) return true;
            property += tokens[j++].text;
            if (j < tokens.size() && tokens[j].punct(']')) {
                if (property == "CONFIG") return true;
                known_property = true;
                break;
            }
            if (j >= tokens.size() || !tokens[j].punct('+')) break;
            ++j;
        }
        // A dynamic global property may name CONFIG; do not resolve JavaScript here.
        if (!known_property) {
            bool exhausted = false;
            const auto global = config_global_receiver(tokens, receiver, exhausted);
            if (global || exhausted) return true;
        }
    }
    return false;
}

bool config_decimal(std::string_view text, bool fraction) {
    const auto dot = text.find('.');
    const auto integer = text.substr(0, dot);
    if (integer.empty() || (integer.size() > 1 && integer.front() == '0') ||
        !std::all_of(integer.begin(), integer.end(), config_digit)) return false;
    if (dot == std::string_view::npos) return true;
    const auto tail = text.substr(dot + 1);
    return fraction && !tail.empty() && std::all_of(tail.begin(), tail.end(), config_digit);
}

bool config_literal_key(const ConfigToken& token) {
    if (token.escaped) return false;
    if (token.kind == ConfigToken::Kind::string) return true;
    if (token.kind != ConfigToken::Kind::word || token.text.empty()) return false;
    if (config_digit(token.text.front())) {
        // Larger JavaScript numeric keys can round to a different property name.
        constexpr std::string_view maximum_safe_integer = "9007199254740991";
        return config_decimal(token.text, false) && (token.text.size() < maximum_safe_integer.size() ||
            (token.text.size() == maximum_safe_integer.size() && token.text <= maximum_safe_integer));
    }
    const auto identifier_start = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '$'; };
    return identifier_start(token.text.front()) && std::all_of(token.text.begin(), token.text.end(), [&](char c) {
        return identifier_start(c) || config_digit(c);
    });
}

using ConfigFields = std::map<std::string, std::vector<ConfigToken>>;
struct ConfigLiteralReader {
    const std::vector<ConfigToken>& tokens;
    std::size_t index = 0;
    bool take(char c) {
        if (index >= tokens.size() || !tokens[index].punct(c)) return false;
        ++index; return true;
    }
    bool value(int depth = 0) {
        if (depth > 32 || index >= tokens.size()) return false;
        if (tokens[index].kind == ConfigToken::Kind::word && tokens[index].escaped) return false;
        if (take('{')) {
            std::set<std::string> keys;
            if (take('}')) return true;
            do {
                if (index >= tokens.size()) return false;
                const auto& key = tokens[index++];
                if (!config_literal_key(key) || !keys.insert(key.text).second || keys.size() > 256 || !take(':') || !value(depth + 1)) return false;
                if (take('}')) return true;
                if (!take(',')) return false;
            } while (!take('}'));
            return true;
        }
        if (take('[')) {
            if (take(']')) return true;
            do {
                if (!value(depth + 1)) return false;
                if (take(']')) return true;
                if (!take(',')) return false;
            } while (!take(']'));
            return true;
        }
        if (tokens[index].kind == ConfigToken::Kind::string) {
            ++index;
            // The fixed page may include a language fallback between literals.
            while (take('|')) {
                if (!take('|') || index >= tokens.size() || tokens[index].kind != ConfigToken::Kind::string) return false;
                ++index;
            }
            return true;
        }
        if (tokens[index].word("true") || tokens[index].word("false") || tokens[index].word("null")) { ++index; return true; }
        (void)take('-');
        if (index >= tokens.size() || tokens[index].kind != ConfigToken::Kind::word ||
            tokens[index].escaped || !config_decimal(tokens[index].text, true)) return false;
        ++index; return true;
    }
};

std::optional<ConfigFields> config_fields(const std::vector<ConfigToken>& tokens) {
    ConfigFields fields;
    std::size_t i = 4;
    while (i < tokens.size() && !tokens[i].punct('}')) {
        const auto& key = tokens[i];
        if (!config_literal_key(key) ||
            fields.contains(key.text) || i + 1 >= tokens.size() || !tokens[i + 1].punct(':')) return std::nullopt;
        const auto name = key.text; i += 2;
        const auto start = i;
        std::vector<char> closing;
        while (i < tokens.size()) {
            const auto& token = tokens[i];
            if (token.kind == ConfigToken::Kind::punctuation) {
                const char c = token.punctuation;
                if (closing.empty() && (c == ',' || c == '}')) break;
                if (c == '{' || c == '[' || c == '(') closing.push_back(c == '{' ? '}' : c == '[' ? ']' : ')');
                else if (c == '}' || c == ']' || c == ')') {
                    if (closing.empty() || closing.back() != c) return std::nullopt;
                    closing.pop_back();
                }
                if (closing.size() > 32) return std::nullopt;
            }
            ++i;
        }
        if (i == start || !closing.empty() || fields.size() >= 256) return std::nullopt;
        std::vector<ConfigToken> value(tokens.begin() + static_cast<std::ptrdiff_t>(start), tokens.begin() + static_cast<std::ptrdiff_t>(i));
        ConfigLiteralReader reader{value};
        if (!reader.value() || reader.index != value.size()) return std::nullopt;
        fields.emplace(name, std::move(value));
        if (i < tokens.size() && tokens[i].punct(',')) ++i;
        else break;
    }
    if (i == tokens.size() || !tokens[i++].punct('}')) return std::nullopt;
    if (i < tokens.size() && tokens[i].punct(';')) ++i;
    return i == tokens.size() ? std::optional<ConfigFields>(std::move(fields)) : std::nullopt;
}

bool config_ipv4(std::string_view text) {
    for (int component = 0; component < 4; ++component) {
        const auto separator = text.find('.');
        if ((component < 3) != (separator != std::string_view::npos)) return false;
        const auto part = text.substr(0, separator);
        if (part.empty() || part.size() > 3 || (part.size() > 1 && part.front() == '0')) return false;
        int number = 0;
        for (const char c : part) { if (c < '0' || c > '9') return false; number = number * 10 + c - '0'; }
        if (number > 255) return false;
        if (component < 3) text.remove_prefix(separator + 1);
    }
    return true;
}
}  // namespace

namespace {
ClientConfigurationResult client_configuration_impl(std::string_view html, std::string_view controller,
                                                   bool controller_validated) {
    if (html.size() > kMaxResponseBytes) return {{}, false, PortalError::responseTooLarge};
    if (!json::valid_utf8(html)) return {{}, false, PortalError::response};
    ClientConfigurationResult result;
    const auto fail = [&]() { result.value.reset(); result.error = PortalError::protocolConflict; return result; };
    const std::string visible(html); // Script bytes, including quoted comment-like text, stay unchanged.
    const auto lower = ascii_lower(visible);
    const auto html_space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; };
    const auto tag_end = [&](std::size_t start, bool& self_closing) -> std::optional<std::size_t> {
        self_closing = false;
        auto i = start;
        while (i < visible.size()) {
            while (i < visible.size() && html_space(visible[i])) ++i;
            if (i == visible.size()) return std::nullopt;
            if (visible[i] == '>') return i;
            if (visible[i] == '/') {
                if (i + 1 < visible.size() && visible[i + 1] == '>') { self_closing = true; return i + 1; }
                return std::nullopt;
            }
            const auto name_start = i;
            while (i < visible.size() && !html_space(visible[i]) && visible[i] != '=' && visible[i] != '/' && visible[i] != '>') {
                const char c = visible[i++];
                if (c == '<' || c == '\'' || c == '"' || c == '`' || static_cast<unsigned char>(c) < 0x20) return std::nullopt;
            }
            if (i == name_start) return std::nullopt;
            while (i < visible.size() && html_space(visible[i])) ++i;
            if (i == visible.size()) return std::nullopt;
            if (visible[i] != '=') continue;
            ++i;
            while (i < visible.size() && html_space(visible[i])) ++i;
            if (i == visible.size()) return std::nullopt;
            if (visible[i] == '\'' || visible[i] == '"') {
                const char quote = visible[i++];
                while (i < visible.size() && visible[i] != quote) {
                    if (visible[i] == '\0') return std::nullopt;
                    ++i;
                }
                if (i == visible.size()) return std::nullopt;
                ++i;
                if (i == visible.size() || (!html_space(visible[i]) && visible[i] != '/' && visible[i] != '>')) return std::nullopt;
            } else {
                const auto value_start = i;
                while (i < visible.size() && !html_space(visible[i]) && visible[i] != '>') {
                    const char c = visible[i++];
                    if (c == '<' || c == '\'' || c == '"' || c == '`' || c == '=' || static_cast<unsigned char>(c) < 0x20) return std::nullopt;
                }
                if (i == value_start) return std::nullopt;
            }
        }
        return std::nullopt;
    };
    const auto doctype_end = [&](std::size_t start) -> std::optional<std::size_t> {
        auto i = start;
        const auto skip_space = [&]() { while (i < visible.size() && html_space(visible[i])) ++i; };
        const auto quoted_identifier = [&]() {
            if (i == visible.size() || (visible[i] != '\'' && visible[i] != '"')) return false;
            const char quote = visible[i++];
            while (i < visible.size() && visible[i] != quote) {
                if (visible[i] == '>' || visible[i] == '\0') return false;
                ++i;
            }
            if (i == visible.size()) return false;
            ++i; return true;
        };
        if (i == visible.size() || !html_space(visible[i])) return std::nullopt;
        skip_space();
        if (lower.compare(i, 4, "html") != 0) return std::nullopt;
        i += 4;
        if (i == visible.size() || (!html_space(visible[i]) && visible[i] != '>')) return std::nullopt;
        skip_space();
        if (i < visible.size() && visible[i] == '>') return i;
        const bool public_id = lower.compare(i, 6, "public") == 0;
        if (!public_id && lower.compare(i, 6, "system") != 0) return std::nullopt;
        i += 6;
        if (i == visible.size() || !html_space(visible[i])) return std::nullopt;
        skip_space();
        if (!quoted_identifier()) return std::nullopt;
        const auto after_identifier = i;
        skip_space();
        if (public_id && i < visible.size() && (visible[i] == '\'' || visible[i] == '"')) {
            if (i == after_identifier || !quoted_identifier()) return std::nullopt;
            skip_space();
        }
        return i < visible.size() && visible[i] == '>' ? std::optional<std::size_t>(i) : std::nullopt;
    };
    const auto close_tag = [&](std::string_view name, std::size_t start) -> std::optional<std::pair<std::size_t, std::size_t>> {
        const std::string prefix = "</" + std::string(name);
        auto cursor = start;
        while (true) {
            const auto open = lower.find(prefix, cursor);
            if (open == std::string::npos) return std::nullopt;
            const auto after = open + prefix.size();
            if (after == visible.size()) return std::nullopt;
            if (html_space(visible[after]) || visible[after] == '>') {
                auto end = after;
                while (end < visible.size() && html_space(visible[end])) ++end;
                if (end == visible.size() || visible[end] != '>') return std::nullopt;
                return std::pair{open, end};
            }
            cursor = after;
        }
    };
    std::optional<std::vector<ConfigToken>> declaration;
    std::size_t offset = 0;
    int template_depth = 0;
    std::vector<std::string> foreign_elements;
    while (offset < visible.size()) {
        const auto open = lower.find('<', offset);
        if (open == std::string::npos) break;
        if (visible.compare(open, 4, "<!--") == 0) {
            const auto comment_end = visible.find("-->", open + 4);
            if (comment_end == std::string::npos || visible.compare(open + 4, 1, ">") == 0 ||
                visible.compare(open + 4, 2, "->") == 0 || visible.find("--!>", open + 4) < comment_end ||
                visible.find("<!--", open + 4) < comment_end) return fail();
            offset = comment_end + 3; continue;
        }
        auto name_start = open + 1;
        if (name_start == visible.size()) break;
        if (visible[name_start] == '!' || visible[name_start] == '?') {
            const auto after_keyword = name_start + 8;
            const bool doctype = visible[name_start] == '!' && lower.compare(name_start + 1, 7, "doctype") == 0 &&
                after_keyword < visible.size() && (html_space(visible[after_keyword]) || visible[after_keyword] == '>');
            if (doctype) {
                const auto end = doctype_end(after_keyword);
                if (!end) return fail();
                offset = *end + 1;
            } else {
                // HTML bogus comments end at the first >, even inside quotes.
                const auto end = visible.find('>', name_start + 1);
                if (end == std::string::npos) return fail();
                offset = end + 1;
            }
            continue;
        }
        const bool closing_tag = name_start < visible.size() && visible[name_start] == '/';
        if (closing_tag) ++name_start;
        if (name_start == visible.size() || lower[name_start] < 'a' || lower[name_start] > 'z') {
            if (closing_tag) return fail();
            offset = name_start; continue;
        }
        auto after_name = name_start;
        while (after_name < visible.size() && !html_space(visible[after_name]) && visible[after_name] != '/' && visible[after_name] != '>') {
            if (visible[after_name] == '<' || visible[after_name] == '\0') return fail();
            ++after_name;
        }
        const auto name = std::string_view(lower).substr(name_start, after_name - name_start);
        bool self_closing = false;
        const auto end = tag_end(after_name, self_closing);
        if (!end) return fail();
        offset = *end + 1;
        // Frameset modes and foreign script execution are outside the literal subset.
        if (name == "frameset" && template_depth == 0) return fail();
        if (name == "svg" || name == "math") {
            if (closing_tag) {
                if (foreign_elements.empty() || foreign_elements.back() != name) return fail();
                foreign_elements.pop_back();
            } else if (!self_closing) {
                foreign_elements.emplace_back(name);
                if (foreign_elements.size() > 32) return fail();
            }
        }
        const bool html_context = foreign_elements.empty();
        if (!closing_tag && name == "plaintext" && html_context) break;
        if (name == "template" && html_context) {
            template_depth += closing_tag ? -1 : 1;
            if (template_depth < 0 || template_depth > 32) return fail();
        }
        if (name != "script" || closing_tag) {
            if (!closing_tag && html_context && (name == "style" || name == "title" || name == "textarea" || name == "xmp" ||
                name == "iframe" || name == "noembed" || name == "noframes" || name == "noscript")) {
                const auto close = close_tag(name, offset);
                if (!close) return fail();
                offset = close->second + 1;
            }
            continue;
        }
        if (template_depth == 0 && !foreign_elements.empty()) return fail();
        const auto close = close_tag(name, offset);
        if (!close) return fail();
        // HTML's legacy double escaping can keep an apparent closing tag inert.
        if (visible.find("<!--", offset) < close->first) return fail();
        const auto body_start = offset;
        offset = close->second + 1;
        if (template_depth) continue;
        const auto tokens = config_tokens(std::string_view(visible).substr(body_start, close->first - body_start));
        if (!tokens) return fail();
        if (config_computed_reference(*tokens)) { result.present = true; return fail(); }
        const auto references = std::count_if(tokens->begin(), tokens->end(), [](const auto& token) { return token.word("CONFIG"); });
        if (!references) continue;
        result.present = true;
        if (declaration || references != 1 || tokens->size() < 5 ||
            (*tokens)[0].escaped || !((*tokens)[0].word("var") || (*tokens)[0].word("let") || (*tokens)[0].word("const")) ||
            !(*tokens)[1].word("CONFIG") || (*tokens)[1].escaped || !(*tokens)[2].punct('=') || !(*tokens)[3].punct('{') ||
            !config_script_attributes(std::string_view(visible).substr(after_name, *end - after_name))) return fail();
        declaration = *tokens;
    }
    if (template_depth || !foreign_elements.empty()) return fail();
    if (!declaration) return result;
    const auto fields = config_fields(*declaration);
    if (!fields) return fail();
    const auto ip = fields->find("ip"), acid = fields->find("acid"), ac_id_field = fields->find("ac_id");
    if (ip == fields->end() || ip->second.size() != 1 || ip->second[0].kind != ConfigToken::Kind::string ||
        ip->second[0].escaped || !config_ipv4(ip->second[0].text) ||
        (acid != fields->end()) == (ac_id_field != fields->end())) return fail();
    const auto& id = (acid != fields->end() ? acid : ac_id_field)->second;
    if (id.size() != 1 || (id[0].kind != ConfigToken::Kind::word && id[0].kind != ConfigToken::Kind::string) ||
        id[0].escaped || id[0].text.empty() || id[0].text.size() > 8 ||
        !std::all_of(id[0].text.begin(), id[0].text.end(), [](char c) { return c >= '0' && c <= '9'; })) return fail();
    const auto first_digit = id[0].text.find_first_not_of('0');
    if (first_digit == std::string::npos || id[0].text.substr(first_digit) != controller) return fail();
    if (!controller_validated) {
        const auto page_controller = ac_id(html);
        if (page_controller.error != PortalError::none || page_controller.value != controller) return fail();
    }
    result.value = ClientConfiguration{std::string(controller), ip->second[0].text};
    return result;
}
}  // namespace

ClientConfigurationResult client_configuration(std::string_view html, std::string_view controller) {
    return client_configuration_impl(html, controller, false);
}

TeachingPageResult teaching_page(std::string_view html) {
    TeachingPageResult result;
    result.controller = ac_id(html);
    if (result.controller.error == PortalError::none)
        result.configuration = client_configuration_impl(html, result.controller.value, true);
    return result;
}

std::optional<ParsedUrl> parse_url(std::string_view url) {
    ParsedUrl out;
    const std::size_t scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos || scheme_end == 0) return std::nullopt;
    out.scheme = ascii_lower(url.substr(0, scheme_end));
    std::string_view rest = url.substr(scheme_end + 3);
    std::size_t authority_end = rest.find_first_of("/?#");
    if (authority_end == std::string_view::npos) authority_end = rest.size();
    std::string_view authority = rest.substr(0, authority_end);
    std::string_view tail = rest.substr(authority_end);
    if (const std::size_t at = authority.rfind('@'); at != std::string_view::npos) {
        out.has_userinfo = true;
        authority = authority.substr(at + 1);
    }
    if (const std::size_t colon = authority.rfind(':'); colon != std::string_view::npos && authority.find(']') == std::string_view::npos) {
        const std::string_view port_text = authority.substr(colon + 1);
        if (port_text.empty() || port_text.size() > 5 ||
            !std::all_of(port_text.begin(), port_text.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return std::nullopt;
        out.port = std::stoi(std::string(port_text));
        authority = authority.substr(0, colon);
    }
    out.host = ascii_lower(authority);
    if (out.host.empty()) return std::nullopt;
    const std::size_t fragment = tail.find('#');
    if (fragment != std::string_view::npos) {
        out.has_fragment = true;
        tail = tail.substr(0, fragment);
    }
    const std::size_t question = tail.find('?');
    if (question != std::string_view::npos) {
        out.has_query = true;
        out.query = std::string(tail.substr(question + 1));
        tail = tail.substr(0, question);
    }
    out.path = std::string(tail);
    return out;
}

bool is_allowed(std::string_view url) {
    const auto parsed = parse_url(url);
    if (!parsed || parsed->has_userinfo || parsed->has_fragment) return false;
    const auto& u = *parsed;
    auto port_is = [&](std::initializer_list<int> ports) {
        if (!u.port) return true;
        return std::find(ports.begin(), ports.end(), *u.port) != ports.end();
    };
    if (u.scheme == "https" && u.host == "net.szu.edu.cn" && port_is({443})) {
        return ((u.path == "/" || u.path == "/srun_portal_pc") && !u.has_query) ||
               u.path == "/cgi-bin/get_challenge" || u.path == "/cgi-bin/srun_portal";
    }
    if (u.scheme == "http" && u.host == "172.30.255.42" && u.port && *u.port == 801) {
        return ((u.path == "/" || u.path == "/eportal") && !u.has_query) || u.path == "/eportal/portal/login";
    }
    if (u.scheme == "https" && u.host == "www.apple.com" && port_is({443})) {
        return u.path == "/library/test/success.html" && !u.has_query;
    }
    if (u.scheme == "http" && u.host == "www.msftconnecttest.com" && port_is({80})) {
        return u.path == "/connecttest.txt" && !u.has_query;
    }
    return false;
}

std::optional<CampusPortalHint> campus_redirect_hint(std::string_view location, std::string_view probe_url) {
    const auto source = parse_url(probe_url);
    if (!source || source->has_userinfo || source->has_query || source->has_fragment || !is_allowed(probe_url)) return std::nullopt;
    const bool fixed_probe = (source->scheme == "https" && source->host == "www.apple.com" &&
        source->path == "/library/test/success.html") || (source->scheme == "http" &&
        source->host == "www.msftconnecttest.com" && source->path == "/connecttest.txt");
    if (!fixed_probe || location.empty() || location.size() > 2048) return std::nullopt;
    auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (std::size_t i = 0; i < location.size(); ++i) {
        const auto c = static_cast<unsigned char>(location[i]);
        if (c <= 0x20 || c >= 0x7f || c == '\\') return std::nullopt;
        if (c == '%') {
            if (i + 2 >= location.size() || hex(location[i + 1]) < 0 || hex(location[i + 2]) < 0) return std::nullopt;
            const int decoded = hex(location[i + 1]) * 16 + hex(location[i + 2]);
            if (decoded <= 0x20 || decoded == 0x7f || decoded == '\\' || decoded == '%') return std::nullopt;
            i += 2;
        }
    }
    std::string destination(location);
    if (location.starts_with("//")) destination = source->scheme + ":" + destination;
    const auto target = parse_url(destination);
    if (!target || target->has_userinfo || target->has_fragment || target->host.find('%') != std::string::npos ||
        target->path.find('%') != std::string::npos) return std::nullopt;
    const bool teaching_protocol = (target->scheme == "http" && target->port.value_or(80) == 80) ||
        (target->scheme == "https" && target->port.value_or(443) == 443);
    if (teaching_protocol && target->host == "net.szu.edu.cn" &&
        (target->path.empty() || target->path == "/" || target->path == "/srun_portal_pc" ||
         target->path == "/srun_portal_phone" || target->path == "/srun_portal_success" ||
         target->path == "/srun_portal_success_mobile" || target->path == "/index_1.html")) return CampusPortalHint::teaching;
    if (target->scheme == "http" && target->host == "172.30.255.42" && target->port == 801 &&
        (target->path.empty() || target->path == "/" || target->path == "/eportal")) return CampusPortalHint::dormitory;
    if (target->scheme == "http" && target->host == "172.30.255.42" && target->port.value_or(80) == 80 &&
        (target->path == "/a70.htm" || target->path == "/a79.htm")) return CampusPortalHint::dormitory;
    return std::nullopt;
}

std::string percent_encode(std::string_view text) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : text) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
            c == '_' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

std::optional<std::string> make_url(std::string_view base, std::string_view path, const Query& query) {
    std::string url(base);
    url += path;
    if (!query.empty()) {
        url.push_back('?');
        for (std::size_t i = 0; i < query.size(); ++i) {
            if (i) url.push_back('&');
            url += percent_encode(query[i].first);
            url.push_back('=');
            url += percent_encode(query[i].second);
        }
    }
    if (!is_allowed(url)) return std::nullopt;
    return url;
}

std::optional<json::Value> object_from(std::string_view body) {
    if (body.size() > kMaxResponseBytes || !json::valid_utf8(body)) return std::nullopt;
    std::string text = trim_ascii(body);
    if (text.empty() || text.front() != '{') {
        if (!text.empty() && text.back() == ';') text.pop_back();
        const std::size_t open = text.find('(');
        if (open == std::string::npos || text.empty() || text.back() != ')') return std::nullopt;
        std::string callback = text.substr(0, open);
        while (!callback.empty() && callback.back() == ' ') callback.pop_back();
        while (!callback.empty() && callback.front() == ' ') callback.erase(callback.begin());
        if (callback != "_" && callback != "jsonpReturn") return std::nullopt;
        text = text.substr(open + 1, text.size() - open - 2);
    }
    auto value = json::parse(text);
    if (!value || !value->is_object()) return std::nullopt;
    return value;
}

bool teaching_success(const json::Value& object) {
    return teaching_verdict(object) == LoginVerdict::accepted;
}

namespace {
bool is_failure_message(std::string_view raw) {
    static const std::string_view zh[] = {"未", "不", "非", "无", "没", "失败", "错误", "异常", "拒绝", "等待", "正在", "稍后"};
    static const std::string_view en[] = {"fail", "error", "denied", "invalid", "incorrect", "reject", "not ", "pending", "waiting"};
    const std::string lower = ascii_lower(raw);
    for (auto word : zh) if (contains(raw, word)) return true;
    for (auto word : en) if (contains(lower, word)) return true;
    return false;
}
}  // namespace

bool is_dormitory_success_message(std::string_view raw) {
    std::string text = trim(raw);
    auto ends_with = [&](std::string_view suffix) {
        return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    bool stripped = true;
    while (stripped && !text.empty()) {
        stripped = false;
        for (std::string_view mark : {"！", "。", "～", "!", ".", "~", " "}) {
            if (ends_with(mark)) {
                text.erase(text.size() - mark.size());
                stripped = true;
                break;
            }
        }
    }
    const std::string lower = ascii_lower(text);
    return lower == "认证成功" || lower == "portal协议认证成功";
}

bool dormitory_success(const json::Value& object) {
    const std::string message = object.string_at("msg").value_or("");
    if (is_failure_message(message)) return false;
    if (is_dormitory_success_message(message)) return true;
    if (const json::Value* result = object.find("result"); result && result->is_number() && result->as_number() == 0) return true;
    return false;
}

LoginVerdict teaching_verdict(const json::Value& object) {
    // Existing IP session is not evidence that this request accepted the supplied password.
    for (const char* key : {"suc_msg", "error_msg", "error", "res"})
        if (object.string_at(key).value_or("") == "ip_already_online_error") return LoginVerdict::uncertain;
    bool positive = false, negative = false, invalid = false;
    for (const char* key : {"error", "res"}) {
        if (const json::Value* raw = object.find(key)) {
            if (!raw->is_string()) { invalid = true; continue; }
            const std::string& value = raw->as_string();
            if (value == "ok") positive = true;
            else if (!value.empty()) {
                const std::string lower = ascii_lower(value);
                if (lower == "pending" || lower == "waiting" || lower == "processing") invalid = true;
                else negative = true;
            }
        }
    }
    if (const json::Value* raw = object.find("st")) {
        if (raw->is_bool()) { if (raw->as_bool()) positive = true; else negative = true; }
        else if (raw->is_number()) {
            if (raw->as_number() == 1) positive = true;
            else if (raw->as_number() == 0) negative = true;
            else invalid = true;
        } else if (raw->is_string()) {
            if (raw->as_string() == "1") positive = true;
            else if (raw->as_string() == "0") negative = true;
            else invalid = true;
        } else invalid = true;
    }
    if (positive && (negative || invalid)) return LoginVerdict::uncertain;
    if (invalid) return LoginVerdict::uncertain;
    if (positive) return LoginVerdict::accepted;
    if (negative) return LoginVerdict::rejected;
    return LoginVerdict::uncertain;
}

LoginVerdict dormitory_verdict(const json::Value& object) {
    const json::Value* raw_message = object.find("msg");
    if (raw_message && !raw_message->is_string()) return LoginVerdict::uncertain;
    const std::string message = object.string_at("msg").value_or("");
    const std::string lower = ascii_lower(message);
    static const std::string_view failures[] = {"失败", "错误", "拒绝", "不存在", "无权限", "未认证成功",
                                                "fail", "error", "denied", "invalid", "incorrect", "reject", "not found"};
    bool failed = false;
    for (auto marker : failures) if (contains(lower, marker)) failed = true;
    const json::Value* result = object.find("result");
    if (result && (!result->is_number() || (result->as_number() != 0 && result->as_number() != 1)))
        return LoginVerdict::uncertain;
    const bool positive_result = result && result->is_number() && result->as_number() == 0;
    if (failed && positive_result) return LoginVerdict::uncertain;
    if (failed) return LoginVerdict::rejected;
    if (dormitory_success(object)) return LoginVerdict::accepted;
    return LoginVerdict::uncertain;
}

RejectionKind rejection_kind(const json::Value& object) {
    static const std::string_view fields[] = {"error_msg", "ploy_msg", "error", "res", "suc_msg", "msg", "message"};
    static const std::string_view password[] = {"e2553", "password is error", "password error", "ldap auth error",
                                                "密码错误", "密码不正确", "账号或密码错误", "用户名或密码错误"};
    static const std::string_view account[] = {"e2531", "user not found", "userid error", "no such user", "用户不存在",
                                               "账号不存在", "账户不存在"};
    static const std::string_view area[] = {"e2833", "dhcp table", "not in the dhcp", "不在认证范围", "ip地址不在", "ip 地址不在",
                                            "不允许在此区域", "nas_ip_error", "ac_id_error"};
    std::string joined;
    for (auto field : fields) {
        if (auto value = object.string_at(field); value && !value->empty()) {
            if (!joined.empty()) joined += " | ";
            joined += ascii_lower(*value);
        }
    }
    for (auto marker : password) if (contains(joined, marker)) return RejectionKind::password;
    for (auto marker : account) if (contains(joined, marker)) return RejectionKind::account;
    for (auto marker : area) if (contains(joined, marker)) return RejectionKind::areaMismatch;
    return RejectionKind::unspecified;
}

namespace {

bool ident_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}
bool attr_boundary_char(char c) { return ident_char(c) || c == ':'; }

// Value after "key\s*[:=]\s*": "..." | '...' | unquoted run (may be empty, as in the Swift regex).
std::string read_value(std::string_view html, std::size_t& j) {
    if (j < html.size() && (html[j] == '"' || html[j] == '\'')) {
        const char quote = html[j];
        const std::size_t close = html.find(quote, j + 1);
        if (close != std::string_view::npos) {
            std::string value(html.substr(j + 1, close - j - 1));
            j = close + 1;
            return value;
        }
        return {};  // unterminated quote: the unquoted alternative matches the empty string
    }
    const std::size_t start = j;
    while (j < html.size() && !is_regex_space(html[j]) && std::string_view("&\"'<>;,)}]").find(html[j]) == std::string_view::npos) ++j;
    return std::string(html.substr(start, j - start));
}

// Values of attribute `name` inside one <input ...> tag (Swift attribute regex port).
std::vector<std::string> attribute_values(std::string_view tag, std::string_view name) {
    std::vector<std::string> values;
    const std::string lower = ascii_lower(tag);
    std::size_t pos = 0;
    while ((pos = lower.find(name, pos)) != std::string::npos) {
        if (pos > 0 && attr_boundary_char(lower[pos - 1])) { ++pos; continue; }
        std::size_t j = pos + name.size();
        while (j < tag.size() && is_regex_space(tag[j])) ++j;
        if (j >= tag.size() || tag[j] != '=') { ++pos; continue; }
        ++j;
        while (j < tag.size() && is_regex_space(tag[j])) ++j;
        if (j < tag.size() && (tag[j] == '"' || tag[j] == '\'')) {
            const char quote = tag[j];
            const std::size_t close = tag.find(quote, j + 1);
            if (close == std::string_view::npos) { ++pos; continue; }
            values.emplace_back(tag.substr(j + 1, close - j - 1));
            pos = close + 1;
            continue;
        }
        const std::size_t start = j;
        while (j < tag.size() && !is_regex_space(tag[j]) && std::string_view("\"'=<>`").find(tag[j]) == std::string_view::npos) ++j;
        if (j == start) { ++pos; continue; }
        values.emplace_back(tag.substr(start, j - start));
        pos = j;
    }
    return values;
}

}  // namespace

AcIdResult ac_id(std::string_view html) {
    AcIdResult result;
    if (html.size() > kMaxResponseBytes) { result.error = PortalError::responseTooLarge; return result; }
    if (!json::valid_utf8(html)) { result.error = PortalError::response; return result; }
    const std::string lower = ascii_lower(html);
    std::vector<std::string> candidates;
    std::size_t pos = 0;
    while (pos < lower.size()) {
        std::size_t token = 0;
        if (lower.compare(pos, 5, "ac_id") == 0) token = 5;
        else if (lower.compare(pos, 7, "\"ac_id\"") == 0 || lower.compare(pos, 7, "'ac_id'") == 0) token = 7;
        else if (lower.compare(pos, 4, "acid") == 0) token = 4;
        else if (lower.compare(pos, 6, "\"acid\"") == 0 || lower.compare(pos, 6, "'acid'") == 0) token = 6;
        if (!token || (pos > 0 && ident_char(lower[pos - 1]))) { ++pos; continue; }
        std::size_t j = pos + token;
        while (j < html.size() && is_regex_space(html[j])) ++j;
        if (j >= html.size() || (html[j] != ':' && html[j] != '=')) { ++pos; continue; }
        ++j;
        while (j < html.size() && is_regex_space(html[j])) ++j;
        candidates.push_back(read_value(html, j));
        pos = std::max(j, pos + 1);
    }
    // Both `acid` and `ac_id` from the fixed HTTPS page denote one controller.
    std::size_t tag = 0;
    while ((tag = lower.find("<input", tag)) != std::string::npos) {
        const std::size_t after = tag + 6;
        if (after < lower.size() && (std::isalnum(static_cast<unsigned char>(lower[after])) || lower[after] == '_')) { tag = after; continue; }
        const std::size_t close = lower.find('>', after);
        if (close == std::string::npos) break;
        const std::string_view body = html.substr(tag, close - tag + 1);
        const auto names = attribute_values(body, "name");
        if (std::find(names.begin(), names.end(), "ac_id") != names.end() ||
            std::find(names.begin(), names.end(), "acid") != names.end()) {
            const auto ids = attribute_values(body, "value");
            const bool one_name = !names.empty() && std::all_of(names.begin(), names.end(), [&](const std::string& n) { return n == names.front(); });
            if (!one_name || ids.empty()) { result.error = PortalError::protocolConflict; return result; }
            candidates.insert(candidates.end(), ids.begin(), ids.end());
        }
        tag = close + 1;
    }
    std::set<std::string> identifiers;
    for (const auto& value : candidates) {
        if (value.empty() || value.size() > 8 || !std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            result.error = PortalError::protocolConflict;
            return result;
        }
        const long number = std::stol(value);
        if (number <= 0) { result.error = PortalError::protocolConflict; return result; }
        identifiers.insert(std::to_string(number));
    }
    if (identifiers.size() != 1) { result.error = PortalError::protocolConflict; return result; }
    result.value = *identifiers.begin();
    return result;
}

ChallengeResult challenge(const json::Value& object) {
    ChallengeResult result;
    std::vector<std::string> states;
    for (const char* key : {"error", "res"}) {
        const json::Value* raw = object.find(key);
        if (!raw) continue;
        if (!raw->is_string() || raw->as_string().empty()) { result.error = PortalError::protocolConflict; return result; }
        states.push_back(raw->as_string());
    }
    if (states.empty()) { result.error = PortalError::protocolConflict; return result; }
    if (std::find(states.begin(), states.end(), "ok") == states.end()) { result.error = PortalError::challengeRefused; return result; }
    if (!std::all_of(states.begin(), states.end(), [](const std::string& s) { return s == "ok"; })) {
        result.error = PortalError::protocolConflict;
        return result;
    }
    const auto token = object.string_at("challenge");
    const auto ip = object.string_at("client_ip");
    // Swift counts Characters for the IP limit; ASCII IPs make bytes and characters equal.
    if (!token || token->empty() || token->size() > 4096 || !ip || ip->empty() || ip->size() > 64) {
        result.error = PortalError::protocolConflict;
        return result;
    }
    result.challenge = *token;
    result.ip = *ip;
    return result;
}

bool is_connectivity_success(std::string_view body, int status) {
    return status == 200 && trim_ascii(body) == "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";
}

bool is_microsoft_success(std::string_view body, int status) {
    return status == 200 && trim_ascii(body) == "Microsoft Connect Test";
}

bool is_campus_homepage(std::string_view body, int status, std::string_view url) {
    const auto parsed = parse_url(url);
    if (status != 200 || body.size() > kMaxResponseBytes || !parsed || parsed->has_query ||
        (parsed->path != "/" && parsed->path != "/srun_portal_pc") || !is_allowed(url) || !json::valid_utf8(body))
        return false;
    const std::string html = ascii_lower(body);
    if (!contains(html, "深圳大学") && !contains(html, "shenzhen university")) return false;
    const bool login = contains(html, "login") || contains(html, "登录");
    if (parsed->host == "net.szu.edu.cn") return contains(html, "srun") && login;
    if (parsed->host == "172.30.255.42") return contains(html, "drcom") && login;
    return false;
}

bool is_dormitory_homepage(std::string_view body, int status, std::string_view url, bool campus_confirmed) {
    const auto parsed = parse_url(url);
    if (!campus_confirmed || status != 200 || body.size() > kMaxResponseBytes || !parsed || parsed->host != "172.30.255.42" ||
        parsed->path != "/eportal" || parsed->has_query || !is_allowed(url) || !json::valid_utf8(body))
        return false;
    const std::string html = ascii_lower(body);
    std::size_t pos = 0;
    while ((pos = html.find("<title>", pos)) != std::string::npos) {
        std::size_t j = pos + 7;
        while (j < html.size() && is_regex_space(html[j])) ++j;
        if (html.compare(j, 7, "eportal") == 0) {
            j += 7;
            while (j < html.size() && is_regex_space(html[j])) ++j;
            if (html.compare(j, 8, "</title>") == 0) return true;
        }
        pos += 7;
    }
    return false;
}

bool is_transient(PortalError error) {
    return error == PortalError::timeout || error == PortalError::network || error == PortalError::dns;
}

bool should_fallback(PortalError error) {
    switch (error) {
        case PortalError::network: case PortalError::response: case PortalError::timeout:
        case PortalError::dns: case PortalError::httpStatus:
            return true;
        default:
            return false;
    }
}

PortalError after_submission(PortalError error) {
    switch (error) {
        case PortalError::network: case PortalError::timeout: case PortalError::dns: case PortalError::httpStatus:
        case PortalError::response: case PortalError::responseTooLarge: case PortalError::redirect:
            return PortalError::authenticationUncertain;
        default:
            return error;
    }
}

Outcome outcome_for(PortalError error, bool submitted) {
    switch (error) {
        case PortalError::none: return Outcome::success;
        case PortalError::invalidCredentials: return Outcome::invalidCredentials;
        case PortalError::challengeRefused: return Outcome::challengeRefused;
        case PortalError::rejected: return submitted ? Outcome::rejected : Outcome::challengeRefused;
        case PortalError::authenticationUncertain: return Outcome::uncertain;
        case PortalError::tls: case PortalError::forbiddenTarget: case PortalError::cryptography:
        case PortalError::protocolConflict:
            return Outcome::security;
        case PortalError::redirect: case PortalError::responseTooLarge:
            return submitted ? Outcome::uncertain : Outcome::security;
        case PortalError::network: case PortalError::timeout: case PortalError::dns: case PortalError::httpStatus:
        case PortalError::response:
            return submitted ? Outcome::uncertain : Outcome::unavailable;
    }
    return Outcome::unavailable;
}

}  // namespace szu::wire
