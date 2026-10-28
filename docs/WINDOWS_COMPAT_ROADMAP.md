# Nexora Windows application compatibility roadmap

The target is a Nexora-native compatibility environment in the spirit of Wine: launch PE32+
applications as user processes and provide Windows ABI behavior through Nexora-owned loaders,
libraries, and services. It is not a promise that every Windows program will run. Compatibility is
measured per application and API set; kernel-mode Windows `.sys` drivers are outside this user-mode
environment and require Nexora-native drivers.

## Current implementation

- The PE32+ AMD64 loader validates/maps an executable and applies supported relocations.
- Bounded on-disk and mapped-image PE export readers resolve named and ordinal exports and identify
  forwarded exports. Boot-time fixtures cover direct exports, ordinal lookup, a forwarded-export
  string, missing names, undersized forwarder buffers, and invalid name ordinals. Forwarders are
  only identified: there is not yet a multi-module table to load their target DLLs or bind imports.
- Each process now tracks its mapped main PE module by case-insensitive basename. `GetModuleHandle`
  uses that record, and `GetProcAddress` resolves named or ordinal exports from the main image.
  The foreign-PE probe resolves and calls its exported test function by both name and ordinal.
  Forwarded exports and handles for unmapped DLLs are rejected. Export lookup currently copies the
  mapped image into bounded kernel memory before parsing it.
- A fail-closed import shim patches a small `kernel32.dll`/`kernelbase.dll` surface to Nexora
  system-call thunks. The tested foreign-PE probe covers console output failure behavior, process
  and thread IDs/handles, monotonic time, ANSI/Unicode lookup of the current executable by handle
  or path, process heap allocation, and process exit.
- The file subset now supports synchronous `CreateFileW` for ASCII drive-letter paths, with Win32
  `CREATE_NEW`, `CREATE_ALWAYS`, `OPEN_EXISTING`, `OPEN_ALWAYS`, and `TRUNCATE_EXISTING` disposition
  mapping, and `GetFileAttributesW` for basic directory/normal attributes. A native PE probe opens
  an existing file, creates a file on the writable FAT32 test volume, writes bytes, checks
  attributes, and closes both handles. The smoke harness verifies the exact payload from the disk
  image after VM shutdown. Sharing semantics, security descriptors, Unicode path conversion,
  overlapped I/O, delete semantics, and complete Win32 error mapping remain unsupported.
- The process PEB contains a bounded UTF-16 command line for `GetCommandLineW` alongside the narrow
  command line. WinCompat maps up to 128 unique imports in a four-page executable thunk region per
  process; this raises import capacity but does not load dependencies.
- The PE file-read ceiling is 256 MiB, and the parser identifies PE32 x86 images as unsupported
  instead of misreporting them as corrupt. The supplied AnimeVault executable was downloaded and
  tried from a disposable FAT32 VM disk. It is 145 MB, machine `I386`/PE32, Windows GUI subsystem,
  and imports broad `USER32`, `GDI32`, `SHELL32`, `ADVAPI32`, `COMCTL32`, and `ole32` APIs. Nexora
  correctly rejects it with `STATUS_NOT_SUPPORTED` (`0xC00000BB`) before import resolution. It did
  not launch or render its application UI; the test screenshot records this rejection. Its
  `Windows-x64` filename does not match the executable's 32-bit architecture.
- A bounded `ws2_32.dll` subset implements `WSAStartup`/`WSACleanup`, stream `socket`, ANSI and
  ASCII-only Unicode `getaddrinfo`/`freeaddrinfo`, `connect`, `send`, `recv`, and `closesocket` over
  Nexora DNS and TCP. A QEMU PE probe resolves `example.com` and numeric IPv4, checks canonical-name
  output, then uses emulated Intel E1000 networking to connect to a host HTTP fixture, send an HTTP
  request, and receive an HTTP response. Socket handles belong to the process and close on final
  cleanup or process exit; repeated startup calls are reference-counted. This does not establish
  arbitrary internet access, TLS from Windows applications, IPv6, nonblocking sockets, or Git
  compatibility.
- `kernel32.dll`/`kernelbase.dll` imports are now checked against that module's implemented API
  surface, Winsock calls are accepted only from `ws2_32.dll`, and `msvcrt.dll`/`ucrtbase.dll` expose
  a small `memcpy`/`memset`/`strlen`/`malloc`/`free` subset. Allocation uses Nexora's bounded,
  process-owned compatibility heap. A foreign PE probe imports these routines from `msvcrt.dll`,
  validates their effects, and exits successfully. This is not a CRT startup/runtime: formatted I/O,
  locale, exceptions, static initialization, and most CRT exports remain unsupported.
- Unsupported DLLs and exports are reported and rejected. There is no general DLL loader, multi-DLL
  dependency/cycle handling, runtime forwarder resolution, full CRT, `ntdll`, `user32`,
  `gdi32`, registry, Windows exception/unwind support, or installer compatibility. Ordinary desktop
  Windows applications and Git for Windows are not supported yet. The real local `git.exe` import
  table requires `CreateProcessW`, dynamic DLL loading, critical sections, TLS, virtual-memory APIs,
  broad CRT startup/file/formatting support, and `SHELL32`, `SHLWAPI`, and `USER32` imports. Git and
  its installer have not been launched in Nexora.

## Implementation sequence

1. **PE module loader (in progress):** map multiple AMD64 PE modules per process; maintain a case-insensitive
   module table; resolve imports by name and ordinal; process base relocations; validate bounds and
   permissions; detect dependency cycles; resolve forwarded exports; unload safely. Add generated
   PE fixtures for malformed directories, ordinal imports, forwarding, dependency graphs, and
   relocation cases before using real application binaries.
2. **Windows runtime foundation:** implement a documented, versioned subset of `ntdll`,
   `kernelbase`, and `kernel32` for status translation, handles, virtual memory, files/directories,
   processes, threads, synchronization, locale/time, environment, command lines, and errors.
   Reimplement these APIs for Nexora; do not load Windows kernel components.
3. **CRT and common libraries:** implement enough UCRT/MSVCRT startup, argument/environment,
   heap, file, string, locale, and exception behavior to run selected console programs. Verify with
   native Windows-built fixtures and real redistributable test applications whose licenses allow
   testing.
4. **Desktop ABI:** add `user32` window/message/input behavior and `gdi32` drawing/font/bitmap
   behavior over Nexora's window manager and compositor. Add clipboard, dialogs, common controls,
   DPI, and accessibility incrementally with app-level tests.
5. **Graphics and advanced services:** test named-host DNS resolution through Winsock; extend
   Winsock with IPv6, nonblocking and readiness APIs, and robust last-error behavior; then add TLS through a Windows
   `schannel`/`WinHTTP` or documented application-facing subset. Separately define selected
   Direct3D/Vulkan, audio, printing, COM, registry, services, and shell interfaces only as matching
   Nexora backends become available. Return explicit unsupported errors for interfaces without a
   safe implementation.
6. **Installer and app validation:** support signed/unsigned PE policy explicitly; add process
   creation, filesystem, registry, service, and reboot contracts; test MSI/EXE installers in a
   disposable VM; never treat an installer starting as proof that the installed application works.
7. **Compatibility reporting and installer target:** publish a machine-readable API/module
   compatibility manifest and per-app results for launch, core workflows, rendering/input,
   persistence, and failure recovery. The requested Git for Windows installer is an end-to-end
   target only after the UI and networking milestones: verify the installer window renders and
   accepts keyboard/mouse input, install into a disposable writable disk, then verify a Git
   network operation such as cloning over HTTPS. A process launch alone is not a pass.

## Newly requested target: AnimeVault and NTFS

AnimeVault cannot be made to run by adding one missing import. Its PE image is 32-bit x86, uses
the Windows GUI subsystem, and imports broad Win32, GDI, shell, registry/security, common-controls,
and COM APIs. Nexora currently has neither a 32-bit Windows execution ABI nor the Windows GUI/API
surface. The compatibility sequence must first add 32-bit PE32 address-space and calling-convention
support, a 32-bit DLL loader, then the concrete imports and GUI behaviors AnimeVault exercises.
The app's real UI workflow and any network/media workflow must be tested in the VM before calling
it supported.

NTFS is also a new storage-driver project, not a formatter option. The initial native read path now
validates NTFS 3.x boot geometry, applies multi-sector update-sequence fixups, decodes resident and
nonresident attributes and mapping pairs, and reads initialized/sparse data through sector I/O. MFT
records can be read through the persistent volume context; root `$I30` entries are bounds-checked.
Synthetic parser and scratch-sector tests pass, and an OVMF smoke case reads a real formatted NTFS
volume's boot sector, `$MFT`, and root record. This is metadata/bootstrap support only: the I/O
manager still registers USTAR and FAT32, NTFS paths are not openable, writes/allocation/rename/delete
are not implemented, and Setup still formats FAT32. Those features and a cold-installed NTFS boot
remain required. Keep the UEFI ESP as EFI FAT; move only the main Nexora system/data volume to NTFS
after the native filesystem and installer are complete. Do not describe Nexora as NTFS-backed until
Setup installs to NTFS and a cold reboot reads the system files from it.

## Acceptance rules

- Every API has a stated Windows ABI signature, implemented semantics, error/last-error behavior,
  ownership rules, bounds, and tests.
- Every PE parser and module operation is bounds-checked and fail-closed. Executable pages obey
  W^X; untrusted images do not gain kernel pointers or unrestricted kernel-mode access.
- API stubs that return success without performing the operation are not counted as support.
- A single probe passing establishes only that probe's imports and behavior, not general app
  compatibility. Report exact applications and workflows tested.
- Implementing a Win32 API does not provide Windows driver compatibility. Nexora device support
  remains in the separate native-driver roadmap.
