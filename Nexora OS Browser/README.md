# Nexora Browser

A native C++ browser shell project for Nexora OS.

## Current status

This repository starts the **native browser application layer**: toolbar, address entry, navigation history, bookmarks, downloads view, keyboard shortcuts, profile-storage integration points, and a bounded HTML text parser.

It is **not yet a functioning general-purpose web browser**. The HTML parser is not a page renderer; Nexora OS still needs DNS/TCP, plain HTTP, CSS layout/painting, TLS with certificate verification, and a compatible JavaScript engine. The app must never pretend a URL loaded when it did not.

## Product direction

- Quiet, polished desktop UI: charcoal surfaces, restrained silver accents, clear hierarchy, no decorative AI cards or fake metrics.
- Keyboard-first navigation and responsive layouts.
- Local profile data by default, with storage behind a profile adapter so it can be connected to the real Nexora OS account/profile service.
- Downloads must use streaming writes, safe filename handling, cancellation, and explicit destination selection.
- Security before compatibility: verified TLS certificates, origin isolation, sandboxing, and no silent certificate bypass.

## Source layout

- `user/apps/browser/browser.cpp` — native app shell and built-in parser demonstration for the Nexora OS `hzui` API.
- `user/apps/browser/html.cpp` — bounded HTML-to-text parser prototype.
- `rootfs/Nexora/Apps/browser.app` — OS application manifest.
- `docs/ENGINE_PORT.md` — engine and networking port plan.

## Integration

The native app shell, URL parser, HTML text parser, and manifest are integrated into the Nexora OS tree under
`user/apps/browser` and `rootfs/Nexora/Apps/browser.app`. The OS Makefile discovers the app and
includes it in the system image. Build the OS and run the `browser` smoke case to verify it launches
and validates addresses. The start page demonstrates the initial HTML text parser; remote pages
are not fetched, and CSS layout is not implemented.

The URL parser can also be tested independently with the CMake workflow below. This project does
not contain the Nexora OS SDK or kernel headers, so the native application itself must be built by
the OS repository.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Engine strategy

See [docs/ENGINE_PORT.md](docs/ENGINE_PORT.md). A mature engine port is preferable to writing a JavaScript engine or TLS stack from scratch. A source drop is not a port: the engine needs platform threading, memory, graphics, font, network, storage, and sandbox adapters, and its license must be respected.
