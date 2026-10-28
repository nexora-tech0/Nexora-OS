#include "../http.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

using namespace nexora::browser;

static void TestContentLength()
{
    const char response[] =
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Content-Length: 15\r\n\r\n<html>ok</html>";
    char body[HtmlMaximumSource + 1];
    size_t bodyLength = 0;
    HttpResponse parsed;
    assert(ParseHttpResponse(response, sizeof(response) - 1, body, sizeof(body), &bodyLength, &parsed) ==
           HttpParseError::None);
    assert(parsed.StatusCode == 200);
    assert(parsed.Framing == HttpBodyFraming::ContentLength);
    assert(strcmp(parsed.ContentType, "text/html") == 0);
    assert(bodyLength == 15);
    assert(memcmp(body, "<html>ok</html>", 15) == 0);

    assert(ParseHttpResponse(response, sizeof(response) - 2, body, sizeof(body), &bodyLength, &parsed) ==
           HttpParseError::Incomplete);
}

static void TestCloseDelimitedAndRedirect()
{
    const char response[] = "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nplain";
    char body[64];
    size_t bodyLength = 0;
    HttpResponse parsed;
    assert(ParseHttpResponse(response, sizeof(response) - 1, body, sizeof(body), &bodyLength, &parsed) ==
           HttpParseError::None);
    assert(parsed.Framing == HttpBodyFraming::CloseDelimited);
    assert(bodyLength == 5);
    assert(strcmp(body, "plain") == 0);

    const char redirect[] =
        "HTTP/1.1 302 Found\r\nLocation: ../next\r\nContent-Length: 0\r\n\r\n";
    assert(ParseHttpResponse(redirect, sizeof(redirect) - 1, body, sizeof(body), &bodyLength, &parsed) ==
           HttpParseError::None);
    assert(parsed.StatusCode == 302);
    assert(strcmp(parsed.Location, "../next") == 0);
}

static void TestChunked()
{
    const char response[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "4\r\nWiki\r\n5\r\npedia\r\n0\r\nX-Test: yes\r\n\r\n";
    char body[64];
    size_t bodyLength = 0;
    HttpResponse parsed;
    assert(ParseHttpResponse(response, sizeof(response) - 1, body, sizeof(body), &bodyLength, &parsed) ==
           HttpParseError::None);
    assert(parsed.Framing == HttpBodyFraming::Chunked);
    assert(bodyLength == 9);
    assert(strcmp(body, "Wikipedia") == 0);

    const char malformed[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\naX\n0\r\n\r\n";
    assert(ParseHttpResponse(malformed, sizeof(malformed) - 1, body, sizeof(body), &bodyLength, &parsed) ==
           HttpParseError::InvalidFraming);
}

static void TestChunkExtensions()
{
    const char response[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "4;name=value\r\nWiki\r\n"
        "5; quoted=\"text;with;semicolons\"\r\npedia\r\n"
        "0;done\r\n\r\n";
    char body[64];
    size_t bodyLength = 0;
    HttpResponse parsed;
    assert(ParseHttpResponse(response, sizeof(response) - 1, body, sizeof(body), &bodyLength, &parsed) ==
           HttpParseError::None);
    assert(parsed.Framing == HttpBodyFraming::Chunked);
    assert(bodyLength == 9);
    assert(strcmp(body, "Wikipedia") == 0);

    const char invalid[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "1;\x01\r\na\r\n0\r\n\r\n";
    assert(ParseHttpResponse(invalid, sizeof(invalid) - 1, body, sizeof(body), &bodyLength, &parsed) ==
           HttpParseError::InvalidFraming);
}

static void TestRejectedFraming()
{
    char body[64];
    size_t bodyLength = 0;
    HttpResponse parsed;
    const char ambiguous[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n";
    assert(ParseHttpResponse(ambiguous, sizeof(ambiguous) - 1, body, sizeof(body), &bodyLength, &parsed) ==
           HttpParseError::InvalidFraming);

    char oversized[96];
    int oversizedLength = snprintf(oversized, sizeof(oversized),
                                   "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n",
                                   HtmlMaximumSource + 1);
    assert(oversizedLength > 0 && (size_t)oversizedLength < sizeof(oversized));
    assert(ParseHttpResponse(oversized, (size_t)oversizedLength, body, sizeof(body), &bodyLength, &parsed) ==
           HttpParseError::TooLarge);

    const char compressed[] =
        "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 1\r\n\r\nx";
    assert(ParseHttpResponse(compressed, sizeof(compressed) - 1, body, sizeof(body), &bodyLength, &parsed) ==
           HttpParseError::None);
    assert(parsed.UnsupportedContentEncoding);
}

static void TestRelativeUrls()
{
    Url base;
    assert(ParseUrl("http://example.com/a/b/index.html?old=1", &base) == UrlError::None);
    Url result;
    char absolute[sizeof(base.host) + sizeof(base.path) + 32];

    assert(ResolveHttpReference(base, "../next?q=1", &result, absolute, sizeof(absolute)));
    assert(strcmp(absolute, "http://example.com/a/next?q=1") == 0);

    assert(ResolveHttpReference(base, "/root/path", &result, absolute, sizeof(absolute)));
    assert(strcmp(absolute, "http://example.com/root/path") == 0);

    assert(ResolveHttpReference(base, "//other.example/page", &result, absolute, sizeof(absolute)));
    assert(strcmp(absolute, "http://other.example/page") == 0);

    assert(ResolveHttpReference(base, "?new=2", &result, absolute, sizeof(absolute)));
    assert(strcmp(absolute, "http://example.com/a/b/index.html?new=2") == 0);

    assert(ResolveHttpReference(base, "#top", &result, absolute, sizeof(absolute)));
    assert(strcmp(absolute, "http://example.com/a/b/index.html?old=1#top") == 0);

    assert(ResolveHttpReference(base, "", &result, absolute, sizeof(absolute)));
    assert(strcmp(absolute, "http://example.com/a/b/index.html?old=1") == 0);

    assert(ResolveHttpReference(base, "../../../root", &result, absolute, sizeof(absolute)));
    assert(strcmp(absolute, "http://example.com/root") == 0);

    assert(ResolveHttpReference(base, "./", &result, absolute, sizeof(absolute)));
    assert(strcmp(absolute, "http://example.com/a/b/") == 0);

    assert(!ResolveHttpReference(base, "javascript:alert(1)", &result, absolute, sizeof(absolute)));
}

static void TestIpv4Literals()
{
    unsigned char octets[4] = {};
    assert(ParseIpv4Literal("192.0.2.17", octets));
    assert(octets[0] == 192 && octets[1] == 0 && octets[2] == 2 && octets[3] == 17);
    assert(ParseIpv4Literal("0.0.0.0", octets));
    assert(!ParseIpv4Literal("256.0.0.1", octets));
    assert(!ParseIpv4Literal("1.2.3", octets));
    assert(!ParseIpv4Literal("1.2.3.4.5", octets));
    assert(!ParseIpv4Literal("1..3.4", octets));
    assert(!ParseIpv4Literal("01.2.3.4", octets));
    assert(!ParseIpv4Literal("1.2.3.-1", octets));
    assert(IsIpv4LiteralCandidate("192.0.2.1"));
    assert(IsIpv4LiteralCandidate("256.0.2.1"));
    assert(IsIpv4LiteralCandidate("1..2.3"));
    assert(!IsIpv4LiteralCandidate("example.192.0.2.1"));
}

int main()
{
    TestContentLength();
    TestCloseDelimitedAndRedirect();
    TestChunked();
    TestChunkExtensions();
    TestRejectedFraming();
    TestRelativeUrls();
    TestIpv4Literals();
    return 0;
}
