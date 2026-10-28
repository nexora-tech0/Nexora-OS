/* Ordinary PE32+ Winsock client: fetches the host fixture over QEMU user networking. */
typedef unsigned long DWORD;
typedef unsigned short WORD;
typedef unsigned long long UINT_PTR;
typedef struct { WORD Family; unsigned short Port; unsigned long Address; char Zero[8]; } SOCKADDR_IN;
typedef struct {
    WORD Version, HighVersion;
    char Description[257], SystemStatus[129];
    unsigned short MaxSockets, MaxUdpDatagram;
    const char *VendorInfo;
} WSADATA;
typedef struct _ADDRINFOA {
    int Flags, Family, SocketType, Protocol;
    unsigned long long AddressLength;
    char *CanonicalName;
    void *Address;
    struct _ADDRINFOA *Next;
} ADDRINFOA;
typedef struct _ADDRINFOW {
    int Flags, Family, SocketType, Protocol;
    unsigned long long AddressLength;
    unsigned short *CanonicalName;
    void *Address;
    struct _ADDRINFOW *Next;
} ADDRINFOW;

__declspec(dllimport) char *__stdcall GetCommandLineA(void);
__declspec(dllimport) __declspec(noreturn) void __stdcall ExitProcess(DWORD);
__declspec(dllimport) int __stdcall WSAStartup(WORD, WSADATA *);
__declspec(dllimport) int __stdcall WSACleanup(void);
__declspec(dllimport) UINT_PTR __stdcall socket(int, int, int);
__declspec(dllimport) int __stdcall connect(UINT_PTR, const SOCKADDR_IN *, int);
__declspec(dllimport) int __stdcall send(UINT_PTR, const char *, int, int);
__declspec(dllimport) int __stdcall recv(UINT_PTR, char *, int, int);
__declspec(dllimport) int __stdcall closesocket(UINT_PTR);
__declspec(dllimport) int __stdcall WSAGetLastError(void);
__declspec(dllimport) int __stdcall getaddrinfo(const char *, const char *, const ADDRINFOA *, ADDRINFOA **);
__declspec(dllimport) void __stdcall freeaddrinfo(ADDRINFOA *);
__declspec(dllimport) int __stdcall GetAddrInfoW(const unsigned short *, const unsigned short *,
                                                  const ADDRINFOW *, ADDRINFOW **);
__declspec(dllimport) void __stdcall FreeAddrInfoW(ADDRINFOW *);

static void Fail(unsigned Code, UINT_PTR Socket)
{
    if (Socket != (UINT_PTR)-1)
        closesocket(Socket);
    WSACleanup();
    ExitProcess(Code);
}

void WinCompatNetworkProbe(void)
{
    WSADATA Data;
    if (WSAStartup(0x0101, &Data) != 10092)
        ExitProcess(42);
    if (WSAStartup(0x0202, &Data) || Data.Version != 0x0202 ||
        WSAStartup(0x0202, &Data) || WSACleanup())
        ExitProcess(31);
    char *Command = GetCommandLineA();
    if (!Command)
        Fail(32, (UINT_PTR)-1);
    while (*Command && *Command != ' ' && *Command != '\t') ++Command;
    while (*Command == ' ' || *Command == '\t') ++Command;
    unsigned Port = 0;
    while (*Command >= '0' && *Command <= '9') {
        Port = Port * 10 + (unsigned)(*Command++ - '0');
        if (Port > 65535) Fail(33, (UINT_PTR)-1);
    }
    if (!Port) Fail(34, (UINT_PTR)-1);

    UINT_PTR S = socket(2, 1, 6);
    if (S == (UINT_PTR)-1) Fail(35, S);
    char Service[6];
    unsigned PortValue = Port;
    int ServiceLength = 0;
    do {
        Service[ServiceLength++] = (char)('0' + PortValue % 10);
        PortValue /= 10;
    } while (PortValue);
    for (int Left = 0, Right = ServiceLength - 1; Left < Right; ++Left, --Right) {
        char Swap = Service[Left]; Service[Left] = Service[Right]; Service[Right] = Swap;
    }
    Service[ServiceLength] = 0;
    ADDRINFOA Hints = {0};
    Hints.Family = 2;
    Hints.SocketType = 1;
    Hints.Protocol = 6;
    Hints.Flags = 0;
    ADDRINFOA *Info = 0;
    if (getaddrinfo("example.com", 0, &Hints, &Info) != 0 || !Info ||
        Info->Family != 2 || Info->SocketType != 1 || Info->AddressLength != 16)
        Fail(43, S);
    freeaddrinfo(Info);

    static const unsigned short WideHost[] = {'1','0','.','0','.','2','.','2',0};
    static const unsigned short WideService[] = {'8','0',0};
    ADDRINFOW WideHints = {0};
    WideHints.Family = 2;
    WideHints.SocketType = 1;
    WideHints.Protocol = 6;
    WideHints.Flags = 0x0006;
    ADDRINFOW *WideInfo = 0;
    if (GetAddrInfoW(WideHost, WideService, &WideHints, &WideInfo) != 0 || !WideInfo ||
        WideInfo->Family != 2 || WideInfo->AddressLength != 16 || !WideInfo->CanonicalName ||
        WideInfo->CanonicalName[0] != '1')
        Fail(44, S);
    FreeAddrInfoW(WideInfo);

    Hints.Flags = 0x0006; /* AI_CANONNAME | AI_NUMERICHOST */
    Info = 0;
    if (getaddrinfo("10.0.2.2", Service, &Hints, &Info) != 0 || !Info ||
        Info->Family != 2 || Info->SocketType != 1 || Info->AddressLength != 16 ||
        !Info->CanonicalName || Info->CanonicalName[0] != '1')
        Fail(36, S);
    if (connect(S, Info->Address, (int)Info->AddressLength) != 0) {
        freeaddrinfo(Info);
        Fail(41, S);
    }
    freeaddrinfo(Info);
    static const char Request[] = "GET / HTTP/1.0\r\nHost: nexora.test\r\n\r\n";
    if (send(S, Request, sizeof(Request) - 1, 0) != (int)(sizeof(Request) - 1)) Fail(37, S);
    char Response[512];
    int Count = recv(S, Response, sizeof(Response) - 1, 0);
    if (Count <= 0) Fail(38, S);
    Response[Count] = 0;
    for (int I = 0; I + 7 < Count; ++I)
        if (Response[I] == 'H' && Response[I+1] == 'T' && Response[I+2] == 'T' &&
            Response[I+3] == 'P' && Response[I+4] == '/' && Response[I+5] == '1' &&
            Response[I+6] == '.' && (Response[I+7] == '0' || Response[I+7] == '1')) {
            if (closesocket(S) || WSACleanup()) ExitProcess(39);
            ExitProcess(0);
        }
    Fail(40, S);
}
