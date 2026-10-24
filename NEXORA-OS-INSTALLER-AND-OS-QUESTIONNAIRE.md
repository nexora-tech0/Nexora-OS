# Nexora OS Installer and Installed System Questionnaire

Use this questionnaire to describe how you want Nexora OS to look and work. Select an option,
write your own answer, or mark an item "not sure." You can answer only the questions that matter
most; unanswered items can be discussed later.

## 1. Project direction

1. **What should Nexora OS feel like overall?**
   - [ ] Minimal and calm
   - [ ] Premium and polished
   - [ ] Friendly and approachable
   - [ ] Technical and configurable
   - [ ] Other:

2. **Which systems should the design take inspiration from?**
   - [ ] Keep the current Nexora design
   - [ ] macOS-like
   - [ ] Windows-like
   - [ ] Linux desktop-like
   - [ ] A specific reference image or product:
   - What should be borrowed, and what should not?

3. **Which visual style should be the default?**
   - [ ] Dark
   - [ ] Light
   - [ ] Follow a choice during setup
   - [ ] Follow the time of day
   - Preferred accent color:

4. **What are the three most important outcomes for the next release?**
   1.
   2.
   3.

## 2. Installer purpose and delivery

5. **What should the installer do?**
   - [ ] Install Nexora OS to a computer disk
   - [ ] Offer a live trial before installation
   - [ ] Support both live trial and installation
   - [ ] Repair an existing Nexora installation
   - [ ] Remove an existing Nexora installation
   - [ ] Other:

6. **How should users start the installer?**
   - [ ] Start from the live desktop
   - [ ] Launch directly into setup from the boot menu
   - [ ] Offer both choices
   - Preferred default:

7. **Which firmware and machine types should the installer support first?**
   - [ ] UEFI only
   - [ ] UEFI and legacy BIOS
   - [ ] VirtualBox first
   - [ ] QEMU first
   - [ ] Specific physical computers or hardware:

8. **How should the installation media be delivered?**
   - [ ] Bootable ISO image
   - [ ] USB image
   - [ ] Both
   - [ ] Other:

## 3. Installer look and feel

9. **What should the installer window look like?**
   - Preferred background:
   - Preferred Nexora logo or artwork:
   - Preferred typography:
   - Preferred accent color:
   - [ ] Dark, translucent cards
   - [ ] Opaque dark surfaces
   - [ ] Light surfaces
   - [ ] Match the installed desktop
   - [ ] Other:

10. **How should the installer be presented?**
    - [ ] One focused page at a time
    - [ ] A sidebar with all steps
    - [ ] A compact centered wizard
    - [ ] Full-screen setup
    - [ ] A resizable window
    - Preferred window size or behavior:

11. **What navigation should it provide?**
    - [ ] Back and Next buttons
    - [ ] A visible step indicator
    - [ ] A summary before any disk changes
    - [ ] Keyboard navigation
    - [ ] Escape cancels before installation starts
    - [ ] Other:

12. **What should motion and sound be like?**
    - [ ] No animation
    - [ ] Subtle transitions
    - [ ] More expressive animation
    - [ ] No sound
    - [ ] Optional sound effects
    - Notes:

## 4. Installer flow

13. **Which steps should the installer include? Choose the desired order.**
    - [ ] Welcome and install/live choice
    - [ ] Language and region
    - [ ] Keyboard layout
    - [ ] Network status or configuration
    - [ ] License or notices
    - [ ] Disk selection
    - [ ] Partitioning choice
    - [ ] Final review and confirmation
    - [ ] File copy and progress
    - [ ] Restart instructions
    - [ ] Other:

14. **Should setup require an internet connection?**
    - [ ] No; install fully offline
    - [ ] No, but offer optional network setup
    - [ ] Yes
    - If optional, what should online setup enable?

15. **How should the installer handle multiple disks?**
    - [ ] Show every detected disk with model and size
    - [ ] Show connection/type details when available
    - [ ] Require the user to choose; never guess
    - [ ] Let the user refresh the disk list
    - [ ] Explain why a disk cannot be selected
    - Other:

16. **Which disks must never be offered as installation targets?**
    - [ ] The disk containing the running system
    - [ ] Read-only disks
    - [ ] Disks already in use
    - [ ] Disks that are too small or unsupported
    - [ ] Other:

17. **Which installation modes should be offered?**
    - [ ] Use the whole selected disk
    - [ ] Install alongside another operating system
    - [ ] Manual partitioning
    - [ ] Nexora-only disks for now; no dual-boot
    - [ ] Other:
    - If multiple modes exist, which should be recommended by default?

18. **What should happen to existing data on the selected disk?**
    - [ ] Never erase until a separate final confirmation
    - [ ] Show the exact selected disk model, size, and partitions before confirmation
    - [ ] Require a checkbox acknowledging data loss
    - [ ] Require typing a phrase such as `ERASE`
    - [ ] Provide a final Cancel option
    - [ ] Other safeguards:

19. **What should the installer do if it encounters existing partitions or another OS?**
    - [ ] Stop and explain what it found
    - [ ] Offer only a full-disk install
    - [ ] Offer a safe alongside-install option, if implemented
    - [ ] Offer manual partitioning, if implemented
    - [ ] Other:

20. **What should the installer do if power is lost or installation fails?**
    - Desired error detail:
    - [ ] Explain whether the disk may be partially changed
    - [ ] Offer a way to retry
    - [ ] Preserve logs for troubleshooting
    - [ ] Never report success unless verification passes
    - Other:

21. **What progress details do you want during installation?**
    - [ ] Overall percentage
    - [ ] Current step
    - [ ] Current file or operation
    - [ ] Estimated remaining time
    - [ ] Installation log/details
    - [ ] Keep it simple; show only status and progress
    - Other:

22. **Should the user be able to cancel after disk writing begins?**
    - [ ] No; explain that closing or powering off is unsafe
    - [ ] Yes, only at safe checkpoints
    - [ ] Yes, at any time, with a warning
    - [ ] Not sure

23. **What should happen when installation completes?**
    - [ ] Restart immediately after confirmation
    - [ ] Return to the live desktop
    - [ ] Show clear instructions to remove the USB/ISO and restart
    - [ ] Offer a shutdown option
    - Other:

## 5. Disk, partition, and installation choices

24. **What should be the default disk layout?**
    - [ ] Let Nexora create the required partitions automatically
    - [ ] Ask for partition sizes
    - [ ] Provide manual partitioning
    - [ ] Other:
    - Minimum supported disk size:

25. **Which filesystems and boot format should be targeted first?**
    - Current supported layout is UEFI with GPT and FAT32.
    - [ ] Keep the current supported layout for the first release
    - [ ] Add other layouts (describe):
    - Compatibility requirements:

26. **Should users be able to choose an installation location or volume name?**
    - Install location options:
    - System volume label:
    - User data location:
    - [ ] Use sensible defaults
    - [ ] Let advanced users configure these

27. **Should setup create a recovery or repair option?**
    - [ ] Not for the first release
    - [ ] Bootable recovery environment
    - [ ] Repair startup files
    - [ ] Restore a previous installation
    - [ ] Other:

## 6. First boot and account setup

28. **When should the user create an account?**
    - [ ] During installation
    - [ ] On the first boot after installation
    - [ ] Offer both paths

29. **What account types should Nexora support?**
    - [ ] One local user account
    - [ ] Multiple local users
    - [ ] Guest account
    - [ ] Online account (only if a real service is implemented)
    - Other:

30. **Which account details should setup ask for?**
    - [ ] Display name
    - [ ] User name or home-folder name
    - [ ] Optional password
    - [ ] Require a password
    - [ ] Confirm the password
    - [ ] Other:
    - Password rules or recovery expectations:

31. **What should the first-boot welcome flow ask the user to configure?**
    - [ ] Desktop wallpaper
    - [ ] Lock-screen wallpaper
    - [ ] Light/dark theme
    - [ ] Time zone
    - [ ] Keyboard layout
    - [ ] Accessibility preferences
    - [ ] Network
    - [ ] Skip setup and use defaults
    - Other:

32. **Should Nexora collect analytics or diagnostic data?**
    - [ ] No collection
    - [ ] Optional, off by default, and clearly explained
    - [ ] Other:
    - What data, if any, may be collected?

## 7. Installed desktop and daily use

33. **What should users see after sign-in or first boot?**
    - [ ] Clean desktop with no open windows
    - [ ] Welcome window
    - [ ] Guided tour
    - [ ] Restore the previous session
    - [ ] Other:

34. **How should the desktop be arranged?**
    - Top dock:
    - Bottom taskbar:
    - Desktop icons:
    - Start menu or launcher:
    - Control Center placement and trigger:
    - [ ] Keep the current arrangement
    - [ ] Change it as described above

35. **How should the top dock look?**
    - [ ] Floating rounded bubbles over the wallpaper
    - [ ] Rounded bar with transparent background
    - [ ] Full-width translucent bar
    - [ ] Other:
    - Which elements should it contain?
    - How should it behave on smaller screens?

36. **How should the taskbar and app launcher work?**
    - [ ] Show running apps
    - [ ] Allow pinning apps
    - [ ] Search installed apps
    - [ ] Show recent files or apps
    - [ ] Allow repositioning
    - [ ] Other:

37. **Which windows and desktop behaviors matter most?**
    - [ ] Snap/tiling
    - [ ] Multiple workspaces
    - [ ] Minimize, maximize, and resize
    - [ ] Keyboard shortcuts
    - [ ] Touch-friendly controls
    - [ ] Other:
    - Highest priority:

38. **What should the lock screen and sign-in screen show?**
    - [ ] Clock and date
    - [ ] User name/avatar
    - [ ] Network status
    - [ ] Power actions
    - [ ] Accessibility options
    - [ ] Wallpaper choice
    - [ ] Other:
    - Should password-free accounts go directly to the desktop?

39. **Which apps should be installed and visible by default?**
    - Keep:
    - Add:
    - Remove or hide:
    - Apps that should be pinned:
    - [ ] Clearly label features that are not implemented

40. **Where should personal files and downloads go?**
    - Preferred home-folder structure:
    - Default save location:
    - Should external drives open automatically?
    - Other:

## 8. Settings and system behavior

41. **Which Settings categories should ship?**
    - [ ] System
    - [ ] Personalization
    - [ ] Network & Internet
    - [ ] Devices
    - [ ] Apps
    - [ ] Accounts
    - [ ] Privacy & Security
    - [ ] Accessibility
    - [ ] Date & Language
    - [ ] Updates & Recovery
    - [ ] About Nexora OS
    - Other:

42. **How should unsupported settings be presented?**
    - [ ] Hide them until implemented
    - [ ] Show them as unavailable with a clear explanation
    - [ ] Keep them visible as a roadmap
    - [ ] Other:

43. **How should updates work?**
    - [ ] Manual download/install only
    - [ ] Check for updates from Settings
    - [ ] Notify, but require user approval
    - [ ] Automatic updates (only after a safe updater exists)
    - [ ] Offline/manual updates for now
    - Update channel preference:

44. **What recovery and reset actions should Settings provide?**
    - [ ] Restart and shut down
    - [ ] Repair boot
    - [ ] Reset settings
    - [ ] Factory reset (with strong confirmation)
    - [ ] Export diagnostic logs
    - [ ] Other:

45. **What system information should users be able to see?**
    - [ ] Nexora version and build
    - [ ] Processor and memory
    - [ ] Storage devices and free space
    - [ ] Display details
    - [ ] Network interface status
    - [ ] License and notices
    - Other:

## 9. Accessibility, localization, and support

46. **Which accessibility options are important?**
    - [ ] Larger text
    - [ ] High contrast
    - [ ] Reduced motion
    - [ ] Keyboard-only navigation
    - [ ] Screen reader support
    - [ ] Pointer size/speed
    - [ ] Other:

47. **Which languages and keyboard layouts are required first?**
    - Language(s):
    - Keyboard layout(s):
    - Date/time format:
    - [ ] English only for the first release
    - [ ] Other:

48. **How should users get help?**
    - [ ] Local help app/documentation
    - [ ] Setup troubleshooting page
    - [ ] Save logs to a user-accessible file
    - [ ] Website or community link
    - Other:

## 10. Priorities and acceptance criteria

49. **Choose the release priorities. Mark each as Must-have, Should-have, or Later.**
    - Installer appearance:
    - Safe disk selection and erase confirmation:
    - UEFI boot after installation:
    - First-boot account setup:
    - Persistent settings and files:
    - Updates/recovery:
    - Additional hardware support:
    - Other:

50. **What must you be able to do to consider installation successful?**
    - 1.
    - 2.
    - 3.

51. **Which test environments should be used for acceptance?**
    - [ ] QEMU
    - [ ] VirtualBox
    - [ ] A specific physical computer:
    - [ ] Disposable blank virtual disk
    - Required screen resolution(s):

52. **What should never happen?**
    - Examples: erase the wrong disk, claim success after a failed copy, silently skip errors.
    - Your non-negotiable safety or usability requirements:

## 11. Open notes

53. **Reference images, links, or design details**


54. **Anything else the installer or installed OS should do?**


55. **Anything the installer or OS must not do?**


56. **Your preferred next step**
    - [ ] Finalize the answers and update the implementation plan
    - [ ] Show a visual mockup before changing the installer
    - [ ] Fix installer reliability first, then revisit appearance
    - [ ] Other:
