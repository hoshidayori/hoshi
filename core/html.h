#pragma once

// The formatted body of a message (format "org.matrix.custom.html"), turned into styled text runs. Only the
// subset of HTML the spec recommends is understood ("Handling of formatted messages"); other tags keep their
// text and lose their meaning. Nothing is executed or fetched: images become their alt text, and a link keeps
// its address only for http, https and mailto.
//
// Blocks (paragraphs, list items, quotes, code blocks, headings) are separated by '\n'. List items start with
// "• " or "1. ", and every line of a quote with "> ", so a plain copy of the text still reads as a message.

#include <string>
#include <string_view>
#include <vector>

namespace NHoshi::NHtml {

enum EStyle : unsigned {
    Bold = 1u << 0,
    Italic = 1u << 1,
    Underline = 1u << 2,
    Strike = 1u << 3,
    Code = 1u << 4,       // inline code
    CodeBlock = 1u << 5,  // inside <pre>
    Quote = 1u << 6,      // inside <blockquote>, including the "> " markers
    Heading = 1u << 7,
    Spoiler = 1u << 8,    // data-mx-spoiler: hidden until the reader asks
};

struct TRun {
    std::string Text;  // UTF-8
    unsigned Style = 0;
    std::string Link;  // the address of a link, empty if the run is not one
};

std::vector<TRun> Parse(std::string_view html);

// The runs' text joined
std::string PlainText(const std::vector<TRun>& runs);

} // namespace NHoshi::NHtml
