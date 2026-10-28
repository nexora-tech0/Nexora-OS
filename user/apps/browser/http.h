#pragma once

#include "html.h"
#include "url.h"

namespace nexora::browser {

constexpr size_t HttpMaximumHeader = 8192;
constexpr size_t HttpMaximumWire = HttpMaximumHeader + HtmlMaximumSource + HttpMaximumHeader;

enum class HttpParseError {
    None,
    Incomplete,
    InvalidStatus,
    InvalidHeader,
    InvalidFraming,
    UnsupportedTransferEncoding,
    TooLarge
};

enum class HttpBodyFraming { CloseDelimited, ContentLength, Chunked };

struct HttpResponse {
    unsigned short StatusCode = 0;
    char Location[HtmlMaximumHref] = {};
    char ContentType[128] = {};
    HttpBodyFraming Framing = HttpBodyFraming::CloseDelimited;
    bool UnsupportedContentEncoding = false;
};

HttpParseError ParseHttpResponse(const char *wire, size_t wireLength, char *body, size_t bodyCapacity,
                                 size_t *bodyLength, HttpResponse *response);
bool ResolveHttpReference(const Url &base, const char *reference, Url *resolved, char *absolute,
                          size_t absoluteCapacity);
bool FormatHttpUrl(const Url &url, char *output, size_t capacity);
bool FormatHttpRequestTarget(const Url &url, char *output, size_t capacity);
bool ParseIpv4Literal(const char *host, unsigned char octets[4]);
bool IsIpv4LiteralCandidate(const char *host);

}
