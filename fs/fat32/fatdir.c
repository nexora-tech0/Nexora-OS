/*
 * Nexora OS - fs/fat32/fatdir.c
 * Directories: entry access, iteration with long-name (VFAT) assembly, lookup, short-name
 * generation, entry insertion and deletion, FAT timestamps.
 *
 * A directory is read as a byte stream over its cluster chain, 32-byte entries at byte offsets.
 * Names are UTF-8 in the API and UTF-16 in long-name entries. A name that is a valid 8.3 name
 * with each part in a single case is stored as a short entry only (with the NT case bits, as
 * Windows and mtools do); any other name gets long-name entries plus a generated "BASIS~N.EXT".
 */
#include "fatp.h"

static STATUS FatLocate(FAT_VOLUME *Vol, FAT_CURSOR *Dir, uint32_t Offset, uint64_t *Lba, uint32_t *Within)
{
    if (Offset >= FAT_MAX_DIRECTORY_BYTES)
        return STATUS_END_OF_FILE;
    uint32_t Cluster;
    STATUS Status = FatSeek(Vol, Dir, Offset / Vol->BytesPerCluster, &Cluster);
    if (!HZ_SUCCESS(Status))
        return Status;
    uint32_t InCluster = Offset % Vol->BytesPerCluster;
    *Lba = FatClusterToSector(Vol, Cluster) + InCluster / Vol->BytesPerSector;
    *Within = InCluster % Vol->BytesPerSector;
    return STATUS_SUCCESS;
}

STATUS FatReadDirEntry(FAT_VOLUME *Vol, FAT_CURSOR *Dir, uint32_t Offset, void *Entry)
{
    uint64_t Lba;
    uint32_t Within;
    STATUS Status = FatLocate(Vol, Dir, Offset, &Lba, &Within);
    uint8_t *Sector;
    if (HZ_SUCCESS(Status))
        Status = FatReadMeta(Vol, Lba, &Sector);
    if (HZ_SUCCESS(Status))
        memcpy(Entry, Sector + Within, FAT_DIRENT_SIZE);
    return Status;
}

STATUS FatWriteDirEntry(FAT_VOLUME *Vol, FAT_CURSOR *Dir, uint32_t Offset, const void *Entry)
{
    uint64_t Lba;
    uint32_t Within;
    STATUS Status = FatLocate(Vol, Dir, Offset, &Lba, &Within);
    uint8_t *Sector;
    if (HZ_SUCCESS(Status))
        Status = FatReadMeta(Vol, Lba, &Sector);
    if (HZ_SUCCESS(Status)) {
        memcpy(Sector + Within, Entry, FAT_DIRENT_SIZE);
        Status = FatWriteMeta(Vol, Lba);
    }
    return Status;
}

static uint8_t FatChecksum(const uint8_t Name[11])
{
    uint8_t Sum = 0;
    for (int i = 0; i < 11; i++)
        Sum = (uint8_t)(((Sum & 1) << 7) + (Sum >> 1) + Name[i]);
    return Sum;
}

static void FatShortNameToString(const FAT_DIRENT *E, char *Out, bool ApplyCase)
{
    size_t n = 0;
    for (int i = 0; i < 8 && E->Name[i] != ' '; i++) {
        char c = (char)(i == 0 && E->Name[0] == 0x05 ? 0xE5 : E->Name[i]);
        Out[n++] = (ApplyCase && (E->NtCase & FAT_CASE_LOWER_BASE) && c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    if (E->Name[8] != ' ') {
        Out[n++] = '.';
        for (int i = 8; i < 11 && E->Name[i] != ' '; i++) {
            char c = (char)E->Name[i];
            Out[n++] = (ApplyCase && (E->NtCase & FAT_CASE_LOWER_EXT) && c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
    }
    Out[n] = '\0';
}

STATUS FatReadDirectory(FAT_VOLUME *Vol, FAT_CURSOR *Dir, uint32_t *Offset, FAT_DIR_ITEM *Item)
{
    uint16_t Long[20 * 13 + 1];
    int Sequence = 0;      /* next expected long-name sequence number, 0 = none in progress */
    int Total = 0;
    uint8_t Checksum = 0;
    uint32_t LongStart = 0;

    for (;;) {
        uint8_t Raw[FAT_DIRENT_SIZE];
        uint32_t Current = *Offset;
        STATUS Status = FatReadDirEntry(Vol, Dir, Current, Raw);
        if (Status == STATUS_END_OF_FILE)
            return STATUS_NO_MORE_FILES;
        if (!HZ_SUCCESS(Status))
            return Status;
        if (Raw[0] == 0x00)
            return STATUS_NO_MORE_FILES; /* end marker: *Offset stays here */
        *Offset = Current + FAT_DIRENT_SIZE;
        if (Raw[0] == 0xE5) {
            Sequence = 0;
            continue;
        }
        if ((Raw[11] & 0x3F) == FAT_ATTR_LFN) {
            const FAT_LFN_ENTRY *L = (const FAT_LFN_ENTRY *)Raw;
            int Order = L->Order & 0x1F;
            if (L->Order & 0x40) {
                if (Order < 1 || Order > 20) {
                    Sequence = 0;
                    continue;
                }
                Sequence = Order;
                Total = Order;
                Checksum = L->Checksum;
                LongStart = Current;
                for (int i = 0; i < (int)HZ_ARRAYSIZE(Long); i++)
                    Long[i] = 0;
            } else if (Sequence == 0 || Order != Sequence || L->Checksum != Checksum) {
                Sequence = 0;
                continue;
            }
            uint16_t Part[13];
            memcpy(Part, L->Name1, 10);
            memcpy(Part + 5, L->Name2, 12);
            memcpy(Part + 11, L->Name3, 4);
            memcpy(Long + (Order - 1) * 13, Part, sizeof(Part));
            Sequence = Order - 1;
            if (Sequence == 0)
                Sequence = -1; /* complete: the short entry must follow */
            continue;
        }
        const FAT_DIRENT *E = (const FAT_DIRENT *)Raw;
        if ((E->Attributes & FAT_ATTR_VOLUME) || E->Name[0] == '.') {
            Sequence = 0; /* volume label, "." and ".." are not listed */
            continue;
        }
        memcpy(&Item->Entry, E, sizeof(*E));
        Item->EntryOffset = Current;
        FatShortNameToString(E, Item->ShortName, false);
        bool HaveLong = false;
        if (Sequence == -1 && FatChecksum(E->Name) == Checksum) {
            int Units = 0;
            while (Units < Total * 13 && Long[Units] != 0x0000 && Long[Units] != 0xFFFF)
                Units++;
            HaveLong = Units > 0 && RtlUtf16ToUtf8(Long, (size_t)Units, Item->Name, sizeof(Item->Name)) > 0;
        }
        if (HaveLong) {
            Item->FirstOffset = LongStart;
        } else {
            FatShortNameToString(E, Item->Name, true);
            Item->FirstOffset = Current;
        }
        return STATUS_SUCCESS;
    }
}

STATUS FatLookup(FAT_VOLUME *Vol, uint32_t DirCluster, const char *Name, FAT_DIR_ITEM *Item)
{
    FAT_CURSOR Dir = {DirCluster, 0, 0};
    uint32_t Offset = 0;
    for (;;) {
        STATUS Status = FatReadDirectory(Vol, &Dir, &Offset, Item);
        if (Status == STATUS_NO_MORE_FILES)
            return STATUS_OBJECT_NAME_NOT_FOUND;
        if (!HZ_SUCCESS(Status))
            return Status;
        if (_stricmp(Item->Name, Name) == 0 || _stricmp(Item->ShortName, Name) == 0)
            return STATUS_SUCCESS;
    }
}

bool FatIsValidLongName(const char *Name)
{
    size_t Length = strlen(Name);
    if (Length == 0 || Length > FAT_MAX_NAME || !strcmp(Name, ".") || !strcmp(Name, ".."))
        return false;
    for (const char *p = Name; *p; p++) {
        uint8_t c = (uint8_t)*p;
        if (c < 0x20 || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' || c == '?' ||
            c == '\\' || c == '|' || c == 0x7F)
            return false;
    }
    /* Windows strips trailing dots and spaces; refuse them instead of silently renaming. */
    return Name[Length - 1] != '.' && Name[Length - 1] != ' ';
}

static bool FatIsShortChar(char c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return true;
    for (const char *p = "!#$%&'()-@^_`{}~"; *p; p++) {
        if (*p == c)
            return true;
    }
    return false;
}

static char FatUpper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

/* Exact 8.3 form with one case per part; fills the 11-byte name and the NT case bits. */
static bool FatTryExact83(const char *Name, uint8_t Out[11], uint8_t *Case)
{
    const char *Dot = NULL;
    for (const char *p = Name; *p; p++) {
        if (*p == '.') {
            if (Dot)
                return false;
            Dot = p;
        }
    }
    size_t BaseLength = Dot ? (size_t)(Dot - Name) : strlen(Name);
    size_t ExtLength = Dot ? strlen(Dot + 1) : 0;
    if (BaseLength == 0 || BaseLength > 8 || ExtLength > 3 || (Dot && ExtLength == 0))
        return false;
    *Case = 0;
    for (int Part = 0; Part < 2; Part++) {
        const char *s = Part ? Dot + 1 : Name;
        size_t n = Part ? ExtLength : BaseLength;
        bool Lower = false, Upper = false;
        for (size_t i = 0; i < n; i++) {
            char c = s[i];
            Lower |= c >= 'a' && c <= 'z';
            Upper |= c >= 'A' && c <= 'Z';
            if (!FatIsShortChar(FatUpper(c)))
                return false;
        }
        if (Lower && Upper)
            return false;
        if (Lower)
            *Case |= Part ? FAT_CASE_LOWER_EXT : FAT_CASE_LOWER_BASE;
    }
    memset(Out, ' ', 11);
    for (size_t i = 0; i < BaseLength; i++)
        Out[i] = (uint8_t)FatUpper(Name[i]);
    for (size_t i = 0; i < ExtLength; i++)
        Out[8 + i] = (uint8_t)FatUpper(Dot[1 + i]);
    if (Out[0] == 0xE5)
        Out[0] = 0x05;
    return true;
}

static STATUS FatShortNameExists(FAT_VOLUME *Vol, uint32_t DirCluster, const uint8_t Name[11], bool *Exists)
{
    FAT_CURSOR Dir = {DirCluster, 0, 0};
    for (uint32_t Offset = 0;; Offset += FAT_DIRENT_SIZE) {
        uint8_t Raw[FAT_DIRENT_SIZE];
        STATUS Status = FatReadDirEntry(Vol, &Dir, Offset, Raw);
        if (Status == STATUS_END_OF_FILE || (HZ_SUCCESS(Status) && Raw[0] == 0x00))
            break;
        if (!HZ_SUCCESS(Status))
            return Status;
        if (Raw[0] != 0xE5 && (Raw[11] & 0x3F) != FAT_ATTR_LFN && !memcmp(Raw, Name, 11)) {
            *Exists = true;
            return STATUS_SUCCESS;
        }
    }
    *Exists = false;
    return STATUS_SUCCESS;
}

/* "Long File Name.txt" -> "LONGFI~1TXT" (unique in the directory). */
static STATUS FatGenerateShortName(FAT_VOLUME *Vol, uint32_t DirCluster, const char *Name, uint8_t Out[11])
{
    const char *Dot = NULL;
    for (const char *p = Name + 1; *p; p++) { /* a leading dot does not start an extension */
        if (*p == '.')
            Dot = p;
    }
    char Base[9] = "", Ext[4] = "";
    size_t b = 0, e = 0;
    for (const char *p = Name; *p && p != Dot && b < 8; p++) {
        if (*p == ' ' || *p == '.')
            continue;
        char c = FatUpper(*p);
        Base[b++] = FatIsShortChar(c) ? c : '_';
    }
    for (const char *p = Dot ? Dot + 1 : ""; *p && e < 3; p++) {
        if (*p == ' ' || *p == '.')
            continue;
        char c = FatUpper(*p);
        Ext[e++] = FatIsShortChar(c) ? c : '_';
    }
    if (b == 0)
        Base[b++] = '_';

    for (uint32_t n = 1; n < 1000000; n++) {
        char Tail[8];
        int TailLength = RtlFormatString(Tail, sizeof(Tail), "~%u", n);
        size_t Keep = HZ_MIN(b, (size_t)(8 - TailLength));
        memset(Out, ' ', 11);
        memcpy(Out, Base, Keep);
        memcpy(Out + Keep, Tail, (size_t)TailLength);
        memcpy(Out + 8, Ext, e);
        bool Exists;
        STATUS Status = FatShortNameExists(Vol, DirCluster, Out, &Exists);
        if (!HZ_SUCCESS(Status))
            return Status;
        if (!Exists)
            return STATUS_SUCCESS;
    }
    return STATUS_OBJECT_NAME_COLLISION;
}

STATUS FatInsertEntries(FAT_VOLUME *Vol, uint32_t DirCluster, const char *Name, FAT_DIRENT *Short,
                        uint32_t *FirstOffset, uint32_t *EntryOffset)
{
    uint16_t Units[FAT_MAX_NAME + 1];
    int UnitCount = RtlUtf8ToUtf16(Name, Units, FAT_MAX_NAME);
    if (UnitCount <= 0)
        return STATUS_OBJECT_NAME_INVALID;
    uint8_t Case = 0;
    bool NeedLong = !FatTryExact83(Name, Short->Name, &Case);
    STATUS Status = STATUS_SUCCESS;
    if (NeedLong) {
        Status = FatGenerateShortName(Vol, DirCluster, Name, Short->Name);
        Case = 0;
    }
    if (!HZ_SUCCESS(Status))
        return Status;
    Short->NtCase = Case;
    uint32_t Count = NeedLong ? (uint32_t)(UnitCount + 12) / 13 + 1 : 1;

    /* Find Count consecutive free entries, growing the directory when it is full. */
    FAT_CURSOR Dir = {DirCluster, 0, 0};
    uint32_t Offset = 0, RunStart = 0, Run = 0;
    for (;;) {
        uint8_t Raw[FAT_DIRENT_SIZE];
        Status = FatReadDirEntry(Vol, &Dir, Offset, Raw);
        if (Status == STATUS_END_OF_FILE) {
            if (Offset >= FAT_MAX_DIRECTORY_BYTES)
                return STATUS_DISK_FULL;
            uint32_t Last, New;
            Status = FatSeek(Vol, &Dir, Offset / Vol->BytesPerCluster - 1, &Last);
            if (HZ_SUCCESS(Status))
                Status = FatAllocateCluster(Vol, Last, true, &New);
            if (!HZ_SUCCESS(Status))
                return Status;
            continue; /* re-read the same offset in the new cluster */
        }
        if (!HZ_SUCCESS(Status))
            return Status;
        if (Raw[0] == 0x00 || Raw[0] == 0xE5) {
            if (Run == 0)
                RunStart = Offset;
            if (++Run == Count)
                break;
        } else {
            Run = 0;
        }
        Offset += FAT_DIRENT_SIZE;
    }

    uint8_t Sum = FatChecksum(Short->Name);
    for (uint32_t i = 0; i + 1 < Count; i++) {
        uint32_t Seq = Count - 1 - i;
        FAT_LFN_ENTRY L;
        memset(&L, 0, sizeof(L));
        L.Order = (uint8_t)(Seq | (i == 0 ? 0x40 : 0));
        L.Attributes = FAT_ATTR_LFN;
        L.Checksum = Sum;
        uint16_t Part[13];
        for (uint32_t k = 0; k < 13; k++) {
            uint32_t Index = (Seq - 1) * 13 + k;
            Part[k] = Index < (uint32_t)UnitCount ? Units[Index] : Index == (uint32_t)UnitCount ? 0x0000 : 0xFFFF;
        }
        memcpy(L.Name1, Part, 10);
        memcpy(L.Name2, Part + 5, 12);
        memcpy(L.Name3, Part + 11, 4);
        Status = FatWriteDirEntry(Vol, &Dir, RunStart + i * FAT_DIRENT_SIZE, &L);
        if (!HZ_SUCCESS(Status))
            return Status;
    }
    uint32_t ShortOffset = RunStart + (Count - 1) * FAT_DIRENT_SIZE;
    Status = FatWriteDirEntry(Vol, &Dir, ShortOffset, Short);
    if (!HZ_SUCCESS(Status))
        return Status;
    *FirstOffset = RunStart;
    *EntryOffset = ShortOffset;
    return STATUS_SUCCESS;
}

STATUS FatMarkDeleted(FAT_VOLUME *Vol, uint32_t DirCluster, uint32_t FirstOffset, uint32_t EntryOffset)
{
    FAT_CURSOR Dir = {DirCluster, 0, 0};
    for (uint32_t Offset = FirstOffset; Offset <= EntryOffset; Offset += FAT_DIRENT_SIZE) {
        uint8_t Raw[FAT_DIRENT_SIZE];
        STATUS Status = FatReadDirEntry(Vol, &Dir, Offset, Raw);
        if (!HZ_SUCCESS(Status))
            return Status;
        Raw[0] = 0xE5;
        Status = FatWriteDirEntry(Vol, &Dir, Offset, Raw);
        if (!HZ_SUCCESS(Status))
            return Status;
    }
    return STATUS_SUCCESS;
}

STATUS FatIsDirectoryEmpty(FAT_VOLUME *Vol, uint32_t DirCluster, bool *Empty)
{
    FAT_CURSOR Dir = {DirCluster, 0, 0};
    uint32_t Offset = 0;
    FAT_DIR_ITEM *Item = ExAllocatePoolWithTag(NonPagedPool, sizeof(FAT_DIR_ITEM), FAT_TAG);
    if (!Item)
        return STATUS_NO_MEMORY;
    STATUS Status = FatReadDirectory(Vol, &Dir, &Offset, Item);
    ExFreePoolWithTag(Item, FAT_TAG);
    if (Status == STATUS_NO_MORE_FILES) {
        *Empty = true;
        return STATUS_SUCCESS;
    }
    *Empty = false;
    return HZ_SUCCESS(Status) ? STATUS_SUCCESS : Status;
}

/* ---- Timestamps -------------------------------------------------------------------------------- */

void FatEncodeTime(uint64_t Time, uint16_t *Date, uint16_t *Clock, uint8_t *Tenths)
{
    TIME_FIELDS F;
    if (Time)
        RtlTimeToTimeFields(Time, &F);
    if (!Time || F.Year < 1980) {
        F.Year = 1980;
        F.Month = 1;
        F.Day = 1;
        F.Hour = F.Minute = F.Second = 0;
        Time = 0;
    }
    if (F.Year > 2107)
        F.Year = 2107;
    *Date = (uint16_t)(((F.Year - 1980) << 9) | (F.Month << 5) | F.Day);
    *Clock = (uint16_t)((F.Hour << 11) | (F.Minute << 5) | (F.Second / 2));
    if (Tenths)
        *Tenths = (uint8_t)((F.Second & 1) * 100 + (Time % HZ_TIME_UNITS_PER_SECOND) / 100000);
}

uint64_t FatDecodeTime(uint16_t Date, uint16_t Clock, uint8_t Tenths)
{
    if (Date == 0)
        return 0;
    TIME_FIELDS F;
    F.Year = (uint16_t)(1980 + (Date >> 9));
    F.Month = (uint8_t)((Date >> 5) & 0xF);
    F.Day = (uint8_t)(Date & 0x1F);
    F.Hour = (uint8_t)(Clock >> 11);
    F.Minute = (uint8_t)((Clock >> 5) & 0x3F);
    F.Second = (uint8_t)((Clock & 0x1F) * 2);
    F.Weekday = 0;
    if (F.Month < 1 || F.Month > 12 || F.Day < 1)
        return 0;
    return RtlTimeFieldsToTime(&F) + (uint64_t)(Tenths <= 199 ? Tenths : 0) * 100000;
}
