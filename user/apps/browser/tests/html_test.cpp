#include "../html.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

using namespace nexora::browser;

static void AssertString(const HtmlDocument &document, HtmlStringRef value, const char *expected)
{
    const size_t length = strlen(expected);
    assert(value.Length == length);
    assert(memcmp(HtmlString(document, value), expected, length) == 0);
}

static const HtmlNode *FindNode(const HtmlDocument &document, HtmlTag tag)
{
    for (size_t i = 0; i < document.NodeCount; ++i)
        if (document.Nodes[i].Type == HtmlNodeType::Element && document.Nodes[i].Tag == tag)
            return &document.Nodes[i];
    return nullptr;
}

static void AssertScript(const HtmlDocument &document, size_t index, const char *expected, size_t length)
{
    assert(index < document.ScriptCount);
    const HtmlScript &script = document.Scripts[index];
    assert(script.Length == length);
    assert(memcmp(HtmlScriptData(document, script), expected, length) == 0);
}

static void TestLegacyTextAndLinks()
{
    const char source[] =
        "<title>One &amp; Two</title><p>Hello <b>world</b></p><a href=\"/x\">Link</a>";
    HtmlDocument document;
    assert(ParseHtml(source, sizeof(source) - 1, &document));
    assert(strcmp(document.Title, "One & Two") == 0);
    assert(strcmp(document.Text, "Hello world\nLink") == 0);
    assert(document.LinkCount == 1);
    assert(strcmp(document.Links[0].Href, "/x") == 0);
    assert(strcmp(document.Links[0].Text, "Link") == 0);
    assert(document.NodeCount > 4);
    assert(document.Nodes[0].Type == HtmlNodeType::Document);
    const HtmlNode *paragraph = FindNode(document, HtmlTag::P);
    assert(paragraph && paragraph->FirstChild != UINT16_MAX);
    const HtmlNode *bold = FindNode(document, HtmlTag::B);
    assert(bold && bold->Parent != UINT16_MAX);

    const char unquoted[] = "<a href=/unquoted/path>Path</a>";
    assert(ParseHtml(unquoted, sizeof(unquoted) - 1, &document));
    assert(document.LinkCount == 1);
    assert(strcmp(document.Links[0].Href, "/unquoted/path") == 0);
}

static void TestImagesStylesAndStylesheetMetadata()
{
    const char source[] =
        "<link rel=\"stylesheet\" href=\"/site.css\">"
        "<p style=\"color: #369; background-color: white; font-weight: bold; "
        "font-style: italic; text-align: center; text-decoration: underline\">Text</p>"
        "<img src=\"/image.png\" alt=\"A picture\" width=\"320\" height=\"200\">";
    HtmlDocument document;
    assert(ParseHtml(source, sizeof(source) - 1, &document));
    assert(document.StylesheetCount == 1);
    AssertString(document, document.Stylesheets[0].Href, "/site.css");

    const HtmlNode *paragraph = FindNode(document, HtmlTag::P);
    assert(paragraph);
    AssertString(document, paragraph->Style.Color, "#369");
    AssertString(document, paragraph->Style.BackgroundColor, "white");
    assert(paragraph->Style.Bold && paragraph->Style.Italic && paragraph->Style.Underline);
    assert(paragraph->Style.TextAlign == HtmlTextAlign::Center);

    const HtmlNode *image = FindNode(document, HtmlTag::Img);
    assert(image && image->HasWidth && image->HasHeight);
    assert(image->Width == 320 && image->Height == 200);
    AssertString(document, image->Source, "/image.png");
    AssertString(document, image->Alt, "A picture");
}

static void TestActiveContentAndUnsafeAttributes()
{
    const char source[] =
        "<script>document.cookie='bad';</script>"
        "<p onclick=\"run()\">Visible</p>"
        "<a href=\"java&#x73;cript:alert(1)\">blocked</a>"
        "<a href=\"java&#x09;script:alert(1)\">also blocked</a>"
        "<img src=\" javascript:alert(1)\" alt=\"kept\">";
    HtmlDocument document;
    assert(ParseHtml(source, sizeof(source) - 1, &document));
    assert(document.UnsupportedContent);
    assert(strcmp(document.Text, "Visible\nblockedalso blocked") == 0);
    assert(document.LinkCount == 2);
    assert(document.Links[0].Href[0] == 0);
    assert(document.Links[1].Href[0] == 0);
    const HtmlNode *image = FindNode(document, HtmlTag::Img);
    assert(image && image->Source.Length == 0);
    AssertString(document, image->Alt, "kept");

    const char nulUrl[] = "<a href=\"java\0script:blocked\">NUL blocked</a>";
    assert(ParseHtml(nulUrl, sizeof(nulUrl) - 1, &document));
    assert(document.LinkCount == 1 && document.Links[0].Href[0] == 0);
}

static void TestRejectedStylesAndAttributeBounds()
{
    const char styleSource[] =
        "<p style=\"color: url(javascript:alert(1)); position: fixed; color: red\">Safe</p>"
        "<link rel=\"stylesheet\" href=\"javascript:alert(1)\">"
        "<img src=\"data:image/svg+xml,bad\" alt=\"not loaded\">";
    HtmlDocument document;
    assert(ParseHtml(styleSource, sizeof(styleSource) - 1, &document));
    assert(document.UnsupportedContent);
    const HtmlNode *paragraph = FindNode(document, HtmlTag::P);
    assert(paragraph);
    AssertString(document, paragraph->Style.Color, "red");
    const HtmlNode *image = FindNode(document, HtmlTag::Img);
    assert(image && image->Source.Length == 0);
    assert(document.StylesheetCount == 0);

    char manyAttributes[512] = "<p";
    size_t cursor = 2;
    for (size_t i = 0; i < HtmlMaximumAttributesPerTag + 1; ++i) {
        const int length = snprintf(manyAttributes + cursor, sizeof(manyAttributes) - cursor, " a%zu=\"x\"", i);
        assert(length > 0 && (size_t)length < sizeof(manyAttributes) - cursor);
        cursor += (size_t)length;
    }
    memcpy(manyAttributes + cursor, ">ok</p>", 7);
    cursor += 7;
    assert(ParseHtml(manyAttributes, cursor, &document));
    assert(document.UnsupportedContent);
    assert(strcmp(document.Text, "ok") == 0);

    char longAttribute[1200] = "<img alt=\"";
    cursor = strlen(longAttribute);
    memset(longAttribute + cursor, 'x', HtmlMaximumAttributeValue + 1);
    cursor += HtmlMaximumAttributeValue + 1;
    memcpy(longAttribute + cursor, "\">", 3);
    cursor += 2;
    assert(ParseHtml(longAttribute, cursor, &document));
    assert(document.UnsupportedContent);
    const HtmlNode *longImage = FindNode(document, HtmlTag::Img);
    assert(longImage && longImage->Alt.Length == 0);

    char longHref[600] = "<a href=\"";
    cursor = strlen(longHref);
    memset(longHref + cursor, 'x', HtmlMaximumHref + 1);
    cursor += HtmlMaximumHref + 1;
    memcpy(longHref + cursor, "\">x</a>", 7);
    cursor += 7;
    assert(ParseHtml(longHref, cursor, &document));
    assert(document.Truncated);

    char manyStrings[18000] = {};
    cursor = 0;
    for (size_t i = 0; i < 17; ++i) {
        memcpy(manyStrings + cursor, "<img alt=\"", 10);
        cursor += 10;
        memset(manyStrings + cursor, 'x', 1000);
        cursor += 1000;
        memcpy(manyStrings + cursor, "\">", 2);
        cursor += 2;
    }
    assert(ParseHtml(manyStrings, cursor, &document));
    assert(document.Truncated);
    assert(document.StringLength <= HtmlMaximumStrings);
}

static void TestBounds()
{
    static char oversized[HtmlMaximumSource + 1] = {};
    HtmlDocument document;
    assert(!ParseHtml(oversized, sizeof(oversized), &document));

    char deep[256] = {};
    size_t cursor = 0;
    for (size_t i = 0; i < HtmlMaximumDepth + 1; ++i) {
        memcpy(deep + cursor, "<div>", 5);
        cursor += 5;
    }
    assert(ParseHtml(deep, cursor, &document));
    assert(document.Truncated);
    assert(document.NodeCount <= HtmlMaximumNodes);

    char manyNodes[2400] = {};
    cursor = 0;
    for (size_t i = 0; i < HtmlMaximumNodes; ++i) {
        memcpy(manyNodes + cursor, "<br>", 4);
        cursor += 4;
    }
    assert(ParseHtml(manyNodes, cursor, &document));
    assert(document.Truncated);
    assert(document.NodeCount <= HtmlMaximumNodes);

    const char tooLongTag[HtmlMaximumTagSource + 2] = {'<', 'a'};
    assert(ParseHtml(tooLongTag, sizeof(tooLongTag), &document));
    assert(document.Truncated);
}

static void TestInlineScriptsPreserveRawData()
{
    const char first[] = "  const x = '&amp;';\r\nif (x) { <b>raw</b>; }\n";
    const char second[] = "window.ready = true;";
    char source[256] = {};
    size_t cursor = 0;
    memcpy(source + cursor, "Before<script>", 14);
    cursor += 14;
    memcpy(source + cursor, first, sizeof(first) - 1);
    cursor += sizeof(first) - 1;
    memcpy(source + cursor, "</script><script TYPE=\"application/javascript\">", 47);
    cursor += 47;
    memcpy(source + cursor, second, sizeof(second) - 1);
    cursor += sizeof(second) - 1;
    memcpy(source + cursor, "</script>After", 14);
    cursor += 14;

    HtmlDocument document;
    assert(ParseHtml(source, cursor, &document));
    assert(document.ScriptCount == 2);
    AssertScript(document, 0, first, sizeof(first) - 1);
    AssertScript(document, 1, second, sizeof(second) - 1);
    assert(document.ScriptByteLength == sizeof(first) + sizeof(second) - 2);
    assert(strcmp(document.Text, "BeforeAfter") == 0);
    assert(document.LinkCount == 0);
    assert(document.NodeCount == 3);
    assert(HtmlScriptData(document, HtmlScript{UINT16_MAX, 1})[0] == 0);
}

static void TestSkippedScriptTypesAndAttributes()
{
    const char source[] =
        "<script type=\"application/json\">{\"x\":1}</script>"
        "<script type=\"module\">import 'x';</script>"
        "<script type=\"text/javascript; charset=utf-8\">not classic</script>"
        "<script src=\"/external.js\">external contents</script>"
        "<script type=\"text/javascript\" onclick=\"run()\">event attr</script>"
        "<script type=\"text/javascript\" defer>boolean attr</script>"
        "<script type=text/javascript>unquoted type</script>"
        "<script type=\"text/javascript\">kept</script>"
        "<p>visible</p>";
    HtmlDocument document;
    assert(ParseHtml(source, sizeof(source) - 1, &document));
    assert(document.ScriptCount == 1);
    AssertScript(document, 0, "kept", 4);
    assert(document.UnsupportedContent);
    assert(strcmp(document.Text, "visible") == 0);
    assert(document.LinkCount == 0);
}

static void TestScriptBoundsAndMissingEnd()
{
    HtmlDocument document;
    char oneTooLong[HtmlMaximumScriptLength + 64] = "<script>";
    size_t cursor = strlen(oneTooLong);
    memset(oneTooLong + cursor, 'x', HtmlMaximumScriptLength + 1);
    cursor += HtmlMaximumScriptLength + 1;
    memcpy(oneTooLong + cursor, "</script><p>after</p>", 21);
    cursor += 21;
    assert(ParseHtml(oneTooLong, cursor, &document));
    assert(document.ScriptCount == 0);
    assert(document.UnsupportedContent);
    assert(strcmp(document.Text, "after") == 0);

    char maxScripts[256] = {};
    cursor = 0;
    for (size_t i = 0; i < HtmlMaximumScripts + 1; ++i) {
        memcpy(maxScripts + cursor, "<script>x</script>", 18);
        cursor += 18;
    }
    assert(ParseHtml(maxScripts, cursor, &document));
    assert(document.ScriptCount == HtmlMaximumScripts);
    assert(document.ScriptByteLength == HtmlMaximumScripts);
    assert(document.UnsupportedContent);
    for (size_t i = 0; i < document.ScriptCount; ++i)
        AssertScript(document, i, "x", 1);

    char totalLimit[HtmlMaximumScriptBytes + 256] = {};
    cursor = 0;
    for (size_t i = 0; i < HtmlMaximumScripts; ++i) {
        memcpy(totalLimit + cursor, "<script>", 8);
        cursor += 8;
        memset(totalLimit + cursor, (int)('a' + i), HtmlMaximumScriptBytes / HtmlMaximumScripts);
        cursor += HtmlMaximumScriptBytes / HtmlMaximumScripts;
        memcpy(totalLimit + cursor, "</script>", 9);
        cursor += 9;
    }
    assert(ParseHtml(totalLimit, cursor, &document));
    assert(document.ScriptCount == HtmlMaximumScripts);
    assert(document.ScriptByteLength == HtmlMaximumScriptBytes);
    assert(!document.UnsupportedContent);

    const char missingEnd[] = "before<script>unfinished <p>must not be parsed";
    assert(ParseHtml(missingEnd, sizeof(missingEnd) - 1, &document));
    assert(document.ScriptCount == 0);
    assert(document.UnsupportedContent);
    assert(strcmp(document.Text, "before") == 0);
    assert(FindNode(document, HtmlTag::P) == nullptr);
}

int main()
{
    TestLegacyTextAndLinks();
    TestImagesStylesAndStylesheetMetadata();
    TestActiveContentAndUnsafeAttributes();
    TestRejectedStylesAndAttributeBounds();
    TestBounds();
    TestInlineScriptsPreserveRawData();
    TestSkippedScriptTypesAndAttributes();
    TestScriptBoundsAndMissingEnd();
    return 0;
}
