#include "../css.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <type_traits>

using namespace nexora::browser;

static void TestValidStylesheet()
{
    const char source[] =
        "/* leading */ BODY { color: #369; background-color: white; font-size: 16px; "
        "font-weight: bold; margin-top: 8px; margin-bottom: 12px; text-align: center; }"
        "p { color: red } /* trailing comment */\n"
        "div { background-color: #123456; } span { font-size: 0px; }"
        "h1{} h2{} h3{} h4{} h5{} h6{} a{} img{}";
    CssStylesheet stylesheet;
    assert(ParseCss(source, sizeof(source) - 1, &stylesheet) == CssParseError::None);
    assert(stylesheet.RuleCount == 12);

    const CssRule &body = stylesheet.Rules[0];
    assert(body.Selector == CssTag::Body);
    assert(body.Declarations == (CssHasColor | CssHasBackgroundColor | CssHasFontSize |
                                 CssHasFontWeight | CssHasMarginTop | CssHasMarginBottom |
                                 CssHasTextAlign));
    assert(body.Color == 0x336699);
    assert(body.BackgroundColor == 0xFFFFFF);
    assert(body.FontSizePx == 16);
    assert(body.FontWeight == CssFontWeight::Bold);
    assert(body.MarginTopPx == 8);
    assert(body.MarginBottomPx == 12);
    assert(body.TextAlign == CssTextAlign::Center);
    assert(stylesheet.Rules[1].Color == 0xFF0000);
    assert(stylesheet.Rules[2].BackgroundColor == 0x123456);
    assert(stylesheet.Rules[3].FontSizePx == 0);
    assert(stylesheet.Rules[11].Selector == CssTag::Img);
}

static void TestCommentsAndOverrides()
{
    const char source[] = "p/* selector comment */{color: red; color: blue; /* ignored */}";
    CssStylesheet stylesheet;
    const CssParseError error = ParseCss(source, sizeof(source) - 1, &stylesheet);
    assert(error == CssParseError::None);
    assert(stylesheet.RuleCount == 1);
    assert(stylesheet.Rules[0].Color == 0x0000FF);
}

static void ExpectRejected(const char *source, CssParseError expected)
{
    CssStylesheet stylesheet;
    stylesheet.RuleCount = 1;
    stylesheet.Rules[0].Color = 0xDEADBEEF;
    assert(ParseCss(source, strlen(source), &stylesheet) == expected);
    assert(stylesheet.RuleCount == 0);
}

static void TestMalformedAndUnsupported()
{
    ExpectRejected("p { color: red", CssParseError::Malformed);
    ExpectRejected("p { color red; }", CssParseError::Malformed);
    ExpectRejected("p { color: red; /* unterminated", CssParseError::Malformed);
    ExpectRejected("p { color: red !important; }", CssParseError::Malformed);
    ExpectRejected("p { color: red; position: fixed; }", CssParseError::UnsupportedProperty);
    ExpectRejected("p { background-color: url(http://example.invalid/x); }",
                   CssParseError::InvalidValue);
    ExpectRejected("@import 'x';", CssParseError::UnsupportedSelector);
    ExpectRejected("p div { color: red; }", CssParseError::UnsupportedSelector);
    ExpectRejected("#target { color: red; }", CssParseError::UnsupportedSelector);
    ExpectRejected("p { color: expression(alert(1)); }", CssParseError::InvalidValue);
    ExpectRejected("p { font-size: -1px; }", CssParseError::InvalidValue);
    ExpectRejected("p { font-size: 257px; }", CssParseError::InvalidValue);
    ExpectRejected("p { text-align: justify; }", CssParseError::InvalidValue);
}

static void TestBoundsAndArguments()
{
    char oversized[CssMaximumSource + 1];
    memset(oversized, ' ', sizeof(oversized));
    CssStylesheet stylesheet;
    assert(ParseCss(oversized, sizeof(oversized), &stylesheet) == CssParseError::TooLarge);
    assert(stylesheet.RuleCount == 0);

    char manyRules[(CssMaximumRules + 1) * 5 + 1];
    size_t position = 0;
    for (size_t index = 0; index < CssMaximumRules + 1; ++index) {
        memcpy(manyRules + position, "p{}", 3);
        position += 3;
        manyRules[position++] = ' ';
    }
    assert(ParseCss(manyRules, position, &stylesheet) == CssParseError::TooManyRules);
    assert(stylesheet.RuleCount == 0);
    assert(ParseCss(nullptr, 0, &stylesheet) == CssParseError::InvalidArgument);
    assert(ParseCss("", 0, nullptr) == CssParseError::InvalidArgument);
}

static_assert(std::is_trivially_copyable<CssRule>::value, "CSS rules must be fixed-size values");
static_assert(std::is_trivially_copyable<CssStylesheet>::value,
              "CSS stylesheets must be fixed-size values");
static_assert(sizeof(CssStylesheet) <= CssMaximumRules * 32 + 8,
              "CSS stylesheet must remain a bounded fixed-size value");

int main()
{
    TestValidStylesheet();
    TestCommentsAndOverrides();
    TestMalformedAndUnsupported();
    TestBoundsAndArguments();
    return 0;
}
