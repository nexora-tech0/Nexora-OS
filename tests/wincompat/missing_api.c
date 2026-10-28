/* Deliberately imports an API outside the current WinCompat surface; it must never execute. */
typedef unsigned long DWORD;

__declspec(dllimport) DWORD __stdcall GetCurrentDirectoryA(DWORD Capacity, char *Buffer);

volatile DWORD WinCompatUnexpectedTickCount;

void WinCompatMissingApiProbe(void)
{
    WinCompatUnexpectedTickCount = GetCurrentDirectoryA(0, 0);
}
