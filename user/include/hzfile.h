/*
 * Nexora OS - user/include/hzfile.h
 * File helpers for programs (user/lib/hzrt/hzfile.cpp): paths, directory listings, whole-file
 * text I/O, volumes, and formatting of sizes and times. Paths are DOS paths ("D:\Notes\a.txt");
 * names compare case-insensitively (ASCII), as the file systems do.
 */
#ifndef HZFILE_H
#define HZFILE_H

#include "hzcore.h"

namespace hz::fs {

struct Entry {
    String Name;
    bool Directory = false;
    uint64_t Size = 0;
    uint64_t Modified = 0;   /* HZ time */
    uint32_t Attributes = 0; /* FILE_ATTRIBUTE_* */
};

struct Volume {
    String Root;      /* "C:\" */
    String Label;     /* volume label, may be empty */
    String FileSystem;
    bool ReadOnly = false;
    uint64_t TotalBytes = 0, FreeBytes = 0;
};

/* ---- Paths ---------------------------------------------------------------------------------- */

bool IsRoot(const String &Path);                     /* "C:\" */
String Join(const String &Directory, const char *Name);
String Parent(const String &Path);                   /* the root's parent is the root */
String FileName(const String &Path);                 /* last component */
String Extension(const String &Path);                /* lower case, without the dot; "" if none */
/* Makes Path absolute against Current: "x", "..\x", "\x", "D:" and "D:\x" forms; folds "." and "..". */
String Resolve(const String &Current, const char *Path);
bool EqualNames(const char *A, const char *B);       /* case-insensitive (ASCII) */
/* Wildcards: '*' any run of characters, '?' one character; case-insensitive. */
bool MatchPattern(const char *Pattern, const char *Name);
bool HasWildcards(const char *Text);

/* ---- Files and directories ------------------------------------------------------------------ */

/* Folders first, then files, each by name (case-insensitive). "." and ".." are left out. */
STATUS List(const String &Directory, Vector<Entry> &Out);
STATUS Stat(const String &Path, Entry &Out);
bool Exists(const String &Path, bool *Directory = nullptr);
STATUS ReadAll(const String &Path, String &Out, uint64_t Maximum = 16 * 1024 * 1024);
STATUS WriteAll(const String &Path, const char *Data, size_t Length); /* creates or replaces */
STATUS CreateDirectory(const String &Path);
STATUS Delete(const String &Path);                   /* a file, or an empty directory */
STATUS Rename(const String &Path, const char *NewName);
STATUS Copy(const String &From, const String &To);   /* files only (any size); replaces To */
/* M11: trees. CopyTree copies a file or a folder with everything in it; To must not exist.
   MoveTree renames on the same volume (any folder), else copies and deletes. DeleteTree deletes a
   file, or a folder with everything in it. A folder is never copied or moved into itself. */
STATUS CopyTree(const String &From, const String &To);
STATUS MoveTree(const String &From, const String &To);
STATUS DeleteTree(const String &Path);
bool IsInside(const String &Path, const String &Folder); /* Path is below Folder (not equal) */

/* ---- Volumes ------------------------------------------------------------------------------------ */

Vector<Volume> Volumes();                             /* C: to Z: that exist */
STATUS QueryVolume(const String &Path, Volume &Out);

/* ---- Settings files (M10) --------------------------------------------------------------------- */

/* "Key=Value" lines ('#' or ';' starts a comment; keys compare without case; values are trimmed).
   Saving writes the items in their order with a header comment. */
class Settings {
public:
    struct Item {
        String Key, Value;
    };
    static bool Parse(const char *Text, size_t Length, Settings &Out);
    STATUS Load(const String &Path);
    STATUS Save(const String &Path) const;
    String Text() const;
    bool Has(const char *Key) const;
    String Get(const char *Key, const char *Default = "") const;
    int GetInt(const char *Key, int Default = 0) const;
    void Set(const char *Key, const char *Value);
    void SetInt(const char *Key, int Value);
    const Vector<Item> &Items() const { return Items_; }

private:
    Vector<Item> Items_;
};

/* C:\Nexora\Settings\<Name>.ini */
String SettingsPath(const char *Name);

/* ---- Trash (recycle bin) --------------------------------------------------------------------- */

/* Deleted items are moved into TrashFolder() under a number, and an index remembers where each one
   came from, so it can be put back and so the desktop can tell whether the bin has anything in it. */
String TrashFolder(); /* "C:\Nexora\Trash" (the folder is created when something is deleted) */

struct TrashItem {
    String Name;        /* the stored name (a number), for Restore/Delete/Purge */
    String OriginalName;/* the name the item had before it was deleted */
    String OriginalPath;/* the full path it is restored to */
    uint64_t DeletedAt; /* HZ time */
};

/* Moves Path (a file or a folder with everything in it) into the trash. */
STATUS Trash(const String &Path);
/* The trash's content, most recently deleted first. */
STATUS TrashList(Vector<TrashItem> &Out);
/* Puts an item back where it came from; a name already taken there gets a " (2)" suffix. */
STATUS TrashRestore(const String &Name, String *RestoredPath = nullptr);
/* Deletes one item for good. */
STATUS TrashDelete(const String &Name);
/* Deletes everything in the trash for good. */
STATUS TrashEmpty();
/* True when the trash holds at least one item. */
bool TrashHasItems();
/* True when C: is the read-only live image (nothing can be saved there). */
bool IsLiveSystem();
enum class KnownFolder : uint8_t { Desktop, Documents, Downloads, Music, Pictures, Videos, Projects, Count };
const char *KnownFolderName(KnownFolder Folder);
const char *KnownFolderLabel(KnownFolder Folder);
String UserProfilePath(const char *UserName);
String KnownFolderPath(const char *UserName, KnownFolder Folder);
String CurrentUserProfilePath();
String CurrentKnownFolderPath(KnownFolder Folder);
/* The user's documents folder (C:\Users\<name>\Documents on an installed system after first-boot
   setup), else the first writable volume's root, else C:\. */
String DocumentsFolder();
/* Letters, digits, spaces (not first or last), '-', '_' and '.'; 1 to 32 characters. */
bool IsValidUserName(const char *Name);

/* ---- Local time (M11) ------------------------------------------------------------------------- */

/* The clock keeps UTC; shell.ini's "UtcOffsetMinutes" (-720 .. +840) is the time zone the clocks
   show (no daylight saving rules). File times stay in UTC. */
struct CivilTime {
    long long Year;
    unsigned Month, Day; /* 1-based */
    unsigned Hour, Minute, Second;
    unsigned Weekday;    /* 0 = Sunday */
};
constexpr int kMinUtcOffset = -12 * 60, kMaxUtcOffset = 14 * 60;
extern const char *const MonthNames[12];
extern const char *const WeekdayNames[7]; /* Sunday first */
CivilTime ToCivil(uint64_t HzTime, int OffsetMinutes = 0);
int ClampUtcOffset(int Minutes);
int UtcOffsetMinutes();                /* read from shell.ini; 0 if unset */
String FormatUtcOffset(int Minutes);   /* "UTC", "UTC+05:30", "UTC-03:00" */

/* ---- Formatting ------------------------------------------------------------------------------- */

String FormatSize(uint64_t Bytes);                   /* "512 bytes", "1.5 KB", "12 MB" */
String FormatTime(uint64_t HzTime);                  /* "2026-10-07 14:05" (UTC) */
String FormatTime(uint64_t HzTime, int OffsetMinutes); /* local time; no daylight saving rules */
const char *StatusText(STATUS Status);               /* short human description */

}  // namespace hz::fs

#endif /* HZFILE_H */
