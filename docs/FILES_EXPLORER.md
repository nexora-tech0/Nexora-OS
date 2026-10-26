# Files Explorer audit and implementation status

This document records the source-level audit requested for the Files application. “Verified using
available evidence” means the behavior is present in the source and backed by a native API or test;
it does not claim that the UI was exercised on a booted system. A fresh ISO was built after these
changes and its UEFI boot entry and packaged ramdisk contents were inspected; it has not been
booted for runtime verification.

## Changes implemented after the audit

- Added one shared known-folder enum and path resolver in `hzfile`; Desktop, Documents and Files
  now use that shared convention.
- First-boot setup now creates Desktop, Documents, Downloads, Music, Pictures, Videos and Projects
  once as part of the profile, and reports a directory-creation failure instead of continuing with
  a partial profile.
- The Files sidebar lists those known folders only when they really exist and adds a Trash
  location.
- Files Delete now moves an item to the existing Trash where possible. Restore, permanent delete
  with confirmation and Empty Trash are available from the Trash view. Shift+Delete remains an
  explicitly confirmed permanent delete. No fallback silently converts a failed move-to-Trash into
  permanent deletion.
- Settings now exposes Files default view/sort, hidden-file visibility and extension display. The
  Files app filters by the native hidden attribute and applies the saved preferences when opened.
- Recursive property/deletion counts now report listing failures rather than presenting a
  success-shaped partial count.

These changes have compiled and are present in the built ISO, but have not been exercised in a
running OS.

## Phase 0: architecture audit

### Existing implementation

The existing `user/apps/files/files.cpp` is a native hzui application, not a mockup. It already has:

- Back, forward, up, editable address path, breadcrumbs and in-folder filename filtering.
- A sidebar containing the current user's Home/Desktop/Documents when present and discovered
  mounted volumes.
- Details, List and Tiles layouts; sort by name, modified time, type or size; per-user-app settings
  in `C:\Nexora\Settings\files.ini`.
- New folder/file, rename, copy, move, delete, refresh, type-ahead selection, context menus and
  file/directory properties.
- Manifest-driven file opening through `hz::apps::OpenPath`; executable files use the native
  process API.
- ZIP, TAR and GZIP-family extraction through `hz::archive`, with bounded archive/member sizes,
  path containment checks and integrity checks in the archive implementation.
- A single-item clipboard owned by the Files window. Copy collisions use a generated copy name;
  move collisions are rejected. Recursive deletion is confirmed but is permanent.

### Filesystem and storage actually available

- User-mode file calls are in `user/include/hzfile.h` and `user/lib/hzrt/hzfile.cpp`; they use the
  native `NtCreateFile`, directory enumeration, volume-information and file-operation APIs.
- The kernel mounts read-only USTAR for the live `C:` image and FAT32 for writable storage.
  `hz::fs::Volumes()` discovers drive letters that successfully answer the native volume query; it
  does not invent drives.
- Volume label, filesystem, read-only state, total bytes and free bytes are exposed by the volume
  information API.
- Native copy/move/delete-tree operations exist. Recursive copy refuses a destination inside its
  source. The API does not expose cancellation, transactional rollback, or per-file progress.
- Trash exists in `hzfile` and is backed by a stored item plus an index containing its original
  path. The Files app currently does not expose that API; its Delete action calls permanent
  `DeleteTree`.
- Archive support is implemented in `user/lib/hzrt/hzarchive.cpp`; supported formats and limits
  are narrower than general-purpose desktop archive software.

### Profiles, associations, permissions and unsupported facilities

- First-boot setup creates `C:\Users\<name>\Documents` and writes the user name to `shell.ini`.
  The shell separately resolves/creates the Desktop directory. The current profile does not create
  Downloads, Music, Pictures, Videos, Projects or AppData directories.
- There is no known-folder registry. Path conventions are repeated in setup, shell and runtime
  helpers.
- App manifests in `C:\Nexora\Apps` declare names, executable paths and handled extensions.
  `hz::apps::OpenPath` uses the first manifest matching an extension. A configurable “Open with”
  or per-user default-handler registry is not present.
- The file API reports read-only and hidden attributes, but does not expose a user/ACL permission
  model, symbolic-link metadata or safe link traversal controls. Do not represent UI filtering as
  access control.
- The available storage drivers include boot ramdisk and AHCI-backed disks. The source does not
  establish removable USB filesystem, network-share, indexed-search or cloud-provider support.
- The current Files list is single-selection. There is no drag/drop or file-operation worker/progress
  service available to this application.
- Image decoding exists in the shared user runtime and Photos app; there is no Files gallery,
  thumbnail cache or metadata/index service.

## Dependency map

| Capability | Existing component | Safe implementation boundary |
|---|---|---|
| Navigation, views, sorting, basic operations | `user/apps/files/files.cpp`, hzui | Extend the existing app and toolkit; do not create a second explorer. |
| Paths, listings, metadata, operations, volume data | `user/include/hzfile.h`, `user/lib/hzrt/hzfile.cpp` | Add shared path helpers only where setup, shell and apps can reuse them. |
| Volume discovery and access | `io/iomgr.c`, FAT32 and USTAR filesystems, `io/iosvc.c` | Display only volumes reported by the native API; unsupported devices remain unavailable. |
| Trash/restore | `user/lib/hzrt/hzfile.cpp` | Wire existing operations into Files; never label permanent deletion as recoverable. |
| Associations and process launch | `rootfs/Nexora/Apps/*.app`, `user/lib/hzrt/hzapps.cpp` | Reuse manifests and process creation; do not claim unsupported handlers. |
| User profile and setup | `user/apps/setup/setup.cpp`, `user/shell/desktop.cpp`, `hzfile.cpp` | Centralize supported known-folder paths and create user folders once during setup. |
| Preferences | `files.ini` via `hz::fs::Settings`; Settings app helpers | Reuse existing INI settings; report read-only/save failures. |
| Archive extraction | `user/include/hzarchive.h`, `user/lib/hzrt/hzarchive.cpp` | Keep existing format/size limits and path-containment checks. |
| UI and input | `user/include/hzui.h`, `user/lib/hzui` | Use native hzui controls; larger async operations need a real worker/cancellation design first. |

## Status checklist

Status vocabulary: **Not implemented**, **In progress**, **Implemented, unverified**,
**Verified using available evidence**, **Blocked by a missing dependency**.

| Area | Status | Evidence / limitation |
|---|---|---|
| 1. Architecture audit | Verified using available evidence | Source and API inspection summarized above; not a runtime audit. |
| 2. Filesystem integration | Verified using available evidence | Native path/list/read/write/volume APIs exist; kernel integration tests cover USTAR and FAT32. |
| 3. Directory hierarchy | Implemented, unverified | First boot creates seven standard folders; Public/Default/AppData omitted because multi-user and permission semantics are absent. |
| 4. Known-folder resolution | Implemented, unverified | Shared helper now resolves profile and seven known-folder paths. |
| 5. Profile creation | Implemented, unverified | First boot creates the profile folders and logs/errors on failure; no profile isolation/ACL support. |
| 6. Sidebar and navigation | Implemented, unverified | Existing Home/volume browsing plus real known folders and Trash; Gallery and persistent pins remain absent. |
| 7. File operations | Implemented, unverified | Core operations, Trash/restore and confirmed permanent delete; single selection and synchronous copy/move remain. |
| 8. File associations | Implemented, unverified | Manifest `Opens` and `OpenPath` work at source level; configurable defaults/Open With are absent. |
| 9. Search and sorting | Implemented, unverified | Bounded to the current in-memory directory listing; filename filtering and four sort keys. |
| 10. View modes | Implemented, unverified | Details, List and Tiles; no gallery/large-icon thumbnail view. |
| 11. Archives | Implemented, unverified | ZIP/TAR/GZIP-family extraction; not all formats or compression variants are supported. |
| 12. File previews | Blocked by a missing dependency | No Files preview pane or preview metadata service; existing Photos/Notepad can open supported files. |
| 13. Volume information | Implemented, unverified | Native label/filesystem/capacity/free/read-only fields are available. |
| 14. Keyboard shortcuts | Implemented, unverified | Common navigation and single-item copy/cut/paste/rename/delete; select-all/multi-select absent. |
| 15. Animations | Not implemented | No Explorer-specific animation work requested or needed for filesystem correctness. |
| 16. Security | Blocked by a missing dependency | Kernel permission/ACL and symlink APIs are not exposed; UI checks cannot substitute for them. |
| 17. Settings | Implemented, unverified | Default view/sort, hidden attribute and extension display saved to `files.ini`; changes apply on next Files launch. |
| 18. Error handling | In progress | File operations, profile-folder creation and recursive counts report failures; partial cross-volume move rollback and cancellation are not available. |
| 19. Documentation | Implemented, unverified | This audit, dependency map, checklist and current limitations are recorded here. |
| 20. Verification | In progress | Existing source tests/build targets are available; no Explorer UI runtime test has been performed. |

## Implementation order

1. **Implemented, unverified:** centralize the supported user-known-folder paths, create those
   directories during first boot, and expose only directories that actually exist.
2. **Implemented, unverified:** wire the existing Trash API into Files with restore and
   permanent-delete confirmation, while clearly distinguishing those operations.
3. **Implemented, unverified:** add default view/sort and hidden/extension preferences using
   `files.ini`.
4. Add broader selection, drag/drop, gallery, previews or asynchronous operations only after the
   UI and kernel/runtime dependencies are verified; do not simulate unsupported behavior.
5. Rebuild the installer only after the source changes are built and checked. Boot/runtime behavior
   remains unverified until a VM or physical system test is explicitly run.
