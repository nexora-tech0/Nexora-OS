# Third-party notices

Nexora OS is an original hobby operating system. It contains no Microsoft code, fonts, logos
or artwork. The following third-party components are used or redistributed:

| Component | Version | Used for | License | Location |
|---|---|---|---|---|
| Limine bootloader | 11.4.1 (binary release) | UEFI boot loader shipped on the ISO and, later, the installed disk | BSD-2-Clause | fetched by `scripts/setup-toolchain.sh`; license copied to `/boot/limine/LICENSE` on the ISO |
| Limine boot protocol header (`limine.h`) | commit 3a0526b | Kernel ↔ bootloader interface | 0BSD | `third_party/limine-protocol/` |
| stb_truetype | 1.26 (commit 2c980bb) | Build-time glyph rasterization (host tool); user-mode text rendering from M7 | Public domain or MIT (dual) | `third_party/stb/` |
| stb_image | 2.30 (single header) | Runtime picture decoding for the Photo Viewer (`hz::image`); no third-party library beyond it is linked | Public domain or MIT (dual) | `third_party/stb/stb_image.h`; license text copied to `Nexora/Licenses/stb-MIT.txt` |
| Inter typeface | 4.1 | UI font; kernel boot/stop screens use glyph bitmaps rendered from it at build time | SIL Open Font License 1.1 | `third_party/inter/` |
| JetBrains Mono typeface | 2.304 | Monospace font; kernel stop-screen dumps use glyph bitmaps rendered from it at build time; Terminal (M9) | SIL Open Font License 1.1 | `third_party/jetbrains-mono/` |
| Material Icons | Regular (master) | The UI icon set: `hzui::Icon` is drawn from this font at runtime, with the built-in vector paths as the fallback when the file is absent | Apache-2.0 | `third_party/icons/material/` |
| BearSSL | 0.6 (`v0.6`, commit `8ef7680081c61b486622f2d983c0d3d21e83caad`; source archive SHA-256 `6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14`) | Browser verified TLS 1.2 client | MIT | `third_party/bearssl/`; license copied to `Nexora/Licenses/BearSSL-MIT.txt` |
| Mozilla-derived CA roots | curl CA bundle dated 2026-10-20 | Browser certificate-chain trust anchors generated with BearSSL `brssl ta` | Mozilla Public License 2.0 | `third_party/bearssl-ca/`; provenance and SHA-256 in `NOTICE.txt`, copied to `Nexora/Licenses/Browser-CA-Store.txt` |
| QuickJS | 2026-06-04 | Staged modern ECMAScript engine source for future browser integration; not yet linked or enabled | MIT | `third_party/quickjs/`; license copied to `Nexora/Licenses/QuickJS-MIT.txt` |
| Rust `core` / `compiler_builtins` | Rust 1.98.1 | Linked into the kernel with the region library | MIT or Apache-2.0 | Rust toolchain |

Build tools (LLVM/Clang/LLD, rustc, NASM, xorriso, QEMU, OVMF) are used to build and test
Nexora OS and are not redistributed in its images.

The Nexora OS mark (`assets/branding/nexora-os-logo.svg`) and all other artwork are original to
this project. The boot splash and taskbar draw the mark directly.
