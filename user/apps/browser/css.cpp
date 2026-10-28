#include "css.h"

namespace nexora::browser {
namespace {

bool IsWhitespace(char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f';
}

char LowerAscii(char value)
{
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

bool EqualsIgnoreCase(const char *left, size_t leftLength, const char *right)
{
    size_t rightLength = 0;
    while (right[rightLength] != '\0') {
        ++rightLength;
    }
    if (leftLength != rightLength) {
        return false;
    }
    for (size_t index = 0; index < leftLength; ++index) {
        if (LowerAscii(left[index]) != right[index]) {
            return false;
        }
    }
    return true;
}

class CssParser {
public:
    CssParser(const char *source, size_t length, CssStylesheet *stylesheet)
        : source_(source), length_(length), stylesheet_(stylesheet)
    {
    }

    CssParseError Parse()
    {
        while (true) {
            if (!SkipIgnored()) {
                return CssParseError::Malformed;
            }
            if (position_ == length_) {
                return CssParseError::None;
            }
            CssRule rule = {};
            CssParseError error = ParseRule(&rule);
            if (error != CssParseError::None) {
                return error;
            }
            if (stylesheet_->RuleCount == CssMaximumRules) {
                return CssParseError::TooManyRules;
            }
            stylesheet_->Rules[stylesheet_->RuleCount++] = rule;
        }
    }

private:
    bool SkipIgnored()
    {
        while (position_ < length_) {
            if (IsWhitespace(source_[position_])) {
                ++position_;
                continue;
            }
            if (source_[position_] == '/' && position_ + 1 < length_ &&
                source_[position_ + 1] == '*') {
                position_ += 2;
                bool closed = false;
                while (position_ + 1 < length_) {
                    if (source_[position_] == '*' && source_[position_ + 1] == '/') {
                        position_ += 2;
                        closed = true;
                        break;
                    }
                    ++position_;
                }
                if (!closed) {
                    return false;
                }
                continue;
            }
            break;
        }
        return true;
    }

    bool ReadToken(char *token, size_t capacity, size_t *tokenLength, bool stopAtColon)
    {
        *tokenLength = 0;
        while (position_ < length_) {
            const char value = source_[position_];
            if (IsWhitespace(value) || value == ';' || value == '}' || value == '{' ||
                (stopAtColon && value == ':') ||
                (value == '/' && position_ + 1 < length_ && source_[position_ + 1] == '*')) {
                break;
            }
            if (*tokenLength + 1 >= capacity) {
                return false;
            }
            token[(*tokenLength)++] = value;
            ++position_;
        }
        token[*tokenLength] = '\0';
        return *tokenLength != 0;
    }

    CssParseError ParseRule(CssRule *rule)
    {
        char token[32];
        size_t tokenLength = 0;
        if (!ReadToken(token, sizeof(token), &tokenLength, false)) {
            return CssParseError::Malformed;
        }
        if (!ParseTag(token, tokenLength, &rule->Selector)) {
            return CssParseError::UnsupportedSelector;
        }
        if (!SkipIgnored() || position_ == length_ || source_[position_] != '{') {
            if (position_ < length_) {
                const char next = source_[position_];
                if ((next >= 'A' && next <= 'Z') || (next >= 'a' && next <= 'z') ||
                    next == '#' || next == '.' || next == '[' || next == ':' ||
                    next == '>' || next == '+' || next == '~' || next == ',') {
                    return CssParseError::UnsupportedSelector;
                }
            }
            return CssParseError::Malformed;
        }
        ++position_;

        while (true) {
            if (!SkipIgnored() || position_ == length_) {
                return CssParseError::Malformed;
            }
            if (source_[position_] == '}') {
                ++position_;
                return CssParseError::None;
            }

            char property[32];
            if (!ReadToken(property, sizeof(property), &tokenLength, true)) {
                return CssParseError::Malformed;
            }
            CssParseError error = ParseDeclaration(property, tokenLength, rule);
            if (error != CssParseError::None) {
                return error;
            }
        }
    }

    CssParseError ParseDeclaration(const char *property, size_t propertyLength, CssRule *rule)
    {
        if (!SkipIgnored() || position_ == length_ || source_[position_] != ':') {
            return CssParseError::Malformed;
        }
        ++position_;
        if (!SkipIgnored()) {
            return CssParseError::Malformed;
        }

        char value[64];
        size_t valueLength = 0;
        if (!ReadToken(value, sizeof(value), &valueLength, false)) {
            return CssParseError::Malformed;
        }
        if (!SkipIgnored() || position_ == length_ ||
            (source_[position_] != ';' && source_[position_] != '}')) {
            return CssParseError::Malformed;
        }
        const char terminator = source_[position_++];

        CssParseError error = ApplyDeclaration(property, propertyLength, value, valueLength, rule);
        if (error != CssParseError::None) {
            return error;
        }
        if (terminator == '}') {
            // Rewind so the declaration loop handles the closing brace uniformly.
            --position_;
        }
        return CssParseError::None;
    }

    bool ParseTag(const char *token, size_t length, CssTag *tag) const
    {
        static const struct {
            const char *Name;
            CssTag Tag;
        } tags[] = {
            {"body", CssTag::Body}, {"p", CssTag::P},   {"div", CssTag::Div},
            {"span", CssTag::Span}, {"h1", CssTag::H1}, {"h2", CssTag::H2},
            {"h3", CssTag::H3},     {"h4", CssTag::H4}, {"h5", CssTag::H5},
            {"h6", CssTag::H6},     {"a", CssTag::A},   {"img", CssTag::Img},
        };
        for (const auto &entry : tags) {
            if (EqualsIgnoreCase(token, length, entry.Name)) {
                *tag = entry.Tag;
                return true;
            }
        }
        return false;
    }

    static int HexDigit(char value)
    {
        if (value >= '0' && value <= '9') {
            return value - '0';
        }
        value = LowerAscii(value);
        if (value >= 'a' && value <= 'f') {
            return value - 'a' + 10;
        }
        return -1;
    }

    static bool ParseColor(const char *value, size_t length, uint32_t *color)
    {
        if (length != 0 && value[0] == '#') {
            if (length != 4 && length != 7) {
                return false;
            }
            if (length == 4) {
                const int red = HexDigit(value[1]);
                const int green = HexDigit(value[2]);
                const int blue = HexDigit(value[3]);
                if (red < 0 || green < 0 || blue < 0) {
                    return false;
                }
                *color = ((static_cast<uint32_t>(red) * 17u) << 16) |
                         ((static_cast<uint32_t>(green) * 17u) << 8) |
                         (static_cast<uint32_t>(blue) * 17u);
                return true;
            }
            uint32_t parsed = 0;
            for (size_t index = 1; index < length; ++index) {
                const int digit = HexDigit(value[index]);
                if (digit < 0) {
                    return false;
                }
                parsed = (parsed << 4) | static_cast<uint32_t>(digit);
            }
            *color = parsed;
            return true;
        }

        static const struct {
            const char *Name;
            uint32_t Value;
        } colors[] = {
            {"black", 0x000000}, {"white", 0xFFFFFF}, {"red", 0xFF0000},
            {"green", 0x008000}, {"blue", 0x0000FF}, {"yellow", 0xFFFF00},
            {"gray", 0x808080}, {"silver", 0xC0C0C0}, {"maroon", 0x800000},
            {"navy", 0x000080}, {"teal", 0x008080}, {"purple", 0x800080},
            {"orange", 0xFFA500},
        };
        for (const auto &entry : colors) {
            if (EqualsIgnoreCase(value, length, entry.Name)) {
                *color = entry.Value;
                return true;
            }
        }
        return false;
    }

    static bool ParsePixels(const char *value, size_t length, uint16_t maximum, uint16_t *pixels)
    {
        if (length < 3 || LowerAscii(value[length - 2]) != 'p' ||
            LowerAscii(value[length - 1]) != 'x') {
            return false;
        }
        uint32_t result = 0;
        for (size_t index = 0; index < length - 2; ++index) {
            if (value[index] < '0' || value[index] > '9') {
                return false;
            }
            result = result * 10u + static_cast<uint32_t>(value[index] - '0');
            if (result > maximum) {
                return false;
            }
        }
        *pixels = static_cast<uint16_t>(result);
        return true;
    }

    static CssParseError ApplyDeclaration(const char *property, size_t propertyLength,
                                          const char *value, size_t valueLength, CssRule *rule)
    {
        uint32_t color = 0;
        uint16_t pixels = 0;
        if (EqualsIgnoreCase(property, propertyLength, "color")) {
            if (!ParseColor(value, valueLength, &color)) {
                return CssParseError::InvalidValue;
            }
            rule->Color = color;
            rule->Declarations |= CssHasColor;
        } else if (EqualsIgnoreCase(property, propertyLength, "background-color")) {
            if (!ParseColor(value, valueLength, &color)) {
                return CssParseError::InvalidValue;
            }
            rule->BackgroundColor = color;
            rule->Declarations |= CssHasBackgroundColor;
        } else if (EqualsIgnoreCase(property, propertyLength, "font-size")) {
            if (!ParsePixels(value, valueLength, CssMaximumFontSizePx, &pixels)) {
                return CssParseError::InvalidValue;
            }
            rule->FontSizePx = pixels;
            rule->Declarations |= CssHasFontSize;
        } else if (EqualsIgnoreCase(property, propertyLength, "font-weight")) {
            if (EqualsIgnoreCase(value, valueLength, "normal")) {
                rule->FontWeight = CssFontWeight::Normal;
            } else if (EqualsIgnoreCase(value, valueLength, "bold")) {
                rule->FontWeight = CssFontWeight::Bold;
            } else {
                return CssParseError::InvalidValue;
            }
            rule->Declarations |= CssHasFontWeight;
        } else if (EqualsIgnoreCase(property, propertyLength, "margin-top") ||
                   EqualsIgnoreCase(property, propertyLength, "margin-bottom")) {
            if (!ParsePixels(value, valueLength, CssMaximumMarginPx, &pixels)) {
                return CssParseError::InvalidValue;
            }
            if (EqualsIgnoreCase(property, propertyLength, "margin-top")) {
                rule->MarginTopPx = pixels;
                rule->Declarations |= CssHasMarginTop;
            } else {
                rule->MarginBottomPx = pixels;
                rule->Declarations |= CssHasMarginBottom;
            }
        } else if (EqualsIgnoreCase(property, propertyLength, "text-align")) {
            if (EqualsIgnoreCase(value, valueLength, "left")) {
                rule->TextAlign = CssTextAlign::Left;
            } else if (EqualsIgnoreCase(value, valueLength, "center")) {
                rule->TextAlign = CssTextAlign::Center;
            } else if (EqualsIgnoreCase(value, valueLength, "right")) {
                rule->TextAlign = CssTextAlign::Right;
            } else {
                return CssParseError::InvalidValue;
            }
            rule->Declarations |= CssHasTextAlign;
        } else {
            return CssParseError::UnsupportedProperty;
        }
        return CssParseError::None;
    }

    const char *source_;
    size_t length_;
    size_t position_ = 0;
    CssStylesheet *stylesheet_;
};

}

CssParseError ParseCss(const char *source, size_t length, CssStylesheet *stylesheet)
{
    if (stylesheet == nullptr) {
        return CssParseError::InvalidArgument;
    }
    *stylesheet = CssStylesheet{};
    if (source == nullptr) {
        return CssParseError::InvalidArgument;
    }
    if (length > CssMaximumSource) {
        return CssParseError::TooLarge;
    }
    CssParser parser(source, length, stylesheet);
    const CssParseError error = parser.Parse();
    if (error != CssParseError::None) {
        *stylesheet = CssStylesheet{};
    }
    return error;
}

}
