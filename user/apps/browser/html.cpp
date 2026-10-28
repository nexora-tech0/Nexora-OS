#include "html.h"

namespace nexora::browser {
namespace {

constexpr uint16_t NoNode = UINT16_MAX;

bool IsSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

char Lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
}

bool EqualsIgnoreCase(const char *left, size_t leftLength, const char *right)
{
    size_t rightLength = 0;
    while (right[rightLength])
        ++rightLength;
    if (leftLength != rightLength)
        return false;
    for (size_t i = 0; i < leftLength; ++i)
        if (Lower(left[i]) != right[i])
            return false;
    return true;
}

bool StartsWithIgnoreCase(const char *value, const char *prefix)
{
    size_t length = 0;
    while (prefix[length]) {
        if (!value[length] || Lower(value[length]) != prefix[length])
            return false;
        ++length;
    }
    return true;
}

void Append(char *output, size_t capacity, size_t *length, char value, bool *truncated)
{
    if (*length + 1 >= capacity) {
        *truncated = true;
        return;
    }
    output[(*length)++] = value;
    output[*length] = 0;
}

bool StoreString(HtmlDocument *document, const char *value, size_t length, HtmlStringRef *reference)
{
    if (length > UINT16_MAX || document->StringLength + length > sizeof(document->Strings)) {
        document->Truncated = true;
        return false;
    }
    reference->Offset = (uint16_t)document->StringLength;
    reference->Length = (uint16_t)length;
    for (size_t i = 0; i < length; ++i)
        document->Strings[document->StringLength++] = value[i];
    return true;
}

uint16_t AddNode(HtmlDocument *document, HtmlNodeType type, HtmlTag tag, uint16_t parent)
{
    if (document->NodeCount >= HtmlMaximumNodes) {
        document->Truncated = true;
        return NoNode;
    }
    const uint16_t index = (uint16_t)document->NodeCount++;
    HtmlNode &node = document->Nodes[index];
    node = HtmlNode{};
    node.Type = type;
    node.Tag = tag;
    node.Parent = parent;
    if (parent != NoNode) {
        HtmlNode &parentNode = document->Nodes[parent];
        if (parentNode.FirstChild == NoNode) {
            parentNode.FirstChild = index;
        } else {
            uint16_t sibling = parentNode.FirstChild;
            while (document->Nodes[sibling].NextSibling != NoNode)
                sibling = document->Nodes[sibling].NextSibling;
            document->Nodes[sibling].NextSibling = index;
        }
    }
    return index;
}

bool IsBlockTag(HtmlTag tag)
{
    return tag == HtmlTag::P || tag == HtmlTag::Div || tag == HtmlTag::Section || tag == HtmlTag::Article ||
           tag == HtmlTag::Header || tag == HtmlTag::Footer || tag == HtmlTag::Li ||
           (tag >= HtmlTag::H1 && tag <= HtmlTag::H6) || tag == HtmlTag::Table || tag == HtmlTag::Thead ||
           tag == HtmlTag::Tbody || tag == HtmlTag::Tfoot || tag == HtmlTag::Caption || tag == HtmlTag::Tr ||
           tag == HtmlTag::Dl || tag == HtmlTag::Dt || tag == HtmlTag::Dd || tag == HtmlTag::Aside ||
           tag == HtmlTag::Figure || tag == HtmlTag::Figcaption || tag == HtmlTag::Address;
}

HtmlTag ParseTag(const char *name, size_t length)
{
#define TAG(literal, value) if (EqualsIgnoreCase(name, length, literal)) return HtmlTag::value
    TAG("html", Html);
    TAG("head", Head);
    TAG("body", Body);
    TAG("title", Title);
    TAG("a", A);
    TAG("img", Img);
    TAG("br", Br);
    TAG("hr", Hr);
    TAG("p", P);
    TAG("div", Div);
    TAG("section", Section);
    TAG("article", Article);
    TAG("header", Header);
    TAG("footer", Footer);
    TAG("li", Li);
    TAG("h1", H1);
    TAG("h2", H2);
    TAG("h3", H3);
    TAG("h4", H4);
    TAG("h5", H5);
    TAG("h6", H6);
    TAG("table", Table);
    TAG("thead", Thead);
    TAG("tbody", Tbody);
    TAG("tfoot", Tfoot);
    TAG("caption", Caption);
    TAG("tr", Tr);
    TAG("td", Td);
    TAG("th", Th);
    TAG("ul", Ul);
    TAG("ol", Ol);
    TAG("dl", Dl);
    TAG("dt", Dt);
    TAG("dd", Dd);
    TAG("main", Main);
    TAG("nav", Nav);
    TAG("aside", Aside);
    TAG("figure", Figure);
    TAG("figcaption", Figcaption);
    TAG("address", Address);
    TAG("span", Span);
    TAG("strong", Strong);
    TAG("b", B);
    TAG("em", Em);
    TAG("i", I);
    TAG("small", Small);
    TAG("pre", Pre);
    TAG("blockquote", Blockquote);
    TAG("code", Code);
    TAG("sub", Sub);
    TAG("sup", Sup);
    TAG("s", S);
    TAG("del", Del);
    TAG("ins", Ins);
    TAG("mark", Mark);
    TAG("time", Time);
    TAG("q", Q);
    TAG("wbr", Wbr);
    TAG("meta", Meta);
    TAG("link", Link);
#undef TAG
    return HtmlTag::Unknown;
}

bool IsNameCharacter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '-' || c == ':' || c == '_';
}

bool IsVoidTag(HtmlTag tag)
{
    return tag == HtmlTag::Img || tag == HtmlTag::Br || tag == HtmlTag::Hr || tag == HtmlTag::Meta ||
           tag == HtmlTag::Link || tag == HtmlTag::Wbr;
}

bool IsActiveTag(const char *name, size_t length)
{
    return EqualsIgnoreCase(name, length, "script") || EqualsIgnoreCase(name, length, "style") ||
           EqualsIgnoreCase(name, length, "iframe") || EqualsIgnoreCase(name, length, "object") ||
           EqualsIgnoreCase(name, length, "embed") || EqualsIgnoreCase(name, length, "applet") ||
           EqualsIgnoreCase(name, length, "svg") || EqualsIgnoreCase(name, length, "math");
}

bool FindClosingTag(const char *source, size_t length, size_t start, const char *name, size_t nameLength,
                    size_t *afterClosing)
{
    for (size_t i = start; i + nameLength + 3 <= length; ++i) {
        if (source[i] != '<' || source[i + 1] != '/')
            continue;
        size_t matched = 0;
        while (matched < nameLength && Lower(source[i + 2 + matched]) == Lower(name[matched]))
            ++matched;
        if (matched != nameLength)
            continue;
        size_t end = i + 2 + nameLength;
        if (end < length && (IsSpace(source[end]) || source[end] == '>')) {
            char quote = 0;
            while (end < length) {
                if (quote) {
                    if (source[end] == quote)
                        quote = 0;
                } else if (source[end] == '\'' || source[end] == '"') {
                    quote = source[end];
                } else if (source[end] == '>') {
                    break;
                }
                ++end;
            }
            if (end < length) {
                *afterClosing = end + 1;
                return true;
            }
        }
    }
    return false;
}

bool FindRawScriptEnd(const char *source, size_t length, size_t start, size_t *closingStart, size_t *afterClosing)
{
    for (size_t i = start; i + 8 <= length; ++i) {
        if (source[i] != '<' || source[i + 1] != '/')
            continue;
        size_t matched = 0;
        const char *name = "script";
        while (matched < 6 && Lower(source[i + 2 + matched]) == name[matched])
            ++matched;
        if (matched != 6)
            continue;
        size_t end = i + 8;
        if (end >= length)
            continue;
        if (source[end] == '>') {
            *closingStart = i;
            *afterClosing = end + 1;
            return true;
        }
        if (!IsSpace(source[end]))
            continue;
        while (end < length && IsSpace(source[end]))
            ++end;
        if (end < length && source[end] == '>') {
            *closingStart = i;
            *afterClosing = end + 1;
            return true;
        }
    }
    return false;
}

bool DecodeAttributeValue(const char *source, size_t length, char *output, size_t capacity, size_t *outputLength);

bool IsSupportedScriptType(const char *type, size_t length)
{
    size_t start = 0;
    size_t end = length;
    while (start < end && IsSpace(type[start]))
        ++start;
    while (end > start && IsSpace(type[end - 1]))
        --end;
    if (start == end)
        return true;
    return EqualsIgnoreCase(type + start, end - start, "text/javascript") ||
           EqualsIgnoreCase(type + start, end - start, "application/javascript") ||
           EqualsIgnoreCase(type + start, end - start, "text/ecmascript") ||
           EqualsIgnoreCase(type + start, end - start, "application/ecmascript");
}

bool IsConservativeInlineScript(const char *source, size_t start, size_t end)
{
    size_t cursor = start;
    size_t count = 0;
    bool seenType = false;
    char type[HtmlMaximumAttributeValue + 1] = {};
    size_t typeLength = 0;
    while (cursor < end) {
        while (cursor < end && (IsSpace(source[cursor]) || source[cursor] == '/'))
            ++cursor;
        if (cursor >= end)
            break;
        const size_t nameStart = cursor;
        while (cursor < end && IsNameCharacter(source[cursor]))
            ++cursor;
        const size_t nameLength = cursor - nameStart;
        if (!nameLength || nameLength > HtmlMaximumAttributeName || ++count > HtmlMaximumAttributesPerTag)
            return false;
        while (cursor < end && IsSpace(source[cursor]))
            ++cursor;
        if (cursor >= end || source[cursor] != '=')
            return false;
        ++cursor;
        while (cursor < end && IsSpace(source[cursor]))
            ++cursor;
        if (cursor >= end || (source[cursor] != '\'' && source[cursor] != '"'))
            return false;
        const char delimiter = source[cursor++];
        const size_t valueStart = cursor;
        while (cursor < end && source[cursor] != delimiter)
            ++cursor;
        if (cursor >= end)
            return false;
        const size_t rawLength = cursor - valueStart;
        ++cursor;
        if (!EqualsIgnoreCase(source + nameStart, nameLength, "type") || seenType ||
            rawLength > HtmlMaximumAttributeValue)
            return false;
        seenType = true;
        if (!DecodeAttributeValue(source + valueStart, rawLength, type, sizeof(type), &typeLength))
            return false;
    }
    return IsSupportedScriptType(type, typeLength);
}

int HexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = Lower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

bool DecodeAttributeValue(const char *source, size_t length, char *output, size_t capacity, size_t *outputLength)
{
    *outputLength = 0;
    for (size_t i = 0; i < length;) {
        char decoded = 0;
        size_t consumed = 1;
        if (source[i] == '&') {
            size_t end = i + 1;
            while (end < length && end - i <= 12 && source[end] != ';' && source[end] != '&' &&
                   !IsSpace(source[end]))
                ++end;
            if (end < length && source[end] == ';' && end - i <= 12) {
                const char *entity = source + i + 1;
                const size_t entityLength = end - i - 1;
                if (EqualsIgnoreCase(entity, entityLength, "amp"))
                    decoded = '&';
                else if (EqualsIgnoreCase(entity, entityLength, "lt"))
                    decoded = '<';
                else if (EqualsIgnoreCase(entity, entityLength, "gt"))
                    decoded = '>';
                else if (EqualsIgnoreCase(entity, entityLength, "quot"))
                    decoded = '"';
                else if (EqualsIgnoreCase(entity, entityLength, "apos") ||
                         EqualsIgnoreCase(entity, entityLength, "#39"))
                    decoded = '\'';
                else if (EqualsIgnoreCase(entity, entityLength, "nbsp"))
                    decoded = ' ';
                else if (entityLength > 1 && entity[0] == '#') {
                    size_t digit = 1;
                    int base = 10;
                    if (digit < entityLength && (entity[digit] == 'x' || entity[digit] == 'X')) {
                        base = 16;
                        ++digit;
                    }
                    unsigned value = 0;
                    bool valid = digit < entityLength;
                    for (; digit < entityLength && valid; ++digit) {
                        const int n = base == 16 ? HexValue(entity[digit])
                                                 : (entity[digit] >= '0' && entity[digit] <= '9'
                                                        ? entity[digit] - '0'
                                                        : -1);
                        if (n < 0 || n >= base || value > 127u / (unsigned)base)
                            valid = false;
                        else
                            value = value * (unsigned)base + (unsigned)n;
                    }
                    if (valid && value > 0 && value <= 127)
                        decoded = (char)value;
                }
                if (decoded)
                    consumed = end + 1 - i;
            }
        }
        if (!decoded) {
            decoded = source[i];
            consumed = 1;
        }
        if (*outputLength + 1 >= capacity)
            return false;
        output[(*outputLength)++] = decoded;
        i += consumed;
    }
    output[*outputLength] = 0;
    return true;
}

bool IsSafeUrl(const char *value, size_t valueLength)
{
    size_t i = 0;
    while (i < valueLength && IsSpace(value[i]))
        ++i;
    for (size_t j = i; j < valueLength; ++j)
        if ((unsigned char)value[j] <= 0x20 || (unsigned char)value[j] == 0x7f)
            return false;
    char scheme[16] = {};
    size_t length = 0;
    bool hasColon = false;
    bool invalidScheme = false;
    for (; i < valueLength && value[i] != '/' && value[i] != '?' && value[i] != '#'; ++i) {
        if (value[i] == ':') {
            hasColon = true;
            break;
        }
        const unsigned char c = (unsigned char)value[i];
        if (c <= 0x20 || c == 0x7f) {
            invalidScheme = true;
            continue;
        }
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (length && c >= '0' && c <= '9') ||
            (length && (c == '+' || c == '.' || c == '-'))) {
            if (length + 1 >= sizeof(scheme))
                invalidScheme = true;
            else
                scheme[length++] = Lower((char)c);
            continue;
        }
        invalidScheme = true;
    }
    if (!hasColon)
        return true;
    if (invalidScheme || !length)
        return false;
    if (EqualsIgnoreCase(scheme, length, "http") || EqualsIgnoreCase(scheme, length, "https"))
        return true;
    return false;
}

struct Attributes {
    char Href[HtmlMaximumAttributeValue + 1] = {};
    size_t HrefLength = 0;
    char Source[HtmlMaximumAttributeValue + 1] = {};
    size_t SourceLength = 0;
    char Alt[HtmlMaximumAttributeValue + 1] = {};
    size_t AltLength = 0;
    char Rel[HtmlMaximumAttributeValue + 1] = {};
    size_t RelLength = 0;
    char Style[HtmlMaximumAttributeValue + 1] = {};
    size_t StyleLength = 0;
    uint16_t Width = 0;
    uint16_t Height = 0;
    bool HasHref = false;
    bool HasSource = false;
    bool HasAlt = false;
    bool HasRel = false;
    bool HasStyle = false;
    bool HasWidth = false;
    bool HasHeight = false;
};

bool ParseDimension(const char *value, size_t length, uint16_t *dimension)
{
    if (!length)
        return false;
    unsigned number = 0;
    for (size_t i = 0; i < length; ++i) {
        if (value[i] < '0' || value[i] > '9')
            return false;
        number = number * 10 + (unsigned)(value[i] - '0');
        if (number > 4096)
            return false;
    }
    if (!number)
        return false;
    *dimension = (uint16_t)number;
    return true;
}

void ReadAttributes(const char *source, size_t start, size_t end, HtmlDocument *document, Attributes *attributes)
{
    size_t cursor = start;
    size_t count = 0;
    while (cursor < end) {
        while (cursor < end && (IsSpace(source[cursor]) || source[cursor] == '/'))
            ++cursor;
        if (cursor >= end)
            break;
        const size_t nameStart = cursor;
        while (cursor < end && IsNameCharacter(source[cursor]))
            ++cursor;
        const size_t nameLength = cursor - nameStart;
        if (!nameLength) {
            ++cursor;
            continue;
        }
        if (++count > HtmlMaximumAttributesPerTag) {
            document->UnsupportedContent = true;
            break;
        }
        if (nameLength > HtmlMaximumAttributeName) {
            document->UnsupportedContent = true;
        }
        while (cursor < end && IsSpace(source[cursor]))
            ++cursor;
        if (cursor >= end || source[cursor] != '=') {
            if (nameLength > 1 && Lower(source[nameStart]) == 'o' && Lower(source[nameStart + 1]) == 'n')
                document->UnsupportedContent = true;
            continue;
        }
        ++cursor;
        while (cursor < end && IsSpace(source[cursor]))
            ++cursor;
        char delimiter = 0;
        if (cursor < end && (source[cursor] == '\'' || source[cursor] == '"'))
            delimiter = source[cursor++];
        const size_t valueStart = cursor;
        while (cursor < end && (delimiter ? source[cursor] != delimiter : !IsSpace(source[cursor])))
            ++cursor;
        const size_t rawLength = cursor - valueStart;
        if (delimiter && cursor < end)
            ++cursor;
        if (nameLength > 1 && Lower(source[nameStart]) == 'o' && Lower(source[nameStart + 1]) == 'n') {
            document->UnsupportedContent = true;
            continue;
        }
        if (rawLength > HtmlMaximumAttributeValue) {
            if (EqualsIgnoreCase(source + nameStart, nameLength, "href"))
                document->Truncated = true;
            else
                document->UnsupportedContent = true;
            continue;
        }

        char value[HtmlMaximumAttributeValue + 1] = {};
        size_t valueLength = 0;
        if (!DecodeAttributeValue(source + valueStart, rawLength, value, sizeof(value), &valueLength)) {
            document->UnsupportedContent = true;
            continue;
        }
        if (EqualsIgnoreCase(source + nameStart, nameLength, "href")) {
            attributes->HasHref = true;
            attributes->HrefLength = valueLength;
            for (size_t i = 0; i <= valueLength; ++i)
                attributes->Href[i] = value[i];
            if (!IsSafeUrl(attributes->Href, valueLength)) {
                attributes->HasHref = false;
                attributes->HrefLength = 0;
                document->UnsupportedContent = true;
            }
        } else if (EqualsIgnoreCase(source + nameStart, nameLength, "src")) {
            attributes->HasSource = true;
            attributes->SourceLength = valueLength;
            for (size_t i = 0; i <= valueLength; ++i)
                attributes->Source[i] = value[i];
            if (!IsSafeUrl(attributes->Source, valueLength)) {
                attributes->HasSource = false;
                attributes->SourceLength = 0;
                document->UnsupportedContent = true;
            }
        } else if (EqualsIgnoreCase(source + nameStart, nameLength, "alt")) {
            attributes->HasAlt = true;
            attributes->AltLength = valueLength;
            for (size_t i = 0; i <= valueLength; ++i)
                attributes->Alt[i] = value[i];
        } else if (EqualsIgnoreCase(source + nameStart, nameLength, "rel")) {
            attributes->HasRel = true;
            attributes->RelLength = valueLength;
            for (size_t i = 0; i <= valueLength; ++i)
                attributes->Rel[i] = value[i];
        } else if (EqualsIgnoreCase(source + nameStart, nameLength, "style")) {
            attributes->HasStyle = true;
            attributes->StyleLength = valueLength;
            for (size_t i = 0; i <= valueLength; ++i)
                attributes->Style[i] = value[i];
        } else if (EqualsIgnoreCase(source + nameStart, nameLength, "width")) {
            attributes->HasWidth = ParseDimension(value, valueLength, &attributes->Width);
        } else if (EqualsIgnoreCase(source + nameStart, nameLength, "height")) {
            attributes->HasHeight = ParseDimension(value, valueLength, &attributes->Height);
        }
    }
}

bool IsSafeColor(const char *value, size_t length)
{
    if (length == 4 || length == 7) {
        if (value[0] != '#')
            return false;
        for (size_t i = 1; i < length; ++i)
            if (HexValue(value[i]) < 0)
                return false;
        return true;
    }
    static const char *const colors[] = {
        "black", "white", "red", "green", "blue", "gray", "grey", "yellow", "navy", "purple", "teal",
        "maroon", "lime", "aqua", "silver", "fuchsia", "olive", "transparent"};
    for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); ++i)
        if (EqualsIgnoreCase(value, length, colors[i]))
            return true;
    return false;
}

void ParseStyle(HtmlDocument *document, const char *source, size_t length, HtmlSafeStyle *style)
{
    size_t cursor = 0;
    while (cursor < length) {
        while (cursor < length && (IsSpace(source[cursor]) || source[cursor] == ';'))
            ++cursor;
        const size_t declarationStart = cursor;
        while (cursor < length && source[cursor] != ';')
            ++cursor;
        const size_t declarationEnd = cursor;
        size_t colon = declarationStart;
        while (colon < declarationEnd && source[colon] != ':')
            ++colon;
        if (colon == declarationEnd) {
            if (declarationEnd > declarationStart)
                document->UnsupportedContent = true;
            continue;
        }
        size_t propertyStart = declarationStart;
        size_t propertyEnd = colon;
        size_t valueStart = colon + 1;
        size_t valueEnd = declarationEnd;
        while (propertyStart < propertyEnd && IsSpace(source[propertyStart]))
            ++propertyStart;
        while (propertyEnd > propertyStart && IsSpace(source[propertyEnd - 1]))
            --propertyEnd;
        while (valueStart < valueEnd && IsSpace(source[valueStart]))
            ++valueStart;
        while (valueEnd > valueStart && IsSpace(source[valueEnd - 1]))
            --valueEnd;
        const size_t propertyLength = propertyEnd - propertyStart;
        const size_t valueLength = valueEnd - valueStart;
        bool accepted = false;
        if (EqualsIgnoreCase(source + propertyStart, propertyLength, "color") ||
            EqualsIgnoreCase(source + propertyStart, propertyLength, "background-color")) {
            if (IsSafeColor(source + valueStart, valueLength)) {
                HtmlStringRef value;
                if (StoreString(document, source + valueStart, valueLength, &value)) {
                    if (EqualsIgnoreCase(source + propertyStart, propertyLength, "color"))
                        style->Color = value;
                    else
                        style->BackgroundColor = value;
                    accepted = true;
                }
            }
        } else if (EqualsIgnoreCase(source + propertyStart, propertyLength, "font-weight")) {
            if (EqualsIgnoreCase(source + valueStart, valueLength, "bold") ||
                EqualsIgnoreCase(source + valueStart, valueLength, "600") ||
                EqualsIgnoreCase(source + valueStart, valueLength, "700") ||
                EqualsIgnoreCase(source + valueStart, valueLength, "800") ||
                EqualsIgnoreCase(source + valueStart, valueLength, "900")) {
                style->Bold = true;
                accepted = true;
            } else if (EqualsIgnoreCase(source + valueStart, valueLength, "normal")) {
                style->Bold = false;
                accepted = true;
            }
        } else if (EqualsIgnoreCase(source + propertyStart, propertyLength, "font-style")) {
            if (EqualsIgnoreCase(source + valueStart, valueLength, "italic") ||
                EqualsIgnoreCase(source + valueStart, valueLength, "oblique")) {
                style->Italic = true;
                accepted = true;
            } else if (EqualsIgnoreCase(source + valueStart, valueLength, "normal")) {
                style->Italic = false;
                accepted = true;
            }
        } else if (EqualsIgnoreCase(source + propertyStart, propertyLength, "text-align")) {
            if (EqualsIgnoreCase(source + valueStart, valueLength, "left"))
                style->TextAlign = HtmlTextAlign::Left;
            else if (EqualsIgnoreCase(source + valueStart, valueLength, "center"))
                style->TextAlign = HtmlTextAlign::Center;
            else if (EqualsIgnoreCase(source + valueStart, valueLength, "right"))
                style->TextAlign = HtmlTextAlign::Right;
            else if (EqualsIgnoreCase(source + valueStart, valueLength, "justify"))
                style->TextAlign = HtmlTextAlign::Justify;
            else
                document->UnsupportedContent = true;
            accepted = style->TextAlign != HtmlTextAlign::Default;
        } else if (EqualsIgnoreCase(source + propertyStart, propertyLength, "text-decoration")) {
            if (EqualsIgnoreCase(source + valueStart, valueLength, "underline")) {
                style->Underline = true;
                style->LineThrough = false;
                accepted = true;
            } else if (EqualsIgnoreCase(source + valueStart, valueLength, "line-through")) {
                style->LineThrough = true;
                style->Underline = false;
                accepted = true;
            } else if (EqualsIgnoreCase(source + valueStart, valueLength, "none")) {
                style->Underline = false;
                style->LineThrough = false;
                accepted = true;
            }
        }
        if (!accepted)
            document->UnsupportedContent = true;
        if (cursor < length)
            ++cursor;
    }
}

void AppendBreak(HtmlDocument *document)
{
    if (!document->TextLength || document->Text[document->TextLength - 1] == '\n')
        return;
    Append(document->Text, sizeof(document->Text), &document->TextLength, '\n', &document->Truncated);
}

bool AppendNodeText(HtmlDocument *document, uint16_t parent, uint16_t *activeTextNode, char value)
{
    if (*activeTextNode == NoNode || document->Nodes[*activeTextNode].Parent != parent ||
        (size_t)document->Nodes[*activeTextNode].Text.Offset + document->Nodes[*activeTextNode].Text.Length !=
            document->StringLength) {
        *activeTextNode = AddNode(document, HtmlNodeType::Text, HtmlTag::Unknown, parent);
        if (*activeTextNode == NoNode)
            return false;
        document->Nodes[*activeTextNode].Text.Offset = (uint16_t)document->StringLength;
    }
    if (document->StringLength >= sizeof(document->Strings) ||
        document->Nodes[*activeTextNode].Text.Length == UINT16_MAX) {
        document->Truncated = true;
        return false;
    }
    document->Strings[document->StringLength++] = value;
    ++document->Nodes[*activeTextNode].Text.Length;
    return true;
}

void AppendVisibleText(HtmlDocument *document, char value, bool *pendingSpace, bool inTitle, HtmlLink *link,
                       uint16_t parent, uint16_t *activeTextNode)
{
    if (IsSpace(value)) {
        *pendingSpace = true;
        return;
    }
    if ((unsigned char)value < 0x20 || (unsigned char)value == 0x7f)
        return;
    if (*pendingSpace) {
        if (inTitle) {
            if (document->Title[0])
                Append(document->Title, sizeof(document->Title), &document->TitleLength, ' ', &document->Truncated);
        } else if (document->TextLength && document->Text[document->TextLength - 1] != '\n') {
            Append(document->Text, sizeof(document->Text), &document->TextLength, ' ', &document->Truncated);
            AppendNodeText(document, parent, activeTextNode, ' ');
        }
        if (link && link->Text[0])
            Append(link->Text, sizeof(link->Text), &link->TextLength, ' ', &document->Truncated);
        *pendingSpace = false;
    }
    if (inTitle) {
        Append(document->Title, sizeof(document->Title), &document->TitleLength, value, &document->Truncated);
    } else {
        Append(document->Text, sizeof(document->Text), &document->TextLength, value, &document->Truncated);
        AppendNodeText(document, parent, activeTextNode, value);
    }
    if (link)
        Append(link->Text, sizeof(link->Text), &link->TextLength, value, &document->Truncated);
}

void AppendEntity(const char *source, size_t length, size_t *position, HtmlDocument *document, bool *pendingSpace,
                  bool inTitle, HtmlLink *link, uint16_t parent, uint16_t *activeTextNode)
{
    size_t end = *position + 1;
    while (end < length && end - *position <= 12 && source[end] != ';' && source[end] != '<' &&
           !IsSpace(source[end]))
        ++end;
    if (end >= length || source[end] != ';' || end - *position > 12) {
        AppendVisibleText(document, source[(*position)++], pendingSpace, inTitle, link, parent, activeTextNode);
        return;
    }
    const char *entity = source + *position + 1;
    const size_t entityLength = end - *position - 1;
    char decoded = 0;
    if (EqualsIgnoreCase(entity, entityLength, "amp"))
        decoded = '&';
    else if (EqualsIgnoreCase(entity, entityLength, "lt"))
        decoded = '<';
    else if (EqualsIgnoreCase(entity, entityLength, "gt"))
        decoded = '>';
    else if (EqualsIgnoreCase(entity, entityLength, "quot"))
        decoded = '"';
    else if (EqualsIgnoreCase(entity, entityLength, "apos") || EqualsIgnoreCase(entity, entityLength, "#39"))
        decoded = '\'';
    else if (EqualsIgnoreCase(entity, entityLength, "nbsp"))
        decoded = ' ';
    if (decoded) {
        AppendVisibleText(document, decoded, pendingSpace, inTitle, link, parent, activeTextNode);
    } else {
        document->UnsupportedContent = true;
        for (size_t k = *position; k <= end; ++k)
            AppendVisibleText(document, source[k], pendingSpace, inTitle, link, parent, activeTextNode);
    }
    *position = end + 1;
}

void EndElement(HtmlDocument *document, HtmlTag tag, uint16_t *stack, size_t *depth)
{
    size_t match = *depth;
    while (match > 1) {
        --match;
        if (document->Nodes[stack[match]].Tag == tag) {
            *depth = match;
            return;
        }
    }
}

}

const char *HtmlString(const HtmlDocument &document, HtmlStringRef value)
{
    if ((size_t)value.Offset + value.Length > document.StringLength)
        return "";
    return document.Strings + value.Offset;
}

const char *HtmlScriptData(const HtmlDocument &document, const HtmlScript &script)
{
    if ((size_t)script.Offset + script.Length > document.ScriptByteLength)
        return "";
    return document.ScriptBytes + script.Offset;
}

bool ParseHtml(const char *source, size_t length, HtmlDocument *document)
{
    if (!source || !document || length > HtmlMaximumSource)
        return false;
    *document = HtmlDocument{};
    document->NodeCount = 1;
    document->Nodes[0].Type = HtmlNodeType::Document;
    document->Nodes[0].Tag = HtmlTag::Unknown;
    document->Nodes[0].Parent = NoNode;

    uint16_t stack[HtmlMaximumDepth + 1] = {};
    stack[0] = 0;
    size_t depth = 1;
    bool pendingSpace = false;
    bool inTitle = false;
    HtmlLink *activeLink = nullptr;
    uint16_t activeTextNode = NoNode;
    size_t i = 0;

    while (i < length && !document->Truncated) {
        if (source[i] == '&') {
            AppendEntity(source, length, &i, document, &pendingSpace, inTitle, activeLink,
                         stack[depth - 1], &activeTextNode);
            continue;
        }
        if (source[i] != '<') {
            AppendVisibleText(document, source[i++], &pendingSpace, inTitle, activeLink, stack[depth - 1],
                              &activeTextNode);
            continue;
        }
        if (i + 3 < length && source[i + 1] == '!' && source[i + 2] == '-' && source[i + 3] == '-') {
            i += 4;
            while (i + 2 < length && !(source[i] == '-' && source[i + 1] == '-' && source[i + 2] == '>'))
                ++i;
            i = i + 2 < length ? i + 3 : length;
            activeTextNode = NoNode;
            continue;
        }

        size_t tagEnd = i + 1;
        char quote = 0;
        while (tagEnd < length) {
            if (tagEnd - i > HtmlMaximumTagSource) {
                document->Truncated = true;
                break;
            }
            const char c = source[tagEnd];
            if (quote) {
                if (c == quote)
                    quote = 0;
            } else if (c == '\'' || c == '"') {
                quote = c;
            } else if (c == '>') {
                break;
            }
            ++tagEnd;
        }
        if (document->Truncated)
            break;
        if (tagEnd >= length) {
            document->UnsupportedContent = true;
            break;
        }

        size_t cursor = i + 1;
        while (cursor < tagEnd && IsSpace(source[cursor]))
            ++cursor;
        if (cursor < tagEnd && (source[cursor] == '!' || source[cursor] == '?')) {
            i = tagEnd + 1;
            activeTextNode = NoNode;
            continue;
        }
        bool closing = cursor < tagEnd && source[cursor] == '/';
        if (closing)
            ++cursor;
        while (cursor < tagEnd && IsSpace(source[cursor]))
            ++cursor;
        const size_t nameStart = cursor;
        while (cursor < tagEnd && IsNameCharacter(source[cursor]))
            ++cursor;
        const size_t nameLength = cursor - nameStart;
        if (!nameLength) {
            document->UnsupportedContent = true;
            i = tagEnd + 1;
            activeTextNode = NoNode;
            continue;
        }
        const char *name = source + nameStart;
        const HtmlTag tag = ParseTag(name, nameLength);
        if (EqualsIgnoreCase(name, nameLength, "script")) {
            if (closing) {
                document->UnsupportedContent = true;
                i = tagEnd + 1;
                activeTextNode = NoNode;
                continue;
            }
            size_t closingStart = 0;
            size_t afterClosing = 0;
            if (!FindRawScriptEnd(source, length, tagEnd + 1, &closingStart, &afterClosing)) {
                document->UnsupportedContent = true;
                i = length;
                activeTextNode = NoNode;
                continue;
            }

            const size_t scriptLength = closingStart - (tagEnd + 1);
            const bool safeAttributes = IsConservativeInlineScript(source, cursor, tagEnd);
            if (safeAttributes && scriptLength <= HtmlMaximumScriptLength &&
                document->ScriptCount < HtmlMaximumScripts &&
                document->ScriptByteLength + scriptLength <= HtmlMaximumScriptBytes) {
                HtmlScript &script = document->Scripts[document->ScriptCount++];
                script.Offset = (uint16_t)document->ScriptByteLength;
                script.Length = (uint16_t)scriptLength;
                for (size_t byte = 0; byte < scriptLength; ++byte)
                    document->ScriptBytes[document->ScriptByteLength++] = source[tagEnd + 1 + byte];
            } else {
                document->UnsupportedContent = true;
            }
            i = afterClosing;
            activeTextNode = NoNode;
            continue;
        }
        if (IsActiveTag(name, nameLength)) {
            document->UnsupportedContent = true;
            if (!closing) {
                size_t afterClosing = 0;
                if (FindClosingTag(source, length, tagEnd + 1, name, nameLength, &afterClosing))
                    i = afterClosing;
                else
                    i = length;
                activeTextNode = NoNode;
                continue;
            }
            i = tagEnd + 1;
            activeTextNode = NoNode;
            continue;
        }

        if (closing) {
            if (tag == HtmlTag::Title) {
                inTitle = false;
                pendingSpace = false;
            }
            if (tag == HtmlTag::A)
                activeLink = nullptr;
            if (IsBlockTag(tag) && !inTitle)
                AppendBreak(document);
            EndElement(document, tag, stack, &depth);
            i = tagEnd + 1;
            activeTextNode = NoNode;
            continue;
        }

        Attributes attributes;
        ReadAttributes(source, cursor, tagEnd, document, &attributes);
        HtmlNode *node = nullptr;
        uint16_t nodeIndex = NoNode;
        if (tag != HtmlTag::Title) {
            if (depth > HtmlMaximumDepth) {
                document->Truncated = true;
                break;
            }
            nodeIndex = AddNode(document, HtmlNodeType::Element, tag, stack[depth - 1]);
            if (nodeIndex == NoNode)
                break;
            node = &document->Nodes[nodeIndex];
            if (attributes.HasStyle) {
                if (attributes.StyleLength > 256) {
                    document->UnsupportedContent = true;
                } else {
                    ParseStyle(document, attributes.Style, attributes.StyleLength, &node->Style);
                }
            }
            if (tag == HtmlTag::A) {
                if (document->LinkCount < HtmlMaximumLinks) {
                    HtmlLink *link = &document->Links[document->LinkCount++];
                    activeLink = link;
                    if (attributes.HasHref) {
                        if (attributes.HrefLength >= sizeof(link->Href)) {
                            document->Truncated = true;
                            activeLink = nullptr;
                        } else {
                            for (size_t k = 0; k <= attributes.HrefLength; ++k)
                                link->Href[k] = attributes.Href[k];
                            if (!StoreString(document, attributes.Href, attributes.HrefLength, &node->Href))
                                activeLink = nullptr;
                        }
                    }
                } else {
                    activeLink = nullptr;
                    document->UnsupportedContent = true;
                }
            }
            if (tag == HtmlTag::Img) {
                if (attributes.HasSource && attributes.SourceLength <= HtmlMaximumHref)
                    StoreString(document, attributes.Source, attributes.SourceLength, &node->Source);
                else if (attributes.HasSource)
                    document->UnsupportedContent = true;
                if (attributes.HasAlt)
                    StoreString(document, attributes.Alt, attributes.AltLength, &node->Alt);
                node->Width = attributes.Width;
                node->Height = attributes.Height;
                node->HasWidth = attributes.HasWidth;
                node->HasHeight = attributes.HasHeight;
            }
            if (tag == HtmlTag::Link && attributes.HasRel && attributes.HasHref &&
                StartsWithIgnoreCase(attributes.Rel, "stylesheet") &&
                (attributes.Rel[10] == 0 || IsSpace(attributes.Rel[10])) &&
                document->StylesheetCount < HtmlMaximumStylesheets &&
                attributes.HrefLength <= HtmlMaximumHref) {
                HtmlStylesheet &stylesheet = document->Stylesheets[document->StylesheetCount++];
                if (StoreString(document, attributes.Href, attributes.HrefLength, &stylesheet.Href))
                    node->Href = stylesheet.Href;
            }
        }

        if (tag == HtmlTag::Title) {
            inTitle = true;
            pendingSpace = false;
        } else if (tag == HtmlTag::Br || tag == HtmlTag::Hr) {
            AppendBreak(document);
        } else if (IsBlockTag(tag)) {
            AppendBreak(document);
        }

        if (tag != HtmlTag::Title && !IsVoidTag(tag) && tag != HtmlTag::Unknown) {
            if (depth > HtmlMaximumDepth) {
                document->Truncated = true;
                break;
            }
            stack[depth++] = nodeIndex;
        } else if (tag == HtmlTag::Unknown && !IsVoidTag(tag)) {
            if (depth > HtmlMaximumDepth) {
                document->Truncated = true;
                break;
            }
            stack[depth++] = nodeIndex;
            document->UnsupportedContent = true;
        }
        i = tagEnd + 1;
        activeTextNode = NoNode;
    }

    while (document->TextLength && (document->Text[document->TextLength - 1] == ' ' ||
                                    document->Text[document->TextLength - 1] == '\n'))
        document->Text[--document->TextLength] = 0;
    return true;
}

}
