/*
 * Nexora OS - user/lib/hzrt/hzapps.cpp
 * Program manifests (user/include/hzapps.h).
 */
#include "hzapps.h"
#include "hzfile.h"

namespace hz::apps {

namespace {

char Lower(char C)
{
    return (C >= 'A' && C <= 'Z') ? (char)(C + 32) : C;
}

bool IsSpace(char C)
{
    return C == ' ' || C == '\t' || C == '\r';
}

String Trim(const char *Start, const char *End)
{
    while (Start < End && IsSpace(*Start))
        Start++;
    while (End > Start && IsSpace(End[-1]))
        End--;
    return String(Start, (size_t)(End - Start));
}

bool KeyIs(const String &Key, const char *Name)
{
    return fs::EqualNames(Key.CStr(), Name);
}

bool ParseHexColor(const String &Text, uint32_t *Color)
{
    const char *P = Text.CStr();
    if (*P == '#')
        P++;
    uint32_t Value = 0;
    int Count = 0;
    for (; *P; P++, Count++) {
        char C = Lower(*P);
        if (C >= '0' && C <= '9')
            Value = Value * 16 + (uint32_t)(C - '0');
        else if (C >= 'a' && C <= 'f')
            Value = Value * 16 + (uint32_t)(C - 'a' + 10);
        else
            return false;
    }
    if (Count != 6)
        return false;
    *Color = 0xFF000000u | Value;
    return true;
}

bool ParseInt(const String &Text, int *Value)
{
    const char *P = Text.CStr();
    bool Negative = *P == '-';
    if (Negative)
        P++;
    if (!*P)
        return false;
    int V = 0;
    for (; *P; P++) {
        if (*P < '0' || *P > '9' || V > 100000000)
            return false;
        V = V * 10 + (*P - '0');
    }
    *Value = Negative ? -V : V;
    return true;
}

}  // namespace

bool ParseManifest(const char *Text, size_t Length, App &Out, String *Error)
{
    Out = App();
    const char *P = Text, *End = Text + Length;
    int LineNumber = 0;
    while (P < End) {
        const char *LineEnd = P;
        while (LineEnd < End && *LineEnd != '\n')
            LineEnd++;
        LineNumber++;
        String Line = Trim(P, LineEnd);
        P = LineEnd < End ? LineEnd + 1 : End;
        if (Line.Empty() || Line[0] == '#' || Line[0] == ';')
            continue;
        size_t Equals = Line.Find("=");
        if (Equals == NotFound) {
            if (Error)
                *Error = String::Format("line %d: expected Key=Value", LineNumber);
            return false;
        }
        String Key = Trim(Line.CStr(), Line.CStr() + Equals);
        String Value = Trim(Line.CStr() + Equals + 1, Line.CStr() + Line.Length());
        if (KeyIs(Key, "Name")) {
            Out.Name = Value;
        } else if (KeyIs(Key, "Description")) {
            Out.Description = Value;
        } else if (KeyIs(Key, "Icon")) {
            Out.Icon = Value;
        } else if (KeyIs(Key, "Color")) {
            if (!ParseHexColor(Value, &Out.Color)) {
                if (Error)
                    *Error = String::Format("line %d: Color must be RRGGBB", LineNumber);
                return false;
            }
        } else if (KeyIs(Key, "Path")) {
            Out.Path = Value;
        } else if (KeyIs(Key, "Opens")) {
            const char *S = Value.CStr();
            while (*S) {
                const char *E = S;
                while (*E && *E != ',')
                    E++;
                String Ext = Trim(S, E);
                String LowerExt;
                for (size_t i = 0; i < Ext.Length(); i++)
                    if (Ext[i] != '.')
                        LowerExt.Append(Lower(Ext[i]));
                if (!LowerExt.Empty())
                    Out.Opens.Push(LowerExt);
                S = *E ? E + 1 : E;
            }
        } else if (KeyIs(Key, "Pinned")) {
            Out.Pinned = Value == "1" || fs::EqualNames(Value.CStr(), "yes") || fs::EqualNames(Value.CStr(), "true");
        } else if (KeyIs(Key, "Order")) {
            if (!ParseInt(Value, &Out.Order)) {
                if (Error)
                    *Error = String::Format("line %d: Order must be a number", LineNumber);
                return false;
            }
        }
        /* Unknown keys are ignored (newer manifests on older systems). */
    }
    if (Out.Name.Empty() || Out.Path.Empty()) {
        if (Error)
            *Error = String(Out.Name.Empty() ? "Name is missing" : "Path is missing");
        return false;
    }
    return true;
}

Vector<App> Load(const char *Folder)
{
    Vector<App> Out;
    Vector<fs::Entry> Entries;
    String Directory(Folder);
    if (!HZ_SUCCESS(fs::List(Directory, Entries)))
        return Out;
    for (const fs::Entry &E : Entries) {
        if (E.Directory || fs::Extension(E.Name) != "app")
            continue;
        String Path = fs::Join(Directory, E.Name.CStr());
        String Text, Error;
        App A;
        if (!HZ_SUCCESS(fs::ReadAll(Path, Text, 64 * 1024)) || !ParseManifest(Text.CStr(), Text.Length(), A, &Error)) {
            HzLog("apps: %s is not a valid manifest (%s)", Path.CStr(), Error.CStr());
            continue;
        }
        A.Id = E.Name.Substring(0, E.Name.Length() - 4);
        /* Keep the list ordered by Order, then Name. */
        size_t At = Out.Size();
        while (At > 0) {
            const App &Before = Out[At - 1];
            bool After = Before.Order < A.Order || (Before.Order == A.Order && strcmp(Before.Name.CStr(), A.Name.CStr()) <= 0);
            if (After)
                break;
            At--;
        }
        Out.Insert(At, Move(A));
    }
    return Out;
}

const App *FindForExtension(const Vector<App> &Apps, const String &Extension)
{
    if (Extension.Empty())
        return nullptr;
    for (const App &A : Apps)
        for (const String &E : A.Opens)
            if (E == Extension)
                return &A;
    return nullptr;
}

const App *FindByImage(const Vector<App> &Apps, const char *ImageName)
{
    String Name = fs::FileName(String(ImageName));
    for (const App &A : Apps)
        if (fs::EqualNames(fs::FileName(A.Path).CStr(), Name.CStr()))
            return &A;
    return nullptr;
}

STATUS Launch(const App &Program, const char *Argument, uint32_t *ProcessId)
{
    String CommandLine = fs::FileName(Program.Path);
    if (Argument && *Argument) {
        CommandLine.Append(" \"");
        CommandLine.Append(Argument);
        CommandLine.Append('"');
    }
    HANDLE Process = 0;
    uint32_t Id = 0;
    STATUS Status = NtCreateProcess(Program.Path.CStr(), CommandLine.CStr(), &Process, &Id);
    if (HZ_SUCCESS(Status)) {
        NtClose(Process);
        if (ProcessId)
            *ProcessId = Id;
    }
    return Status;
}

STATUS OpenPath(const String &Path, uint32_t *ProcessId, String *Program)
{
    String Extension = fs::Extension(Path);
    if (Extension == "exe") {
        HANDLE Process = 0;
        uint32_t Id = 0;
        String Name = fs::FileName(Path);
        STATUS Status = NtCreateProcess(Path.CStr(), Name.CStr(), &Process, &Id);
        if (HZ_SUCCESS(Status)) {
            NtClose(Process);
            if (ProcessId)
                *ProcessId = Id;
            if (Program)
                *Program = Name;
        }
        return Status;
    }
    Vector<App> Apps = Load();
    const App *Handler = FindForExtension(Apps, Extension);
    if (!Handler)
        return STATUS_NOT_FOUND;
    if (Program)
        *Program = Handler->Name;
    return Launch(*Handler, Path.CStr(), ProcessId);
}

}  // namespace hz::apps
