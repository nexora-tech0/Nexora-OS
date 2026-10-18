# ADR 0002: Evaluate Smithay and wlroots before selecting a compositor base

- **Status:** proposed; selection blocked on a prototype and dependency review.
- **Date:** 2026-10-18.

## Context

Nexora needs a Wayland compositor for x86_64 PCs and VMs. The brief suggests
Smithay and wlroots; choosing based on language preference or a shell demo alone
would ignore protocol, backend, maintenance, licensing, and integration costs.

## Options

| Criterion | Smithay | wlroots |
| --- | --- | --- |
| Form | Rust building blocks for a compositor; not a ready-made desktop or complete compositor | Composable C libraries/modules for compositor backends, protocols, rendering and XWayland integration |
| Strengths | Rust safety properties; modular APIs; good fit if the project wants to own compositor policy in Rust | Broad compositor infrastructure and backend support; existing wlroots-based projects provide prior art |
| Costs and risks | Nexora must integrate the building blocks and implement significant compositor behavior; confirm required protocol and hardware coverage in a prototype | C integration and ABI/dependency management; protocol and API compatibility must be validated against the chosen release; project development has moved from GitHub to freedesktop.org GitLab |
| Maintenance evidence | GitHub's `master` branch showed commits through 2026-10-15 during this review | freedesktop.org GitLab's `master` branch showed commits through 2026-10-17 during this review |
| Qt/QML relationship | Qt shell applications remain Wayland clients; Smithay does not replace Qt or provide the shell | Qt shell applications remain Wayland clients; wlroots does not replace Qt or provide the shell |
| License evidence | Smithay 0.7.0 `Cargo.toml` declares MIT; audit the exact pinned revision and transitive crates before distribution | The upstream `LICENSE` is MIT-style; audit the exact pinned revision and transitive dependencies before distribution |

This comparison is an initial screening, not a compatibility or performance
claim. The upstream repositories showed recent commits during this review; that
is a point-in-time activity signal, not a maintenance guarantee. Confirm
supported protocols, current release policy, license texts, and packaging
details against the exact revision selected for a prototype. Qt is separately
available under LGPL/GPL or commercial terms; check the selected Qt modules,
linking model, and distribution obligations before shipping a Qt-based image.

## Decision

Do not choose either library yet. Build a small, disposable VM prototype for
each viable candidate, or establish from source/build evidence why one candidate
cannot meet the requirements. Evaluate:

1. A Wayland session with the intended Qt/QML shell client.
2. Input and display operation in QEMU and on the target DRM/KMS path.
3. Required shell protocols, XWayland, multi-monitor behavior, and session
   integration.
4. Build reproducibility, diagnostics, testability, maintainer activity, and
   upgrade process.
5. Exact dependency licenses and the practical amount of Nexora-owned code.

Record tested revisions, commands, VM configuration, observed failures, and
decision rationale before changing this ADR to accepted.

## Evidence

- [Smithay project](https://github.com/Smithay/smithay)
- [Smithay package manifest](https://raw.githubusercontent.com/Smithay/smithay/master/Cargo.toml)
- [Smithay recent commits](https://github.com/Smithay/smithay/commits/master)
- [wlroots project migration notice and overview](https://github.com/swaywm/wlroots)
- [wlroots on freedesktop.org GitLab](https://gitlab.freedesktop.org/wlroots/wlroots)
- [wlroots license](https://gitlab.freedesktop.org/wlroots/wlroots/-/raw/master/LICENSE)
- [wlroots recent commits](https://gitlab.freedesktop.org/wlroots/wlroots/-/commits/master)
- [Qt open-source licensing](https://www.qt.io/development/open-source-lgpl-obligations)

Upstream metadata and commit histories were reviewed on 2026-10-18. Release-
specific compatibility and the complete transitive dependency licenses remain
to be verified during the prototype.
