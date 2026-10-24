#pragma once
#include <stddef.h>

namespace nexora::browser {

constexpr size_t HtmlMaximumSource = 65536;
constexpr size_t HtmlMaximumText = 8192;
constexpr size_t HtmlMaximumTitle = 128;
constexpr size_t HtmlMaximumLinks = 32;
constexpr size_t HtmlMaximumLinkText = 128;
constexpr size_t HtmlMaximumHref = 512;

struct HtmlLink {
    char Text[HtmlMaximumLinkText] = {};
    char Href[HtmlMaximumHref] = {};
    size_t TextLength = 0;
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
};

bool ParseHtml(const char *source, size_t length, HtmlDocument *document);

}
