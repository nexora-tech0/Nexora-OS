#include "url.h"

namespace nexora::browser {
namespace {
size_t Length(const char *text)
{
    size_t length = 0;
    while (text[length])
        ++length;
    return length;
}

bool EqualPrefix(const char *text, const char *prefix, size_t length)
{
    for (size_t i = 0; i < length; ++i)
        if (text[i] != prefix[i])
            return false;
    return true;
}

void CopyBytes(char *destination, const char *source, size_t length)
{
    for (size_t i = 0; i < length; ++i)
        destination[i] = source[i];
}

bool ContainsPair(const char *text, char first, char second)
{
    for (; text[0] && text[1]; ++text)
        if (text[0] == first && text[1] == second)
            return true;
    return false;
}

bool IsControlOrSpace(unsigned char c)
{
    return c <= 0x20 || c == 0x7f;
}

bool IsHostChar(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '-';
}

bool CopyPart(char *dest, size_t capacity, const char *start, size_t length)
{
    if (length >= capacity)
        return false;
    CopyBytes(dest, start, length);
    dest[length] = 0;
    return true;
}
}

UrlError ParseUrl(const char *input, Url *out)
{
    if (!out)
        return UrlError::Empty;
    *out = Url{};
    if (!input || !*input)
        return UrlError::Empty;
    const size_t length = Length(input);
    if (length >= sizeof(out->path) + sizeof(out->host))
        return UrlError::TooLong;
    for (size_t i = 0; i < length; ++i)
        if (IsControlOrSpace((unsigned char)input[i]))
            return UrlError::InvalidCharacter;

    const char *authority = nullptr;
    if (EqualPrefix(input, "https://", 8)) {
        out->scheme = Scheme::Https;
        out->port = 443;
        authority = input + 8;
    } else if (EqualPrefix(input, "http://", 7)) {
        out->scheme = Scheme::Http;
        out->port = 80;
        authority = input + 7;
    } else {
        return UrlError::UnsupportedScheme;
    }
    const char *end = input + length;
    const char *authorityEnd = authority;
    while (authorityEnd < end && *authorityEnd != '/' && *authorityEnd != '?' && *authorityEnd != '#')
        ++authorityEnd;
    if (authority == authorityEnd)
        return UrlError::MissingHost;
    for (const char *p = authority; p < authorityEnd; ++p)
        if (*p == '@')
            return UrlError::InvalidAuthority;

    const char *hostEnd = authorityEnd;
    const char *portStart = nullptr;
    for (const char *p = authority; p < authorityEnd; ++p) {
        if (*p == ':') {
            if (portStart)
                return UrlError::InvalidAuthority;
            hostEnd = p;
            portStart = p + 1;
        }
    }
    if (hostEnd == authority)
        return UrlError::MissingHost;
    if (!CopyPart(out->host, sizeof(out->host), authority, (size_t)(hostEnd - authority)))
        return UrlError::TooLong;
    size_t hostLength = Length(out->host);
    if (out->host[0] == '.' || out->host[hostLength - 1] == '.' || ContainsPair(out->host, '.', '.'))
        return UrlError::InvalidHost;
    for (const unsigned char *p = (const unsigned char *)out->host; *p; ++p)
        if (!IsHostChar(*p))
            return UrlError::InvalidHost;

    if (portStart) {
        if (portStart == authorityEnd)
            return UrlError::InvalidPort;
        unsigned long port = 0;
        for (const char *p = portStart; p < authorityEnd; ++p) {
            if (*p < '0' || *p > '9')
                return UrlError::InvalidPort;
            unsigned long digit = (unsigned long)(*p - '0');
            if (port > (65535ul - digit) / 10ul)
                return UrlError::InvalidPort;
            port = port * 10 + digit;
        }
        if (port == 0)
            return UrlError::InvalidPort;
        out->port = (unsigned short)port;
        out->explicitPort = true;
    }

    const char *pathStart = authorityEnd;
    if (pathStart == end || *pathStart == '?' || *pathStart == '#') {
        if (*pathStart == '?' || *pathStart == '#') {
            size_t remain = (size_t)(end - pathStart);
            if (remain >= sizeof(out->path))
                return UrlError::TooLong;
            out->path[0] = '/';
            CopyBytes(out->path + 1, pathStart, remain);
            out->path[remain + 1] = 0;
        } else {
            out->path[0] = '/';
            out->path[1] = 0;
        }
    } else if (!CopyPart(out->path, sizeof(out->path), pathStart, (size_t)(end - pathStart))) {
        return UrlError::TooLong;
    }
    out->valid = true;
    return UrlError::None;
}

bool IsSecure(const Url &url)
{
    return url.valid && url.scheme == Scheme::Https;
}

const char *SchemeName(Scheme scheme)
{
    switch (scheme) {
    case Scheme::Http: return "http";
    case Scheme::Https: return "https";
    default: return "";
    }
}
}
