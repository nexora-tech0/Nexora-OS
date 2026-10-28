#pragma once
#include <stddef.h>
#include <stdint.h>

namespace nexora::browser {

// This is an intentionally small safe CSS subset, not a full CSS implementation.
constexpr size_t CssMaximumSource = 32768;
constexpr size_t CssMaximumRules = 128;
constexpr uint16_t CssMaximumFontSizePx = 256;
constexpr uint16_t CssMaximumMarginPx = 4096;

enum class CssTag : uint8_t {
    Body,
    P,
    Div,
    Span,
    H1,
    H2,
    H3,
    H4,
    H5,
    H6,
    A,
    Img
};

enum class CssFontWeight : uint8_t {
    Normal,
    Bold
};

enum class CssTextAlign : uint8_t {
    Left,
    Center,
    Right
};

enum CssDeclaration : uint8_t {
    CssHasColor = 1u << 0,
    CssHasBackgroundColor = 1u << 1,
    CssHasFontSize = 1u << 2,
    CssHasFontWeight = 1u << 3,
    CssHasMarginTop = 1u << 4,
    CssHasMarginBottom = 1u << 5,
    CssHasTextAlign = 1u << 6
};

struct CssRule {
    CssTag Selector = CssTag::Body;
    uint8_t Declarations = 0;
    uint32_t Color = 0;
    uint32_t BackgroundColor = 0;
    uint16_t FontSizePx = 0;
    uint16_t MarginTopPx = 0;
    uint16_t MarginBottomPx = 0;
    CssFontWeight FontWeight = CssFontWeight::Normal;
    CssTextAlign TextAlign = CssTextAlign::Left;
};

struct CssStylesheet {
    CssRule Rules[CssMaximumRules] = {};
    uint16_t RuleCount = 0;
};

enum class CssParseError : uint8_t {
    None,
    InvalidArgument,
    TooLarge,
    Malformed,
    UnsupportedSelector,
    UnsupportedProperty,
    InvalidValue,
    TooManyRules
};

CssParseError ParseCss(const char *source, size_t length, CssStylesheet *stylesheet);

}
