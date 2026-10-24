# Nexora Browser implementation status

Updated: 2026-10-24

This repository is an application project for a custom OS, not a normal Windows/Linux browser
application. The target Nexora OS source currently exposes no user-mode DNS, TCP socket, TLS, HTTP,
or browser-engine interface in its public source tree. The app cannot safely claim full web support
until these platform dependencies exist.

## Current source delivered

- Native C++ UI shell and navigation controls.
- Local address history and bookmark persistence in the prototype.
- Downloads-directory listing (not an Internet download manager).
- Keyboard shortcuts.
- A strict, standalone HTTP(S) URL parser in `user/apps/browser/core/url.cpp`.
- Parser tests in `tests/url_parser_test.cpp`.

## Validation

Run the URL parser test on a development host:

```sh
c++ -std=c++11 -Wall -Wextra -Werror user/apps/browser/core/url.cpp tests/url_parser_test.cpp -o url_parser_test
./url_parser_test
```

This validates URL parsing only. It does not test the OS UI, networking, TLS, page rendering, or a
browser engine. The current OS app shell has not been compiled against the Nexora OS SDK in this
environment.

## What “full browser” actually requires

A browser capable of modern sites needs a mature HTML/CSS/JavaScript engine, image/font support,
networking, DNS, TCP, TLS certificate verification, HTTP, cookies and site storage, cache, downloads,
origin isolation, sandboxing, permissions, accessibility, and substantial security testing. Those
cannot honestly be implemented by adding a few UI files. A production engine port is a major OS
platform project.

## Next dependency order

1. Land and compile the parser tests.
2. Define user-mode networking contracts in Nexora OS; implement DNS and TCP with deterministic tests.
3. Add a TLS implementation with trusted roots and hostname validation. Never disable certificate checks.
4. Implement HTTP response parsing, redirects, bounded streaming, cancellation, and download safety.
5. Choose and port a mature web engine after verifying platform and license requirements.
6. Integrate renderer surfaces, navigation callbacks, per-profile storage, and sandbox boundaries.
7. Run protocol, web-platform, crash, memory-pressure, and security tests before calling the browser ready.
