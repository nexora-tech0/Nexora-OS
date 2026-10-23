/*
 * Nexora OS - ob/namespace.c
 * The object namespace: directories, symbolic links and path resolution.
 *
 * Paths are absolute, '\'-separated and case-insensitive. Resolution walks directories one
 * component at a time; a symbolic link restarts the walk at its target with the unparsed
 * remainder appended; an object whose type has a parse procedure (a device) receives the
 * remainder (with its leading '\') and resolves it itself.
 */
#include "obp.h"

KSPIN_LOCK ObpNamespaceLock;
OBJECT_DIRECTORY *ObpRootDirectory;

int ObpCompareName(const char *Component, size_t Length, const char *Name)
{
    int Result = _strnicmp(Component, Name, Length);
    if (Result != 0)
        return Result;
    return Name[Length] == '\0' ? 0 : -1; /* Component is a strict prefix of Name: it sorts first */
}

/* Object names may contain anything printable except the separator ("\??" is a valid name);
   file-name rules belong to the file systems behind parse procedures. */
static bool ObpIsValidComponentChar(char c)
{
    return (uint8_t)c >= 0x20 && c != '\\' && c != 0x7F;
}

/* Directory lookup; caller holds ObpNamespaceLock. */
static void *ObpLookupEntry(OBJECT_DIRECTORY *Directory, const char *Component, size_t Length)
{
    for (OBJECT_DIRECTORY_ENTRY *E = Directory->Entries; E; E = E->Next) {
        int Order = ObpCompareName(Component, Length, ObpHeader(E->Object)->Name);
        if (Order == 0)
            return E->Object;
        if (Order < 0)
            break;
    }
    return NULL;
}

STATUS ObpInsertObjectByName(void *Object, const char *Path)
{
    size_t PathLength = strnlen(Path, OBP_MAX_PATH);
    if (Path[0] != '\\' || PathLength >= OBP_MAX_PATH)
        return STATUS_OBJECT_NAME_INVALID;
    size_t Split = PathLength;
    while (Split > 0 && Path[Split - 1] != '\\')
        Split--;
    const char *Leaf = Path + Split;
    size_t LeafLength = PathLength - Split;
    if (LeafLength == 0 || LeafLength >= OBP_MAX_NAME)
        return STATUS_OBJECT_NAME_INVALID;
    for (size_t i = 0; i < LeafLength; i++) {
        if (!ObpIsValidComponentChar(Leaf[i]))
            return STATUS_OBJECT_NAME_INVALID;
    }

    OBJECT_DIRECTORY *Directory;
    if (Split == 1) {
        Directory = ObpRootDirectory;
        ObReferenceObject(Directory);
    } else {
        char Parent[OBP_MAX_PATH];
        memcpy(Parent, Path, Split - 1);
        Parent[Split - 1] = '\0';
        STATUS Status = ObReferenceObjectByName(Parent, ObDirectoryObjectType, (void **)&Directory);
        if (!HZ_SUCCESS(Status))
            return Status == STATUS_OBJECT_NAME_NOT_FOUND ? STATUS_OBJECT_PATH_NOT_FOUND : Status;
    }

    char *Name = ExAllocatePoolWithTag(NonPagedPool, LeafLength + 1, OBP_NAME_TAG);
    OBJECT_DIRECTORY_ENTRY *Entry = ExAllocatePoolZero(NonPagedPool, sizeof(*Entry), OBP_ENTRY_TAG);
    if (!Name || !Entry) {
        if (Name)
            ExFreePoolWithTag(Name, OBP_NAME_TAG);
        if (Entry)
            ExFreePoolWithTag(Entry, OBP_ENTRY_TAG);
        ObDereferenceObject(Directory);
        return STATUS_NO_MEMORY;
    }
    memcpy(Name, Leaf, LeafLength);
    Name[LeafLength] = '\0';
    Entry->Object = Object;

    OBJECT_HEADER *Header = ObpHeader(Object);
    STATUS Status = STATUS_SUCCESS;
    KIRQL OldIrql;
    KeAcquireSpinLock(&ObpNamespaceLock, &OldIrql);
    if (Header->Directory || Header->Name) {
        Status = STATUS_OBJECT_NAME_COLLISION; /* already named */
    } else if (ObpLookupEntry(Directory, Leaf, LeafLength)) {
        Status = STATUS_OBJECT_NAME_COLLISION;
    } else {
        OBJECT_DIRECTORY_ENTRY **Link = &Directory->Entries;
        while (*Link && _stricmp(ObpHeader((*Link)->Object)->Name, Name) < 0)
            Link = &(*Link)->Next;
        Entry->Next = *Link;
        *Link = Entry;
        Directory->Count++;
        Header->Name = Name;
        Header->Directory = Directory;
        Header->Flags |= OBP_FLAG_PERMANENT;
        ObReferenceObject(Object); /* held by the directory entry */
    }
    KeReleaseSpinLock(&ObpNamespaceLock, OldIrql);

    if (!HZ_SUCCESS(Status)) {
        ExFreePoolWithTag(Name, OBP_NAME_TAG);
        ExFreePoolWithTag(Entry, OBP_ENTRY_TAG);
    }
    ObDereferenceObject(Directory);
    return Status;
}

STATUS ObReferenceObjectByName(const char *Path, OBJECT_TYPE *Type, void **Object)
{
    return ObReferenceObjectByNameEx(Path, Type, NULL, Object);
}

STATUS ObReferenceObjectByNameEx(const char *Path, OBJECT_TYPE *Type, void *ParseContext, void **Object)
{
    char Buffer[OBP_MAX_PATH];
    if (RtlStringCopy(Buffer, sizeof(Buffer), Path) >= sizeof(Buffer))
        return STATUS_OBJECT_NAME_INVALID;
    if (!ObpRootDirectory)
        return STATUS_OBJECT_PATH_NOT_FOUND;

    for (int Reparse = 0; Reparse <= OBP_MAX_REPARSE; Reparse++) {
        if (Buffer[0] != '\\')
            return STATUS_OBJECT_NAME_INVALID;
        void *Current = ObpRootDirectory;
        ObReferenceObject(Current);
        const char *p = Buffer + 1;
        bool Restart = false;

        while (*p && !Restart) {
            OBJECT_TYPE *CurrentType = ObGetObjectType(Current);
            if (CurrentType == ObDirectoryObjectType) {
                size_t Length = 0;
                while (p[Length] && p[Length] != '\\')
                    Length++;
                if (Length == 0 || Length >= OBP_MAX_NAME) {
                    ObDereferenceObject(Current);
                    return STATUS_OBJECT_NAME_INVALID;
                }
                KIRQL OldIrql;
                KeAcquireSpinLock(&ObpNamespaceLock, &OldIrql);
                void *Found = ObpLookupEntry((OBJECT_DIRECTORY *)Current, p, Length);
                if (Found)
                    ObReferenceObject(Found);
                KeReleaseSpinLock(&ObpNamespaceLock, OldIrql);
                ObDereferenceObject(Current);
                if (!Found)
                    return p[Length] ? STATUS_OBJECT_PATH_NOT_FOUND : STATUS_OBJECT_NAME_NOT_FOUND;
                Current = Found;
                p += Length;
                if (*p == '\\')
                    p++;

                if (ObGetObjectType(Current) == ObSymbolicLinkObjectType) {
                    /* Target + '\' + remainder, then walk again from the root. */
                    char Next[OBP_MAX_PATH];
                    const char *Target = ((OBJECT_SYMBOLIC_LINK *)Current)->Target;
                    int Needed = *p ? RtlFormatString(Next, sizeof(Next), "%s\\%s", Target, p)
                                    : RtlFormatString(Next, sizeof(Next), "%s", Target);
                    ObDereferenceObject(Current);
                    if (Needed < 0 || (size_t)Needed >= sizeof(Next))
                        return STATUS_OBJECT_NAME_INVALID;
                    memcpy(Buffer, Next, (size_t)Needed + 1);
                    Restart = true;
                }
            } else if (CurrentType->Info.ParseProcedure) {
                void *Result = NULL;
                STATUS Status = CurrentType->Info.ParseProcedure(Current, p - 1, ParseContext, &Result);
                ObDereferenceObject(Current);
                if (!HZ_SUCCESS(Status))
                    return Status;
                Current = Result;
                break;
            } else {
                ObDereferenceObject(Current); /* not a container */
                return STATUS_OBJECT_PATH_NOT_FOUND;
            }
        }
        if (Restart)
            continue;

        if (Type && ObGetObjectType(Current) != Type) {
            ObDereferenceObject(Current);
            return STATUS_OBJECT_TYPE_MISMATCH;
        }
        *Object = Current;
        return STATUS_SUCCESS;
    }
    return STATUS_OBJECT_NAME_INVALID; /* symbolic-link loop */
}

STATUS ObCreateDirectoryObject(const char *Path)
{
    void *Directory;
    STATUS Status = ObCreateObject(ObDirectoryObjectType, sizeof(OBJECT_DIRECTORY), Path, &Directory);
    if (HZ_SUCCESS(Status))
        ObDereferenceObject(Directory); /* permanent: the parent keeps it */
    return Status;
}

STATUS ObCreateSymbolicLink(const char *LinkPath, const char *TargetPath)
{
    size_t Length = strnlen(TargetPath, OBP_MAX_PATH);
    if (TargetPath[0] != '\\' || Length >= OBP_MAX_PATH)
        return STATUS_OBJECT_NAME_INVALID;
    OBJECT_SYMBOLIC_LINK *Link;
    STATUS Status = ObCreateObject(ObSymbolicLinkObjectType, sizeof(*Link), NULL, (void **)&Link);
    if (!HZ_SUCCESS(Status))
        return Status;
    Link->Target = ExAllocatePoolWithTag(NonPagedPool, Length + 1, OBP_NAME_TAG);
    if (!Link->Target) {
        ObDereferenceObject(Link);
        return STATUS_NO_MEMORY;
    }
    memcpy(Link->Target, TargetPath, Length + 1);
    Status = ObpInsertObjectByName(Link, LinkPath);
    ObDereferenceObject(Link);
    return Status;
}

static void ObpDumpDirectory(OBJECT_DIRECTORY *Directory, int Depth)
{
    for (OBJECT_DIRECTORY_ENTRY *E = Directory->Entries; E; E = E->Next) {
        OBJECT_HEADER *Header = ObpHeader(E->Object);
        if (Header->Type == ObSymbolicLinkObjectType) {
            DbgPrintEx(DPFLTR_TRACE_LEVEL, "  %*s%s -> %s\n", Depth * 2, "", Header->Name,
                       ((OBJECT_SYMBOLIC_LINK *)E->Object)->Target);
        } else {
            DbgPrintEx(DPFLTR_TRACE_LEVEL, "  %*s%s%s (%s)\n", Depth * 2, "", Header->Name,
                       Header->Type == ObDirectoryObjectType ? "\\" : "", Header->Type->Name);
        }
        if (Header->Type == ObDirectoryObjectType && Depth < 8)
            ObpDumpDirectory((OBJECT_DIRECTORY *)E->Object, Depth + 1);
    }
}

void ObDumpNamespace(void)
{
    KIRQL OldIrql;
    KeAcquireSpinLock(&ObpNamespaceLock, &OldIrql);
    DbgPrintEx(DPFLTR_TRACE_LEVEL, "Object namespace:\n  \\\n");
    ObpDumpDirectory(ObpRootDirectory, 1);
    KeReleaseSpinLock(&ObpNamespaceLock, OldIrql);
}

void ObpInitializeNamespace(void)
{
    void *Root;
    if (!HZ_SUCCESS(ObpAllocateObject(ObDirectoryObjectType, sizeof(OBJECT_DIRECTORY), &Root)))
        KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, PS_INIT_FAILURE_OBJECT_TYPES, 3, 0, 0);
    ObpRootDirectory = Root; /* permanent: its creation reference is never dropped */

    static const char *const Directories[] = {"\\ObjectTypes", "\\Device", "\\Driver", "\\FileSystem", "\\??",
                                                "\\KernelObjects"};
    for (size_t i = 0; i < HZ_ARRAYSIZE(Directories); i++) {
        STATUS Status = ObCreateDirectoryObject(Directories[i]);
        if (!HZ_SUCCESS(Status))
            KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, PS_INIT_FAILURE_OBJECT_TYPES, 4, (uint64_t)Status, i);
    }
    /* The bootstrap types were created before the namespace existed. */
    OBJECT_TYPE *Bootstrap[] = {ObTypeObjectType, ObDirectoryObjectType, ObSymbolicLinkObjectType};
    for (size_t i = 0; i < HZ_ARRAYSIZE(Bootstrap); i++) {
        char Path[OBP_MAX_PATH];
        RtlFormatString(Path, sizeof(Path), "\\ObjectTypes\\%s", Bootstrap[i]->Name);
        STATUS Status = ObpInsertObjectByName(Bootstrap[i], Path);
        if (!HZ_SUCCESS(Status))
            KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, PS_INIT_FAILURE_OBJECT_TYPES, 5, (uint64_t)Status, i);
    }
}
