#pragma once
#include <stddef.h>
#include <stdint.h>

namespace nexora::browser {

constexpr size_t HtmlMaximumSource = 524288;
constexpr size_t HtmlMaximumText = 8192;
constexpr size_t HtmlMaximumTitle = 128;
constexpr size_t HtmlMaximumLinks = 32;
constexpr size_t HtmlMaximumLinkText = 128;
constexpr size_t HtmlMaximumHref = 512;
constexpr size_t HtmlMaximumNodes = 256;
constexpr size_t HtmlMaximumDepth = 32;
constexpr size_t HtmlMaximumAttributesPerTag = 16;
constexpr size_t HtmlMaximumAttributeName = 32;
constexpr size_t HtmlMaximumAttributeValue = 1024;
constexpr size_t HtmlMaximumTagSource = 4096;
constexpr size_t HtmlMaximumStrings = 16384;
constexpr size_t HtmlMaximumStylesheets = 8;
constexpr size_t HtmlMaximumScripts = 8;
constexpr size_t HtmlMaximumScriptBytes = 16384;
constexpr size_t HtmlMaximumScriptLength = 8192;

struct HtmlLink {
    char Text[HtmlMaximumLinkText] = {};
    char Href[HtmlMaximumHref] = {};
    size_t TextLength = 0;
};

struct HtmlStringRef {
    uint16_t Offset = 0;
    uint16_t Length = 0;
};

enum class HtmlNodeType : uint8_t {
    Document,
    Element,
    Text
};

enum class HtmlTag : uint8_t {
    Unknown,
    Html,
    Head,
    Body,
    Title,
    A,
    Img,
    Br,
    Hr,
    P,
    Div,
    Section,
    Article,
    Header,
    Footer,
    Li,
    H1,
    H2,
    H3,
    H4,
    H5,
    H6,
    Table,
    Thead,
    Tbody,
    Tfoot,
    Caption,
    Tr,
    Td,
    Th,
    Ul,
    Ol,
    Dl,
    Dt,
    Dd,
    Main,
    Nav,
    Aside,
    Figure,
    Figcaption,
    Address,
    Span,
    Strong,
    B,
    Em,
    I,
    Small,
    Pre,
    Blockquote,
    Code,
    Sub,
    Sup,
    S,
    Del,
    Ins,
    Mark,
    Time,
    Q,
    Wbr,
    Meta,
    Link
};

enum class HtmlTextAlign : uint8_t {
    Default,
    Left,
    Center,
    Right,
    Justify
};

struct HtmlSafeStyle {
    HtmlStringRef Color;
    HtmlStringRef BackgroundColor;
    HtmlTextAlign TextAlign = HtmlTextAlign::Default;
    bool Bold = false;
    bool Italic = false;
    bool Underline = false;
    bool LineThrough = false;
};

struct HtmlNode {
    HtmlNodeType Type = HtmlNodeType::Element;
    HtmlTag Tag = HtmlTag::Unknown;
    uint16_t Parent = UINT16_MAX;
    uint16_t FirstChild = UINT16_MAX;
    uint16_t NextSibling = UINT16_MAX;
    HtmlStringRef Text;
    HtmlStringRef Href;
    HtmlStringRef Source;
    HtmlStringRef Alt;
    HtmlSafeStyle Style;
    uint16_t Width = 0;
    uint16_t Height = 0;
    bool HasWidth = false;
    bool HasHeight = false;
};

struct HtmlStylesheet {
    HtmlStringRef Href;
};

struct HtmlScript {
    uint16_t Offset = 0;
    uint16_t Length = 0;
};

struct HtmlDocument {
    char Title[HtmlMaximumTitle] = {};
    char Text[HtmlMaximumText] = {};
    HtmlLink Links[HtmlMaximumLinks] = {};
    size_t LinkCount = 0;
    size_t TextLength = 0;
    size_t TitleLength = 0;
    bool Truncated = false;
    bool UnsupportedContent = false;

    HtmlNode Nodes[HtmlMaximumNodes] = {};
    size_t NodeCount = 0;
    char Strings[HtmlMaximumStrings] = {};
    size_t StringLength = 0;
    HtmlStylesheet Stylesheets[HtmlMaximumStylesheets] = {};
    size_t StylesheetCount = 0;
    HtmlScript Scripts[HtmlMaximumScripts] = {};
    size_t ScriptCount = 0;
    char ScriptBytes[HtmlMaximumScriptBytes] = {};
    size_t ScriptByteLength = 0;
};

const char *HtmlString(const HtmlDocument &document, HtmlStringRef value);
const char *HtmlScriptData(const HtmlDocument &document, const HtmlScript &script);
bool ParseHtml(const char *source, size_t length, HtmlDocument *document);

}
