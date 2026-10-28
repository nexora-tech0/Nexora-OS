# Browser networking scope

The native Browser supports HTTP and verified HTTPS GET requests over the user-mode IPv4 DNS/TCP
APIs. It also accepts strict dotted-decimal IPv4 literals without DNS; malformed numeric/dotted
hosts and out-of-range octets are rejected before connecting. Response headers are limited to
8 KiB and page source to 512 KiB. It accepts content-length, chunked, or connection-close-delimited
HTTP responses, and chunk extensions are ignored after validating their visible-ASCII syntax.
Redirects are limited to five hops and must stay on the original scheme.

HTML is parsed into a bounded tree (256 nodes, 32 levels, 16 KiB of attribute strings) and rendered
with basic block/inline flow, headings, links, and alt text. At most eight linked stylesheets are
fetched (32 KiB each); the CSS parser accepts a small tag-selector/declaration subset, not the
complete CSS cascade or layout model. At most eight images are fetched, 128 KiB per image and
512 KiB total, with a 512 Ki-pixel per-image and 2 Mi-pixel total decoded-pixel budget. PNG, JPEG,
GIF, and BMP are accepted; unsupported or failed resources are reported and fall back to alt text.
Redirects for subresources stay on the page's original scheme to prevent mixed-content downgrade.
Scripts and active embedded content are removed and never executed. There is no DOM, JavaScript
engine, or general-purpose web compatibility claim.

HTTPS uses the pinned BearSSL 0.6 (MIT) dependency and bundled Mozilla-derived CA roots. It
negotiates TLS 1.2 only, with ECDHE and AES-GCM or ChaCha20-Poly1305 suites, sends SNI for DNS
hostnames, and verifies the certificate chain, DNS hostname, and validity interval. The system
clock must be available and fall in the supported 2020-2100 range; secure handshake entropy must
be provided by the kernel `HzGenerateRandomBytes` RDRAND-only service. If either requirement is
unavailable, navigation fails closed. TLS 1.3 is not supported by this BearSSL release. HTTPS to
IPv4 literals is also rejected because BearSSL 0.6's minimal X.509 verifier only matches DNS
subjectAltName names and does not validate IP-address SAN identities. HTTP IPv4 literal requests
remain supported. The minimal verifier does not implement X.509 Name Constraints, so chains that
depend on those constraints fail closed when the extension is critical (as required by X.509);
the 118 bundled trust anchors were also checked and contain no Name Constraints extension. The
verifier does not check certificate revocation with CRLs or OCSP. TLS streams normally end with a
valid `close_notify`. If a peer closes TCP without that alert, the Browser accepts only a complete
HTTP response with content-length or chunked framing; truncated or close-delimited HTTPS responses
remain rejected.

Focused parser tests can be built without modifying the OS build configuration:

```sh
clang++ -std=c++20 -Wall -Wextra -Werror -Iuser/apps/browser \
  user/apps/browser/http.cpp user/apps/browser/url.cpp \
  user/apps/browser/tests/http_test.cpp -o build/http_test
build/http_test
clang++ -std=c++20 -Wall -Wextra -Werror -Iuser/apps/browser \
  user/apps/browser/html.cpp user/apps/browser/tests/html_test.cpp -o build/html_test
build/html_test
clang++ -std=c++20 -Wall -Wextra -Werror -Iuser/apps/browser \
  user/apps/browser/css.cpp user/apps/browser/tests/css_test.cpp -o build/css_test
build/css_test
```

The native OS target is built with `make build/user/browser.exe`. Runtime HTTP(S) access still
depends on the kernel TCP service and an active network interface; DNS names additionally require
working DNS configuration. A resolver status of `0xC00000A3` (`STATUS_DEVICE_NOT_READY`) is
reported as an unavailable network interface or DNS service, not as a DNS `NXDOMAIN` response;
the Browser preserves the original status code for diagnosis. See
`third_party/bearssl-ca/NOTICE.txt` for trust-store provenance.
