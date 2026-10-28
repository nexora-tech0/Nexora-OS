/*
 * Nexora OS - ps/module.c
 * Process-owned Windows PE module records. Names are matched case-insensitively by basename;
 * records describe mappings in the owning process and are removed with that process.
 */
#include "psp.h"

static const char *PspWinModuleBaseName(const char *Name)
{
    const char *Base = Name;
    for (const char *Character = Name; *Character; ++Character)
        if (*Character == '/' || *Character == '\\')
            Base = Character + 1;
    return Base;
}

static char PspWinModuleLowerAscii(char Character)
{
    return Character >= 'A' && Character <= 'Z' ? (char)(Character + ('a' - 'A')) : Character;
}

static bool PspWinModuleNameEqual(const char *Left, const char *Right)
{
    Left = PspWinModuleBaseName(Left);
    Right = PspWinModuleBaseName(Right);
    while (*Left && *Right && PspWinModuleLowerAscii(*Left) == PspWinModuleLowerAscii(*Right)) {
        ++Left;
        ++Right;
    }
    return *Left == 0 && *Right == 0;
}

STATUS PspRegisterWinModule(EPROCESS *Process, const char *Name, const char *Path,
                            uint64_t Base, uint64_t Size, uint32_t Flags)
{
    if (!Process || !Name || !Name[0] || !Base || !Size ||
        (Flags & ~PSP_WIN_MODULE_MAIN_IMAGE))
        return STATUS_INVALID_PARAMETER;
    const char *Leaf = PspWinModuleBaseName(Name);
    size_t NameLength = strlen(Leaf);
    size_t PathLength = Path ? strlen(Path) : 0;
    if (!NameLength || NameLength >= PSP_WIN_MODULE_NAME_MAX ||
        PathLength >= PSP_WIN_MODULE_PATH_MAX)
        return STATUS_NAME_TOO_LONG;

    PSP_WIN_MODULE *Module = ExAllocatePoolZero(NonPagedPool, sizeof(*Module), PSP_WIN_MODULE_TAG);
    if (!Module)
        return STATUS_NO_MEMORY;
    Module->Base = Base;
    Module->Size = Size;
    Module->Flags = Flags;
    memcpy(Module->Name, Leaf, NameLength + 1);
    if (PathLength)
        memcpy(Module->Path, Path, PathLength + 1);

    KIRQL OldIrql;
    KeAcquireSpinLock(&PspLock, &OldIrql);
    for (LIST_ENTRY *Entry = Process->WinCompatModules.Flink;
         Entry != &Process->WinCompatModules; Entry = Entry->Flink) {
        PSP_WIN_MODULE *Existing = CONTAINING_RECORD(Entry, PSP_WIN_MODULE, Link);
        if (PspWinModuleNameEqual(Existing->Name, Module->Name)) {
            bool SameMapping = Existing->Base == Module->Base && Existing->Size == Module->Size;
            KeReleaseSpinLock(&PspLock, OldIrql);
            ExFreePoolWithTag(Module, PSP_WIN_MODULE_TAG);
            return SameMapping ? STATUS_SUCCESS : STATUS_OBJECT_NAME_COLLISION;
        }
    }
    InsertTailList(&Process->WinCompatModules, &Module->Link);
    KeReleaseSpinLock(&PspLock, OldIrql);
    return STATUS_SUCCESS;
}

STATUS PspLookupWinModule(EPROCESS *Process, const char *Name, uint64_t *Base, uint64_t *Size)
{
    if (!Process || !Name || !Name[0] || !Base)
        return STATUS_INVALID_PARAMETER;
    KIRQL OldIrql;
    KeAcquireSpinLock(&PspLock, &OldIrql);
    for (LIST_ENTRY *Entry = Process->WinCompatModules.Flink;
         Entry != &Process->WinCompatModules; Entry = Entry->Flink) {
        PSP_WIN_MODULE *Module = CONTAINING_RECORD(Entry, PSP_WIN_MODULE, Link);
        if (PspWinModuleNameEqual(Module->Name, Name)) {
            *Base = Module->Base;
            if (Size)
                *Size = Module->Size;
            KeReleaseSpinLock(&PspLock, OldIrql);
            return STATUS_SUCCESS;
        }
    }
    KeReleaseSpinLock(&PspLock, OldIrql);
    return STATUS_DLL_NOT_FOUND;
}

STATUS PspLookupWinModuleByBase(EPROCESS *Process, uint64_t Base, char *Name,
                                size_t NameCapacity, uint64_t *Size, uint32_t *Flags)
{
    if (!Process || !Base || !Name || NameCapacity < 2)
        return STATUS_INVALID_PARAMETER;
    KIRQL OldIrql;
    KeAcquireSpinLock(&PspLock, &OldIrql);
    for (LIST_ENTRY *Entry = Process->WinCompatModules.Flink;
         Entry != &Process->WinCompatModules; Entry = Entry->Flink) {
        PSP_WIN_MODULE *Module = CONTAINING_RECORD(Entry, PSP_WIN_MODULE, Link);
        if (Module->Base == Base) {
            size_t Length = strlen(Module->Name);
            if (Length >= NameCapacity) {
                KeReleaseSpinLock(&PspLock, OldIrql);
                return STATUS_BUFFER_TOO_SMALL;
            }
            memcpy(Name, Module->Name, Length + 1);
            if (Size)
                *Size = Module->Size;
            if (Flags)
                *Flags = Module->Flags;
            KeReleaseSpinLock(&PspLock, OldIrql);
            return STATUS_SUCCESS;
        }
    }
    KeReleaseSpinLock(&PspLock, OldIrql);
    return STATUS_DLL_NOT_FOUND;
}

void PspFreeWinModules(EPROCESS *Process)
{
    if (!Process)
        return;
    for (;;) {
        KIRQL OldIrql;
        KeAcquireSpinLock(&PspLock, &OldIrql);
        if (IsListEmpty(&Process->WinCompatModules)) {
            KeReleaseSpinLock(&PspLock, OldIrql);
            return;
        }
        LIST_ENTRY *Entry = RemoveHeadList(&Process->WinCompatModules);
        KeReleaseSpinLock(&PspLock, OldIrql);
        PSP_WIN_MODULE *Module = CONTAINING_RECORD(Entry, PSP_WIN_MODULE, Link);
        ExFreePoolWithTag(Module, PSP_WIN_MODULE_TAG);
    }
}
