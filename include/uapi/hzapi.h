/*
 * Nexora OS - include/uapi/hzapi.h
 * The user/kernel ABI: system service numbers, constants and the structures exchanged through
 * system calls or shared pages (TEB, PEB). Included by the kernel and by user mode (which is
 * compiled for the Microsoft x64 ABI), so only fixed-width types are used and every structure
 * has the same layout on both sides. The API is Nexora OS's own (NT-style names and status
 * numbers, but not binary compatible with Windows).
 */
#ifndef HZAPI_H
#define HZAPI_H

#include "hzbase.h"

HZ_EXTERN_C_BEGIN

typedef void *HANDLE;
typedef uint32_t ACCESS_MASK;

#define HZ_CURRENT_PROCESS ((HANDLE)(intptr_t)-1)
#define HZ_CURRENT_THREAD ((HANDLE)(intptr_t)-2)

/* ---- System services (rax = number; args in r10, rdx, r8, r9, then the user stack) ---------
 * Every service returns a STATUS in eax. Pointers are user addresses checked by the kernel.
 * Timeouts are relative microseconds; a NULL timeout pointer waits forever.
 *
 *   Close(HANDLE)
 *   DisplayString(const char *Text, uint64_t Length)               -> kernel debug log
 *   TerminateProcess(HANDLE Process, STATUS ExitStatus)
 *   TerminateThread(HANDLE Thread, STATUS ExitStatus)
 *   CreateThread(HANDLE Process, void *Start, uint64_t Arg1, uint64_t Arg2,
 *                HANDLE *Thread, uint32_t *ThreadId)                 rip = Start, rcx/rdx = Arg1/2
 *   AllocateVirtualMemory(HANDLE Process, void **Base, uint64_t *Size, uint32_t Type,
 *                         uint32_t PageProtection)
 *   FreeVirtualMemory(HANDLE Process, void *Base, uint32_t FreeType = MEM_RELEASE)
 *   QueryVirtualMemory(HANDLE Process, void *Address, HZ_MEMORY_BASIC_INFORMATION *Info,
 *                      uint64_t InfoLength)
 *   ProtectVirtualMemory(HANDLE Process, void *Base, uint64_t Size, uint32_t PageProtection,
 *                        uint32_t *OldPageProtection)
 *   CreateEvent(HANDLE *Event, uint32_t EventType, uint32_t InitialState)
 *   SetEvent(HANDLE Event, int32_t *PreviousState)
 *   ResetEvent(HANDLE Event)
 *   WaitForSingleObject(HANDLE Object, uint32_t Reserved, const uint64_t *TimeoutUs)
 *   WaitForMultipleObjects(uint32_t Count, const HANDLE *Objects, uint32_t WaitType,
 *                          const uint64_t *TimeoutUs)
 *   DelayExecution(uint64_t Microseconds)
 *   YieldExecution()
 *   QuerySystemInformation(uint32_t Class, void *Buffer, uint32_t Length, uint32_t *ReturnLength)
 *   QueryUptime(uint64_t *Microseconds)
 *   CreateProcess(const char *ImagePath, const char *CommandLine, HANDLE *Process,
 *                 uint32_t *ProcessId)                 path "C:\..." or an object path "\..."
 *   QueryInformationProcess(HANDLE Process, uint32_t Class, void *Buffer, uint32_t Length)
 *   QueryInformationThread(HANDLE Thread, uint32_t Class, void *Buffer, uint32_t Length)
 *   CreateFile(HANDLE *File, ACCESS_MASK Access, const char *Path, uint32_t Disposition,
 *              uint32_t Options, uint32_t *Result)     path "C:\..." or an object path "\...";
 *                                                      *Result (optional) = FILE_OPENED, ...
 *   ReadFile(HANDLE File, void *Buffer, uint32_t Length, const uint64_t *ByteOffset,
 *            uint32_t *BytesRead)                      ByteOffset NULL = current position
 *   WriteFile(HANDLE File, const void *Buffer, uint32_t Length, const uint64_t *ByteOffset,
 *             uint32_t *BytesWritten)
 *   QueryInformationFile(HANDLE File, uint32_t Class, void *Buffer, uint32_t Length)
 *   SetInformationFile(HANDLE File, uint32_t Class, const void *Buffer, uint32_t Length)
 *   QueryDirectoryFile(HANDLE Directory, HZ_FILE_DIRECTORY_INFORMATION *Buffer, uint32_t Length,
 *                      uint32_t Flags, uint32_t *EntriesReturned)
 *   DeviceIoControlFile(HANDLE File, uint32_t Code, const void *Input, uint32_t InputLength,
 *                       void *Output, uint32_t OutputLength, uint32_t *BytesReturned)
 *   ShutdownSystem(uint32_t Action)                    HZ_SHUTDOWN_POWER_OFF / _REBOOT
 *   QuerySystemTime(uint64_t *Time)                    HZ time (100 ns units since 1601, UTC)
 *   CreateSection(HANDLE *Section, uint64_t Size)      shared memory, zero-filled
 *   MapViewOfSection(HANDLE Section, HANDLE Process, void **Base, uint64_t *ViewSize,
 *                    uint32_t PageProtection)          PAGE_READONLY/READWRITE, own process
 *   UnmapViewOfSection(HANDLE Process, void *Base)
 *   CreatePipe(HANDLE *ReadEnd, HANDLE *WriteEnd, uint32_t Size)   anonymous pipe (M9); Size 0 =
 *                                                      16 KiB; use ReadFile/WriteFile/Close
 *   CreateProcessEx(const char *ImagePath, const char *CommandLine,
 *                   const HZ_PROCESS_PARAMETERS *Parameters, HANDLE *Process, uint32_t *ProcessId)
 *                                                      standard handles for the new process (M9)
 *   OpenProcess(HANDLE *Process, ACCESS_MASK Access, uint32_t ProcessId)    (M9)
 *   ResolveHostA(const char *HostName, uint8_t Ipv4Address[4])   resolve one DNS A record
 *
 * Window manager services (win32k) use numbers from HZ_WIN32K_SERVICE_BASE (uapi/hzuser.h).
 */

enum HZ_SYSTEM_SERVICE {
    HzSysClose = 0,
    HzSysDisplayString,
    HzSysTerminateProcess,
    HzSysTerminateThread,
    HzSysCreateThread,
    HzSysAllocateVirtualMemory,
    HzSysFreeVirtualMemory,
    HzSysQueryVirtualMemory,
    HzSysProtectVirtualMemory,
    HzSysCreateEvent,
    HzSysSetEvent,
    HzSysResetEvent,
    HzSysWaitForSingleObject,
    HzSysWaitForMultipleObjects,
    HzSysDelayExecution,
    HzSysYieldExecution,
    HzSysQuerySystemInformation,
    HzSysQueryUptime,
    HzSysCreateProcess,
    HzSysQueryInformationProcess,
    HzSysQueryInformationThread,
    HzSysCreateFile,
    HzSysReadFile,
    HzSysWriteFile,
    HzSysQueryInformationFile,
    HzSysSetInformationFile,
    HzSysQueryDirectoryFile,
    HzSysDeviceIoControlFile,
    HzSysShutdownSystem,
    HzSysQuerySystemTime,
    HzSysCreateSection,
    HzSysMapViewOfSection,
    HzSysUnmapViewOfSection,
    HzSysCreatePipe,
    HzSysCreateProcessEx,
    HzSysOpenProcess,
    HzSysResolveHostA,
    HzSysServiceCount
};

/* ---- Access rights ------------------------------------------------------------------------- */

#define SYNCHRONIZE 0x00100000u
#define GENERIC_ALL 0x10000000u
#define EVENT_QUERY_STATE 0x0001u
#define EVENT_MODIFY_STATE 0x0002u
#define EVENT_ALL_ACCESS 0x001F0003u
#define PROCESS_TERMINATE 0x0001u
#define PROCESS_CREATE_THREAD 0x0002u
#define PROCESS_VM_OPERATION 0x0008u
#define SECTION_MAP_WRITE 0x0002u
#define SECTION_MAP_READ 0x0004u
#define SECTION_ALL_ACCESS 0x000F001Fu
#define PROCESS_QUERY_INFORMATION 0x0400u
#define PROCESS_ALL_ACCESS 0x001FFFFFu
#define THREAD_TERMINATE 0x0001u
#define THREAD_QUERY_INFORMATION 0x0040u
#define THREAD_ALL_ACCESS 0x001FFFFFu

/* ---- Virtual memory ------------------------------------------------------------------------ */

#define MEM_COMMIT 0x00001000u
#define MEM_RESERVE 0x00002000u
#define MEM_RELEASE 0x00008000u
#define MEM_FREE 0x00010000u
#define MEM_PRIVATE 0x00020000u
#define MEM_IMAGE 0x01000000u
#define MEM_MAPPED 0x00040000u

#define PAGE_NOACCESS 0x01u
#define PAGE_READONLY 0x02u
#define PAGE_READWRITE 0x04u
#define PAGE_EXECUTE 0x10u
#define PAGE_EXECUTE_READ 0x20u
#define PAGE_EXECUTE_READWRITE 0x40u

typedef struct _HZ_MEMORY_BASIC_INFORMATION {
    uint64_t BaseAddress;
    uint64_t AllocationBase;
    uint64_t RegionSize;
    uint32_t State;   /* MEM_COMMIT, MEM_RESERVE or MEM_FREE */
    uint32_t Protect; /* PAGE_* */
    uint32_t Type;    /* MEM_PRIVATE or MEM_IMAGE */
    uint32_t Reserved;
} HZ_MEMORY_BASIC_INFORMATION;

/* ---- Events and waits ---------------------------------------------------------------------- */

#define HZ_NOTIFICATION_EVENT 0u
#define HZ_SYNCHRONIZATION_EVENT 1u
#define HZ_WAIT_ALL 0u
#define HZ_WAIT_ANY 1u
#define HZ_MAXIMUM_WAIT_OBJECTS 64u

/* ---- Per-thread and per-process blocks (user memory, GS base = TEB) ----------------------- */

typedef struct _HZ_PEB {
    uint64_t ImageBase;
    uint64_t ImageSize;
    uint64_t ProcessId;
    uint64_t ParentProcessId;
    char ImageName[64];
    char CommandLine[256];
    /* Standard handles given by the creator (CreateProcessEx), 0 if none (M9). hzrt writes
       HzPrint output to StandardOutput when there is one, else to the kernel log. */
    uint64_t StandardInput;
    uint64_t StandardOutput;
    uint64_t StandardError;
} HZ_PEB;

/* CreateProcessEx parameters: handles of the caller (0 = none) that the new process gets as its
   standard handles (new handles to the same objects, with the same access). */
typedef struct _HZ_PROCESS_PARAMETERS {
    HANDLE StandardInput;
    HANDLE StandardOutput;
    HANDLE StandardError;
    uint32_t Flags;      /* none defined */
    uint32_t Reserved;
} HZ_PROCESS_PARAMETERS;

typedef struct _HZ_TEB {
    struct _HZ_TEB *Self; /* gs:[0] */
    HZ_PEB *Peb;
    uint64_t ProcessId;
    uint64_t ThreadId;
    uint64_t StackBase;  /* top (highest address) */
    uint64_t StackLimit; /* lowest committed address */
    int32_t LastStatus;
    uint32_t Reserved;
    void *TlsSlots[64];
} HZ_TEB;

/* ---- System information --------------------------------------------------------------------- */

#define HZ_SYSTEM_BASIC_INFORMATION_CLASS 0u
#define HZ_SYSTEM_PROCESS_INFORMATION_CLASS 1u
#define HZ_SYSTEM_PROCESSOR_INFORMATION_CLASS 2u /* M11: one entry per online processor */
#define HZ_SYSTEM_NETWORK_INTERFACE_INFORMATION_CLASS 3u
#define HZ_MAX_NETWORK_INTERFACES 8u

typedef struct _HZ_SYSTEM_PROCESSOR_INFORMATION {
    uint32_t Number;
    uint32_t ApicId;
    uint64_t IdleTimeUs;     /* time its idle thread ran */
    uint64_t ContextSwitches;
    uint64_t Interrupts;
    uint64_t TlbShootdowns;  /* flush requests from other processors it handled */
} HZ_SYSTEM_PROCESSOR_INFORMATION;

typedef struct _HZ_SYSTEM_NETWORK_INTERFACE_INFORMATION {
    char Name[16];
    uint8_t MacAddress[6];
    uint8_t LinkUp;
    uint8_t ConfigurationState; /* 0 = unconfigured, 1 = DHCP in progress, 2 = configured, 3 = failed */
    uint32_t Mtu;
    uint8_t Ipv4Address[4];
    uint8_t SubnetMask[4];
    uint8_t Gateway[4];
    uint8_t DnsServer[4];
} HZ_SYSTEM_NETWORK_INTERFACE_INFORMATION;

typedef struct _HZ_SYSTEM_BASIC_INFORMATION {
    uint32_t PageSize;
    uint32_t ProcessorCount;
    uint64_t TotalPhysicalPages;
    uint64_t AvailablePages;
    uint64_t UptimeUs;
    uint64_t IdleTimeUs;
    uint64_t ContextSwitches;
    uint64_t InterruptCount;
    uint32_t ProcessCount;
    uint32_t ThreadCount;
} HZ_SYSTEM_BASIC_INFORMATION;

typedef struct _HZ_SYSTEM_PROCESS_INFORMATION {
    uint32_t ProcessId;
    uint32_t ParentProcessId;
    uint32_t ThreadCount;
    uint32_t HandleCount;
    uint64_t CpuTimeUs;
    uint64_t ResidentPages;
    uint64_t CreateTimeUs;
    char ImageName[32];
} HZ_SYSTEM_PROCESS_INFORMATION;

/* ---- Process and thread information --------------------------------------------------------- */

#define HZ_PROCESS_BASIC_INFORMATION_CLASS 0u
#define HZ_THREAD_BASIC_INFORMATION_CLASS 0u

typedef struct _HZ_PROCESS_BASIC_INFORMATION {
    int32_t ExitStatus;  /* STATUS_PENDING while running */
    uint32_t ProcessId;
    uint32_t ParentProcessId;
    uint32_t ThreadCount;
    uint32_t HandleCount;
    uint32_t Reserved;
    uint64_t ImageBase;
} HZ_PROCESS_BASIC_INFORMATION;

typedef struct _HZ_THREAD_BASIC_INFORMATION {
    int32_t ExitStatus;  /* STATUS_PENDING while running */
    uint32_t ThreadId;
    uint32_t ProcessId;
    int32_t Priority;
    uint64_t TebAddress;
} HZ_THREAD_BASIC_INFORMATION;

/* ---- Time ------------------------------------------------------------------------------------
 * System time is UTC in 100-nanosecond units since 1601-01-01 (the NT FILETIME epoch). */

#define HZ_TIME_UNIX_EPOCH 116444736000000000ull /* 1970-01-01 in HZ time */
#define HZ_TIME_UNITS_PER_SECOND 10000000ull

/* ---- Files and devices ---------------------------------------------------------------------- */

/* Access rights (FILE_READ_DATA doubles as list-directory, as in NT). */
#define FILE_READ_DATA 0x00000001u
#define FILE_LIST_DIRECTORY 0x00000001u
#define FILE_WRITE_DATA 0x00000002u
#define FILE_READ_ATTRIBUTES 0x00000080u
#define FILE_WRITE_ATTRIBUTES 0x00000100u
#define DELETE 0x00010000u
#define FILE_ALL_ACCESS 0x001F01FFu
#define GENERIC_WRITE 0x40000000u
#define GENERIC_READ 0x80000000u

/* Create dispositions */
#define FILE_SUPERSEDE 0u    /* replace if it exists, create if not */
#define FILE_OPEN 1u         /* open, fail if missing */
#define FILE_CREATE 2u       /* create, fail if it exists */
#define FILE_OPEN_IF 3u      /* open or create */
#define FILE_OVERWRITE 4u    /* open and truncate, fail if missing */
#define FILE_OVERWRITE_IF 5u /* open and truncate, or create */

/* Create options */
#define FILE_DIRECTORY_FILE 0x00000001u
#define FILE_NON_DIRECTORY_FILE 0x00000040u
#define FILE_DELETE_ON_CLOSE 0x00001000u

/* What CreateFile did (*Result) */
#define FILE_SUPERSEDED 0u
#define FILE_OPENED 1u
#define FILE_CREATED 2u
#define FILE_OVERWRITTEN 3u

/* Attributes */
#define FILE_ATTRIBUTE_READONLY 0x01u
#define FILE_ATTRIBUTE_HIDDEN 0x02u
#define FILE_ATTRIBUTE_SYSTEM 0x04u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_ARCHIVE 0x20u
#define FILE_ATTRIBUTE_DEVICE 0x40u

/* Information classes (Query/SetInformationFile) */
#define HZ_FILE_BASIC_INFORMATION_CLASS 0u       /* query */
#define HZ_FILE_POSITION_INFORMATION_CLASS 1u    /* query, set */
#define HZ_FILE_END_OF_FILE_INFORMATION_CLASS 2u /* set: truncate or extend (zero-filled) */
#define HZ_FILE_DISPOSITION_INFORMATION_CLASS 3u /* set: delete when the last reference closes */
#define HZ_FILE_RENAME_INFORMATION_CLASS 4u      /* set */
#define HZ_FILE_VOLUME_INFORMATION_CLASS 5u      /* query (any file on the volume) */

typedef struct _HZ_FILE_BASIC_INFORMATION {
    uint64_t CreationTime;
    uint64_t LastWriteTime;
    uint64_t EndOfFile;      /* size in bytes */
    uint64_t AllocationSize; /* bytes allocated on the volume */
    uint32_t Attributes;     /* FILE_ATTRIBUTE_* */
    uint32_t Reserved;
} HZ_FILE_BASIC_INFORMATION;

typedef struct _HZ_FILE_POSITION_INFORMATION {
    uint64_t CurrentByteOffset;
} HZ_FILE_POSITION_INFORMATION;

typedef struct _HZ_FILE_END_OF_FILE_INFORMATION {
    uint64_t EndOfFile;
} HZ_FILE_END_OF_FILE_INFORMATION;

typedef struct _HZ_FILE_DISPOSITION_INFORMATION {
    uint32_t DeleteFile;
    uint32_t Reserved;
} HZ_FILE_DISPOSITION_INFORMATION;

/* NewName: a plain name (same directory) or a path from the volume root ("\Docs\x.txt"). */
typedef struct _HZ_FILE_RENAME_INFORMATION {
    uint32_t ReplaceIfExists;
    uint32_t Reserved;
    char NewName[256];
} HZ_FILE_RENAME_INFORMATION;

typedef struct _HZ_FILE_VOLUME_INFORMATION {
    uint64_t TotalBytes;
    uint64_t FreeBytes;
    uint32_t BytesPerCluster;
    uint32_t BytesPerSector;
    uint32_t SerialNumber;
    uint32_t ReadOnly;
    char FileSystem[16];
    char Label[32];
} HZ_FILE_VOLUME_INFORMATION;

/* QueryDirectoryFile returns as many fixed-size entries as fit; STATUS_NO_MORE_FILES at the end. */
#define HZ_DIRECTORY_RESTART_SCAN 0x1u

typedef struct _HZ_FILE_DIRECTORY_INFORMATION {
    uint64_t CreationTime;
    uint64_t LastWriteTime;
    uint64_t EndOfFile;
    uint64_t AllocationSize;
    uint32_t Attributes;
    uint32_t NameLength; /* bytes, without the terminator */
    char Name[256];      /* UTF-8, NUL-terminated */
} HZ_FILE_DIRECTORY_INFORMATION;

/* Device I/O control codes */
#define IOCTL_DISK_GET_GEOMETRY 0x00070000u       /* out: HZ_DISK_GEOMETRY */
#define IOCTL_DISK_GET_PARTITION_INFO 0x00074004u /* out: HZ_PARTITION_INFORMATION */
#define IOCTL_DISK_GET_INFO 0x00074008u           /* whole disk (DR0); out: HZ_DISK_INFORMATION (M10) */
/* Whole disk (DR0), opened for writing: reads the partition table again (after a program wrote
   one) and updates the partition devices. STATUS_DEVICE_BUSY while a volume on the disk is
   mounted. Out (optional): uint32_t partition count. (M10) */
#define IOCTL_DISK_UPDATE_PARTITIONS 0x0007C00Cu

typedef struct _HZ_DISK_GEOMETRY {
    uint64_t SectorCount;
    uint32_t BytesPerSector;
    uint32_t Reserved;
} HZ_DISK_GEOMETRY;

#define HZ_PARTITION_STYLE_RAW 0u
#define HZ_PARTITION_STYLE_MBR 1u
#define HZ_PARTITION_STYLE_GPT 2u

typedef struct _HZ_PARTITION_INFORMATION {
    uint64_t StartingOffset; /* bytes */
    uint64_t Length;         /* bytes */
    uint32_t Number;         /* 0 = the whole disk */
    uint32_t Style;
    uint8_t TypeGuid[16];    /* GPT; for MBR byte 0 is the partition type */
    char Name[40];           /* GPT name (UTF-8) */
    uint8_t UniqueGuid[16];  /* GPT (M10) */
} HZ_PARTITION_INFORMATION;

/* IOCTL_DISK_GET_INFO */
#define HZ_DISK_READ_ONLY 0x1u   /* write-protected media */
#define HZ_DISK_IN_USE 0x2u      /* a volume on it is mounted (the partition table cannot change) */
#define HZ_DISK_SYSTEM 0x4u      /* it holds the running system volume (C:) */

typedef struct _HZ_DISK_INFORMATION {
    char Model[48];
    uint64_t SectorCount;
    uint32_t BytesPerSector;
    uint32_t Style;          /* HZ_PARTITION_STYLE_* */
    uint32_t PartitionCount;
    uint32_t Flags;          /* HZ_DISK_* */
    uint32_t DiskNumber;
    uint32_t Reserved;
} HZ_DISK_INFORMATION;

/* Input devices: ReadFile on \Device\Keyboard0 / \Device\Mouse0 returns whole events and blocks
   until at least one is available. */
#define HZ_KEY_BREAK 0x0001u    /* key released */
#define HZ_KEY_EXTENDED 0x0002u /* E0-prefixed scan code */

#define HZ_MOD_SHIFT 0x0001u
#define HZ_MOD_CTRL 0x0002u
#define HZ_MOD_ALT 0x0004u
#define HZ_MOD_CAPS_LOCK 0x0010u
#define HZ_MOD_NUM_LOCK 0x0020u

typedef struct _HZ_KEY_EVENT {
    uint64_t TimeUs;     /* uptime */
    uint16_t ScanCode;   /* set 1, without the break bit */
    uint16_t Flags;      /* HZ_KEY_* */
    uint16_t Modifiers;  /* HZ_MOD_* after this event */
    uint8_t VirtualKey;  /* HZ_VK_* (ASCII for letters and digits) */
    uint8_t Character;   /* US layout, 0 if none */
} HZ_KEY_EVENT;

#define HZ_MOUSE_LEFT 0x1u
#define HZ_MOUSE_RIGHT 0x2u
#define HZ_MOUSE_MIDDLE 0x4u

typedef struct _HZ_MOUSE_EVENT {
    uint64_t TimeUs;
    int16_t DeltaX;   /* right positive */
    int16_t DeltaY;   /* down positive (screen direction) */
    int16_t Wheel;
    uint16_t Buttons; /* HZ_MOUSE_* held after this event */
} HZ_MOUSE_EVENT;

/* Virtual keys beyond ASCII letters/digits/space */
#define HZ_VK_BACK 0x08
#define HZ_VK_TAB 0x09
#define HZ_VK_RETURN 0x0D
#define HZ_VK_SHIFT 0x10
#define HZ_VK_CONTROL 0x11
#define HZ_VK_MENU 0x12 /* Alt */
#define HZ_VK_CAPITAL 0x14
#define HZ_VK_ESCAPE 0x1B
#define HZ_VK_SPACE 0x20
#define HZ_VK_PRIOR 0x21
#define HZ_VK_NEXT 0x22
#define HZ_VK_END 0x23
#define HZ_VK_HOME 0x24
#define HZ_VK_LEFT 0x25
#define HZ_VK_UP 0x26
#define HZ_VK_RIGHT 0x27
#define HZ_VK_DOWN 0x28
#define HZ_VK_INSERT 0x2D
#define HZ_VK_DELETE 0x2E
#define HZ_VK_LWIN 0x5B
#define HZ_VK_F1 0x70 /* .. F12 = 0x7B */
#define HZ_VK_NUMLOCK 0x90
#define HZ_VK_SCROLL 0x91
#define HZ_VK_OEM 0xBA /* punctuation keys: see Character */

/* ---- Power ------------------------------------------------------------------------------------ */

#define HZ_SHUTDOWN_POWER_OFF 0u
#define HZ_SHUTDOWN_REBOOT 1u

HZ_EXTERN_C_END

#endif /* HZAPI_H */
