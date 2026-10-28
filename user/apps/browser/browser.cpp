/*
 * Nexora Browser — native shell and bounded HTML text renderer for Nexora OS.
 *
 * This UI does not fake successful network navigation or pretend that addresses were fetched.
 * See docs/ENGINE_PORT.md.
 */
#include "hzapps.h"
#include "hzfile.h"
#include "hzsys.h"
#include "hzui.h"
#include "hzimage.h"
#include "css.h"
#include "html.h"
#include "http.h"
#include "tls.h"
#include "url.h"

using namespace hzui;
namespace BrowserUrl = nexora::browser;
using namespace BrowserUrl;
using BrowserUrl::HtmlDocument;
using BrowserUrl::HtmlTag;

namespace {
constexpr size_t MaxHistory = 300;
constexpr size_t MaxBookmarks = 100;
constexpr uint32_t NetworkReadTimeoutMs = 10000;
constexpr size_t NetworkReadChunk = 4096;
constexpr uint32_t NetworkReadyRetryMs = 1000;
constexpr uint32_t NetworkReadyRetryLimit = 12;
constexpr size_t BrowserMaximumImages = 8;
constexpr size_t BrowserMaximumImageBytes = 128 * 1024;
constexpr size_t BrowserMaximumTotalImageBytes = 512 * 1024;
constexpr uint64_t BrowserMaximumImagePixels = 512 * 1024;
constexpr uint64_t BrowserMaximumTotalImagePixels = 2 * 1024 * 1024;
const char *Profile = "C:\\Nexora\\Profiles\\Default\\Browser";
const char *Downloads = "C:\\Nexora\\Profiles\\Default\\Downloads";
const char *HistoryFile = "C:\\Nexora\\Profiles\\Default\\Browser\\history.txt";
const char *BookmarksFile = "C:\\Nexora\\Profiles\\Default\\Browser\\bookmarks.txt";
const char HomePage[] =
    "<!doctype html><html><head><title>Nexora Browser</title></head><body>"
    "<h1>Nexora Browser</h1>"
    "<p>Open a local file with an absolute path or a file:/// URL. HTTP and verified TLS 1.2 HTTPS "
    "pages are fetched and rendered as bounded text.</p>"
    "</body></html>";

bool HasLineBreak(const char *s)
{
    for (; s && *s; ++s)
        if (*s == '\n' || *s == '\r')
            return true;
    return false;
}

bool StartsWith(const char *Text, const char *Prefix)
{
    while (*Prefix && *Text == *Prefix) {
        ++Text;
        ++Prefix;
    }
    return *Prefix == 0;
}

bool CopyHtmlString(const BrowserUrl::HtmlDocument &Document, BrowserUrl::HtmlStringRef Value,
                    char *Output, size_t Capacity)
{
    if (!Output || !Capacity || Value.Length >= Capacity ||
        (size_t)Value.Offset + Value.Length > Document.StringLength)
        return false;
    const char *source = BrowserUrl::HtmlString(Document, Value);
    for (size_t index = 0; index < Value.Length; ++index)
        Output[index] = source[index];
    Output[Value.Length] = 0;
    return true;
}

bool IsRedirect(unsigned short StatusCode)
{
    return StatusCode == 301 || StatusCode == 302 || StatusCode == 303 ||
           StatusCode == 307 || StatusCode == 308;
}

bool IsHtml(const char *ContentType)
{
    return !ContentType[0] || !strcmp(ContentType, "text/html") ||
           !strcmp(ContentType, "application/xhtml+xml");
}

bool IsPlainText(const char *ContentType)
{
    return !strcmp(ContentType, "text/plain");
}

bool IsSupportedImageType(const char *ContentType)
{
    return !strcmp(ContentType, "image/png") || !strcmp(ContentType, "image/jpeg") ||
           !strcmp(ContentType, "image/gif") || !strcmp(ContentType, "image/bmp") ||
           !strcmp(ContentType, "image/x-ms-bmp");
}

const char *HttpErrorText(BrowserUrl::HttpParseError Error)
{
    switch (Error) {
    case BrowserUrl::HttpParseError::Incomplete: return "The HTTP response ended before it was complete.";
    case BrowserUrl::HttpParseError::InvalidStatus: return "The server sent an invalid HTTP status line.";
    case BrowserUrl::HttpParseError::InvalidHeader: return "The server sent malformed or oversized HTTP headers.";
    case BrowserUrl::HttpParseError::InvalidFraming: return "The HTTP response framing was invalid.";
    case BrowserUrl::HttpParseError::UnsupportedTransferEncoding: return "The server used an unsupported transfer encoding.";
    case BrowserUrl::HttpParseError::TooLarge: return "The HTTP response exceeded the browser's 512 KiB page limit.";
    default: return "The HTTP response could not be parsed.";
    }
}

class AnimatedPageText : public TextBlock {
public:
    explicit AnimatedPageText(FontRole Role) : TextBlock("", Role), target_(Foreground) {}

    void Reveal()
    {
        target_ = Foreground;
        fade_.Jump(0);
        fade_.To(1, Motion::ControlStateMs);
        Foreground = WithAlpha(target_, 0);
        StartAnimation();
    }

    bool Animate() override
    {
        Foreground = WithAlpha(target_, (uint32_t)(fade_.Value() * 255.0f));
        Invalidate();
        return fade_.Running();
    }

private:
    Transition fade_;
    Color target_;
};

struct PageTextStyle {
    FontRole Font = FontRole::Body;
    Color Foreground = Theme::TextPrimary;
    bool Bold = false;
    uint16_t MarginTopPx = 0;
    uint16_t MarginBottomPx = 0;
    bool HasMarginTop = false;
    bool HasMarginBottom = false;
};

struct BrowserImage {
    uint16_t NodeIndex = UINT16_MAX;
    hz::image::Bitmap Bitmap;
};

class BrowserPageView : public Element {
public:
    BrowserPageView()
    {
        Padding = Thickness(24, 20, 24, 20);
        Background = Theme::CardBackground;
        BorderColor = Theme::CardStroke;
        CornerRadius = Theme::OverlayRadius;
        HitTestVisible = false;
    }

    void SetDocument(const BrowserUrl::HtmlDocument *Document)
    {
        document_ = Document;
        InvalidateLayout();
        Invalidate();
    }

    void SetStylesheets(const CssStylesheet *Stylesheets, size_t Count)
    {
        stylesheetCount_ = hz::Min(Count, HtmlMaximumStylesheets);
        Background = Theme::CardBackground;
        for (size_t index = 0; index < stylesheetCount_; ++index)
            stylesheets_[index] = Stylesheets[index];
        for (size_t sheetIndex = 0; sheetIndex < stylesheetCount_; ++sheetIndex) {
            const CssStylesheet &sheet = stylesheets_[sheetIndex];
            for (size_t ruleIndex = 0; ruleIndex < sheet.RuleCount; ++ruleIndex) {
                const CssRule &rule = sheet.Rules[ruleIndex];
                if (rule.Selector == CssTag::Body &&
                    (rule.Declarations & CssHasBackgroundColor))
                    Background = 0xFF000000u | rule.BackgroundColor;
            }
        }
        InvalidateLayout();
        Invalidate();
    }

    void SetImages(const BrowserImage *Images, size_t Count)
    {
        images_ = Images;
        imageCount_ = Count;
        InvalidateLayout();
        Invalidate();
    }

    Size MeasureOverride(Size Available) override
    {
        Flow flow{0, 0, Available.Width, 0, 0};
        if (document_ && Available.Width > 0)
            Visit(0, PageTextStyle{}, flow, nullptr);
        return Size{Available.Width, flow.Y + flow.LineHeight};
    }

    void Paint(Painter &P) override
    {
        Element::Paint(P);
        if (!document_)
            return;
        const Rect content = ContentBounds();
        P.PushClip(content);
        Flow flow{content.X, content.Y, content.Width, 0, 0};
        Visit(0, PageTextStyle{}, flow, &P);
        P.PopClip();
    }

private:
    struct Flow {
        int32_t Left;
        int32_t Y;
        int32_t Width;
        int32_t XOffset;
        int32_t LineHeight;
    };

    static bool IsBlock(HtmlTag Tag)
    {
        return Tag == HtmlTag::Body || Tag == HtmlTag::P || Tag == HtmlTag::Div ||
               Tag == HtmlTag::Section || Tag == HtmlTag::Article || Tag == HtmlTag::Header ||
               Tag == HtmlTag::Footer || Tag == HtmlTag::Li ||
               (Tag >= HtmlTag::H1 && Tag <= HtmlTag::H6) || Tag == HtmlTag::Table ||
               Tag == HtmlTag::Thead || Tag == HtmlTag::Tbody || Tag == HtmlTag::Tfoot ||
               Tag == HtmlTag::Caption || Tag == HtmlTag::Tr || Tag == HtmlTag::Td ||
               Tag == HtmlTag::Th || Tag == HtmlTag::Ul || Tag == HtmlTag::Ol ||
               Tag == HtmlTag::Dl || Tag == HtmlTag::Dt || Tag == HtmlTag::Dd ||
               Tag == HtmlTag::Main || Tag == HtmlTag::Nav || Tag == HtmlTag::Aside ||
               Tag == HtmlTag::Figure || Tag == HtmlTag::Figcaption ||
               Tag == HtmlTag::Address || Tag == HtmlTag::Blockquote || Tag == HtmlTag::Pre;
    }

    static Color ParseColor(const HtmlDocument &Document, HtmlStringRef Value, Color Fallback)
    {
        const char *text = HtmlString(Document, Value);
        if (!Value.Length)
            return Fallback;
        struct NamedColor {
            const char *Name;
            Color Value;
        };
        static const NamedColor colors[] = {
            {"black", 0xFF000000}, {"white", 0xFFFFFFFF}, {"red", 0xFFFF0000},
            {"green", 0xFF008000}, {"blue", 0xFF0000FF}, {"gray", 0xFF808080},
            {"grey", 0xFF808080}, {"yellow", 0xFFFFFF00}, {"navy", 0xFF000080},
            {"purple", 0xFF800080}, {"teal", 0xFF008080}, {"maroon", 0xFF800000},
            {"lime", 0xFF00FF00}, {"aqua", 0xFF00FFFF}, {"silver", 0xFFC0C0C0},
            {"fuchsia", 0xFFFF00FF}, {"olive", 0xFF808000}, {"transparent", 0x00000000}
        };
        for (const NamedColor &color : colors) {
            size_t index = 0;
            while (index < Value.Length && color.Name[index] &&
                   (text[index] == color.Name[index] ||
                    (text[index] >= 'A' && text[index] <= 'Z' &&
                     text[index] - 'A' + 'a' == color.Name[index])))
                ++index;
            if (index == Value.Length && !color.Name[index])
                return color.Value;
        }
        if ((Value.Length != 4 && Value.Length != 7) || text[0] != '#')
            return Fallback;
        uint32_t rgb = 0;
        if (Value.Length == 4) {
            uint32_t channels[3] = {};
            for (size_t index = 0; index < 3; ++index) {
                const char c = text[index + 1];
                const uint32_t digit = c >= '0' && c <= '9' ? (uint32_t)(c - '0')
                                        : c >= 'a' && c <= 'f' ? (uint32_t)(c - 'a' + 10)
                                        : c >= 'A' && c <= 'F' ? (uint32_t)(c - 'A' + 10) : 16;
                if (digit > 15)
                    return Fallback;
                channels[index] = digit * 17;
            }
            rgb = (channels[0] << 16) | (channels[1] << 8) | channels[2];
            return 0xFF000000u | rgb;
        }
        for (size_t index = 1; index < Value.Length; ++index) {
            const char c = text[index];
            uint32_t digit = c >= '0' && c <= '9' ? (uint32_t)(c - '0')
                             : c >= 'a' && c <= 'f' ? (uint32_t)(c - 'a' + 10)
                             : c >= 'A' && c <= 'F' ? (uint32_t)(c - 'A' + 10) : 16;
            if (digit > 15)
                return Fallback;
            rgb = (rgb << 4) | digit;
        }
        return 0xFF000000u | rgb;
    }

    static FontRole FontForTag(HtmlTag Tag, bool Bold)
    {
        if (Tag == HtmlTag::H1)
            return FontRole::Display;
        if (Tag == HtmlTag::H2 || Tag == HtmlTag::H3)
            return FontRole::Title;
        if (Tag == HtmlTag::H4 || Tag == HtmlTag::H5 || Tag == HtmlTag::H6)
            return FontRole::Subtitle;
        if (Tag == HtmlTag::Small)
            return FontRole::Caption;
        if (Tag == HtmlTag::Strong || Tag == HtmlTag::B || Bold)
            return FontRole::BodyStrong;
        return FontRole::Body;
    }

    static bool ToCssTag(HtmlTag Tag, CssTag *Result)
    {
        switch (Tag) {
        case HtmlTag::Body: *Result = CssTag::Body; return true;
        case HtmlTag::P: *Result = CssTag::P; return true;
        case HtmlTag::Div: *Result = CssTag::Div; return true;
        case HtmlTag::Span: *Result = CssTag::Span; return true;
        case HtmlTag::H1: *Result = CssTag::H1; return true;
        case HtmlTag::H2: *Result = CssTag::H2; return true;
        case HtmlTag::H3: *Result = CssTag::H3; return true;
        case HtmlTag::H4: *Result = CssTag::H4; return true;
        case HtmlTag::H5: *Result = CssTag::H5; return true;
        case HtmlTag::H6: *Result = CssTag::H6; return true;
        case HtmlTag::A: *Result = CssTag::A; return true;
        case HtmlTag::Img: *Result = CssTag::Img; return true;
        default: return false;
        }
    }

    void ApplyStyles(HtmlTag Tag, PageTextStyle *Style)
    {
        CssTag selector;
        if (!document_ || !ToCssTag(Tag, &selector))
            return;
        for (size_t sheetIndex = 0; sheetIndex < stylesheetCount_; ++sheetIndex) {
            const CssStylesheet &sheet = stylesheets_[sheetIndex];
            for (size_t ruleIndex = 0; ruleIndex < sheet.RuleCount; ++ruleIndex) {
                const CssRule &rule = sheet.Rules[ruleIndex];
                if (rule.Selector != selector)
                    continue;
                if (rule.Declarations & CssHasColor)
                    Style->Foreground = 0xFF000000u | rule.Color;
                if (rule.Declarations & CssHasFontWeight) {
                    Style->Bold = rule.FontWeight == CssFontWeight::Bold;
                    if (Style->Font == FontRole::BodyStrong)
                        Style->Font = FontRole::Body;
                    if (Style->Bold && Style->Font == FontRole::Body)
                        Style->Font = FontRole::BodyStrong;
                }
                if (rule.Declarations & CssHasFontSize) {
                    if (rule.FontSizePx >= 36)
                        Style->Font = FontRole::Display;
                    else if (rule.FontSizePx >= 25)
                        Style->Font = FontRole::Title;
                    else if (rule.FontSizePx >= 19)
                        Style->Font = FontRole::Subtitle;
                    else if (rule.FontSizePx <= 12)
                        Style->Font = FontRole::Caption;
                    else
                        Style->Font = Style->Bold ? FontRole::BodyStrong : FontRole::Body;
                }
                if (rule.Declarations & CssHasMarginTop) {
                    Style->MarginTopPx = rule.MarginTopPx;
                    Style->HasMarginTop = true;
                }
                if (rule.Declarations & CssHasMarginBottom) {
                    Style->MarginBottomPx = rule.MarginBottomPx;
                    Style->HasMarginBottom = true;
                }
            }
        }
    }

    void FlowText(const char *text, size_t length, PageTextStyle style, Flow &flow, Painter *painter)
    {
        const FontRole fontRole =
            style.Bold && style.Font == FontRole::Body ? FontRole::BodyStrong : style.Font;
        HZ_FONT *font = GetFont(fontRole);
        if (!font)
            return;
        const int32_t lineHeight = HzFontLineHeight(font);
        flow.LineHeight = hz::Max(flow.LineHeight, lineHeight);
        size_t position = 0;
        while (position < length) {
            if (text[position] == '\n' || text[position] == '\r') {
                if (flow.XOffset) {
                    flow.Y += hz::Max(flow.LineHeight, lineHeight);
                    flow.XOffset = 0;
                    flow.LineHeight = 0;
                }
                ++position;
                continue;
            }
            if (text[position] == ' ' || text[position] == '\t') {
                ++position;
                if (flow.XOffset)
                    flow.XOffset += HzMeasureTextN(font, " ", 1);
                continue;
            }
            const size_t start = position;
            while (position < length && text[position] != ' ' && text[position] != '\t' &&
                   text[position] != '\n' && text[position] != '\r')
                ++position;
            const size_t wordLength = position - start;
            const int32_t wordWidth = HzMeasureTextN(font, text + start, wordLength);
            if (flow.XOffset && flow.XOffset + wordWidth > flow.Width) {
                flow.Y += hz::Max(flow.LineHeight, lineHeight);
                flow.XOffset = 0;
                flow.LineHeight = lineHeight;
            }
            if (painter && wordLength)
                painter->Text(font, flow.Left + flow.XOffset, flow.Y + HzFontAscent(font),
                              text + start, wordLength, style.Foreground);
            flow.XOffset += wordWidth;
        }
    }

    void NewLine(Flow &flow, int32_t extra = 0)
    {
        if (flow.XOffset || flow.LineHeight) {
            flow.Y += hz::Max(flow.LineHeight, 1);
            flow.XOffset = 0;
            flow.LineHeight = 0;
        }
        flow.Y += extra;
    }

    const hz::image::Bitmap *FindImage(uint16_t NodeIndex) const
    {
        for (size_t index = 0; index < imageCount_; ++index)
            if (images_[index].NodeIndex == NodeIndex)
                return &images_[index].Bitmap;
        return nullptr;
    }

    void FlowImage(uint16_t NodeIndex, const HtmlNode &node, PageTextStyle style,
                   Flow &flow, Painter *painter)
    {
        const hz::image::Bitmap *image = FindImage(NodeIndex);
        if (!image || !image->Pixels) {
            const char *alt = HtmlString(*document_, node.Alt);
            FlowText(node.Alt.Length ? alt : "[image]",
                     node.Alt.Length ? node.Alt.Length : 7, style, flow, painter);
            return;
        }
        int32_t width = node.HasWidth ? node.Width : image->Width;
        int32_t height = node.HasHeight ? node.Height : image->Height;
        if (node.HasWidth && !node.HasHeight)
            height = (int32_t)((int64_t)image->Height * width / image->Width);
        else if (!node.HasWidth && node.HasHeight)
            width = (int32_t)((int64_t)image->Width * height / image->Height);
        if (width > 800 || height > 600) {
            const float scale = hz::Min(800.0f / width, 600.0f / height);
            width = hz::Max(1, (int32_t)(width * scale));
            height = hz::Max(1, (int32_t)(height * scale));
        }
        if (flow.XOffset && flow.XOffset + width > flow.Width) {
            flow.Y += hz::Max(flow.LineHeight, 1);
            flow.XOffset = 0;
            flow.LineHeight = 0;
        }
        if (painter)
            painter->DrawBitmap(Rect{flow.Left + flow.XOffset, flow.Y, width, height},
                                image->Pixels, image->Width, image->Height);
        flow.XOffset += width + 8;
        flow.LineHeight = hz::Max(flow.LineHeight, height);
    }

    void Visit(uint16_t index, PageTextStyle inherited, Flow &flow, Painter *painter)
    {
        if (!document_ || index >= document_->NodeCount)
            return;
        const HtmlNode &node = document_->Nodes[index];
        if (node.Tag == HtmlTag::Head || node.Tag == HtmlTag::Title || node.Tag == HtmlTag::Meta ||
            node.Tag == HtmlTag::Link)
            return;
        PageTextStyle style = inherited;
        if (node.Type == HtmlNodeType::Element) {
            style.MarginTopPx = 0;
            style.MarginBottomPx = 0;
            style.HasMarginTop = false;
            style.HasMarginBottom = false;
            style.Font = FontForTag(node.Tag, inherited.Bold);
            style.Bold = inherited.Bold || node.Tag == HtmlTag::Strong || node.Tag == HtmlTag::B;
            ApplyStyles(node.Tag, &style);
            if (node.Style.Color.Length)
                style.Foreground = ParseColor(*document_, node.Style.Color, style.Foreground);
            if (node.Style.Bold) {
                style.Bold = true;
                style.Font = FontRole::BodyStrong;
            }
        }
        const bool block = node.Type == HtmlNodeType::Element && IsBlock(node.Tag);
        if (block)
            NewLine(flow, style.HasMarginTop ? style.MarginTopPx
                                             : (node.Tag == HtmlTag::H1 || node.Tag == HtmlTag::H2 ? 8 : 4));
        if (node.Type == HtmlNodeType::Text) {
            const char *text = HtmlString(*document_, node.Text);
            FlowText(text, node.Text.Length, inherited, flow, painter);
        } else if (node.Type == HtmlNodeType::Element && node.Tag == HtmlTag::Img) {
            FlowImage(index, node, style, flow, painter);
        } else if (node.Tag == HtmlTag::Br) {
            NewLine(flow);
        } else {
            for (uint16_t child = node.FirstChild; child != UINT16_MAX;
                 child = document_->Nodes[child].NextSibling)
                Visit(child, style, flow, painter);
        }
        if (block)
            NewLine(flow, style.HasMarginBottom ? style.MarginBottomPx : 4);
    }

    const BrowserUrl::HtmlDocument *document_ = nullptr;
    CssStylesheet stylesheets_[HtmlMaximumStylesheets] = {};
    size_t stylesheetCount_ = 0;
    const BrowserImage *images_ = nullptr;
    size_t imageCount_ = 0;
};

hz::String Normalize(const hz::String &Input)
{
    if (Input.Empty() || HasLineBreak(Input.CStr()))
        return hz::String();
    if (StartsWith(Input.CStr(), "https://") || StartsWith(Input.CStr(), "http://") ||
        StartsWith(Input.CStr(), "file:///"))
        return Input;

    if (Input.Length() >= 3 &&
        ((Input[0] >= 'A' && Input[0] <= 'Z') || (Input[0] >= 'a' && Input[0] <= 'z')) &&
        Input[1] == ':' && (Input[2] == '\\' || Input[2] == '/')) {
        hz::String Address("file:///");
        for (const char *P = Input.CStr(); *P; ++P) {
            if (*P == '\\')
                Address.Append('/');
            else if (*P == ' ')
                Address.Append("%20");
            else if (*P == '%' || *P == '#' || *P == '?') {
                const char *Hex = "0123456789ABCDEF";
                Address.Append('%');
                Address.Append(Hex[((uint8_t)*P) >> 4]);
                Address.Append(Hex[((uint8_t)*P) & 0x0F]);
            } else
                Address.Append(*P);
        }
        return Address;
    }

    bool Domain = false;
    for (const char *p = Input.CStr(); *p; ++p) {
        if (*p == '.' && p != Input.CStr() && p[1])
            Domain = true;
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '-' || *p == '.' || *p == '/' || *p == ':' || *p == '?' || *p == '&' || *p == '=' ||
              *p == '%' || *p == '_' || *p == '~' || *p == '#'))
            return hz::String();
    }
    return Domain ? hz::String::Format("http://%s", Input.CStr()) : hz::String();
}

void LoadLines(const char *Path, hz::Vector<hz::String> &Out, size_t Limit)
{
    Out.Clear();
    hz::String Text;
    if (!HZ_SUCCESS(hz::fs::ReadAll(hz::String(Path), Text, 256 * 1024)))
        return;
    const char *p = Text.CStr(), *End = p + Text.Length();
    while (p < End && Out.Size() < Limit) {
        const char *q = p;
        while (q < End && *q != '\n' && *q != '\r')
            ++q;
        if (q > p) {
            hz::String Line(p, (size_t)(q - p));
            if (!HasLineBreak(Line.CStr()))
                Out.Push(hz::Move(Line));
        }
        while (q < End && (*q == '\n' || *q == '\r'))
            ++q;
        p = q;
    }
}

bool SaveLines(const char *Path, const hz::Vector<hz::String> &Items)
{
    hz::String Text;
    for (const auto &Item : Items) {
        if (HasLineBreak(Item.CStr()))
            continue;
        Text.Append(Item);
        Text.Append('\n');
    }
    return HZ_SUCCESS(hz::fs::WriteAll(hz::String(Path), Text.CStr(), Text.Length()));
}

void EnsureDir(const char *Path)
{
    if (!hz::fs::Exists(hz::String(Path)))
        hz::fs::CreateDirectory(hz::String(Path));
}

void EnsureProfile()
{
    EnsureDir("C:\\Nexora\\Profiles");
    EnsureDir("C:\\Nexora\\Profiles\\Default");
    EnsureDir(Profile);
    EnsureDir(Downloads);
}

int HexDigit(char C)
{
    if (C >= '0' && C <= '9')
        return C - '0';
    if (C >= 'a' && C <= 'f')
        return C - 'a' + 10;
    if (C >= 'A' && C <= 'F')
        return C - 'A' + 10;
    return -1;
}

bool DecodeLocalPath(const char *UrlPath, hz::String &Path)
{
    Path.Clear();
    for (size_t I = 0; UrlPath[I]; ++I) {
        char C = UrlPath[I];
        if (C == '%') {
            int Hi = HexDigit(UrlPath[I + 1]), Lo = UrlPath[I + 1] ? HexDigit(UrlPath[I + 2]) : -1;
            if (Hi < 0 || Lo < 0)
                return false;
            C = (char)((Hi << 4) | Lo);
            I += 2;
            if (!C || (uint8_t)C < 0x20 || (uint8_t)C == 0x7F)
                return false;
        }
        Path.Append(C == '/' ? '\\' : C);
    }
    return Path.Length() >= 3 && Path[1] == ':' && Path[2] == '\\';
}

hz::String FileUrl(const hz::String &Path)
{
    hz::String Address("file:///");
    const char *Hex = "0123456789ABCDEF";
    for (const char *P = Path.CStr(); *P; ++P) {
        uint8_t C = (uint8_t)*P;
        if (C == '\\')
            Address.Append('/');
        else if (C == ' ' || C == '%' || C == '#' || C == '?') {
            Address.Append('%');
            Address.Append(Hex[C >> 4]);
            Address.Append(Hex[C & 0x0F]);
        } else
            Address.Append(*P);
    }
    return Address;
}

enum class Page { Start, History, Bookmarks, Downloads };

class Browser {
public:
    bool Create();
    ~Browser();

private:
    void Build();
    void Go(const hz::String &Raw, bool Record = true);
    void StartPage();
    void ShowList(Page View);
    void Back();
    void Forward();
    void Bookmark();
    void Refresh();
    void UpdateButtons();
    void SetTabTitle(const char *Title);
    void Status(const char *Text);
    void UsePageText();
    void UsePageDocument();
    void ClearImages();
    void CancelNetworkRetry();
    void RetryWhenNetworkReady(const hz::String &Address);
    void ShowLocalFile(const hz::String &Address);
    void ShowRemote(const BrowserUrl::Url &Initial, const hz::String &InitialAddress, bool Stored);
    bool FetchHttp(const BrowserUrl::Url &Url, char *Wire, size_t WireCapacity,
                   BrowserUrl::HttpResponse *Response, char *Body, size_t BodyCapacity,
                   size_t *BodyLength, char *Error, size_t ErrorCapacity);
    bool LoadStylesheets(const BrowserUrl::Url &Base, char *Wire, size_t WireCapacity,
                         char *Body, size_t BodyCapacity);
    bool LoadImages(const BrowserUrl::Url &Base, char *Wire, size_t WireCapacity,
                    char *Body, size_t BodyCapacity);
    void SetLinks(const BrowserUrl::HtmlDocument &Document);
    void OpenLink(size_t Index);

    Window *window_ = nullptr;
    Button *tab_ = nullptr;
    TextBox *address_ = nullptr;
    Button *back_ = nullptr, *forward_ = nullptr, *reload_ = nullptr, *bookmark_ = nullptr;
    TextBlock *status_ = nullptr;
    TextBlock *heading_ = nullptr, *detail_ = nullptr;
    AnimatedPageText *items_ = nullptr;
    BrowserPageView *pageView_ = nullptr;
    StackPanel *pageHost_ = nullptr;
    ScrollViewer *pageScroll_ = nullptr;
    ListView *links_ = nullptr;
    BrowserUrl::HtmlDocument document_{};
    CssStylesheet stylesheets_[HtmlMaximumStylesheets] = {};
    size_t stylesheetCount_ = 0;
    BrowserImage images_[BrowserMaximumImages] = {};
    size_t imageCount_ = 0;
    hz::Vector<hz::String> linkTargets_, linkLabels_;
    hz::Vector<hz::String> history_, bookmarks_;
    hz::String currentAddress_;
    size_t index_ = 0;
    bool hasHistory_ = false;
    bool networkNotReady_ = false;
    bool retryingNetwork_ = false;
    uint32_t networkRetryAttempts_ = 0;
    uint64_t networkRetryTimer_ = 0;
    hz::String networkRetryAddress_;
    Page page_ = Page::Start;
};

bool Browser::Create()
{
    EnsureProfile();
    LoadLines(HistoryFile, history_, MaxHistory);
    LoadLines(BookmarksFile, bookmarks_, MaxBookmarks);
    hasHistory_ = !history_.Empty();
    index_ = hasHistory_ ? history_.Size() - 1 : 0;

    /* Keep page and chrome colors on an opaque surface; the other apps may still opt into Mica. */
    HZ_DISPLAY_INFO Display{};
    STATUS DisplayStatus = NtUserGetDisplayInfo(&Display);
    if (!HZ_SUCCESS(DisplayStatus)) {
        HzPrint("browser: could not read display work area (0x%08X)", (uint32_t)DisplayStatus);
        return false;
    }
    Rect Area = Rect::FromHz(Display.WorkArea);
    if (Area.Empty()) {
        HzPrint("browser: display work area is empty");
        return false;
    }
    int32_t Width = hz::Min(1120, hz::Max(760, Area.Width - 32));
    int32_t Height = hz::Min(740, hz::Max(500, Area.Height - 16));
    window_ = new Window("Nexora Browser", Width, Height, Window::DefaultStyle & ~HZ_WS_MICA);
    window_->SetMinimumSize(760, 500);
    Build();
    window_->OnClosed = [this]() {
        SaveLines(HistoryFile, history_);
        SaveLines(BookmarksFile, bookmarks_);
        NtUserPostQuitMessage(0);
    };
    window_->OnUnhandledKey = [this](KeyEvent &e) {
        if (e.Ctrl() && e.Key == 'L') {
            e.Handled = true;
            window_->SetFocus(address_, true);
            address_->SelectAll();
        } else if (e.Ctrl() && e.Key == 'D') {
            e.Handled = true;
            Bookmark();
        } else if (e.Ctrl() && e.Key == 'H') {
            e.Handled = true;
            ShowList(Page::History);
        } else if (e.Ctrl() && e.Key == 'B') {
            e.Handled = true;
            ShowList(Page::Bookmarks);
        } else if (e.Ctrl() && e.Key == 'J') {
            e.Handled = true;
            ShowList(Page::Downloads);
        } else if (e.Ctrl() && e.Key == '1') {
            e.Handled = true;
            StartPage();
        } else if (e.Alt() && e.Key == HZ_VK_LEFT) {
            e.Handled = true;
            Back();
        } else if (e.Alt() && e.Key == HZ_VK_RIGHT) {
            e.Handled = true;
            Forward();
        } else if (e.Key == HZ_VK_F1 + 4) {
            e.Handled = true;
            Refresh();
        }
    };
    StartPage();
    if (!window_->Show())
        return false;
    window_->SetFocus(address_);
    return true;
}

Browser::~Browser()
{
    ClearImages();
}

void Browser::Build()
{
    Grid *root = new Grid();
    root->Rows.Push(GridLength::AutoSize());
    root->Rows.Push(GridLength::AutoSize());
    root->Rows.Push(GridLength::Stars(1));
    root->Rows.Push(GridLength::AutoSize());

    Grid *tabs = root->Add<Grid>();
    tabs->Padding = Thickness(20, 10, 20, 4);
    tabs->ColumnSpacing = 8;
    tabs->Columns.Push(GridLength::AutoSize());
    tabs->Columns.Push(GridLength::Stars(1));
    tabs->Columns.Push(GridLength::AutoSize());
    tab_ = tabs->Add<Button>("Start page", ButtonStyle::Subtle);
    tab_->Glyph = Icon::Nexora;
    tab_->SetEnabled(false);
    tab_->MaxWidth = 240;
    Button *startPage = tabs->Add<Button>("Start page", ButtonStyle::Subtle);
    startPage->GridColumn = 2;
    startPage->Glyph = Icon::Home;
    startPage->ToolTip = "Return to the start page in the current tab; multiple tabs are not supported";
    startPage->OnClick = [this]() { StartPage(); };

    Grid *bar = root->Add<Grid>();
    bar->GridRow = 1;
    bar->Padding = Thickness(20, 6, 20, 12);
    bar->ColumnSpacing = 8;
    bar->Columns.Push(GridLength::AutoSize());
    bar->Columns.Push(GridLength::AutoSize());
    bar->Columns.Push(GridLength::Stars(1));

    Grid *navigation = bar->Add<Grid>();
    navigation->Background = Theme::CardBackground;
    navigation->BorderColor = Theme::CardStroke;
    navigation->CornerRadius = Theme::OverlayRadius;
    navigation->Padding = Thickness(4, 0, 4, 0);
    navigation->ColumnSpacing = 2;
    navigation->Columns.Push(GridLength::AutoSize());
    navigation->Columns.Push(GridLength::AutoSize());
    navigation->Columns.Push(GridLength::AutoSize());
    navigation->Columns.Push(GridLength::AutoSize());
    back_ = navigation->Add<Button>("", ButtonStyle::Subtle);
    back_->Glyph = Icon::ChevronLeft;
    back_->ToolTip = "Back · Alt+Left";
    back_->OnClick = [this]() { Back(); };
    forward_ = navigation->Add<Button>("", ButtonStyle::Subtle);
    forward_->GridColumn = 1;
    forward_->Glyph = Icon::ChevronRight;
    forward_->ToolTip = "Forward · Alt+Right";
    forward_->OnClick = [this]() { Forward(); };
    reload_ = navigation->Add<Button>("", ButtonStyle::Subtle);
    reload_->GridColumn = 2;
    reload_->Glyph = Icon::Refresh;
    reload_->ToolTip = "Reload · F5";
    reload_->OnClick = [this]() { Refresh(); };
    Button *home = navigation->Add<Button>("", ButtonStyle::Subtle);
    home->GridColumn = 3;
    home->Glyph = Icon::Home;
    home->ToolTip = "Home · Ctrl+1";
    home->OnClick = [this]() { StartPage(); };

    Grid *library = bar->Add<Grid>();
    library->GridColumn = 1;
    library->Background = Theme::CardBackground;
    library->BorderColor = Theme::CardStroke;
    library->CornerRadius = Theme::OverlayRadius;
    library->Padding = Thickness(4, 0, 4, 0);
    library->ColumnSpacing = 2;
    library->Columns.Push(GridLength::AutoSize());
    library->Columns.Push(GridLength::AutoSize());
    library->Columns.Push(GridLength::AutoSize());
    Button *hist = library->Add<Button>("", ButtonStyle::Subtle);
    hist->Glyph = Icon::Clock;
    hist->ToolTip = "History · Ctrl+H";
    hist->OnClick = [this]() { ShowList(Page::History); };
    Button *downloads = library->Add<Button>("", ButtonStyle::Subtle);
    downloads->GridColumn = 1;
    downloads->Glyph = Icon::Folder;
    downloads->ToolTip = "Downloads · Ctrl+J";
    downloads->OnClick = [this]() { ShowList(Page::Downloads); };
    Button *bookmarks = library->Add<Button>("", ButtonStyle::Subtle);
    bookmarks->GridColumn = 2;
    bookmarks->Glyph = Icon::Star;
    bookmarks->ToolTip = "Bookmarks · Ctrl+B";
    bookmarks->OnClick = [this]() { ShowList(Page::Bookmarks); };

    Grid *addressPill = bar->Add<Grid>();
    addressPill->GridColumn = 2;
    addressPill->Background = Theme::CardBackground;
    addressPill->BorderColor = Theme::CardStroke;
    addressPill->CornerRadius = Theme::OverlayRadius;
    addressPill->Padding = Thickness(5, 4, 5, 4);
    addressPill->ColumnSpacing = 6;
    addressPill->Columns.Push(GridLength::AutoSize());
    addressPill->Columns.Push(GridLength::Stars(1));
    addressPill->Columns.Push(GridLength::AutoSize());
    Button *addressAction = addressPill->Add<Button>("", ButtonStyle::Subtle);
    addressAction->Glyph = Icon::Globe;
    addressAction->ToolTip = "Focus address · Ctrl+L";
    addressAction->OnClick = [this]() {
        window_->SetFocus(address_, true);
        address_->SelectAll();
    };
    address_ = addressPill->Add<TextBox>("Search or enter address");
    address_->GridColumn = 1;
    address_->MaxLength = 2048;
    address_->MinWidth = 120;
    address_->OnSubmit = [this](const hz::String &Text) { Go(Text); };
    bookmark_ = addressPill->Add<Button>("", ButtonStyle::Subtle);
    bookmark_->GridColumn = 2;
    bookmark_->Glyph = Icon::Star;
    bookmark_->ToolTip = "Bookmark · Ctrl+D";
    bookmark_->OnClick = [this]() { Bookmark(); };

    Grid *body = root->Add<Grid>();
    body->GridRow = 2;
    body->MaxWidth = 1040;
    body->HorizontalAlignment = Align::Center;
    body->Padding = Thickness(40, 32, 40, 28);
    body->Background = Theme::CardBackground;
    body->BorderColor = Theme::CardStroke;
    body->CornerRadius = Theme::OverlayRadius;
    body->Margin = Thickness(16, 0, 16, 8);
    body->Rows.Push(GridLength::AutoSize());
    body->Rows.Push(GridLength::AutoSize());
    body->Rows.Push(GridLength::Stars(1));
    body->Rows.Push(GridLength::Pixels(126));
    heading_ = body->Add<TextBlock>("Nexora Browser", FontRole::Display);
    heading_->Margin = Thickness(0, 0, 0, 8);
    detail_ = body->Add<TextBlock>("A calm, native starting point for the web.", FontRole::Body);
    detail_->GridRow = 1;
    detail_->Foreground = Theme::TextSecondary;
    detail_->Wrap = true;

    pageScroll_ = body->Add<ScrollViewer>();
    pageScroll_->GridRow = 2;
    pageScroll_->Margin = Thickness(0, 22, 0, 0);
    pageHost_ = new StackPanel(Orientation::Vertical);
    items_ = new AnimatedPageText(FontRole::BodyLarge);
    items_->Wrap = true;
    items_->Ellipsis = false;
    items_->Padding = Thickness(24);
    items_->Background = Theme::CardBackground;
    items_->BorderColor = Theme::CardStroke;
    items_->CornerRadius = Theme::OverlayRadius;
    pageHost_->AddChild(items_);
    pageView_ = new BrowserPageView();
    pageView_->SetVisible(false);
    pageHost_->AddChild(pageView_);
    pageScroll_->SetContent(pageHost_);

    links_ = body->Add<ListView>();
    links_->GridRow = 3;
    links_->ItemHeight = 30;
    links_->SetItemCount(0);
    links_->PaintItem = [this](Painter &P, const Rect &R, size_t Index) {
        if (Index >= linkLabels_.Size() || Index >= linkTargets_.Size())
            return;
        P.DrawIcon(Icon::ChevronRight, Rect{R.X + 6, R.Y + (R.Height - 14) / 2, 14, 14}, Theme::Accent);
        hz::String Text = hz::String::Format("%s  ·  %s", linkLabels_[Index].CStr(), linkTargets_[Index].CStr());
        P.TextLine(GetFont(FontRole::Caption), Rect{R.X + 26, R.Y, R.Width - 32, R.Height}, Text.CStr(),
                   Theme::TextPrimary);
    };
    links_->OnItemInvoked = [this](size_t Index) { OpenLink(Index); };

    Grid *footer = root->Add<Grid>();
    footer->GridRow = 3;
    footer->Padding = Thickness(20, 8, 20, 10);
    footer->Columns.Push(GridLength::Stars(1));
    status_ = footer->Add<TextBlock>("Ready", FontRole::Caption);
    window_->SetContent(root);
}

void Browser::Status(const char *Text)
{
    status_->SetText(Text);
}

void Browser::UsePageText()
{
    ClearImages();
    if (items_)
        items_->SetVisible(true);
    if (pageView_)
        pageView_->SetVisible(false);
}

void Browser::UsePageDocument()
{
    if (items_)
        items_->SetVisible(false);
    if (pageView_) {
        pageView_->SetDocument(&document_);
        pageView_->SetStylesheets(stylesheets_, stylesheetCount_);
        pageView_->SetImages(images_, imageCount_);
        pageView_->SetVisible(true);
    }
}

void Browser::ClearImages()
{
    for (size_t index = 0; index < imageCount_; ++index) {
        images_[index].Bitmap.Release();
        images_[index].NodeIndex = UINT16_MAX;
    }
    imageCount_ = 0;
    if (pageView_)
        pageView_->SetImages(images_, 0);
}

void Browser::CancelNetworkRetry()
{
    if (networkRetryTimer_ && window_)
        window_->KillTimer(networkRetryTimer_);
    networkRetryTimer_ = 0;
    networkRetryAttempts_ = 0;
    networkRetryAddress_.Clear();
}

void Browser::RetryWhenNetworkReady(const hz::String &Address)
{
    if (!networkNotReady_ || networkRetryTimer_ || !window_)
        return;
    if (networkRetryAttempts_ >= NetworkReadyRetryLimit) {
        Status("Network unavailable · automatic retries stopped");
        HzPrint("browser: network unavailable after %u retries", networkRetryAttempts_);
        return;
    }
    if (networkRetryAddress_.Empty())
        networkRetryAddress_ = Address;
    networkRetryTimer_ = window_->SetTimer(NetworkReadyRetryMs, [this]() {
        window_->KillTimer(networkRetryTimer_);
        networkRetryTimer_ = 0;
        networkRetryAttempts_++;
        hz::String Address = networkRetryAddress_;
        retryingNetwork_ = true;
        Go(Address, false);
        retryingNetwork_ = false;
        if (!networkNotReady_) {
            networkRetryAttempts_ = 0;
            networkRetryAddress_.Clear();
        }
    });
    if (!networkRetryTimer_) {
        Status("Network unavailable · retry could not be scheduled");
        HzPrint("browser: could not schedule network retry");
    }
}

void Browser::SetTabTitle(const char *Title)
{
    if (tab_)
        tab_->SetText(Title);
}

void Browser::Go(const hz::String &Raw, bool Record)
{
    if (!retryingNetwork_)
        CancelNetworkRetry();
    networkNotReady_ = false;
    hz::String Address = Normalize(Raw);
    BrowserUrl::Url Parsed;
    if (Address.Empty() || BrowserUrl::ParseUrl(Address.CStr(), &Parsed) != BrowserUrl::UrlError::None) {
        Status("Enter an HTTP(S) address, file:///C:/... URL, or an absolute local path");
        HzPrint("browser: rejected invalid address");
        return;
    }
    UsePageText();
    bool Stored = true;
    if (Record) {
        if (hasHistory_ && index_ + 1 < history_.Size())
            while (history_.Size() > index_ + 1)
                history_.Pop();
        if (history_.Empty() || history_.Back() != Address)
            history_.Push(Address);
        if (history_.Size() > MaxHistory)
            history_.Erase(0);
        index_ = history_.Size() - 1;
        hasHistory_ = !history_.Empty();
        Stored = SaveLines(HistoryFile, history_);
    }
    address_->SetText(Address.CStr(), false);
    currentAddress_ = Address;
    if (BrowserUrl::IsLocalFile(Parsed)) {
        ShowLocalFile(Address);
        UpdateButtons();
        return;
    }
    page_ = Page::Start;
    SetLinks(BrowserUrl::HtmlDocument{});
    ShowRemote(Parsed, Address, Stored);
    UpdateButtons();
}

bool Browser::FetchHttp(const BrowserUrl::Url &Url, char *Wire, size_t WireCapacity,
                        BrowserUrl::HttpResponse *Response, char *Body, size_t BodyCapacity,
                        size_t *BodyLength, char *Error, size_t ErrorCapacity)
{
    uint8_t Address[4] = {};
    bool Ipv4Literal = BrowserUrl::ParseIpv4Literal(Url.host, Address);
    if (!Ipv4Literal) {
        if (BrowserUrl::IsIpv4LiteralCandidate(Url.host)) {
            HzFormat(Error, ErrorCapacity, "The dotted-decimal IPv4 address is malformed or out of range.");
            return false;
        }
        STATUS ResolveStatus = HzResolveHostA(Url.host, Address);
        if (!HZ_SUCCESS(ResolveStatus)) {
            if (ResolveStatus == STATUS_DEVICE_NOT_READY) {
                networkNotReady_ = true;
                HzFormat(Error, ErrorCapacity,
                         "DNS lookup for %s could not run because the network interface or DNS service is not ready (0x%08X).",
                         Url.host, (uint32_t)ResolveStatus);
            } else {
                HzFormat(Error, ErrorCapacity, "DNS lookup for %s failed (0x%08X).",
                         Url.host, (uint32_t)ResolveStatus);
            }
            return false;
        }
        HzPrint("browser: DNS resolved %s to %u.%u.%u.%u", Url.host,
                (uint32_t)Address[0], (uint32_t)Address[1],
                (uint32_t)Address[2], (uint32_t)Address[3]);
    } else {
        HzPrint("browser: using IPv4 literal %s without DNS", Url.host);
    }
    if (Url.scheme == BrowserUrl::Scheme::Https && Ipv4Literal) {
        HzFormat(Error, ErrorCapacity,
                 "HTTPS to IPv4 literals is blocked: BearSSL 0.6 does not validate iPAddress subjectAltName identities.");
        return false;
    }
    uint32_t Ipv4 = ((uint32_t)Address[0] << 24) | ((uint32_t)Address[1] << 16) |
                    ((uint32_t)Address[2] << 8) | (uint32_t)Address[3];
    int32_t Socket = HzTcpConnect(Ipv4, Url.port);
    if (Socket < 0) {
        HzFormat(Error, ErrorCapacity, "TCP connection to %s:%u failed.", Url.host, (uint32_t)Url.port);
        return false;
    }

    bool Success = false;
    char Target[sizeof(Url.path)];
    char Absolute[sizeof(Url.host) + sizeof(Url.path) + 32];
    if (!BrowserUrl::FormatHttpRequestTarget(Url, Target, sizeof(Target)) ||
        !BrowserUrl::FormatHttpUrl(Url, Absolute, sizeof(Absolute))) {
        HzFormat(Error, ErrorCapacity, "The HTTP request URL could not be formatted safely.");
    } else {
        hz::String Host(Url.host);
        if (Url.explicitPort)
            Host.Append(hz::String::Format(":%u", (uint32_t)Url.port));
        hz::String Request("GET ");
        Request.Append(Target);
        Request.Append(" HTTP/1.1\r\nHost: ");
        Request.Append(Host);
        Request.Append("\r\nUser-Agent: NexoraBrowser/1.0\r\nAccept: text/html, text/plain;q=0.9, text/css;q=0.8, image/*;q=0.7, */*;q=0.5\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n");
        if (Url.scheme == BrowserUrl::Scheme::Https) {
            size_t WireLength = 0;
            bool CleanTlsClose = false;
            if (!nexora::browser::FetchTlsResponse(Socket, Url.host, Request.CStr(),
                                                   Request.Length(), Wire, WireCapacity,
                                                   &WireLength, &CleanTlsClose, Error, ErrorCapacity))
                goto close_socket;
            BrowserUrl::HttpParseError ParseError =
                BrowserUrl::ParseHttpResponse(Wire, WireLength, Body, BodyCapacity,
                                              BodyLength, Response);
            if (ParseError != BrowserUrl::HttpParseError::None) {
                HzFormat(Error, ErrorCapacity, "%s", HttpErrorText(ParseError));
                goto close_socket;
            }
            if (!CleanTlsClose && Response->Framing == BrowserUrl::HttpBodyFraming::CloseDelimited) {
                HzFormat(Error, ErrorCapacity, "The HTTPS connection closed without TLS close_notify.");
                goto close_socket;
            }
            Success = true;
        } else {
            uint32_t Sent = 0;
            while (Sent < Request.Length()) {
                uint32_t Written = 0;
                STATUS SendStatus = HzTcpSend(Socket, Request.CStr() + Sent,
                                              (uint32_t)(Request.Length() - Sent), &Written);
                if (!HZ_SUCCESS(SendStatus) || !Written || Written > Request.Length() - Sent) {
                    HzFormat(Error, ErrorCapacity, "Sending the HTTP request failed (0x%08X).",
                             (uint32_t)SendStatus);
                    goto close_socket;
                }
                Sent += Written;
            }

            size_t WireLength = 0;
            uint8_t Chunk[NetworkReadChunk];
            bool EndOfStream = false;
            bool Complete = false;
            for (uint32_t ReadCount = 0; ReadCount < 256 && !Complete && !EndOfStream; ++ReadCount) {
                uint32_t Received = 0;
                STATUS ReceiveStatus = HzTcpReceive(Socket, Chunk, sizeof(Chunk),
                                                    NetworkReadTimeoutMs, &Received);
                if (!HZ_SUCCESS(ReceiveStatus)) {
                    HzFormat(Error, ErrorCapacity, "Receiving the HTTP response failed (0x%08X).",
                             (uint32_t)ReceiveStatus);
                    goto close_socket;
                }
                if (Received > sizeof(Chunk) || Received > WireCapacity - WireLength) {
                    HzFormat(Error, ErrorCapacity, "The HTTP response exceeded the bounded response limit.");
                    goto close_socket;
                }
                for (uint32_t i = 0; i < Received; ++i)
                    Wire[WireLength + i] = (char)Chunk[i];
                WireLength += Received;
                EndOfStream = Received == 0;
                BrowserUrl::HttpParseError ParseError =
                    BrowserUrl::ParseHttpResponse(Wire, WireLength, Body, BodyCapacity,
                                                  BodyLength, Response);
                if (ParseError == BrowserUrl::HttpParseError::None) {
                    Complete = Response->Framing != BrowserUrl::HttpBodyFraming::CloseDelimited ||
                               EndOfStream;
                } else if (ParseError != BrowserUrl::HttpParseError::Incomplete) {
                    HzFormat(Error, ErrorCapacity, "%s", HttpErrorText(ParseError));
                    goto close_socket;
                } else if (EndOfStream) {
                    HzFormat(Error, ErrorCapacity, "%s", HttpErrorText(ParseError));
                    goto close_socket;
                }
            }
            if (!Complete) {
                HzFormat(Error, ErrorCapacity, "The HTTP response timed out or exceeded the bounded read limit.");
                goto close_socket;
            }
            Success = true;
        }
    }

close_socket:
    HzTcpClose(Socket);
    return Success;
}

bool Browser::LoadStylesheets(const BrowserUrl::Url &Base, char *Wire, size_t WireCapacity,
                              char *Body, size_t BodyCapacity)
{
    stylesheetCount_ = 0;
    bool complete = true;
    for (size_t index = 0; index < document_.StylesheetCount; ++index) {
        char reference[HtmlMaximumHref + 1];
        BrowserUrl::Url current{};
        char address[sizeof(Base.host) + sizeof(Base.path) + 32];
        if (!CopyHtmlString(document_, document_.Stylesheets[index].Href, reference, sizeof(reference)) ||
            !BrowserUrl::ResolveHttpReference(Base, reference, &current, address, sizeof(address)) ||
            current.scheme != Base.scheme) {
            complete = false;
            HzPrint("browser: stylesheet reference invalid or blocked");
            continue;
        }

        BrowserUrl::HttpResponse response{};
        size_t bodyLength = 0;
        char error[256] = {};
        bool loaded = false;
        for (unsigned int redirects = 0; redirects <= 5; ++redirects) {
            if (!FetchHttp(current, Wire, WireCapacity, &response, Body, BodyCapacity,
                           &bodyLength, error, sizeof(error)))
                break;
            if (!IsRedirect(response.StatusCode)) {
                loaded = response.StatusCode >= 200 && response.StatusCode < 300 &&
                         !response.UnsupportedContentEncoding &&
                         !strcmp(response.ContentType, "text/css");
                if (!loaded)
                    HzFormat(error, sizeof(error), "The stylesheet returned an unsupported status or content type.");
                break;
            }
            if (redirects == 5 || !response.Location[0]) {
                HzFormat(error, sizeof(error), "The stylesheet exceeded the redirect limit or omitted Location.");
                break;
            }
            BrowserUrl::Url next{};
            char nextAddress[sizeof(Base.host) + sizeof(Base.path) + 32];
            if (!BrowserUrl::ResolveHttpReference(current, response.Location, &next, nextAddress,
                                                   sizeof(nextAddress)) ||
                next.scheme != Base.scheme) {
                HzFormat(error, sizeof(error), "The stylesheet redirect was invalid or changed schemes.");
                break;
            }
            current = next;
        }
        if (!loaded) {
            complete = false;
            HzPrint("browser: stylesheet not loaded %s: %s", address,
                    error[0] ? error : "request failed");
            continue;
        }
        if (ParseCss(Body, bodyLength, &stylesheets_[stylesheetCount_]) != CssParseError::None) {
            complete = false;
            HzPrint("browser: stylesheet rejected by the bounded CSS parser: %s", address);
            continue;
        }
        ++stylesheetCount_;
        HzPrint("browser: loaded stylesheet %s (%u bytes)", address, (uint32_t)bodyLength);
    }
    return complete;
}

bool Browser::LoadImages(const BrowserUrl::Url &Base, char *Wire, size_t WireCapacity,
                         char *Body, size_t BodyCapacity)
{
    size_t totalBytes = 0;
    uint64_t totalPixels = 0;
    size_t imageRequests = 0;
    bool complete = true;
    for (size_t nodeIndex = 0; nodeIndex < document_.NodeCount; ++nodeIndex) {
        const HtmlNode &node = document_.Nodes[nodeIndex];
        if (node.Type != HtmlNodeType::Element || node.Tag != HtmlTag::Img ||
            !node.Source.Length)
            continue;
        if (imageRequests++ >= BrowserMaximumImages) {
            complete = false;
            HzPrint("browser: image limit reached; remaining images omitted");
            break;
        }

        char reference[HtmlMaximumHref + 1];
        BrowserUrl::Url current{};
        char address[sizeof(Base.host) + sizeof(Base.path) + 32];
        if (!CopyHtmlString(document_, node.Source, reference, sizeof(reference)) ||
            !BrowserUrl::ResolveHttpReference(Base, reference, &current, address, sizeof(address)) ||
            current.scheme != Base.scheme) {
            complete = false;
            HzPrint("browser: image reference invalid or blocked");
            continue;
        }

        BrowserUrl::HttpResponse response{};
        size_t bodyLength = 0;
        char error[256] = {};
        bool loaded = false;
        for (unsigned int redirects = 0; redirects <= 5; ++redirects) {
            if (!FetchHttp(current, Wire, WireCapacity, &response, Body, BodyCapacity,
                           &bodyLength, error, sizeof(error)))
                break;
            if (!IsRedirect(response.StatusCode)) {
                loaded = response.StatusCode >= 200 && response.StatusCode < 300 &&
                         !response.UnsupportedContentEncoding &&
                         IsSupportedImageType(response.ContentType);
                if (!loaded)
                    HzFormat(error, sizeof(error), "The image returned an unsupported status or content type.");
                break;
            }
            if (redirects == 5 || !response.Location[0]) {
                HzFormat(error, sizeof(error), "The image exceeded the redirect limit or omitted Location.");
                break;
            }
            BrowserUrl::Url next{};
            char nextAddress[sizeof(Base.host) + sizeof(Base.path) + 32];
            if (!BrowserUrl::ResolveHttpReference(current, response.Location, &next, nextAddress,
                                                   sizeof(nextAddress)) ||
                next.scheme != Base.scheme) {
                HzFormat(error, sizeof(error), "The image redirect was invalid or changed schemes.");
                break;
            }
            current = next;
        }
        if (!loaded) {
            complete = false;
            HzPrint("browser: image not loaded %s: %s", address,
                    error[0] ? error : "request failed");
            continue;
        }
        if (bodyLength > BrowserMaximumImageBytes ||
            totalBytes > BrowserMaximumTotalImageBytes - bodyLength) {
            complete = false;
            HzPrint("browser: image byte budget exceeded: %s", address);
            continue;
        }
        totalBytes += bodyLength;

        BrowserImage &image = images_[imageCount_];
        const STATUS decodeStatus = hz::image::DecodeMemoryBounded(
            (const uint8_t *)Body, bodyLength, BrowserMaximumImagePixels, &image.Bitmap);
        if (!HZ_SUCCESS(decodeStatus)) {
            complete = false;
            HzPrint("browser: image decode failed for %s (0x%08X): %s", address,
                    (uint32_t)decodeStatus, hz::image::LastError());
            continue;
        }
        const uint64_t pixels = (uint64_t)image.Bitmap.Width * (uint64_t)image.Bitmap.Height;
        if (totalPixels > BrowserMaximumTotalImagePixels - pixels) {
            image.Bitmap.Release();
            complete = false;
            HzPrint("browser: image pixel budget exceeded: %s", address);
            continue;
        }
        image.NodeIndex = (uint16_t)nodeIndex;
        totalPixels += pixels;
        ++imageCount_;
        HzPrint("browser: loaded image %s (%u bytes, %ux%u)", address, (uint32_t)bodyLength,
                (uint32_t)image.Bitmap.Width, (uint32_t)image.Bitmap.Height);
    }
    return complete;
}

void Browser::ShowRemote(const BrowserUrl::Url &Initial, const hz::String &InitialAddress, bool Stored)
{
    char *Wire = new char[BrowserUrl::HttpMaximumWire + 1];
    char *Body = new char[BrowserUrl::HtmlMaximumSource + 1];
    if (!Wire || !Body) {
        if (Wire)
            delete[] Wire;
        if (Body)
            delete[] Body;
        heading_->SetText("Not loaded");
        detail_->SetText("The browser could not allocate its bounded HTTP response buffers.");
        items_->SetText("");
        SetLinks(BrowserUrl::HtmlDocument{});
        Status("HTTP request failed · insufficient memory");
        return;
    }

    BrowserUrl::Url Current = Initial;
    hz::String Address = InitialAddress;
    char Error[384] = {};
    BrowserUrl::HttpResponse Response{};
    size_t BodyLength = 0;
    bool Loaded = false;
    for (unsigned int Redirects = 0; Redirects <= 5; ++Redirects) {
        if (!FetchHttp(Current, Wire, BrowserUrl::HttpMaximumWire, &Response, Body,
                       BrowserUrl::HtmlMaximumSource + 1, &BodyLength, Error, sizeof(Error)))
            break;
        if (!IsRedirect(Response.StatusCode)) {
            if (Response.StatusCode < 200 || Response.StatusCode >= 300) {
                HzFormat(Error, sizeof(Error), "The server returned HTTP status %u.", (uint32_t)Response.StatusCode);
                break;
            }
            if (Response.UnsupportedContentEncoding) {
                HzFormat(Error, sizeof(Error), "The server returned a content encoding the browser cannot decode.");
                break;
            }
            if (!IsHtml(Response.ContentType) && !IsPlainText(Response.ContentType)) {
                HzFormat(Error, sizeof(Error), "Unsupported page content type: %s.",
                         Response.ContentType[0] ? Response.ContentType : "(missing)");
                break;
            }
            Loaded = true;
            break;
        }
        if (Redirects == 5) {
            HzFormat(Error, sizeof(Error), "The server exceeded the five-redirect limit.");
            break;
        }
        if (!Response.Location[0]) {
            HzFormat(Error, sizeof(Error), "HTTP status %u did not include a Location header.",
                     (uint32_t)Response.StatusCode);
            break;
        }
        BrowserUrl::Url Next{};
        char NextAddress[sizeof(Current.host) + sizeof(Current.path) + 32];
        if (!BrowserUrl::ResolveHttpReference(Current, Response.Location, &Next, NextAddress,
                                              sizeof(NextAddress))) {
            HzFormat(Error, sizeof(Error), "The server sent an invalid or unsupported redirect target.");
            break;
        }
        if (Next.scheme != Current.scheme) {
            HzFormat(Error, sizeof(Error), "The redirect changed schemes and was blocked; redirects must stay on the same scheme.");
            break;
        }
        Address = NextAddress;
        Current = Next;
        currentAddress_ = Address;
        address_->SetText(Address.CStr(), false);
        if (hasHistory_ && index_ < history_.Size()) {
            history_[index_] = Address;
            Stored = SaveLines(HistoryFile, history_);
        }
    }

    if (!Loaded) {
        heading_->SetText("Page not loaded");
        SetTabTitle("Page not loaded");
        detail_->SetText(Error[0] ? Error : "The web request did not complete.");
        items_->SetText("No page content was rendered.");
        SetLinks(BrowserUrl::HtmlDocument{});
        if (networkNotReady_ && networkRetryAttempts_ < NetworkReadyRetryLimit) {
            detail_->SetText("The network is not ready yet. Nexora Browser will retry this address automatically.");
            Status("Waiting for network · retrying automatically");
            RetryWhenNetworkReady(Address);
        } else if (networkNotReady_) {
            Status("Network unavailable · automatic retries stopped");
            if (networkRetryAttempts_ == NetworkReadyRetryLimit)
                HzPrint("browser: network unavailable after %u retries", networkRetryAttempts_);
        } else {
            Status(Stored ? "Web request failed" : "Web request failed · history could not be saved");
        }
        window_->SetTitle(hz::String::Format("%s - Nexora Browser", Address.CStr()).CStr());
        if (!networkNotReady_ || networkRetryAttempts_ == 0)
            HzPrint("browser: %s", Error[0] ? Error : "Web request failed");
        delete[] Body;
        delete[] Wire;
        return;
    }

    currentAddress_ = Address;
    address_->SetText(Address.CStr(), false);
    if (hasHistory_ && index_ < history_.Size() && history_[index_] != Address) {
        history_[index_] = Address;
        Stored = SaveLines(HistoryFile, history_);
    }
    const unsigned short PageStatusCode = Response.StatusCode;
    char PageContentType[sizeof(Response.ContentType)];
    for (size_t i = 0; i < sizeof(PageContentType); ++i)
        PageContentType[i] = Response.ContentType[i];
    const char *SchemeName = Initial.scheme == BrowserUrl::Scheme::Https ? "HTTPS" : "HTTP";
    if (IsPlainText(PageContentType)) {
        heading_->SetText(Address.CStr());
        detail_->SetText(hz::String::Format("%s %u · plain text · active content disabled",
                                           SchemeName, (uint32_t)PageStatusCode).CStr());
        items_->SetText(Body);
        UsePageText();
        stylesheetCount_ = 0;
        SetLinks(BrowserUrl::HtmlDocument{});
    } else if (!BrowserUrl::ParseHtml(Body, BodyLength, &document_)) {
        UsePageText();
        stylesheetCount_ = 0;
        heading_->SetText("HTML response could not be rendered");
        detail_->SetText("The response exceeded the bounded HTML renderer or was invalid.");
        items_->SetText("The web response was fetched, but no page content was rendered.");
        SetLinks(BrowserUrl::HtmlDocument{});
        Status("Web response loaded · HTML rendering failed");
        window_->SetTitle(hz::String::Format("%s - Nexora Browser", Address.CStr()).CStr());
        delete[] Body;
        delete[] Wire;
        return;
    } else {
        const bool StylesheetsLoaded = LoadStylesheets(Current, Wire, BrowserUrl::HttpMaximumWire,
                                                       Body, CssMaximumSource + 1);
        const bool ImagesLoaded = LoadImages(Current, Wire, BrowserUrl::HttpMaximumWire,
                                             Body, BrowserMaximumImageBytes + 1);
        heading_->SetText(document_.Title[0] ? document_.Title : Address.CStr());
        detail_->SetText(hz::String::Format("%s %u · structured HTML · %s · %s · scripts disabled",
                                           SchemeName, (uint32_t)PageStatusCode,
                                           StylesheetsLoaded ? "bounded CSS subset" : "some stylesheets unavailable",
                                           ImagesLoaded ? "bounded images" : "some images unavailable").CStr());
        UsePageDocument();
        SetLinks(document_);
    }
    items_->Foreground = Theme::TextPrimary;
    items_->Reveal();
    SetTabTitle(heading_->Text().CStr());
    window_->SetTitle(hz::String::Format("%s - Nexora Browser", heading_->Text().CStr()).CStr());
    Status(Stored ? "Web page loaded · structured HTML" : "Web page loaded · history could not be saved");
    HzPrint("browser: %s %u loaded %s (%u bytes)", SchemeName, (uint32_t)PageStatusCode,
            Address.CStr(), (uint32_t)BodyLength);
    delete[] Body;
    delete[] Wire;
}

void Browser::SetLinks(const BrowserUrl::HtmlDocument &Document)
{
    linkTargets_.Clear();
    linkLabels_.Clear();
    for (size_t Index = 0; Index < Document.LinkCount; ++Index) {
        if (!Document.Links[Index].Href[0])
            continue;
        linkLabels_.Push(hz::String(Document.Links[Index].Text[0] ? Document.Links[Index].Text
                                                                  : Document.Links[Index].Href));
        linkTargets_.Push(hz::String(Document.Links[Index].Href));
    }
    links_->SetItemCount(linkTargets_.Size());
    links_->SetVisible(!linkTargets_.Empty());
}

void Browser::ShowLocalFile(const hz::String &Address)
{
    page_ = Page::Start;
    UsePageText();
    stylesheetCount_ = 0;
    SetTabTitle("Local file");
    BrowserUrl::Url Parsed;
    hz::String Path, Source;
    if (BrowserUrl::ParseUrl(Address.CStr(), &Parsed) != BrowserUrl::UrlError::None ||
        !BrowserUrl::IsLocalFile(Parsed) || !DecodeLocalPath(Parsed.path, Path)) {
        heading_->SetText("Invalid local file address");
        detail_->SetText("Use an absolute local path such as file:///C:/Users/Name/Documents/page.html.");
        items_->SetText("");
        SetLinks(BrowserUrl::HtmlDocument{});
        Status("Invalid local file URL");
        return;
    }
    STATUS ReadStatus = hz::fs::ReadAll(Path, Source, BrowserUrl::HtmlMaximumSource);
    if (!HZ_SUCCESS(ReadStatus)) {
        heading_->SetText("Local file could not be opened");
        detail_->SetText(Path.CStr());
        items_->SetText(hz::String::Format("%s (0x%08X).", hz::fs::StatusText(ReadStatus),
                                           (uint32_t)ReadStatus).CStr());
        SetLinks(BrowserUrl::HtmlDocument{});
        Status("Local file open failed");
        HzPrint("browser: local read failed for %s (0x%08X)", Path.CStr(), (uint32_t)ReadStatus);
        return;
    }
    currentAddress_ = Address;
    address_->SetText(Address.CStr(), false);
    bool Html = hz::fs::Extension(Path) == "html" || hz::fs::Extension(Path) == "htm";
    if (!Html) {
        UsePageText();
        items_->SetText(Source.CStr());
        heading_->SetText(hz::fs::FileName(Path).CStr());
        detail_->SetText("Local text file · no active content executed.");
        SetLinks(BrowserUrl::HtmlDocument{});
    } else {
        if (!BrowserUrl::ParseHtml(Source.CStr(), Source.Length(), &document_)) {
            heading_->SetText("HTML document is too large or invalid");
            detail_->SetText(Path.CStr());
            items_->SetText("Local HTML source is limited to 512 KiB.");
            SetLinks(BrowserUrl::HtmlDocument{});
            Status("HTML source exceeds the renderer limit");
            return;
        }
        heading_->SetText(document_.Title[0] ? document_.Title : hz::fs::FileName(Path).CStr());
        detail_->SetText("Local HTML · structured layout · external resources and scripts are disabled.");
        UsePageDocument();
        SetLinks(document_);
    }
    SetTabTitle(heading_->Text().CStr());
    items_->Foreground = Theme::TextPrimary;
    if (items_->IsVisible())
        items_->Reveal();
    window_->SetTitle(hz::String::Format("%s - Nexora Browser", heading_->Text().CStr()).CStr());
    Status("Local file loaded · active content disabled");
    HzPrint("browser: local file loaded %s (%u bytes)", Path.CStr(), (uint32_t)Source.Length());
    if (Html)
        HzPrint("browser: local HTML parsed title=%s links=%u", heading_->Text().CStr(),
                (uint32_t)linkTargets_.Size());
}

void Browser::OpenLink(size_t Index)
{
    if (Index >= linkTargets_.Size())
        return;
    const hz::String &Href = linkTargets_[Index];
    if (StartsWith(Href.CStr(), "http://") || StartsWith(Href.CStr(), "https://")) {
        Go(Href);
        return;
    }
    BrowserUrl::Url Current;
    if (BrowserUrl::ParseUrl(currentAddress_.CStr(), &Current) != BrowserUrl::UrlError::None) {
        Status("This link type is not supported");
        return;
    }
    if (StartsWith(Href.CStr(), "file:///")) {
        if (!BrowserUrl::IsLocalFile(Current)) {
            Status("Remote pages cannot navigate directly to local file URLs");
            return;
        }
        Go(Href);
        return;
    }
    if (Current.scheme == BrowserUrl::Scheme::Http || Current.scheme == BrowserUrl::Scheme::Https) {
        BrowserUrl::Url Resolved{};
        char Absolute[sizeof(Current.host) + sizeof(Current.path) + 32];
        if (!BrowserUrl::ResolveHttpReference(Current, Href.CStr(), &Resolved, Absolute, sizeof(Absolute))) {
            Status("This relative link is invalid or uses an unsupported scheme");
            return;
        }
        Go(Absolute);
        return;
    }
    if (!BrowserUrl::IsLocalFile(Current)) {
        Status("This link type is not supported");
        return;
    }
    hz::String Base;
    if (Href[0] == '/') {
        Base = hz::String::Format("%c:\\", Current.path[0]);
        Base.Append(Href.CStr() + 1);
    } else {
        Base = hz::fs::Parent(hz::String(Current.path));
        Base = hz::fs::Resolve(Base, Href.CStr());
    }
    Go(FileUrl(Base));
}

void Browser::StartPage()
{
    page_ = Page::Start;
    UsePageText();
    stylesheetCount_ = 0;
    currentAddress_.Clear();
    SetTabTitle("Start page");
    address_->SetText("", false);
    nexora::browser::HtmlDocument Document;
    if (!nexora::browser::ParseHtml(HomePage, sizeof(HomePage) - 1, &Document)) {
        heading_->SetText("HTML rendering failed");
        detail_->SetText("The built-in page exceeded the native renderer's input limit.");
        items_->SetText("");
        Status("HTML renderer error");
        return;
    }
    document_ = Document;
    heading_->SetText(document_.Title);
    detail_->SetText("HTTP and verified TLS 1.2 HTTPS use structured HTML and a bounded CSS subset.");
    UsePageDocument();
    SetLinks(document_);
    window_->SetTitle("Nexora Browser");
    Status("Ready · local files, HTTP, and verified TLS 1.2 HTTPS available");
    UpdateButtons();
}

void Browser::ShowList(Page View)
{
    page_ = View;
    UsePageText();
    SetLinks(BrowserUrl::HtmlDocument{});
    hz::String Lines;
    if (View == Page::History) {
        LoadLines(HistoryFile, history_, MaxHistory);
        heading_->SetText("History");
        SetTabTitle("History");
        detail_->SetText("Recent addresses in this local profile.");
        for (const auto &Item : history_) {
            Lines.Append(Item);
            Lines.Append("\n");
        }
        Status("History · Ctrl+H");
    } else if (View == Page::Bookmarks) {
        LoadLines(BookmarksFile, bookmarks_, MaxBookmarks);
        heading_->SetText("Bookmarks");
        SetTabTitle("Bookmarks");
        detail_->SetText("Saved locally in the default profile. Use Ctrl+D to add or remove the current address.");
        for (const auto &Item : bookmarks_) {
            Lines.Append(Item);
            Lines.Append("\n");
        }
        Status("Bookmarks · Ctrl+B");
    } else {
        heading_->SetText("Downloads");
        SetTabTitle("Downloads");
        detail_->SetText("Files in the profile Downloads folder. Internet downloads are unavailable until the network and engine layers exist.");
        hz::Vector<hz::fs::Entry> Files;
        if (HZ_SUCCESS(hz::fs::List(hz::String(Downloads), Files)))
            for (const auto &File : Files) {
                Lines.Append(File.Name);
                Lines.Append("\n");
            }
        Status("Downloads · network download service not connected");
    }
    if (Lines.Empty())
        Lines = View == Page::History ? "No history yet." : View == Page::Bookmarks ? "No bookmarks yet." : "No files in Downloads.";
    items_->SetText(Lines.CStr());
    items_->Reveal();
}

void Browser::Back()
{
    if (!hasHistory_ || index_ == 0)
        return;
    --index_;
    Go(history_[index_], false);
    UpdateButtons();
}

void Browser::Forward()
{
    if (!hasHistory_ || index_ + 1 >= history_.Size())
        return;
    ++index_;
    Go(history_[index_], false);
    UpdateButtons();
}

void Browser::Bookmark()
{
    if (page_ != Page::Start || currentAddress_.Empty() || !hasHistory_ || history_.Empty()) {
        Status("Open an address before bookmarking it");
        return;
    }
    const hz::String Current = history_[index_];
    for (size_t i = 0; i < bookmarks_.Size(); ++i) {
        if (bookmarks_[i] == Current) {
            bookmarks_.Erase(i);
            SaveLines(BookmarksFile, bookmarks_);
            Status("Bookmark removed");
            if (page_ == Page::Bookmarks)
                ShowList(page_);
            return;
        }
    }
    if (bookmarks_.Size() >= MaxBookmarks) {
        Status("Bookmark limit reached");
        return;
    }
    bookmarks_.Push(Current);
    SaveLines(BookmarksFile, bookmarks_);
    Status("Bookmark saved to the default local profile");
    if (page_ == Page::Bookmarks)
        ShowList(page_);
}

void Browser::Refresh()
{
    if (page_ == Page::Start) {
        if (!currentAddress_.Empty())
            Go(currentAddress_, false);
        else
            StartPage();
    } else {
        ShowList(page_);
    }
}

void Browser::UpdateButtons()
{
    back_->SetEnabled(hasHistory_ && index_ > 0);
    forward_->SetEnabled(hasHistory_ && index_ + 1 < history_.Size());
    reload_->SetEnabled(page_ != Page::Start || !currentAddress_.Empty());
    bookmark_->SetEnabled(page_ == Page::Start && !currentAddress_.Empty());
}
} // namespace

extern "C" int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    Browser *App = new Browser();
    if (!App->Create()) {
        HzPrint("browser: failed to create window");
        return 1;
    }
    HzPrint("browser: started; local files, bounded HTTP, and verified TLS 1.2 HTTPS ready");
    return RunMessageLoop();
}
