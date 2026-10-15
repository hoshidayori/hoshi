#include "json.h"

#include <charconv>
#include <cmath>
#include <cstdio>

namespace NHoshi {

namespace {

const TJson NullJson;

[[noreturn]] void Fail(const char* what, size_t pos) {
    throw std::runtime_error(std::string("json: ") + what + " at offset " + std::to_string(pos));
}

void AppendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class TParser {
public:
    explicit TParser(std::string_view text) : Text(text) {}

    TJson ParseDocument() {
        TJson value = ParseValue(0);
        SkipSpace();
        if (Pos != Text.size()) {
            Fail("trailing characters", Pos);
        }
        return value;
    }

private:
    // Deep nesting from a hostile server must not overflow the stack
    static constexpr int MaxDepth = 128;

    void SkipSpace() {
        while (Pos < Text.size() && (Text[Pos] == ' ' || Text[Pos] == '\t' || Text[Pos] == '\n' || Text[Pos] == '\r')) {
            ++Pos;
        }
    }

    bool Consume(std::string_view word) {
        if (Text.substr(Pos, word.size()) == word) {
            Pos += word.size();
            return true;
        }
        return false;
    }

    TJson ParseValue(int depth) {
        if (depth > MaxDepth) {
            Fail("nesting too deep", Pos);
        }
        SkipSpace();
        if (Pos >= Text.size()) {
            Fail("unexpected end", Pos);
        }
        switch (Text[Pos]) {
            case '{': return ParseObject(depth);
            case '[': return ParseArray(depth);
            case '"': return TJson(ParseString());
            case 't': if (Consume("true")) return TJson(true); break;
            case 'f': if (Consume("false")) return TJson(false); break;
            case 'n': if (Consume("null")) return TJson(nullptr); break;
            default: return ParseNumber();
        }
        Fail("unexpected literal", Pos);
    }

    TJson ParseObject(int depth) {
        ++Pos;
        TJson::TObject object;
        SkipSpace();
        if (Pos < Text.size() && Text[Pos] == '}') {
            ++Pos;
            return TJson(std::move(object));
        }
        while (true) {
            SkipSpace();
            if (Pos >= Text.size() || Text[Pos] != '"') {
                Fail("expected key", Pos);
            }
            std::string key = ParseString();
            SkipSpace();
            if (Pos >= Text.size() || Text[Pos] != ':') {
                Fail("expected ':'", Pos);
            }
            ++Pos;
            object.insert_or_assign(std::move(key), ParseValue(depth + 1));
            SkipSpace();
            if (Pos < Text.size() && Text[Pos] == ',') {
                ++Pos;
            } else if (Pos < Text.size() && Text[Pos] == '}') {
                ++Pos;
                return TJson(std::move(object));
            } else {
                Fail("expected ',' or '}'", Pos);
            }
        }
    }

    TJson ParseArray(int depth) {
        ++Pos;
        TJson::TArray array;
        SkipSpace();
        if (Pos < Text.size() && Text[Pos] == ']') {
            ++Pos;
            return TJson(std::move(array));
        }
        while (true) {
            array.push_back(ParseValue(depth + 1));
            SkipSpace();
            if (Pos < Text.size() && Text[Pos] == ',') {
                ++Pos;
            } else if (Pos < Text.size() && Text[Pos] == ']') {
                ++Pos;
                return TJson(std::move(array));
            } else {
                Fail("expected ',' or ']'", Pos);
            }
        }
    }

    std::uint32_t ParseHex4() {
        if (Pos + 4 > Text.size()) {
            Fail("short \\u escape", Pos);
        }
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = Text[Pos++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= c - '0';
            else if (c >= 'a' && c <= 'f') value |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') value |= c - 'A' + 10;
            else Fail("bad hex digit", Pos - 1);
        }
        return value;
    }

    std::string ParseString() {
        ++Pos;
        std::string out;
        while (true) {
            if (Pos >= Text.size()) {
                Fail("unterminated string", Pos);
            }
            const char c = Text[Pos++];
            if (c == '"') {
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                Fail("control character in string", Pos - 1);
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (Pos >= Text.size()) {
                Fail("unterminated escape", Pos);
            }
            switch (const char e = Text[Pos++]) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    std::uint32_t cp = ParseHex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (!Consume("\\u")) {
                            Fail("lone high surrogate", Pos);
                        }
                        const std::uint32_t low = ParseHex4();
                        if (low < 0xDC00 || low > 0xDFFF) {
                            Fail("bad low surrogate", Pos);
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        Fail("lone low surrogate", Pos);
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default:
                    (void)e;
                    Fail("bad escape", Pos - 1);
            }
        }
    }

    TJson ParseNumber() {
        const size_t start = Pos;
        if (Pos < Text.size() && Text[Pos] == '-') ++Pos;
        bool isInt = true;
        while (Pos < Text.size()) {
            const char c = Text[Pos];
            if (c >= '0' && c <= '9') {
                ++Pos;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                isInt = false;
                ++Pos;
            } else {
                break;
            }
        }
        const std::string_view number = Text.substr(start, Pos - start);
        if (number.empty() || number == "-") {
            Fail("expected value", start);
        }
        if (isInt) {
            std::int64_t value = 0;
            const auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), value);
            if (ec == std::errc() && end == number.data() + number.size()) {
                return TJson(value);
            }
        }
        double value = 0;
        const auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), value);
        if (ec != std::errc() || end != number.data() + number.size()) {
            Fail("bad number", start);
        }
        return TJson(value);
    }

    std::string_view Text;
    size_t Pos = 0;
};

void DumpString(std::string& out, const std::string& s) {
    out += '"';
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

void DumpTo(std::string& out, const TJson& value) {
    if (value.IsNull()) {
        out += "null";
    } else if (value.IsBool()) {
        out += value.AsBool() ? "true" : "false";
    } else if (value.IsInt()) {
        out += std::to_string(value.AsInt());
    } else if (value.IsDouble()) {
        const double d = value.AsDouble();
        if (!std::isfinite(d)) {
            throw std::runtime_error("json: cannot dump a non-finite number");
        }
        char buf[32];
        const auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), d);
        out.append(buf, end);
    } else if (value.IsString()) {
        DumpString(out, value.AsString());
    } else if (value.IsArray()) {
        out += '[';
        bool first = true;
        for (const auto& item : value.AsArray()) {
            if (!first) out += ',';
            first = false;
            DumpTo(out, item);
        }
        out += ']';
    } else {
        out += '{';
        bool first = true;
        for (const auto& [key, item] : value.AsObject()) {
            if (!first) out += ',';
            first = false;
            DumpString(out, key);
            out += ':';
            DumpTo(out, item);
        }
        out += '}';
    }
}

} // namespace

const TJson& TJson::operator[](std::string_view key) const {
    if (!IsObject()) {
        return NullJson;
    }
    const auto& object = AsObject();
    const auto it = object.find(std::string(key));
    return it == object.end() ? NullJson : it->second;
}

std::string TJson::StringOr(std::string_view key, std::string fallback) const {
    const TJson& value = (*this)[key];
    return value.IsString() ? value.AsString() : fallback;
}

std::string TJson::Dump() const {
    std::string out;
    DumpTo(out, *this);
    return out;
}

TJson TJson::Parse(std::string_view text) {
    return TParser(text).ParseDocument();
}

} // namespace NHoshi
