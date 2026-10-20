#include "html.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <optional>

namespace NHoshi::NHtml {

namespace {

constexpr int MaxDepth = 64;  // deeper nesting adds no more meaning; it only costs
constexpr long MaxListStart = 1'000'000'000;

std::string Lower(std::string_view s) {
    std::string result(s);
    for (char& c : result) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

void AppendUtf8(std::string& out, std::uint32_t cp) {
    // A code point that cannot be text becomes the replacement character
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        cp = 0xFFFD;
    }
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

// Decodes character references; one that is not understood stays as written
std::string DecodeEntities(std::string_view s) {
    static const std::map<std::string, std::uint32_t> named{
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0},
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            const size_t semi = s.find(';', i + 1);
            if (semi != std::string_view::npos && semi - i <= 10) {
                const std::string_view name = s.substr(i + 1, semi - i - 1);
                std::optional<std::uint32_t> cp;
                if (name.size() > 1 && name[0] == '#') {
                    const bool hex = name[1] == 'x' || name[1] == 'X';
                    const std::string_view digits = name.substr(hex ? 2 : 1);
                    std::uint32_t value = 0;
                    bool ok = !digits.empty();
                    for (char c : digits) {
                        const int d = std::isdigit(static_cast<unsigned char>(c)) ? c - '0'
                            : hex && std::isxdigit(static_cast<unsigned char>(c)) ? std::tolower(static_cast<unsigned char>(c)) - 'a' + 10
                            : -1;
                        if (d < 0 || value > 0x10FFFF) {
                            ok = false;
                            break;
                        }
                        value = value * (hex ? 16 : 10) + static_cast<std::uint32_t>(d);
                    }
                    if (ok) {
                        cp = value;
                    }
                } else if (const auto it = named.find(Lower(name)); it != named.end()) {
                    cp = it->second;
                }
                if (cp) {
                    AppendUtf8(out, *cp);
                    i = semi + 1;
                    continue;
                }
            }
        }
        out += s[i++];
    }
    return out;
}

bool SafeLink(const std::string& href) {
    const std::string lower = Lower(href);
    return lower.rfind("https://", 0) == 0 || lower.rfind("http://", 0) == 0 || lower.rfind("mailto:", 0) == 0;
}

struct TTag {
    std::string Name;  // lower case
    bool Closing = false;
    bool SelfClosing = false;
    bool Unended = false;  // the input ended before its '>'
    std::map<std::string, std::string> Attributes;  // names in lower case, values decoded
};

// Parses a tag starting after '<'; returns where it ends (after '>'), or nullopt if this is not a tag
std::optional<size_t> ParseTag(std::string_view html, size_t pos, TTag& tag) {
    size_t i = pos;
    if (i < html.size() && html[i] == '/') {
        tag.Closing = true;
        ++i;
    }
    const size_t nameStart = i;
    while (i < html.size() && (std::isalnum(static_cast<unsigned char>(html[i])) || html[i] == '-')) {
        ++i;
    }
    if (i == nameStart || !std::isalpha(static_cast<unsigned char>(html[nameStart]))) {
        return std::nullopt;
    }
    tag.Name = Lower(html.substr(nameStart, i - nameStart));
    while (i < html.size()) {
        const char c = html[i];
        if (c == '>') {
            return i + 1;
        }
        if (c == '/' ) {
            tag.SelfClosing = true;
            ++i;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        tag.SelfClosing = false;
        const size_t attrStart = i;
        while (i < html.size() && !std::isspace(static_cast<unsigned char>(html[i])) && html[i] != '=' && html[i] != '>' && html[i] != '/') {
            ++i;
        }
        const std::string name = Lower(html.substr(attrStart, i - attrStart));
        while (i < html.size() && std::isspace(static_cast<unsigned char>(html[i]))) {
            ++i;
        }
        std::string value;
        if (i < html.size() && html[i] == '=') {
            ++i;
            while (i < html.size() && std::isspace(static_cast<unsigned char>(html[i]))) {
                ++i;
            }
            if (i < html.size() && (html[i] == '"' || html[i] == '\'')) {
                const char quote = html[i++];
                const size_t end = html.find(quote, i);
                if (end == std::string_view::npos) {
                    return std::nullopt;
                }
                value = DecodeEntities(html.substr(i, end - i));
                i = end + 1;
            } else {
                const size_t valueStart = i;
                while (i < html.size() && !std::isspace(static_cast<unsigned char>(html[i])) && html[i] != '>') {
                    ++i;
                }
                value = DecodeEntities(html.substr(valueStart, i - valueStart));
            }
        }
        if (!name.empty()) {
            tag.Attributes.emplace(name, std::move(value));
        }
    }
    tag.Unended = true;
    return std::nullopt;  // no '>': the rest is text
}

bool IsBlock(const std::string& name) {
    static const char* const blocks[] = {"p", "div", "blockquote", "pre", "ul", "ol", "li", "h1", "h2", "h3", "h4", "h5",
        "h6", "table", "thead", "tbody", "tr", "caption", "details", "summary", "hr"};
    return std::any_of(std::begin(blocks), std::end(blocks), [&](const char* b) { return name == b; });
}

enum EKind { Plain, QuoteKind, PreKind, ListKind };

struct TOpen {
    std::string Name;
    unsigned Style = 0;
    std::string Link;
    EKind Kind = Plain;
};

struct TList {
    bool Ordered = false;
    long Next = 1;
};

class TBuilder {
public:
    // A void element (br, hr, img): written at once, it has no content
    void Void(const TTag& tag) {
        if (tag.Name == "br") {
            LineBreak();
        } else if (tag.Name == "hr") {
            BlockBoundary();
            Put("———");
            BlockBoundary();
        } else if (tag.Name == "img") {
            const auto alt = tag.Attributes.find("alt");
            Word(alt != tag.Attributes.end() && !alt->second.empty() ? alt->second : "🖼");
        }
    }

    // An element with content starts; Close ends the innermost one
    void Open(const TTag& tag) {
        if (IsBlock(tag.Name)) {
            BlockBoundary();
        }
        TOpen open;
        open.Name = tag.Name;
        const std::string& n = tag.Name;
        if (n == "b" || n == "strong") {
            open.Style = Bold;
        } else if (n == "i" || n == "em") {
            open.Style = Italic;
        } else if (n == "u" || n == "ins") {
            open.Style = Underline;
        } else if (n == "del" || n == "s" || n == "strike") {
            open.Style = Strike;
        } else if (n == "code") {
            open.Style = Pre ? 0 : Code;
        } else if (n.size() == 2 && n[0] == 'h' && n[1] >= '1' && n[1] <= '6') {
            open.Style = Bold | Heading;
        } else if (n == "a") {
            const auto href = tag.Attributes.find("href");
            if (href != tag.Attributes.end() && SafeLink(href->second)) {
                open.Link = href->second;
            }
        } else if (n == "span" && tag.Attributes.count("data-mx-spoiler")) {
            open.Style = Spoiler;
        } else if (n == "blockquote") {
            open.Style = Quote;
            open.Kind = QuoteKind;
            ++QuoteDepth;
        } else if (n == "pre") {
            open.Style = CodeBlock;
            open.Kind = PreKind;
            ++Pre;
        } else if (n == "ul" || n == "ol") {
            open.Kind = ListKind;
            TList list{n == "ol"};
            if (const auto start = tag.Attributes.find("start"); list.Ordered && start != tag.Attributes.end()) {
                try {
                    // Numbering goes on from the start, so a start near the limit of long would overflow it
                    list.Next = std::clamp(std::stol(start->second), -MaxListStart, MaxListStart);
                } catch (const std::exception&) {
                }
            }
            Lists.push_back(list);
        } else if (n == "li") {
            std::string marker(2 * (Lists.empty() ? 0 : Lists.size() - 1), ' ');
            if (!Lists.empty() && Lists.back().Ordered) {
                marker += std::to_string(Lists.back().Next++) + ". ";
            } else {
                marker += "• ";
            }
            Put(marker);
            DropSpace = true;
        } else if (n == "td" || n == "th") {
            if (!AtLineStart) {
                Put(" | ");
                DropSpace = true;
            }
            if (n == "th") {
                open.Style = Bold;
            }
        }
        Stack.push_back(std::move(open));
    }

    void Close() {
        if (Stack.empty()) {
            return;
        }
        const TOpen open = std::move(Stack.back());
        Stack.pop_back();
        if (open.Kind == QuoteKind) {
            --QuoteDepth;
        } else if (open.Kind == PreKind) {
            --Pre;
        } else if (open.Kind == ListKind && !Lists.empty()) {
            Lists.pop_back();
        }
        if (IsBlock(open.Name)) {
            BlockBoundary();
        }
    }

    void Text(std::string_view text) {
        if (Pre) {
            size_t start = 0;
            for (size_t i = 0; i <= text.size(); ++i) {
                if (i == text.size() || text[i] == '\n') {
                    std::string_view line = text.substr(start, i - start);
                    if (!line.empty() && line.back() == '\r') {
                        line.remove_suffix(1);
                    }
                    Put(line);
                    if (i < text.size()) {
                        LineBreak();
                    }
                    start = i + 1;
                }
            }
            return;
        }
        size_t i = 0;
        while (i < text.size()) {
            if (IsSpace(text[i])) {
                // The space keeps the style of where it was written: in "a <u>b</u>" it is not underlined
                if (!PendingSpace) {
                    PendingSpace = true;
                    SpaceStyle = Style();
                    SpaceLink = Link();
                }
                ++i;
                continue;
            }
            const size_t start = i;
            while (i < text.size() && !IsSpace(text[i])) {
                ++i;
            }
            Word(text.substr(start, i - start));
        }
    }

    std::vector<TRun> Finish() {
        // No line breaks at the end
        while (!Runs.empty()) {
            std::string& text = Runs.back().Text;
            while (!text.empty() && text.back() == '\n') {
                text.pop_back();
            }
            if (!text.empty()) {
                break;
            }
            Runs.pop_back();
        }
        return std::move(Runs);
    }

private:
    static bool IsSpace(char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
    }

    unsigned Style() const {
        unsigned style = 0;
        for (const auto& open : Stack) {
            style |= open.Style;
        }
        return style;
    }

    std::string Link() const {
        for (auto it = Stack.rbegin(); it != Stack.rend(); ++it) {
            if (!it->Link.empty()) {
                return it->Link;
            }
        }
        return {};
    }

    void Append(std::string_view text, unsigned style, const std::string& link) {
        if (!Runs.empty() && Runs.back().Style == style && Runs.back().Link == link) {
            Runs.back().Text += text;
        } else {
            Runs.push_back({std::string(text), style, link});
        }
    }

    void Word(std::string_view word) {
        if (PendingSpace && !DropSpace) {
            Append(" ", SpaceStyle, SpaceLink);  // never at a line start: DropSpace is set there
        }
        PendingSpace = false;
        Put(word);
        DropSpace = false;
    }

    // Writes text at the current position; a line inside a quote starts with its markers
    void Put(std::string_view text) {
        if (text.empty()) {
            return;
        }
        if (AtLineStart) {
            AtLineStart = false;
            for (int i = 0; i < std::min(QuoteDepth, 8); ++i) {
                Append("> ", Style() | Quote, {});
            }
        }
        Append(text, Style(), Link());
    }

    void LineBreak() {
        Append("\n", Style() & ~(Code | Spoiler), {});
        AtLineStart = true;
        DropSpace = true;
        PendingSpace = false;
    }

    // A block starts or ends on a line of its own
    void BlockBoundary() {
        if (!AtLineStart) {
            LineBreak();
        }
        DropSpace = true;
        PendingSpace = false;
    }

    std::vector<TRun> Runs;
    std::vector<TOpen> Stack;
    std::vector<TList> Lists;
    int QuoteDepth = 0;
    int Pre = 0;
    bool AtLineStart = true;
    bool PendingSpace = false;
    unsigned SpaceStyle = 0;
    std::string SpaceLink;
    bool DropSpace = true;
};

// Elements whose content is not shown: the reply fallback (the reply is shown from the replied-to event) and
// what a browser would not show either
bool SkipsContent(const std::string& name) {
    return name == "mx-reply" || name == "script" || name == "style" || name == "head" || name == "title";
}

bool IsVoid(const std::string& name) {
    return name == "br" || name == "hr" || name == "img";
}

// Recursive descent over the grammar
//   content := (text | comment | element | stray-closing-tag)*
//   element := start-tag content end-tag     (no content for void, self-closed and skipped elements)
// HTML in messages is often not well formed, so an element also ends where its parent's end tag comes, or at
// the end of the input; an end tag that closes nothing open is ignored.
class TParser {
public:
    explicit TParser(std::string_view html)
        : Html(html)
    {}

    std::vector<TRun> Parse() {
        Content();
        return Builder.Finish();
    }

private:
    // Parses content up to the end tag of the innermost open element, which it consumes, or up to an end tag
    // of an outer one, which it leaves for that element, or to the end of the input
    void Content() {
        while (Pos < Html.size()) {
            const size_t lt = Html.find('<', Pos);
            if (lt == std::string_view::npos) {
                Builder.Text(DecodeEntities(Html.substr(Pos)));
                Pos = Html.size();
                return;
            }
            Builder.Text(DecodeEntities(Html.substr(Pos, lt - Pos)));
            Pos = lt;
            if (Html.substr(Pos, 4) == "<!--") {
                const size_t end = Html.find("-->", Pos + 4);
                Pos = end == std::string_view::npos ? Html.size() : end + 3;
                continue;
            }
            TTag tag;
            const auto end = ParseTag(Html, Pos + 1, tag);
            if (!end && tag.Unended) {
                // A later '<' would scan to the end again, which is quadratic on a long body: the rest is text
                Builder.Text(DecodeEntities(Html.substr(Pos)));
                Pos = Html.size();
                return;
            }
            if (!end) {
                Builder.Text("<");  // a '<' that starts no tag is text
                ++Pos;
                continue;
            }
            if (tag.Closing) {
                if (!Open.empty() && Open.back() == tag.Name) {
                    Pos = *end;
                    return;
                }
                if (std::find(Open.begin(), Open.end(), tag.Name) != Open.end()) {
                    return;  // closes an outer element: the elements inside it end here
                }
                Pos = *end;  // closes nothing
                continue;
            }
            Pos = *end;
            Element(tag);
        }
    }

    void Element(const TTag& tag) {
        if (IsVoid(tag.Name)) {
            Builder.Void(tag);
            return;
        }
        if (tag.SelfClosing) {
            return;
        }
        if (SkipsContent(tag.Name)) {
            SkipTo(tag.Name);
            return;
        }
        if (Open.size() >= MaxDepth) {
            return;  // its content goes on as part of the parent's
        }
        Builder.Open(tag);
        Open.push_back(tag.Name);
        Content();
        Open.pop_back();
        Builder.Close();
    }

    // Past the end tag of the element, or to the end of the input
    void SkipTo(const std::string& name) {
        while (Pos < Html.size()) {
            const size_t lt = Html.find("</", Pos);
            if (lt == std::string_view::npos) {
                break;
            }
            TTag tag;
            const auto end = ParseTag(Html, lt + 1, tag);
            if (end && tag.Name == name) {
                Pos = *end;
                return;
            }
            if (tag.Unended) {
                break;
            }
            Pos = lt + 2;
        }
        Pos = Html.size();
    }

    std::string_view Html;
    size_t Pos = 0;
    std::vector<std::string> Open;  // names of the elements being parsed, outermost first
    TBuilder Builder;
};

} // namespace

std::vector<TRun> Parse(std::string_view html) {
    return TParser(html).Parse();
}

std::string PlainText(const std::vector<TRun>& runs) {
    std::string text;
    for (const auto& run : runs) {
        text += run.Text;
    }
    return text;
}

} // namespace NHoshi::NHtml
