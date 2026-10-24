# Browser engine and networking port plan

This document is the boundary between the native app shell and a real browser engine. The current
application deliberately does not claim that a web page has loaded unless an engine actually loads it.

## Required components

1. **URL and origin model** — canonical URL parsing, redirect policy, origin calculation, and safe
   handling of user-entered addresses.
2. **Network API** — DNS resolution, TCP streams, UDP where needed, timeouts, cancellation, and
   bounded buffering exposed to user-mode applications.
3. **TLS** — certificate-chain validation, hostname validation, trust store, expiry and revocation
   policy, secure trust-store updates. No "ignore certificate errors" fallback.
4. **HTTP** — redirects, status codes, content types, content encodings, streaming response bodies,
   caching semantics, and download cancellation.
5. **Web engine** — HTML parsing/DOM, CSS cascade and layout, paint output, image decoding, fonts,
   JavaScript, timers, promises, modules, and required Web APIs.
6. **Security boundary** — per-origin storage, same-origin policy, CSP, mixed-content handling,
   cookies, permissions, process isolation, and sandboxed renderer execution.
7. **Browser integration** — navigation callbacks, composited surfaces, downloads, profile storage,
   keyboard input, accessibility, and crash recovery.

The OS tree now includes an initial, bounded HTML-to-text parser in
`user/apps/browser/html.cpp`. It extracts document titles, basic block text and anchor labels/hrefs,
decodes a small set of named entities, skips script/style blocks, and reports truncation or
unsupported content. It is not a DOM, CSS layout engine, image renderer, or JavaScript runtime.

The browser shell presents its current page area as a wrapped, scrollable surface and uses the
existing hzui easing for page-text transitions. This improves the native shell layout only; it does
not add CSS layout or remote-page rendering.

## Engine selection

Porting Chromium, WebKit, or Gecko is a substantial platform project. These engines depend on
platform threading, memory allocation, graphics, fonts, IPC, file access, networking, and many build
tools. Nexora OS currently uses a custom ABI and statically linked PE32+ programs without DLL imports,
so copying engine source into this repository will not make it work.

Choose a mature engine only after checking its license and the required third-party notices. Keep the
engine behind a narrow adapter so the browser UI does not depend on engine-specific implementation
details. Do not write a new JavaScript engine or TLS stack as a shortcut to a working browser.

## Profile, history, bookmarks, downloads

The prototype writes history and bookmarks under
`C:\\Nexora\\Profiles\\Default\\Browser` and lists existing files under
`C:\\Nexora\\Profiles\\Default\\Downloads`. This is a **temporary local default profile**, not
account sync. The public OS source currently does not expose a documented profile identity/sync API.

To connect to the real OS profile:
- expose a profile service that returns a per-user writable data directory and stable profile ID;
- route history, bookmarks, cookies, cache, and preferences through that service;
- keep downloads in the user's chosen directory, use safe filenames, stream to a temporary file, and
  atomically finalize only after successful completion;
- provide explicit import/export and deletion controls;
- never sync passwords/cookies until the OS has secure credential storage and authenticated sync.

## Milestones

- [x] Native shell, address entry, local history and bookmarks
- [x] Bounded HTML text parsing prototype
- [x] Keyboard shortcuts and downloads listing
- [x] Build against the current Nexora OS tree
- [ ] OS profile service integration
- [ ] DNS and TCP APIs with tests
- [ ] TLS and certificate verification
- [ ] Plain HTTP client with bounded response parsing and streaming downloads
- [ ] CSS layout and composited page rendering
- [ ] HTTPS enablement only after certificate verification
- [ ] Mature engine port and composited rendering surface
- [ ] Sandboxing, origin isolation, persistence, accessibility, and web-platform/security tests

No milestone should be marked complete without a build or test artifact proving it.
