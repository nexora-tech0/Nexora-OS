#pragma once
#include <stddef.h>

namespace nexora::browser {
enum class Scheme { Invalid, Http, Https };

struct Url {
    Scheme scheme = Scheme::Invalid;
    char host[256] = {};
    char path[2048] = "/";
    unsigned short port = 0;
    bool explicitPort = false;
    bool valid = false;
};

enum class UrlError {
    None, Empty, TooLong, UnsupportedScheme, MissingHost, InvalidHost,
    InvalidPort, InvalidCharacter, InvalidAuthority
};

/* Strict parser for the first HTTP(S) milestone. It performs no network I/O. */
UrlError ParseUrl(const char *input, Url *out);
bool IsSecure(const Url &url);
const char *SchemeName(Scheme scheme);
}