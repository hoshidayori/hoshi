#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace NHoshi {

// A JSON value. Objects keep keys sorted, which is what Matrix canonical JSON needs for signatures.
class TJson {
public:
    using TArray = std::vector<TJson>;
    using TObject = std::map<std::string, TJson>;

    TJson() = default;
    TJson(std::nullptr_t) {}
    TJson(bool value) : Value(value) {}
    TJson(int value) : Value(static_cast<std::int64_t>(value)) {}
    TJson(std::int64_t value) : Value(value) {}
    TJson(double value) : Value(value) {}
    TJson(const char* value) : Value(std::string(value)) {}
    TJson(std::string value) : Value(std::move(value)) {}
    TJson(TArray value) : Value(std::make_shared<TArray>(std::move(value))) {}
    TJson(TObject value) : Value(std::make_shared<TObject>(std::move(value))) {}

    bool IsNull() const { return std::holds_alternative<std::monostate>(Value); }
    bool IsBool() const { return std::holds_alternative<bool>(Value); }
    bool IsInt() const { return std::holds_alternative<std::int64_t>(Value); }
    bool IsDouble() const { return std::holds_alternative<double>(Value); }
    bool IsString() const { return std::holds_alternative<std::string>(Value); }
    bool IsArray() const { return std::holds_alternative<std::shared_ptr<TArray>>(Value); }
    bool IsObject() const { return std::holds_alternative<std::shared_ptr<TObject>>(Value); }

    bool AsBool() const { return Get<bool>("bool"); }
    std::int64_t AsInt() const { return Get<std::int64_t>("integer"); }
    double AsDouble() const { return IsInt() ? static_cast<double>(AsInt()) : Get<double>("number"); }
    const std::string& AsString() const { return Get<std::string>("string"); }
    const TArray& AsArray() const { return *Get<std::shared_ptr<TArray>>("array"); }
    const TObject& AsObject() const { return *Get<std::shared_ptr<TObject>>("object"); }

    // Returns the member or a null value if this is not an object or has no such member
    const TJson& operator[](std::string_view key) const;
    // Returns the string member or the fallback, so optional fields of server responses read in one line
    std::string StringOr(std::string_view key, std::string fallback = {}) const;

    // Compact output with sorted keys and no spaces: canonical JSON for integers, strings, objects and arrays
    std::string Dump() const;

    static TJson Parse(std::string_view text);

private:
    template <typename T>
    const T& Get(const char* expected) const {
        if (const T* value = std::get_if<T>(&Value)) {
            return *value;
        }
        throw std::runtime_error(std::string("json: expected ") + expected);
    }

    std::variant<std::monostate, bool, std::int64_t, double, std::string,
        std::shared_ptr<TArray>, std::shared_ptr<TObject>> Value;
};

} // namespace NHoshi
