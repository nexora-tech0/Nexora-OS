# ADR 0001: Arch Linux and archiso as the initial image direction

- **Status:** accepted as an initial direction; not yet implemented.
- **Date:** 2026-10-18.

## Context

Nexora needs an x86_64 Linux base with a reproducible live-image workflow. The
project brief recommends Arch Linux and archiso. The archiso project documents
profiles for customizable Arch live images and states that image creation is
currently supported on Arch Linux.

## Decision

Use Arch Linux packages and an owned archiso profile for the first bootable
prototype, subject to validating the workflow on a dedicated Arch Linux build
host. Keep Nexora package definitions and system configuration distinct from
the base distribution. Do not patch or replace the host system to produce an
image.

This is a prototype decision, not a promise of indefinite upstream, security,
or hardware support. Reassess before a public release and whenever maintenance
or update requirements change.

## Consequences

- Image creation requires an Arch Linux build environment and Arch packaging
  expertise.
- Rolling updates make snapshotting, package pinning for reproducibility, and
  update recovery important design work; these are not implemented yet.
- Windows is not the image-build host in the current setup. A dedicated Linux
  machine or VM is required.
- Image generation, checksum verification, and VM boot are acceptance gates;
  none has passed yet.

## Evidence

- [Archiso project documentation](https://github.com/archlinux/archiso)
- [ArchWiki: Archiso](https://wiki.archlinux.org/title/Archiso)

These upstream pages were reviewed on 2026-10-18. Recheck project state and
dependencies when implementing the profile.
