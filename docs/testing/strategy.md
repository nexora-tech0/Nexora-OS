# Testing strategy

**Status:** planned; no executable tests exist yet.

## Test layers

1. **Build checks:** formatting, static analysis, dependency/license inventory,
   and reproducible package/image build checks.
2. **Unit tests:** pure logic in settings, launcher providers, notification
   policy, and other components, with no privileged host operations.
3. **Integration tests:** Wayland protocol interaction, D-Bus/service behavior,
   package installation/update, and error recovery in an isolated environment.
4. **VM desktop tests:** boot an exact image artifact; exercise session startup,
   launching/closing applications, window focus, keyboard navigation, lock,
   logout, and recovery from a failed shell process.
5. **Hardware tests:** only after VM gates pass; verify supported devices and
   document each tested configuration.

## Required behavior coverage

As features are implemented, add tests for launcher activation/search, dock
behavior, workspaces, focus and Alt+Tab, notification exceptions in Quiet Mode,
accessibility and reduced motion, display scaling, multi-monitor handling,
network/audio/power controls, session locking, suspend/resume, package updates,
and failed-update recovery.

Tests must observe the user's actual outcome, not merely confirm that files
exist or a process started. Tests involving privileged actions must use a
disposable VM and must verify denial, invalid input, and failure reporting as
well as the successful path.

## Current baseline

There is no source code, image profile, test harness, or CI workflow yet, so no
build or test command is available. Add only the checks needed for implemented
code; do not create passing placeholder tests.

The first acceptance test is to build a project-owned Archiso image, verify its
checksum, and boot that same artifact in a disposable VM to a usable Wayland
session. See [Arch Linux development setup](../development/arch-linux.md).
