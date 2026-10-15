#include "json.h"

#include <cstdio>
#include <cstdlib>

using NHoshi::TJson;

namespace {

int Failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++Failures;
    }
}

void ExpectCanonical(const char* input, const char* expected) {
    const std::string got = TJson::Parse(input).Dump();
    if (got != expected) {
        std::printf("FAIL: canonical %s\n  got:      %s\n  expected: %s\n", input, got.c_str(), expected);
        ++Failures;
    }
}

void ExpectThrows(const char* input) {
    try {
        TJson::Parse(input);
        std::printf("FAIL: no error for %s\n", input);
        ++Failures;
    } catch (const std::runtime_error&) {
    }
}

} // namespace

int main() {
    // Examples from the canonical JSON section of the Matrix specification (appendices)
    ExpectCanonical("{}", "{}");
    ExpectCanonical(R"({"one": 1, "two": "Two"})", R"({"one":1,"two":"Two"})");
    ExpectCanonical(R"({"b": "2", "a": "1"})", R"({"a":"1","b":"2"})");
    ExpectCanonical(R"({"auth": {"success": true, "mxid": "@john.doe:example.com", "profile": {"display_name": "John Doe",
        "three_pids": [{"medium": "email", "address": "john.doe@example.org"}, {"medium": "msisdn", "address": "123456789"}]}}})",
        R"({"auth":{"mxid":"@john.doe:example.com","profile":{"display_name":"John Doe","three_pids":[{"address":"john.doe@example.org","medium":"email"},{"address":"123456789","medium":"msisdn"}]},"success":true}})");
    ExpectCanonical(R"({"a": "日本語"})", R"({"a":"日本語"})");
    ExpectCanonical(R"({"本": 2, "日": 1})", R"({"日":1,"本":2})");
    ExpectCanonical(R"({"a": "日"})", R"({"a":"日"})");
    ExpectCanonical(R"({"a": null})", R"({"a":null})");

    // Escapes and surrogate pairs
    ExpectCanonical(R"(["a\"b\\c\nd\u0001"])", R"(["a\"b\\c\nd\u0001"])");
    ExpectCanonical(R"(["😀"])", "[\"\xF0\x9F\x98\x80\"]");
    ExpectCanonical(R"(["\/"])", R"(["/"])");

    // Numbers
    const TJson numbers = TJson::Parse(R"([0, -7, 9007199254740991, 1.5, 2e3])");
    Expect(numbers.AsArray()[1].IsInt() && numbers.AsArray()[1].AsInt() == -7, "negative integer");
    Expect(numbers.AsArray()[2].AsInt() == 9007199254740991LL, "large integer");
    Expect(numbers.AsArray()[3].IsDouble() && numbers.AsArray()[3].AsDouble() == 1.5, "fraction");
    Expect(numbers.AsArray()[4].AsDouble() == 2000.0, "exponent");

    // Access helpers on a /sync-like fragment
    const TJson sync = TJson::Parse(R"({"next_batch": "s72595_4483_1934", "rooms": {"join": {}}})");
    Expect(sync.StringOr("next_batch") == "s72595_4483_1934", "StringOr present");
    Expect(sync.StringOr("missing", "none") == "none", "StringOr missing");
    Expect(sync["rooms"]["join"].IsObject(), "nested access");
    Expect(sync["rooms"]["leave"]["x"].IsNull(), "missing chain is null");

    // Malformed input
    ExpectThrows("");
    ExpectThrows("{");
    ExpectThrows(R"({"a" 1})");
    ExpectThrows("[1,]");
    ExpectThrows(R"("unterminated)");
    ExpectThrows(R"(["\ud800"])");
    ExpectThrows("[1] 2");
    ExpectThrows(std::string(200, '[').c_str());

    if (Failures == 0) {
        std::printf("json_test: all passed\n");
    }
    return Failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
