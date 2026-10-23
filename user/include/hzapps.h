/*
 * Nexora OS - user/include/hzapps.h
 * Program manifests (user/lib/hzrt/hzapps.cpp). Each installed program describes itself in a
 * text file C:\Nexora\Apps\<id>.app:
 *
 *     # comment
 *     Name=Notepad
 *     Description=Edit text files
 *     Icon=Document              (an hzui icon name; the shell draws it on a coloured tile)
 *     Color=3A6FD8               (tile colour, RRGGBB)
 *     Path=C:\Nexora\System\notepad.exe
 *     Opens=txt,md,log           (file extensions it opens; the path is its only argument)
 *     Pinned=1                   (shown in Start's pinned grid)
 *     Order=10                   (pinned order, small first)
 *
 * Keys are case-insensitive; Name and Path are required. The shell builds Start from these, and
 * Files and the terminal use them to open documents (hz::apps::OpenPath).
 */
#ifndef HZAPPS_H
#define HZAPPS_H

#include "hzcore.h"

namespace hz::apps {

constexpr const char *kManifestFolder = "C:\\Nexora\\Apps";

struct App {
    String Id;          /* manifest file name without ".app" */
    String Name;
    String Description;
    String Icon;
    uint32_t Color = 0xFF3A6FD8;
    String Path;
    Vector<String> Opens; /* lower case, without dots */
    bool Pinned = false;
    int Order = 1000;
};

/* Parses one manifest. Error (optional) gets a short reason on failure. */
bool ParseManifest(const char *Text, size_t Length, App &Out, String *Error = nullptr);
/* Every valid manifest in Folder, by Order then Name. Invalid ones are logged and skipped. */
Vector<App> Load(const char *Folder = kManifestFolder);
const App *FindForExtension(const Vector<App> &Apps, const String &Extension);
const App *FindByImage(const Vector<App> &Apps, const char *ImageName); /* "notepad.exe" or a full path */
/* Starts App (with Argument quoted as its only argument when not null). */
STATUS Launch(const App &Program, const char *Argument, uint32_t *ProcessId = nullptr);
/* Opens Path: a Nexora-compatible .exe runs, a document opens in the program whose manifest lists
   its extension. The kernel currently supports statically linked PE32+ images without DLL imports.
   Program (optional) gets the program's name. STATUS_NOT_FOUND: nothing opens it. */
STATUS OpenPath(const String &Path, uint32_t *ProcessId = nullptr, String *Program = nullptr);

}  // namespace hz::apps

#endif /* HZAPPS_H */
