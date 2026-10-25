/*
 * Nexora OS - user/lib/hzrt/hzfile.cpp
 * File helpers for programs (user/include/hzfile.h).
 */
#include "hzfile.h"

namespace hz::fs {

namespace {

char Lower(char C)
{
    return (C >= 'A' && C <= 'Z') ? (char)(C + 32) : C;
}

int CompareNames(const char *A, const char *B)
{
    for (;; A++, B++) {
        char X = Lower(*A), Y = Lower(*B);
        if (X != Y || !X)
            return (int)(uint8_t)X - (int)(uint8_t)Y;
    }
}

/* Opens Path for the given access; a directory is opened as one when the plain open says so. */
STATUS OpenAny(HANDLE *Handle, const String &Path, ACCESS_MASK Access)
{
    STATUS Status = NtCreateFile(Handle, Access, Path.CStr(), FILE_OPEN, 0, nullptr);
    if (Status == STATUS_FILE_IS_A_DIRECTORY)
        Status = NtCreateFile(Handle, Access, Path.CStr(), FILE_OPEN, FILE_DIRECTORY_FILE, nullptr);
    return Status;
}

}  // namespace

bool EqualNames(const char *A, const char *B)
{
    return CompareNames(A, B) == 0;
}

bool MatchPattern(const char *Pattern, const char *Name)
{
    const char *Star = nullptr, *Resume = nullptr;
    while (*Name) {
        if (*Pattern == '*') {
            Star = Pattern++;
            Resume = Name;
        } else if (*Pattern == '?' || (*Pattern && Lower(*Pattern) == Lower(*Name))) {
            Pattern++;
            Name++;
        } else if (Star) {
            Pattern = Star + 1;
            Name = ++Resume;
        } else {
            return false;
        }
    }
    while (*Pattern == '*')
        Pattern++;
    return *Pattern == 0;
}

bool HasWildcards(const char *Text)
{
    for (; *Text; Text++)
        if (*Text == '*' || *Text == '?')
            return true;
    return false;
}

/* ---- Paths -------------------------------------------------------------------------------------- */

bool IsRoot(const String &Path)
{
    return Path.Length() == 3 && Path[1] == ':' && Path[2] == '\\';
}

String Join(const String &Directory, const char *Name)
{
    String Out = Directory;
    if (Out.Length() && Out[Out.Length() - 1] != '\\')
        Out.Append('\\');
    Out.Append(Name);
    return Out;
}

String Parent(const String &Path)
{
    if (Path.Length() <= 3)
        return Path.Substring(0, 3);
    size_t End = Path.Length();
    if (Path[End - 1] == '\\')
        End--;
    while (End > 3 && Path[End - 1] != '\\')
        End--;
    if (End > 3)
        End--; /* the separator */
    return Path.Substring(0, End);
}

String FileName(const String &Path)
{
    size_t End = Path.Length();
    if (End && Path[End - 1] == '\\')
        End--;
    size_t Start = End;
    while (Start > 0 && Path[Start - 1] != '\\')
        Start--;
    return Path.Substring(Start, End - Start);
}

String Extension(const String &Path)
{
    String Name = FileName(Path);
    size_t Dot = NotFound;
    for (size_t i = 0; i < Name.Length(); i++)
        if (Name[i] == '.')
            Dot = i;
    if (Dot == NotFound || Dot == 0)
        return String();
    String Out;
    for (size_t i = Dot + 1; i < Name.Length(); i++)
        Out.Append(Lower(Name[i]));
    return Out;
}

String Resolve(const String &Current, const char *Path)
{
    String Base;
    const char *Rest = Path;
    if (Path[0] && Path[1] == ':') { /* drive given */
        Base.Append((char)(Path[0] >= 'a' && Path[0] <= 'z' ? Path[0] - 32 : Path[0]));
        Base.Append(":\\");
        Rest = Path + 2;
        if (*Rest == '\\')
            Rest++;
        else if (Current.Length() >= 3 && Lower(Current[0]) == Lower(Path[0]))
            Base = Current; /* "D:x" is relative to the current directory on D: */
    } else if (Path[0] == '\\') {
        Base = Current.Substring(0, 3);
        Rest = Path + 1;
    } else {
        Base = Current;
    }
    /* Fold the components. */
    Vector<String> Parts;
    String Root = Base.Substring(0, 3);
    for (size_t i = 3, Start = 3; i <= Base.Length(); i++) {
        if (i == Base.Length() || Base[i] == '\\') {
            if (i > Start)
                Parts.Push(Base.Substring(Start, i - Start));
            Start = i + 1;
        }
    }
    const char *P = Rest;
    while (*P) {
        const char *End = P;
        while (*End && *End != '\\' && *End != '/')
            End++;
        String Part(P, (size_t)(End - P));
        if (Part == "..") {
            if (!Parts.Empty())
                Parts.Pop();
        } else if (!Part.Empty() && Part != ".") {
            Parts.Push(Part);
        }
        P = *End ? End + 1 : End;
    }
    String Out = Root;
    for (size_t i = 0; i < Parts.Size(); i++) {
        if (i)
            Out.Append('\\');
        Out.Append(Parts[i]);
    }
    return Out;
}

/* ---- Files and directories ------------------------------------------------------------------- */

STATUS List(const String &Directory, Vector<Entry> &Out)
{
    Out.Clear();
    HANDLE Dir;
    STATUS Status = NtCreateFile(&Dir, FILE_LIST_DIRECTORY, Directory.CStr(), FILE_OPEN, FILE_DIRECTORY_FILE, nullptr);
    if (!HZ_SUCCESS(Status))
        return Status;
    static HZ_FILE_DIRECTORY_INFORMATION Batch[16];
    uint32_t Flags = HZ_DIRECTORY_RESTART_SCAN, Returned = 0;
    while (HZ_SUCCESS(Status = NtQueryDirectoryFile(Dir, Batch, sizeof(Batch), Flags, &Returned)) &&
           Status != STATUS_NO_MORE_FILES) {
        Flags = 0;
        for (uint32_t i = 0; i < Returned; i++) {
            const HZ_FILE_DIRECTORY_INFORMATION &I = Batch[i];
            if (strcmp(I.Name, ".") == 0 || strcmp(I.Name, "..") == 0)
                continue;
            Entry E;
            E.Name = I.Name;
            E.Directory = (I.Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            E.Size = I.EndOfFile;
            E.Modified = I.LastWriteTime;
            E.Attributes = I.Attributes;
            Out.Push(Move(E));
        }
        if (Returned == 0)
            break;
    }
    NtClose(Dir);
    /* Insertion sort: folders first, then by name. Directories are small. */
    for (size_t i = 1; i < Out.Size(); i++) {
        for (size_t j = i; j > 0; j--) {
            Entry &A = Out[j - 1], &B = Out[j];
            bool Swap = A.Directory != B.Directory ? B.Directory : CompareNames(A.Name.CStr(), B.Name.CStr()) > 0;
            if (!Swap)
                break;
            hz::Swap(A, B);
        }
    }
    return Status == STATUS_NO_MORE_FILES || HZ_SUCCESS(Status) ? STATUS_SUCCESS : Status;
}

STATUS Stat(const String &Path, Entry &Out)
{
    HANDLE File;
    STATUS Status = OpenAny(&File, Path, FILE_READ_ATTRIBUTES);
    if (!HZ_SUCCESS(Status))
        return Status;
    HZ_FILE_BASIC_INFORMATION Basic = {};
    Status = NtQueryInformationFile(File, HZ_FILE_BASIC_INFORMATION_CLASS, &Basic, sizeof(Basic));
    NtClose(File);
    if (HZ_SUCCESS(Status)) {
        Out.Name = FileName(Path);
        Out.Directory = (Basic.Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 || IsRoot(Path);
        Out.Size = Basic.EndOfFile;
        Out.Modified = Basic.LastWriteTime;
        Out.Attributes = Basic.Attributes;
    }
    return Status;
}

bool Exists(const String &Path, bool *Directory)
{
    Entry E;
    if (!HZ_SUCCESS(Stat(Path, E)))
        return false;
    if (Directory)
        *Directory = E.Directory;
    return true;
}

STATUS ReadAll(const String &Path, String &Out, uint64_t Maximum)
{
    HANDLE File;
    STATUS Status = NtCreateFile(&File, GENERIC_READ, Path.CStr(), FILE_OPEN, FILE_NON_DIRECTORY_FILE, nullptr);
    if (!HZ_SUCCESS(Status))
        return Status;
    HZ_FILE_BASIC_INFORMATION Basic = {};
    Status = NtQueryInformationFile(File, HZ_FILE_BASIC_INFORMATION_CLASS, &Basic, sizeof(Basic));
    if (HZ_SUCCESS(Status) && Basic.EndOfFile > Maximum)
        Status = STATUS_FILE_TOO_LARGE;
    Out.Clear();
    static char Chunk[8192];
    while (HZ_SUCCESS(Status)) {
        uint32_t Got = 0;
        Status = NtReadFile(File, Chunk, sizeof(Chunk), nullptr, &Got);
        if (Status == STATUS_END_OF_FILE || (HZ_SUCCESS(Status) && Got == 0)) {
            Status = STATUS_SUCCESS;
            break;
        }
        if (HZ_SUCCESS(Status))
            Out.Append(Chunk, Got);
    }
    NtClose(File);
    return Status;
}

STATUS WriteAll(const String &Path, const char *Data, size_t Length)
{
    HANDLE File;
    STATUS Status = NtCreateFile(&File, GENERIC_WRITE, Path.CStr(), FILE_OVERWRITE_IF, FILE_NON_DIRECTORY_FILE, nullptr);
    if (!HZ_SUCCESS(Status))
        return Status;
    size_t Done = 0;
    while (HZ_SUCCESS(Status) && Done < Length) {
        uint32_t Chunk = (uint32_t)Min<size_t>(Length - Done, 64 * 1024), Written = 0;
        Status = NtWriteFile(File, Data + Done, Chunk, nullptr, &Written);
        Done += Written;
        if (HZ_SUCCESS(Status) && Written == 0)
            Status = STATUS_DISK_FULL;
    }
    NtClose(File);
    return Status;
}

STATUS CreateDirectory(const String &Path)
{
    HANDLE Dir;
    STATUS Status = NtCreateFile(&Dir, FILE_LIST_DIRECTORY, Path.CStr(), FILE_CREATE, FILE_DIRECTORY_FILE, nullptr);
    if (HZ_SUCCESS(Status))
        NtClose(Dir);
    return Status;
}

STATUS Delete(const String &Path)
{
    HANDLE File;
    STATUS Status = OpenAny(&File, Path, DELETE);
    if (!HZ_SUCCESS(Status))
        return Status;
    HZ_FILE_DISPOSITION_INFORMATION Disposition = {1, 0};
    Status = NtSetInformationFile(File, HZ_FILE_DISPOSITION_INFORMATION_CLASS, &Disposition, sizeof(Disposition));
    NtClose(File);
    return Status;
}

STATUS Rename(const String &Path, const char *NewName)
{
    HANDLE File;
    STATUS Status = OpenAny(&File, Path, DELETE | FILE_WRITE_ATTRIBUTES);
    if (!HZ_SUCCESS(Status))
        return Status;
    HZ_FILE_RENAME_INFORMATION Info = {};
    Info.ReplaceIfExists = 0;
    size_t Length = Min<size_t>(strlen(NewName), sizeof(Info.NewName) - 1);
    memcpy(Info.NewName, NewName, Length);
    Status = NtSetInformationFile(File, HZ_FILE_RENAME_INFORMATION_CLASS, &Info, sizeof(Info));
    NtClose(File);
    return Status;
}

STATUS Copy(const String &From, const String &To)
{
    if (EqualNames(From.CStr(), To.CStr()))
        return STATUS_OBJECT_NAME_COLLISION; /* opening To would truncate From */
    HANDLE In, Out;
    STATUS Status = NtCreateFile(&In, GENERIC_READ, From.CStr(), FILE_OPEN, FILE_NON_DIRECTORY_FILE, nullptr);
    if (!HZ_SUCCESS(Status))
        return Status;
    Status = NtCreateFile(&Out, GENERIC_WRITE, To.CStr(), FILE_OVERWRITE_IF, FILE_NON_DIRECTORY_FILE, nullptr);
    if (!HZ_SUCCESS(Status)) {
        NtClose(In);
        return Status;
    }
    /* Streams in large pieces: FAT32 turns them into multi-cluster transfers. */
    constexpr uint32_t kChunk = 256 * 1024;
    char *Buffer = new char[kChunk];
    while (HZ_SUCCESS(Status)) {
        uint32_t Got = 0;
        Status = NtReadFile(In, Buffer, kChunk, nullptr, &Got);
        if (Status == STATUS_END_OF_FILE || (HZ_SUCCESS(Status) && Got == 0)) {
            Status = STATUS_SUCCESS;
            break;
        }
        uint32_t Done = 0;
        while (HZ_SUCCESS(Status) && Done < Got) {
            uint32_t Written = 0;
            Status = NtWriteFile(Out, Buffer + Done, Got - Done, nullptr, &Written);
            Done += Written;
            if (HZ_SUCCESS(Status) && Written == 0)
                Status = STATUS_DISK_FULL;
        }
    }
    delete[] Buffer;
    NtClose(Out);
    NtClose(In);
    if (!HZ_SUCCESS(Status))
        Delete(To); /* no partial copies */
    return Status;
}

bool IsInside(const String &Path, const String &Folder)
{
    size_t Length = Folder.Length();
    if (Length && Folder[Length - 1] == '\\')
        Length--; /* "C:\" */
    return Path.Length() > Length + 1 && Path[Length] == '\\' &&
           EqualNames(Path.Substring(0, Length).CStr(), Folder.Substring(0, Length).CStr());
}

STATUS CopyTree(const String &From, const String &To)
{
    Entry Info;
    STATUS Status = Stat(From, Info);
    if (!HZ_SUCCESS(Status))
        return Status;
    if (Exists(To))
        return STATUS_OBJECT_NAME_COLLISION;
    if (!Info.Directory)
        return Copy(From, To);
    if (IsInside(To, From) || EqualNames(To.CStr(), From.CStr()))
        return STATUS_INVALID_PARAMETER;
    Vector<Entry> Children;
    Status = List(From, Children);
    if (HZ_SUCCESS(Status))
        Status = CreateDirectory(To);
    for (size_t i = 0; HZ_SUCCESS(Status) && i < Children.Size(); i++)
        Status = CopyTree(Join(From, Children[i].Name.CStr()), Join(To, Children[i].Name.CStr()));
    return Status;
}

STATUS DeleteTree(const String &Path)
{
    Entry Info;
    STATUS Status = Stat(Path, Info);
    if (!HZ_SUCCESS(Status))
        return Status;
    if (Info.Directory) {
        Vector<Entry> Children;
        Status = List(Path, Children);
        for (size_t i = 0; HZ_SUCCESS(Status) && i < Children.Size(); i++)
            Status = DeleteTree(Join(Path, Children[i].Name.CStr()));
        if (!HZ_SUCCESS(Status))
            return Status;
    }
    return Delete(Path);
}

STATUS MoveTree(const String &From, const String &To)
{
    if (IsInside(To, From) || EqualNames(To.CStr(), From.CStr()))
        return STATUS_INVALID_PARAMETER;
    if (Exists(To))
        return STATUS_OBJECT_NAME_COLLISION;
    if (From.Length() > 3 && To.Length() > 3 && EqualNames(From.Substring(0, 2).CStr(), To.Substring(0, 2).CStr()))
        return Rename(From, To.CStr() + 2); /* the file system takes a path from the volume root */
    STATUS Status = CopyTree(From, To);
    if (HZ_SUCCESS(Status))
        Status = DeleteTree(From);
    return Status;
}

/* ---- Volumes ------------------------------------------------------------------------------------- */

STATUS QueryVolume(const String &Path, Volume &Out)
{
    HANDLE Root;
    String RootPath = Path.Substring(0, 3);
    STATUS Status = NtCreateFile(&Root, FILE_LIST_DIRECTORY, RootPath.CStr(), FILE_OPEN, FILE_DIRECTORY_FILE, nullptr);
    if (!HZ_SUCCESS(Status))
        return Status;
    HZ_FILE_VOLUME_INFORMATION Info = {};
    Status = NtQueryInformationFile(Root, HZ_FILE_VOLUME_INFORMATION_CLASS, &Info, sizeof(Info));
    NtClose(Root);
    if (HZ_SUCCESS(Status)) {
        Out.Root = RootPath;
        Out.Label = Info.Label;
        Out.FileSystem = Info.FileSystem;
        Out.ReadOnly = Info.ReadOnly != 0;
        Out.TotalBytes = Info.TotalBytes;
        Out.FreeBytes = Info.FreeBytes;
    }
    return Status;
}

Vector<Volume> Volumes()
{
    Vector<Volume> Out;
    for (char Letter = 'C'; Letter <= 'Z'; Letter++) {
        char Root[4] = {Letter, ':', '\\', 0};
        Volume V;
        if (HZ_SUCCESS(QueryVolume(String(Root), V)))
            Out.Push(Move(V));
    }
    return Out;
}

/* ---- Settings files ----------------------------------------------------------------------------- */

namespace {

bool IsBlank(char C)
{
    return C == ' ' || C == '\t' || C == '\r';
}

String TrimRange(const char *Start, const char *End)
{
    while (Start < End && IsBlank(*Start))
        Start++;
    while (End > Start && IsBlank(End[-1]))
        End--;
    return String(Start, (size_t)(End - Start));
}

}  // namespace

bool Settings::Parse(const char *Text, size_t Length, Settings &Out)
{
    Out.Items_.Clear();
    const char *P = Text, *End = Text + Length;
    while (P < End) {
        const char *LineEnd = P;
        while (LineEnd < End && *LineEnd != '\n')
            LineEnd++;
        String Line = TrimRange(P, LineEnd);
        P = LineEnd < End ? LineEnd + 1 : End;
        if (Line.Empty() || Line[0] == '#' || Line[0] == ';')
            continue;
        size_t Equals = Line.Find("=");
        if (Equals == NotFound || Equals == 0)
            return false;
        Out.Set(TrimRange(Line.CStr(), Line.CStr() + Equals).CStr(),
                TrimRange(Line.CStr() + Equals + 1, Line.CStr() + Line.Length()).CStr());
    }
    return true;
}

STATUS Settings::Load(const String &Path)
{
    String Text;
    STATUS Status = ReadAll(Path, Text, 256 * 1024);
    if (!HZ_SUCCESS(Status))
        return Status;
    return Parse(Text.CStr(), Text.Length(), *this) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

String Settings::Text() const
{
    String Out("# Nexora OS settings (Key=Value)\n");
    for (const Item &I : Items_) {
        Out.Append(I.Key);
        Out.Append('=');
        Out.Append(I.Value);
        Out.Append('\n');
    }
    return Out;
}

STATUS Settings::Save(const String &Path) const
{
    String Body = Text();
    return WriteAll(Path, Body.CStr(), Body.Length());
}

bool Settings::Has(const char *Key) const
{
    for (const Item &I : Items_)
        if (EqualNames(I.Key.CStr(), Key))
            return true;
    return false;
}

String Settings::Get(const char *Key, const char *Default) const
{
    for (const Item &I : Items_)
        if (EqualNames(I.Key.CStr(), Key))
            return I.Value;
    return String(Default);
}

int Settings::GetInt(const char *Key, int Default) const
{
    String V = Get(Key, "");
    const char *P = V.CStr();
    bool Negative = *P == '-';
    if (Negative)
        P++;
    if (!*P)
        return Default;
    int Value = 0;
    for (; *P; P++) {
        if (*P < '0' || *P > '9' || Value > 100000000)
            return Default;
        Value = Value * 10 + (*P - '0');
    }
    return Negative ? -Value : Value;
}

void Settings::Set(const char *Key, const char *Value)
{
    for (Item &I : Items_) {
        if (EqualNames(I.Key.CStr(), Key)) {
            I.Value = Value;
            return;
        }
    }
    Item I;
    I.Key = Key;
    I.Value = Value;
    Items_.Push(Move(I));
}

void Settings::SetInt(const char *Key, int Value)
{
    Set(Key, String::Format("%d", Value).CStr());
}

String SettingsPath(const char *Name)
{
    return String::Format("C:\\Nexora\\Settings\\%s.ini", Name);
}

bool IsLiveSystem()
{
    Volume V;
    return !HZ_SUCCESS(QueryVolume(String("C:\\"), V)) || V.ReadOnly;
}

String DocumentsFolder()
{
    Settings Shell;
    if (HZ_SUCCESS(Shell.Load(SettingsPath("shell")))) {
        String Name = Shell.Get("UserName");
        bool Directory = false;
        String Path = String::Format("C:\\Users\\%s\\Documents", Name.CStr());
        if (!Name.Empty() && Exists(Path, &Directory) && Directory)
            return Path;
    }
    for (const Volume &V : Volumes())
        if (!V.ReadOnly)
            return V.Root;
    return String("C:\\");
}

bool IsValidUserName(const char *Name)
{
    size_t Length = strlen(Name);
    if (Length == 0 || Length > 32 || Name[0] == ' ' || Name[Length - 1] == ' ')
        return false;
    for (size_t i = 0; i < Length; i++) {
        char C = Name[i];
        bool Ok = (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9') || C == ' ' || C == '-' ||
                  C == '_' || C == '.';
        if (!Ok)
            return false;
    }
    return strcmp(Name, ".") != 0 && strcmp(Name, "..") != 0;
}

/* ---- Trash (recycle bin) ---------------------------------------------------------------------- */

String TrashFolder()
{
    return String("C:\\Nexora\\Trash");
}

namespace {

String TrashFolderPath(const String &Name)
{
    return Join(TrashFolder(), Name.CStr());
}

String TrashIndexPath()
{
    return Join(TrashFolder(), "index.ini");
}

void EnsureTrashFolder()
{
    bool Directory = false;
    if (!Exists(TrashFolder(), &Directory))
        CreateDirectory(TrashFolder());
}

/* The index is plain "Item<number>=<original path>|<deleted time>" lines; it is written whole
   because items are added and removed. */
bool LoadTrashIndex(Vector<TrashItem> &Out)
{
    Out.Clear();
    String Text;
    if (!HZ_SUCCESS(ReadAll(TrashIndexPath(), Text)))
        return false;
    Settings Index;
    if (!Settings::Parse(Text.CStr(), Text.Length(), Index))
        return false;
    for (const Settings::Item &Line : Index.Items()) {
        if (!Line.Key.StartsWith("Item"))
            continue;
        size_t Bar = Line.Value.Find("|");
        if (Bar == NotFound)
            continue;
        TrashItem Item;
        Item.Name = Line.Key.Substring(4);
        Item.OriginalPath = Line.Value.Substring(0, Bar);
        Item.OriginalName = FileName(Item.OriginalPath);
        Item.DeletedAt = 0;
        for (const char *P = Line.Value.CStr() + Bar + 1; *P >= '0' && *P <= '9'; P++)
            Item.DeletedAt = Item.DeletedAt * 10 + (uint64_t)(*P - '0');
        Out.Push(Move(Item));
    }
    return true;
}

STATUS SaveTrashIndex(const Vector<TrashItem> &Items)
{
    EnsureTrashFolder();
    String Text("# Nexora OS trash index: one line per deleted item.\n");
    for (const TrashItem &Item : Items)
        Text.Append(String::Format("Item%s=%s|%llu\n", Item.Name.CStr(), Item.OriginalPath.CStr(),
                                   (unsigned long long)Item.DeletedAt)
                        .CStr());
    return WriteAll(TrashIndexPath(), Text.CStr(), Text.Length());
}

/* A free name in Path's folder: "notes.txt" -> "notes (2).txt", "notes (3).txt", ... */
String FreeName(const String &Path)
{
    bool Directory = false;
    if (!Exists(Path, &Directory))
        return Path;
    String Name = FileName(Path);
    size_t Dot = NotFound;
    for (size_t i = 1; i < Name.Length(); i++)
        if (Name.CStr()[i] == '.')
            Dot = i;
    String Stem = Dot == NotFound ? Name : Name.Substring(0, Dot);
    String Suffix = Dot == NotFound ? String() : Name.Substring(Dot);
    String Folder = Parent(Path);
    for (unsigned i = 2; i < 1000; i++) {
        String Candidate = Join(Folder, String::Format("%s (%u)%s", Stem.CStr(), i, Suffix.CStr()).CStr());
        if (!Exists(Candidate, &Directory))
            return Candidate;
    }
    return Path;
}

void SortByNewest(Vector<TrashItem> &Items)
{
    for (size_t i = 1; i < Items.Size(); i++) {
        TrashItem Key = Items[i];
        size_t j = i;
        while (j > 0 && Items[j - 1].DeletedAt < Key.DeletedAt) {
            Items[j] = Items[j - 1];
            j--;
        }
        Items[j] = Key;
    }
}

}  // namespace

STATUS Trash(const String &Path)
{
    if (Path.Empty() || IsRoot(Path) || IsInside(Path, TrashFolder()) || EqualNames(Path.CStr(), TrashFolder().CStr()))
        return STATUS_INVALID_PARAMETER;
    EnsureTrashFolder();
    Vector<TrashItem> Items;
    LoadTrashIndex(Items);
    int Highest = 0;
    for (const TrashItem &Item : Items) {
        int Number = 0;
        for (const char *P = Item.Name.CStr(); *P >= '0' && *P <= '9'; P++)
            Number = Number * 10 + (*P - '0');
        if (Number > Highest)
            Highest = Number;
    }
    String Name = String::Format("%d", Highest + 1);
    STATUS Status = MoveTree(Path, TrashFolderPath(Name));
    if (!HZ_SUCCESS(Status))
        return Status;
    TrashItem Item;
    Item.Name = Name;
    Item.OriginalPath = Path;
    Item.OriginalName = FileName(Path);
    NtQuerySystemTime(&Item.DeletedAt);
    Items.Push(Move(Item));
    return SaveTrashIndex(Items);
}

STATUS TrashList(Vector<TrashItem> &Out)
{
    Out.Clear();
    Vector<Entry> Stored;
    if (!HZ_SUCCESS(List(TrashFolder(), Stored)))
        return STATUS_SUCCESS; /* no trash folder yet: nothing was ever deleted */
    Vector<TrashItem> Items;
    LoadTrashIndex(Items);
    for (const Entry &Stored_ : Stored) {
        if (!Stored_.Directory)
            continue;
        bool Found = false;
        for (const TrashItem &Item : Items)
            if (Item.Name == Stored_.Name) {
                Out.Push(Item);
                Found = true;
                break;
            }
        if (!Found) { /* an item the index does not know: it can still be put back */
            TrashItem Item;
            Item.Name = Stored_.Name;
            Item.OriginalName = Stored_.Name;
            Item.DeletedAt = Stored_.Modified;
            Out.Push(Move(Item));
        }
    }
    SortByNewest(Out);
    return STATUS_SUCCESS;
}

STATUS TrashRestore(const String &Name, String *RestoredPath)
{
    if (Name.Empty() || Name.Find("\\") != NotFound || Name.Find("/") != NotFound)
        return STATUS_INVALID_PARAMETER;
    Vector<TrashItem> Items;
    LoadTrashIndex(Items);
    String Target;
    for (size_t i = 0; i < Items.Size(); i++)
        if (Items[i].Name == Name) {
            Target = Items[i].OriginalPath;
            Items.Erase(i);
            break;
        }
    if (Target.Empty()) /* not in the index: the documents folder is the best guess */
        Target = Join(DocumentsFolder(), Name.CStr());
    String Folder = Parent(Target);
    bool Directory = false;
    if (!Exists(Folder, &Directory))
        Target = Join(DocumentsFolder(), FileName(Target).CStr());
    Target = FreeName(Target);
    STATUS Status = MoveTree(TrashFolderPath(Name), Target);
    if (!HZ_SUCCESS(Status))
        return Status;
    if (RestoredPath)
        *RestoredPath = Target;
    return SaveTrashIndex(Items);
}

STATUS TrashDelete(const String &Name)
{
    if (Name.Empty() || Name.Find("\\") != NotFound || Name.Find("/") != NotFound)
        return STATUS_INVALID_PARAMETER;
    Vector<TrashItem> Items;
    LoadTrashIndex(Items);
    for (size_t i = 0; i < Items.Size(); i++)
        if (Items[i].Name == Name) {
            Items.Erase(i);
            break;
        }
    STATUS Status = DeleteTree(TrashFolderPath(Name));
    if (!HZ_SUCCESS(Status) && Status != STATUS_OBJECT_NAME_NOT_FOUND)
        return Status;
    return SaveTrashIndex(Items);
}

STATUS TrashEmpty()
{
    Vector<Entry> Stored;
    List(TrashFolder(), Stored);
    for (const Entry &Entry_ : Stored) {
        if (!Entry_.Directory)
            continue;
        STATUS Status = DeleteTree(TrashFolderPath(Entry_.Name));
        if (!HZ_SUCCESS(Status))
            return Status;
    }
    Delete(TrashIndexPath());
    return STATUS_SUCCESS;
}

bool TrashHasItems()
{
    Vector<Entry> Stored;
    if (!HZ_SUCCESS(List(TrashFolder(), Stored)))
        return false;
    for (const Entry &Entry_ : Stored)
        if (Entry_.Directory)
            return true;
    return false;
}

/* ---- Local time (M11) --------------------------------------------------------------------------- */

const char *const MonthNames[12] = {"January", "February", "March",     "April",   "May",      "June",
                                    "July",    "August",   "September", "October", "November", "December"};
const char *const WeekdayNames[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

CivilTime ToCivil(uint64_t HzTime, int OffsetMinutes)
{
    int64_t Seconds = (int64_t)(HzTime / HZ_TIME_UNITS_PER_SECOND) - (int64_t)(HZ_TIME_UNIX_EPOCH / HZ_TIME_UNITS_PER_SECOND) +
                      (int64_t)OffsetMinutes * 60;
    int64_t Days = Seconds >= 0 ? Seconds / 86400 : (Seconds - 86399) / 86400;
    int64_t OfDay = Seconds - Days * 86400;
    /* Civil date from days since 1970-01-01 (proleptic Gregorian; H. Hinnant's algorithm). */
    int64_t Z = Days + 719468, Era = (Z >= 0 ? Z : Z - 146096) / 146097;
    unsigned Doe = (unsigned)(Z - Era * 146097);
    unsigned Yoe = (Doe - Doe / 1460 + Doe / 36524 - Doe / 146096) / 365;
    unsigned Doy = Doe - (365 * Yoe + Yoe / 4 - Yoe / 100), Mp = (5 * Doy + 2) / 153;
    CivilTime T;
    T.Day = Doy - (153 * Mp + 2) / 5 + 1;
    T.Month = Mp < 10 ? Mp + 3 : Mp - 9;
    T.Year = (long long)Yoe + Era * 400 + (T.Month <= 2);
    T.Hour = (unsigned)(OfDay / 3600);
    T.Minute = (unsigned)(OfDay / 60 % 60);
    T.Second = (unsigned)(OfDay % 60);
    T.Weekday = (unsigned)(((Days + 4) % 7 + 7) % 7); /* 1970-01-01 was a Thursday */
    return T;
}

int ClampUtcOffset(int Minutes)
{
    return Minutes < kMinUtcOffset ? kMinUtcOffset : (Minutes > kMaxUtcOffset ? kMaxUtcOffset : Minutes);
}

int UtcOffsetMinutes()
{
    Settings Shell;
    if (!HZ_SUCCESS(Shell.Load(SettingsPath("shell"))))
        return 0;
    return ClampUtcOffset(Shell.GetInt("UtcOffsetMinutes", 0));
}

String FormatUtcOffset(int Minutes)
{
    if (Minutes == 0)
        return String("UTC");
    int Magnitude = Minutes < 0 ? -Minutes : Minutes;
    return String::Format("UTC%c%02d:%02d", Minutes < 0 ? '-' : '+', Magnitude / 60, Magnitude % 60);
}

/* ---- Formatting --------------------------------------------------------------------------------- */

String FormatSize(uint64_t Bytes)
{
    if (Bytes < 1024)
        return String::Format("%llu bytes", (unsigned long long)Bytes);
    static const char *const Units[] = {"KB", "MB", "GB", "TB"};
    uint64_t Scaled = Bytes * 10 / 1024; /* tenths */
    int Unit = 0;
    while (Scaled >= 10240 && Unit < 3) {
        Scaled /= 1024;
        Unit++;
    }
    if (Scaled >= 1000)
        return String::Format("%llu %s", (unsigned long long)((Scaled + 5) / 10), Units[Unit]);
    return String::Format("%llu.%llu %s", (unsigned long long)(Scaled / 10), (unsigned long long)(Scaled % 10), Units[Unit]);
}

String FormatTime(uint64_t HzTime)
{
    if (HzTime < HZ_TIME_UNIX_EPOCH)
        return String("");
    return FormatTime(HzTime, 0);
}

String FormatTime(uint64_t HzTime, int OffsetMinutes)
{
    if (HzTime < HZ_TIME_UNIX_EPOCH)
        return String("");
    CivilTime T = ToCivil(HzTime, OffsetMinutes);
    return String::Format("%04lld-%02u-%02u %02u:%02u", T.Year, T.Month, T.Day, T.Hour, T.Minute);
}

const char *StatusText(STATUS Status)
{
    switch (Status) {
    case STATUS_SUCCESS:
        return "Done";
    case STATUS_OBJECT_NAME_NOT_FOUND:
        return "The file was not found";
    case STATUS_OBJECT_PATH_NOT_FOUND:
        return "The folder was not found";
    case STATUS_OBJECT_NAME_COLLISION:
        return "Something with that name already exists";
    case STATUS_OBJECT_NAME_INVALID:
        return "The name is not valid";
    case STATUS_MEDIA_WRITE_PROTECTED:
        return "The volume is read-only";
    case STATUS_ACCESS_DENIED:
        return "Access is denied";
    case STATUS_DIRECTORY_NOT_EMPTY:
        return "The folder is not empty";
    case STATUS_DISK_FULL:
        return "The volume is full";
    case STATUS_FILE_IS_A_DIRECTORY:
        return "That is a folder";
    case STATUS_NOT_A_DIRECTORY:
        return "That is not a folder";
    case STATUS_FILE_TOO_LARGE:
        return "The file is too large";
    case STATUS_SHARING_VIOLATION:
        return "The file is in use";
    default:
        return "The operation failed";
    }
}

}  // namespace hz::fs
