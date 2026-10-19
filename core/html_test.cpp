// The formatted body of messages: styles, blocks, links, entities, the reply fallback and broken HTML
#include "html.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace NHoshi::NHtml;

namespace {

int Failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++Failures;
    }
}

void ExpectText(const std::string& html, const std::string& text, const char* what) {
    const std::string actual = PlainText(Parse(html));
    if (actual != text) {
        std::printf("FAIL: %s\n  html:     %s\n  expected: %s\n  actual:   %s\n", what, html.c_str(), text.c_str(), actual.c_str());
        ++Failures;
    }
}

// The style of the run that contains the text
unsigned StyleOf(const std::vector<TRun>& runs, const std::string& text) {
    for (const auto& run : runs) {
        if (run.Text.find(text) != std::string::npos) {
            return run.Style;
        }
    }
    return ~0u;
}

std::string LinkOf(const std::vector<TRun>& runs, const std::string& text) {
    for (const auto& run : runs) {
        if (run.Text.find(text) != std::string::npos) {
            return run.Link;
        }
    }
    return "<none>";
}

void TestInline() {
    ExpectText("plain text", "plain text", "text without tags");
    const auto runs = Parse("<b>bold</b> and <i>italic</i>, <strong><em>both</em></strong>");
    Expect(StyleOf(runs, "bold") == Bold, "bold");
    Expect(StyleOf(runs, " and ") == 0, "plain between, both spaces included");
    const auto underlined = Parse("a <u>b</u> c");
    Expect(StyleOf(underlined, "a ") == 0 && StyleOf(underlined, " c") == 0, "spaces around an underlined word are not underlined");
    Expect(StyleOf(runs, "italic") == Italic, "italic");
    Expect(StyleOf(runs, "both") == (Bold | Italic), "nested styles add up");
    const auto more = Parse("<u>u</u><del>d</del><s>s</s><code>c</code>");
    Expect(StyleOf(more, "u") == Underline && StyleOf(more, "d") == Strike && StyleOf(more, "s") == Strike, "underline and strike");
    Expect(StyleOf(more, "c") == Code, "inline code");
    Expect(StyleOf(Parse("<span data-mx-spoiler>secret</span>"), "secret") == Spoiler, "spoiler");
    ExpectText("<img src=\"mxc://x/y\" alt=\"cat\">", "cat", "an image is its alt text");
}

void TestEntitiesAndSpaces() {
    ExpectText("a &amp; b &lt;c&gt; &#x41;&#66;", "a & b <c> AB", "character references");
    ExpectText("&#0;&#xD800;&#x110000;", "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD", "impossible code points become U+FFFD");
    ExpectText("&unknown; &amp", "&unknown; &amp", "unknown references stay");
    ExpectText("<p>  a\n  b  </p>", "a b", "spaces and newlines collapse");
    ExpectText("a&nbsp;&nbsp;b", "a\xC2\xA0\xC2\xA0" "b", "no-break spaces stay");
    ExpectText("1 < 2 and 3 > 2", "1 < 2 and 3 > 2", "a lone < is text");
    ExpectText("x <", "x <", "a < at the end is text");
}

void TestBlocks() {
    ExpectText("<p>a</p><p>b</p>", "a\nb", "paragraphs");
    ExpectText("a<br>b<br/><br>c", "a\nb\n\nc", "line breaks");
    ExpectText("<h1>Title</h1>text", "Title\ntext", "heading");
    Expect(StyleOf(Parse("<h2>Title</h2>"), "Title") == (Bold | Heading), "heading style");
    ExpectText("<ul><li>one</li><li> two </li></ul>", "• one\n• two", "list");
    ExpectText("<ol start=\"3\"><li>a</li><li>b</li></ol>", "3. a\n4. b", "numbered list with a start");
    ExpectText("<ul><li>a<ul><li>b</li></ul></li><li>c</li></ul>", "• a\n  • b\n• c", "nested list");
    ExpectText("before<blockquote>q1<br>q2</blockquote>after", "before\n> q1\n> q2\nafter", "quote");
    ExpectText("<blockquote>a<blockquote>b</blockquote></blockquote>", "> a\n> > b", "nested quote");
    const auto quote = Parse("<blockquote>quoted</blockquote>");
    Expect(StyleOf(quote, "quoted") == Quote && StyleOf(quote, "> ") == Quote, "quote style, markers included");
    ExpectText("<table><tr><td>a</td><td>b</td></tr><tr><th>c</th><td>d</td></tr></table>", "a | b\nc | d", "table");
    ExpectText("a<hr>b", "a\n———\nb", "rule");
}

void TestCode() {
    const auto runs = Parse("<pre><code class=\"language-cpp\">int x;\n  if (a &lt; b) {}\n</code></pre>after");
    ExpectText("<pre><code>int x;\n  if (a &lt; b) {}\n</code></pre>after", "int x;\n  if (a < b) {}\nafter", "code block keeps spaces");
    Expect(StyleOf(runs, "int x;") == CodeBlock, "code block style, not inline code");
}

void TestLinks() {
    const auto runs = Parse("<a href=\"https://example.org/?a=1&amp;b=2\">site</a> <a href='mailto:a@b.c'>mail</a> "
                            "<a href=\"javascript:alert(1)\">bad</a> <a>none</a>");
    Expect(LinkOf(runs, "site") == "https://example.org/?a=1&b=2", "https link with a decoded address");
    Expect(LinkOf(runs, "mail") == "mailto:a@b.c", "mailto link");
    Expect(LinkOf(runs, "bad").empty(), "javascript: is not a link");
    Expect(LinkOf(runs, "none").empty(), "no href, no link");
}

void TestSkipped() {
    ExpectText("<mx-reply><blockquote><a href=\"https://matrix.to/#/x\">In reply to</a> old</blockquote></mx-reply>new",
        "new", "the reply fallback is dropped");
    ExpectText("a<script>alert('<b>')</script>b<style>p{}</style>c", "abc", "script and style are dropped");
    ExpectText("a<!-- <b>hidden</b> -->b", "ab", "comments");
    ExpectText("a<mx-reply>never closed", "a", "an unclosed skipped element runs to the end");
}

void TestBroken() {
    ExpectText("<b>unclosed", "unclosed", "an unclosed element");
    Expect(StyleOf(Parse("<b>unclosed"), "unclosed") == Bold, "keeps its style");
    ExpectText("a</i>b", "ab", "an end tag that closes nothing");
    const auto crossed = Parse("<b><i>x</b>y</i>z");
    Expect(StyleOf(crossed, "x") == (Bold | Italic), "inside both");
    Expect(StyleOf(crossed, "y") == 0 && StyleOf(crossed, "z") == 0, "the outer end tag ends the inner element too");
    ExpectText("<ul><li>a<li>b</ul>c", "• a\n• b\nc", "list items without end tags");
    ExpectText("<p class=\"x\" title='y' hidden>t</p>", "t", "attributes of all kinds");
    ExpectText("<a href=\"unterminated>text", "<a href=\"unterminated>text", "a tag without its end is text");

    std::string deep;
    for (int i = 0; i < 10000; ++i) {
        deep += "<b>";
    }
    deep += "x";
    for (int i = 0; i < 10000; ++i) {
        deep += "</b>";
    }
    ExpectText(deep, "x", "deep nesting stops at the limit and does not crash");
    ExpectText("", "", "empty");
    ExpectText("<p></p><br><p> </p>", "", "only empty blocks");
}

} // namespace

int main() {
    TestInline();
    TestEntitiesAndSpaces();
    TestBlocks();
    TestCode();
    TestLinks();
    TestSkipped();
    TestBroken();
    if (Failures) {
        std::printf("%d failures\n", Failures);
        return EXIT_FAILURE;
    }
    std::printf("html_test: ok\n");
    return EXIT_SUCCESS;
}
