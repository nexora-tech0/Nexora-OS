/*
 * Nexora OS - ex/svctable.c
 * The system-service table (indexed by HZ_SYSTEM_SERVICE from uapi/hzapi.h) and executive
 * initialization. Each entry gives the routine, its argument count (arguments beyond four are
 * copied from the user stack by ke/syscall.c) and a name for logs.
 */
#include "ex.h"
#include "io.h"
#include "mm.h"
#include "ob.h"
#include "ps.h"

void ExpInitializeEventType(void);

static const KSYSTEM_SERVICE ExpServiceTable[HzSysServiceCount] = {
    [HzSysClose] = {NtClose, 1, "Close"},
    [HzSysDisplayString] = {NtDisplayString, 2, "DisplayString"},
    [HzSysTerminateProcess] = {NtTerminateProcess, 2, "TerminateProcess"},
    [HzSysTerminateThread] = {NtTerminateThread, 2, "TerminateThread"},
    [HzSysCreateThread] = {NtCreateThread, 6, "CreateThread"},
    [HzSysAllocateVirtualMemory] = {NtAllocateVirtualMemory, 5, "AllocateVirtualMemory"},
    [HzSysFreeVirtualMemory] = {NtFreeVirtualMemory, 3, "FreeVirtualMemory"},
    [HzSysQueryVirtualMemory] = {NtQueryVirtualMemory, 4, "QueryVirtualMemory"},
    [HzSysProtectVirtualMemory] = {NtProtectVirtualMemory, 5, "ProtectVirtualMemory"},
    [HzSysCreateEvent] = {NtCreateEvent, 3, "CreateEvent"},
    [HzSysSetEvent] = {NtSetEvent, 2, "SetEvent"},
    [HzSysResetEvent] = {NtResetEvent, 1, "ResetEvent"},
    [HzSysWaitForSingleObject] = {NtWaitForSingleObject, 3, "WaitForSingleObject"},
    [HzSysWaitForMultipleObjects] = {NtWaitForMultipleObjects, 4, "WaitForMultipleObjects"},
    [HzSysDelayExecution] = {NtDelayExecution, 1, "DelayExecution"},
    [HzSysYieldExecution] = {NtYieldExecution, 0, "YieldExecution"},
    [HzSysQuerySystemInformation] = {NtQuerySystemInformation, 4, "QuerySystemInformation"},
    [HzSysQueryUptime] = {NtQueryUptime, 1, "QueryUptime"},
    [HzSysCreateProcess] = {NtCreateProcess, 4, "CreateProcess"},
    [HzSysQueryInformationProcess] = {NtQueryInformationProcess, 4, "QueryInformationProcess"},
    [HzSysQueryInformationThread] = {NtQueryInformationThread, 4, "QueryInformationThread"},
    [HzSysCreateFile] = {NtCreateFile, 6, "CreateFile"},
    [HzSysReadFile] = {NtReadFile, 5, "ReadFile"},
    [HzSysWriteFile] = {NtWriteFile, 5, "WriteFile"},
    [HzSysQueryInformationFile] = {NtQueryInformationFile, 4, "QueryInformationFile"},
    [HzSysSetInformationFile] = {NtSetInformationFile, 4, "SetInformationFile"},
    [HzSysQueryDirectoryFile] = {NtQueryDirectoryFile, 5, "QueryDirectoryFile"},
    [HzSysDeviceIoControlFile] = {NtDeviceIoControlFile, 7, "DeviceIoControlFile"},
    [HzSysShutdownSystem] = {NtShutdownSystem, 1, "ShutdownSystem"},
    [HzSysQuerySystemTime] = {NtQuerySystemTime, 1, "QuerySystemTime"},
    [HzSysCreateSection] = {NtCreateSection, 2, "CreateSection"},
    [HzSysMapViewOfSection] = {NtMapViewOfSection, 5, "MapViewOfSection"},
    [HzSysUnmapViewOfSection] = {NtUnmapViewOfSection, 2, "UnmapViewOfSection"},
    [HzSysCreatePipe] = {NtCreatePipe, 3, "CreatePipe"},
    [HzSysCreateProcessEx] = {NtCreateProcessEx, 5, "CreateProcessEx"},
    [HzSysOpenProcess] = {NtOpenProcess, 3, "OpenProcess"},
    [HzSysResolveHostA] = {NtResolveHostA, 2, "ResolveHostA"},
    [HzSysTcpConnect] = {NtTcpConnect, 3, "TcpConnect"},
    [HzSysTcpSend] = {NtTcpSend, 4, "TcpSend"},
    [HzSysTcpReceive] = {NtTcpReceive, 5, "TcpReceive"},
    [HzSysTcpClose] = {NtTcpClose, 1, "TcpClose"},
    [HzSysGenerateRandomBytes] = {NtGenerateRandomBytes, 2, "GenerateRandomBytes"},
    [HzSysWinGetStdHandle] = {NtWinGetStdHandle, 1, "WinGetStdHandle"},
    [HzSysWinWriteFile] = {NtWinWriteFile, 5, "WinWriteFile"},
    [HzSysWinExitProcess] = {NtWinExitProcess, 1, "WinExitProcess"},
    [HzSysQueryWinCompatReport] = {NtQueryWinCompatReport, 3, "QueryWinCompatReport"},
    [HzSysWinHeapAlloc] = {NtWinHeapAlloc, 4, "WinHeapAlloc"},
    [HzSysWinHeapFree] = {NtWinHeapFree, 3, "WinHeapFree"},
    [HzSysWinGetModuleHandle] = {NtWinGetModuleHandle, 3, "WinGetModuleHandle"},
    [HzSysWinSocketApi] = {NtWinSocketApi, 8, "WinSocketApi"},
    [HzSysWinGetProcAddress] = {NtWinGetProcAddress, 3, "WinGetProcAddress"},
    [HzSysWinCreateFile] = {NtWinCreateFile, 4, "WinCreateFile"},
    [HzSysWinGetFileAttributes] = {NtWinGetFileAttributes, 1, "WinGetFileAttributes"},
    [HzSysWinGetFileVersionInfoSizeW] = {NtWinGetFileVersionInfoSizeW, 2, "WinGetFileVersionInfoSizeW"},
    [HzSysWinGetFileVersionInfoW] = {NtWinGetFileVersionInfoW, 4, "WinGetFileVersionInfoW"},
    [HzSysWinVerQueryValueW] = {NtWinVerQueryValueW, 4, "WinVerQueryValueW"},
};

void ExInitializeSystem(void)
{
    for (uint32_t i = 0; i < HzSysServiceCount; i++) {
        if (!ExpServiceTable[i].Routine)
            KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, PS_INIT_FAILURE_OBJECT_TYPES, 0x31, i, 0);
    }
    ExpInitializeEventType();
    KeSetSystemServiceTable(ExpServiceTable, HzSysServiceCount);
}
