#include "../user/apps/browser/core/url.h"
#include <assert.h>
#include <string.h>
using namespace nexora::browser;

int main() {
    Url u;
    assert(ParseUrl("https://example.com/", &u) == UrlError::None);
    assert(u.valid && u.scheme == Scheme::Https && !strcmp(u.host, "example.com"));
    assert(u.port == 443 && !strcmp(u.path, "/"));
    assert(ParseUrl("http://example.com:8080/a?q=1", &u) == UrlError::None);
    assert(u.port == 8080 && u.explicitPort && !strcmp(u.path, "/a?q=1"));
    assert(ParseUrl("https://example.com?q=1", &u) == UrlError::None);
    assert(!strcmp(u.path, "/?q=1"));
    assert(ParseUrl("file:///etc/passwd", &u) == UrlError::UnsupportedScheme);
    assert(ParseUrl("https://user@example.com/", &u) == UrlError::InvalidAuthority);
    assert(ParseUrl("https://example.com:99999/", &u) == UrlError::InvalidPort);
    assert(ParseUrl("https:// bad.example/", &u) == UrlError::InvalidCharacter);
    assert(ParseUrl("https:///missing-host", &u) == UrlError::MissingHost);
    assert(ParseUrl("", &u) == UrlError::Empty);
    return 0;
}