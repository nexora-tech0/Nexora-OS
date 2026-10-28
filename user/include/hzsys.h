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
STATUS NtTcpConnect(uint32_t IPv4Address, uint16_t Port, HZ_TCP_HANDLE *Connection);
STATUS NtTcpSend(HZ_TCP_HANDLE Connection, const void *Buffer, uint32_t Length, uint32_t *BytesSent);
STATUS NtTcpReceive(HZ_TCP_HANDLE Connection, void *Buffer, uint32_t Capacity, uint64_t TimeoutUs,
                    uint32_t *BytesReceived);
STATUS NtTcpClose(HZ_TCP_HANDLE Connection);
/* Fills 1..HZ_RANDOM_MAX_BYTES using CPU RDRAND only; returns STATUS_NOT_SUPPORTED if absent. */
STATUS NtGenerateRandomBytes(void *Buffer, uint32_t Length);
/* Returns a bounded, human-readable compatibility report for an image path. */
STATUS NtQueryWinCompatReport(const char *ImagePath, char *Buffer, uint32_t Capacity);

/* Kernel-backed cryptographic random bytes. No time, TSC, process, or software fallback is used.
   Requests are bounded to HZ_RANDOM_MAX_BYTES. Unsupported RDRAND returns STATUS_NOT_SUPPORTED;
   if the instruction exhausts its bounded retries, STATUS_UNSUCCESSFUL is returned. */
static inline STATUS HzGenerateRandomBytes(void *Buffer, uint32_t Length)
{
    return NtGenerateRandomBytes(Buffer, Length);
}

static inline STATUS HzResolveHostA(const char *HostName, uint8_t Ipv4Address[4])
{
    return NtResolveHostA(HostName, Ipv4Address);
}

/* Minimal IPv4 TCP client API. IPv4Address is written in network-order notation
   (for example 0xC0000201 for 192.0.2.1). Connect returns a positive socket token on success or
   a negative STATUS on failure. Receive timeout is in milliseconds; zero waits indefinitely.
   Receive returns STATUS_SUCCESS and zero BytesReceived after the peer closes its write side.
   Send/receive calls transfer no more than 4096 bytes per call. Sockets are process-owned opaque
   tokens, not pointers; connect and send use a six-second timeout, and close invalidates the
   token even if graceful shutdown times out (up to eight seconds). */
#define HZ_TCP_DEFAULT_TIMEOUT_US 6000000ull
#define HZ_TCP_MAX_TRANSFER 4096u

static inline HZ_TCP_HANDLE HzTcpConnect(uint32_t IPv4Address, uint16_t Port)
{
    HZ_TCP_HANDLE Connection = HZ_TCP_INVALID_HANDLE;
    STATUS Status = NtTcpConnect(IPv4Address, Port, &Connection);
    return HZ_SUCCESS(Status) ? Connection : (HZ_TCP_HANDLE)Status;
}

static inline STATUS HzTcpSend(HZ_TCP_HANDLE Connection, const void *Buffer, uint32_t Length,
                               uint32_t *BytesSent)
{
    return NtTcpSend(Connection, Buffer, Length, BytesSent);
}

static inline STATUS HzTcpReceive(HZ_TCP_HANDLE Connection, void *Buffer, uint32_t Capacity,
                                  uint32_t TimeoutMs, uint32_t *BytesReceived)
{
    uint64_t TimeoutUs = (uint64_t)TimeoutMs * 1000ull;
    return NtTcpReceive(Connection, Buffer, Capacity, TimeoutUs, BytesReceived);
}

static inline STATUS HzTcpClose(HZ_TCP_HANDLE Connection)
{
    return NtTcpClose(Connection);
}

/* ---- Runtime (user/lib/hzrt) ------------------------------------------------------------------- */

static inline HZ_TEB *HzCurrentTeb(void)
{
    HZ_TEB *Teb;
    __asm__ volatile("mov %%gs:0x30, %0" : "=r"(Teb));
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
