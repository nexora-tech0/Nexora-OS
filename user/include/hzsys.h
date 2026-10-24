/*
 * Nexora OS - user/include/hzsys.h
 * User-mode system interface: the system-call stubs (hzsys.lib, the Nexora OS counterpart of a
 * native API library) and the small runtime every program links (hzrt.lib: process start-up,
 * thread start-up, formatted output, memory/string routines).
 *
 * Programs define `int main(int argc, char **argv)`; HzProcessStartup (the PE entry point) splits
 * the PEB command line, calls main and terminates the process with its return value.
 */
#ifndef HZSYS_H
#define HZSYS_H

#include "uapi/hzapi.h"

HZ_EXTERN_C_BEGIN

/* ---- System calls (user/lib/hzsys/hzsys.asm; see uapi/hzapi.h for the argument contracts) ---- */

STATUS NtClose(HANDLE Handle);
STATUS NtDisplayString(const char *Text, uint64_t Length);
STATUS NtTerminateProcess(HANDLE Process, STATUS ExitStatus);
STATUS NtTerminateThread(HANDLE Thread, STATUS ExitStatus);
STATUS NtCreateThread(HANDLE Process, void *StartAddress, uint64_t Argument1, uint64_t Argument2, HANDLE *Thread,
                      uint32_t *ThreadId);
STATUS NtAllocateVirtualMemory(HANDLE Process, void **BaseAddress, uint64_t *RegionSize, uint32_t AllocationType,
                               uint32_t Protect);
STATUS NtFreeVirtualMemory(HANDLE Process, void *BaseAddress, uint32_t FreeType);
STATUS NtQueryVirtualMemory(HANDLE Process, const void *Address, HZ_MEMORY_BASIC_INFORMATION *Information,
                            uint64_t Length);
STATUS NtProtectVirtualMemory(HANDLE Process, void *BaseAddress, uint64_t Size, uint32_t NewProtect,
                              uint32_t *OldProtect);
STATUS NtCreateEvent(HANDLE *Event, uint32_t EventType, uint32_t InitialState);
STATUS NtSetEvent(HANDLE Event, int32_t *PreviousState);
STATUS NtResetEvent(HANDLE Event);
STATUS NtWaitForSingleObject(HANDLE Object, uint32_t Reserved, const uint64_t *TimeoutUs);
STATUS NtWaitForMultipleObjects(uint32_t Count, const HANDLE *Objects, uint32_t WaitType, const uint64_t *TimeoutUs);
STATUS NtDelayExecution(uint64_t Microseconds);
STATUS NtYieldExecution(void);
STATUS NtQuerySystemInformation(uint32_t Class, void *Buffer, uint32_t Length, uint32_t *ReturnLength);
STATUS NtQueryUptime(uint64_t *Microseconds);
STATUS NtCreateProcess(const char *ImagePath, const char *CommandLine, HANDLE *Process, uint32_t *ProcessId);
STATUS NtQueryInformationProcess(HANDLE Process, uint32_t Class, void *Buffer, uint32_t Length);
STATUS NtQueryInformationThread(HANDLE Thread, uint32_t Class, void *Buffer, uint32_t Length);
STATUS NtCreateFile(HANDLE *File, ACCESS_MASK DesiredAccess, const char *Path, uint32_t Disposition, uint32_t Options,
                    uint32_t *Result);
STATUS NtReadFile(HANDLE File, void *Buffer, uint32_t Length, const uint64_t *ByteOffset, uint32_t *BytesRead);
STATUS NtWriteFile(HANDLE File, const void *Buffer, uint32_t Length, const uint64_t *ByteOffset,
                   uint32_t *BytesWritten);
STATUS NtQueryInformationFile(HANDLE File, uint32_t Class, void *Buffer, uint32_t Length);
STATUS NtSetInformationFile(HANDLE File, uint32_t Class, const void *Buffer, uint32_t Length);
STATUS NtQueryDirectoryFile(HANDLE Directory, HZ_FILE_DIRECTORY_INFORMATION *Buffer, uint32_t Length, uint32_t Flags,
                            uint32_t *EntriesReturned);
STATUS NtDeviceIoControlFile(HANDLE File, uint32_t Code, const void *Input, uint32_t InputLength, void *Output,
                             uint32_t OutputLength, uint32_t *BytesReturned);
STATUS NtShutdownSystem(uint32_t Action);
STATUS NtQuerySystemTime(uint64_t *Time);
STATUS NtCreateSection(HANDLE *Section, uint64_t Size);
STATUS NtMapViewOfSection(HANDLE Section, HANDLE Process, void **Base, uint64_t *ViewSize, uint32_t PageProtection);
STATUS NtUnmapViewOfSection(HANDLE Process, void *Base);
STATUS NtCreatePipe(HANDLE *ReadEnd, HANDLE *WriteEnd, uint32_t Size);
STATUS NtCreateProcessEx(const char *ImagePath, const char *CommandLine, const HZ_PROCESS_PARAMETERS *Parameters,
                         HANDLE *Process, uint32_t *ProcessId);
STATUS NtOpenProcess(HANDLE *Process, ACCESS_MASK Access, uint32_t ProcessId);
/* Synchronous IPv4 A-record lookup using the configured DNS server. */
STATUS NtResolveHostA(const char *HostName, uint8_t Ipv4Address[4]);

static inline STATUS HzResolveHostA(const char *HostName, uint8_t Ipv4Address[4])
{
    return NtResolveHostA(HostName, Ipv4Address);
}

/* ---- Runtime (user/lib/hzrt) ------------------------------------------------------------------- */

static inline HZ_TEB *HzCurrentTeb(void)
{
    HZ_TEB *Teb;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(Teb));
    return Teb;
}

static inline HZ_PEB *HzCurrentPeb(void)
{
    return HzCurrentTeb()->Peb;
}

typedef uint32_t (*HZ_THREAD_ROUTINE)(void *Parameter);

/* Starts Routine(Parameter) in a new thread of this process; its return value is the thread's
   exit status. *Thread receives a handle (close it with NtClose). */
STATUS HzCreateThread(HZ_THREAD_ROUTINE Routine, void *Parameter, HANDLE *Thread, uint32_t *ThreadId);

/* printf-style output, one line per call: to the standard output if the process has one (M9),
   else to the kernel log (and the boot console). HzLog always goes to the kernel log. */
int HzPrint(const char *Format, ...) HZ_PRINTF(1, 2);
int HzLog(const char *Format, ...) HZ_PRINTF(1, 2);

/* Standard handles from the PEB (0 if the creator gave none). */
#define HZ_STD_INPUT 0u
#define HZ_STD_OUTPUT 1u
#define HZ_STD_ERROR 2u
HANDLE HzGetStandardHandle(uint32_t Which);
STATUS HzWriteStandard(uint32_t Which, const char *Text, size_t Length);
int HzFormat(char *Buffer, size_t Size, const char *Format, ...) HZ_PRINTF(3, 4);

HZ_NORETURN void HzExitProcess(STATUS ExitStatus);

/* rtl/string.c, compiled into hzrt */
void *memcpy(void *Dest, const void *Src, size_t Count);
void *memmove(void *Dest, const void *Src, size_t Count);
void *memset(void *Dest, int Value, size_t Count);
int memcmp(const void *A, const void *B, size_t Count);
size_t strlen(const char *Str);
int strcmp(const char *A, const char *B);
int strncmp(const char *A, const char *B, size_t Count);

/* user/lib/hzrt/heap.cpp: the process heap (16-byte aligned blocks) */
void *malloc(size_t Size);
void *calloc(size_t Count, size_t Size);
void *realloc(void *Pointer, size_t Size);
void free(void *Pointer);

HZ_EXTERN_C_END

#endif /* HZSYS_H */
