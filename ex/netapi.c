/*
 * Nexora OS - ex/netapi.c
 * Application-facing network services.
 */
#include "ex.h"
#include "ke.h"
#include "mm.h"
#include "net.h"
#include "netdns.h"
#include "ps.h"

typedef struct _EXP_TCP_HANDLE_ENTRY {
    HZ_TCP_HANDLE Token;
    EPROCESS *Owner;
    NET_TCP_CONNECTION *Connection;
} EXP_TCP_HANDLE_ENTRY;

static EXP_TCP_HANDLE_ENTRY ExpTcpHandles[NET_TCP_MAX_CONNECTIONS];
static KSPIN_LOCK ExpTcpHandleLock;
static volatile uint32_t ExpTcpHandleLockState;
static uint32_t ExpTcpNextToken = 0x1000u;

static void ExpInitializeTcpHandleLock(void)
{
    uint32_t Expected = 0;
    if (__atomic_compare_exchange_n(&ExpTcpHandleLockState, &Expected, 1, false, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE)) {
        KeInitializeSpinLock(&ExpTcpHandleLock);
        __atomic_store_n(&ExpTcpHandleLockState, 2, __ATOMIC_RELEASE);
        return;
    }
    while (__atomic_load_n(&ExpTcpHandleLockState, __ATOMIC_ACQUIRE) != 2)
        __asm__ volatile("pause");
}

static NET_TCP_CONNECTION *ExpLookupTcpHandle(HZ_TCP_HANDLE Token)
{
    if (!Token)
        return NULL;
    ExpInitializeTcpHandleLock();
    NET_TCP_CONNECTION *Connection = NULL;
    KIRQL OldIrql;
    KeAcquireSpinLock(&ExpTcpHandleLock, &OldIrql);
    EPROCESS *Owner = PsGetCurrentProcess();
    for (uint32_t i = 0; i < NET_TCP_MAX_CONNECTIONS; ++i) {
        if (ExpTcpHandles[i].Token == Token && ExpTcpHandles[i].Owner == Owner) {
            Connection = ExpTcpHandles[i].Connection;
            break;
        }
    }
    KeReleaseSpinLock(&ExpTcpHandleLock, OldIrql);
    return Connection;
}

typedef struct _EXP_WINSOCK_ENTRY {
    uint64_t Socket;
    EPROCESS *Owner;
    HZ_TCP_HANDLE TcpToken;
    bool Connecting;
} EXP_WINSOCK_ENTRY;

#define EXP_WSA_ADDRINFO_ALLOCATIONS 16u
typedef struct _EXP_WSA_ADDRINFO_ALLOCATION {
    uint64_t Base;
    EPROCESS *Owner;
} EXP_WSA_ADDRINFO_ALLOCATION;

typedef struct _EXP_WSA_PROCESS {
    EPROCESS *Owner;
    uint32_t StartupReferences;
} EXP_WSA_PROCESS;

typedef struct _EXP_WSA_ADDRINFOA {
    int32_t Flags;
    int32_t Family;
    int32_t SocketType;
    int32_t Protocol;
    uint64_t AddressLength;
    const char *CanonicalName;
    void *Address;
    struct _EXP_WSA_ADDRINFOA *Next;
} EXP_WSA_ADDRINFOA;

typedef struct _EXP_WSA_SOCKADDR_IN {
    uint16_t Family;
    uint16_t Port;
    uint8_t Address[4];
    uint8_t Padding[8];
} EXP_WSA_SOCKADDR_IN;

_Static_assert(sizeof(EXP_WSA_ADDRINFOA) == 48, "64-bit ADDRINFOA layout");
_Static_assert(sizeof(EXP_WSA_SOCKADDR_IN) == 16, "IPv4 sockaddr layout");

static EXP_WINSOCK_ENTRY ExpWinSockets[NET_TCP_MAX_CONNECTIONS];
static EXP_WSA_ADDRINFO_ALLOCATION ExpWinAddrInfo[EXP_WSA_ADDRINFO_ALLOCATIONS];
static EXP_WSA_PROCESS ExpWinSockProcesses[NET_TCP_MAX_CONNECTIONS];
static uint64_t ExpWinNextSocket = 0x50000000ull;

static HZ_TCP_HANDLE ExpLookupWinSocket(uint64_t Socket)
{
    HZ_TCP_HANDLE Token = HZ_TCP_INVALID_HANDLE;
    ExpInitializeTcpHandleLock();
    KIRQL OldIrql;
    KeAcquireSpinLock(&ExpTcpHandleLock, &OldIrql);
    EPROCESS *Owner = PsGetCurrentProcess();
    for (uint32_t Index = 0; Index < NET_TCP_MAX_CONNECTIONS; ++Index) {
        if (ExpWinSockets[Index].Socket == Socket && ExpWinSockets[Index].Owner == Owner) {
            Token = ExpWinSockets[Index].TcpToken;
            break;
        }
    }
    KeReleaseSpinLock(&ExpTcpHandleLock, OldIrql);
    return Token;
}

static STATUS ExpValidateOutput(void *UserBuffer, uint32_t Length)
{
    if (!Length)
        return STATUS_SUCCESS;
    uint8_t Probe[NET_TCP_RX_BUFFER_SIZE];
    if (Length > sizeof(Probe))
        return STATUS_BUFFER_TOO_SMALL;
    /* Preserve the caller's bytes while checking the range is both readable and writable. */
    STATUS Status = MmCopyFromUser(Probe, UserBuffer, Length);
    if (!HZ_SUCCESS(Status))
        return Status;
    return MmCopyToUser(UserBuffer, Probe, Length);
}

STATUS NtResolveHostA(const uint64_t *Args)
{
    char HostName[NET_DNS_MAX_NAME_LENGTH + 1];
    uint8_t Address[4] = {0};
    STATUS Status = MmCopyStringFromUser(HostName, sizeof(HostName), (const char *)Args[0]);
    if (!HZ_SUCCESS(Status))
        return Status;

    Status = MmCopyToUser((void *)Args[1], Address, sizeof(Address));
    if (!HZ_SUCCESS(Status))
        return Status;

    Status = NetDnsResolveA(HostName, Address);
    if (!HZ_SUCCESS(Status))
        return Status;
    return MmCopyToUser((void *)Args[1], Address, sizeof(Address));
}

static STATUS ExpCreateTcpHandle(uint32_t AddressValue, uint16_t Port, HZ_TCP_HANDLE *TokenOut)
{
    if (!TokenOut)
        return STATUS_INVALID_PARAMETER;
    *TokenOut = HZ_TCP_INVALID_HANDLE;
    uint8_t Address[4] = {(uint8_t)(AddressValue >> 24), (uint8_t)(AddressValue >> 16),
                          (uint8_t)(AddressValue >> 8), (uint8_t)AddressValue};
    NET_TCP_CONNECTION *Connection = NULL;
    STATUS Status = NetTcpCreate(&Connection);
    if (!HZ_SUCCESS(Status))
        return Status;
    Status = NetTcpConnect(Connection, Address, Port, NET_TCP_CONNECT_TIMEOUT_US);
    if (!HZ_SUCCESS(Status)) {
        NetTcpDestroy(Connection);
        return Status;
    }

    ExpInitializeTcpHandleLock();
    HZ_TCP_HANDLE Token = HZ_TCP_INVALID_HANDLE;
    KIRQL OldIrql;
    KeAcquireSpinLock(&ExpTcpHandleLock, &OldIrql);
    for (uint32_t i = 0; i < NET_TCP_MAX_CONNECTIONS; ++i) {
        if (ExpTcpHandles[i].Connection)
            continue;
        bool Collision;
        do {
            ExpTcpNextToken = ExpTcpNextToken >= 0x7FFFFFFEu ? 1u : ExpTcpNextToken + 1u;
            Token = (HZ_TCP_HANDLE)ExpTcpNextToken;
            Collision = false;
            for (uint32_t j = 0; j < NET_TCP_MAX_CONNECTIONS; ++j)
                if (ExpTcpHandles[j].Token == Token)
                    Collision = true;
        } while (Token == HZ_TCP_INVALID_HANDLE || Collision);
        ExpTcpHandles[i].Token = Token;
        ExpTcpHandles[i].Owner = PsGetCurrentProcess();
        ExpTcpHandles[i].Connection = Connection;
        break;
    }
    KeReleaseSpinLock(&ExpTcpHandleLock, OldIrql);
    if (!Token) {
        NetTcpClose(Connection);
        NetTcpDestroy(Connection);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    *TokenOut = Token;
    return STATUS_SUCCESS;
}

static STATUS ExpCloseTcpHandle(HZ_TCP_HANDLE Token)
{
    ExpInitializeTcpHandleLock();
    NET_TCP_CONNECTION *Connection = NULL;
    KIRQL OldIrql;
    KeAcquireSpinLock(&ExpTcpHandleLock, &OldIrql);
    EPROCESS *Owner = PsGetCurrentProcess();
    for (uint32_t Index = 0; Index < NET_TCP_MAX_CONNECTIONS; ++Index) {
        if (ExpTcpHandles[Index].Token == Token && ExpTcpHandles[Index].Owner == Owner) {
            Connection = ExpTcpHandles[Index].Connection;
            memset(&ExpTcpHandles[Index], 0, sizeof(ExpTcpHandles[Index]));
            break;
        }
    }
    KeReleaseSpinLock(&ExpTcpHandleLock, OldIrql);
    if (!Connection)
        return STATUS_INVALID_PARAMETER;
    STATUS Status = NetTcpClose(Connection);
    NetTcpDestroy(Connection);
    return Status;
}

STATUS NtTcpConnect(const uint64_t *Args)
{
    HZ_TCP_HANDLE Token = HZ_TCP_INVALID_HANDLE;
    STATUS Status = MmCopyToUser((void *)Args[2], &Token, sizeof(Token));
    if (!HZ_SUCCESS(Status))
        return Status;
    Status = ExpCreateTcpHandle((uint32_t)Args[0], (uint16_t)Args[1], &Token);
    if (!HZ_SUCCESS(Status))
        return Status;
    Status = MmCopyToUser((void *)Args[2], &Token, sizeof(Token));
    if (!HZ_SUCCESS(Status))
        (void)ExpCloseTcpHandle(Token);
    return Status;
}

STATUS NtTcpSend(const uint64_t *Args)
{
    uint32_t Zero = 0;
    if (Args[3]) {
        STATUS Probe = MmCopyToUser((void *)Args[3], &Zero, sizeof(Zero));
        if (!HZ_SUCCESS(Probe))
            return Probe;
    }
    uint32_t Length = (uint32_t)Args[2];
    if (Length > NET_TCP_TX_BUFFER_SIZE)
        return STATUS_BUFFER_TOO_SMALL;
    uint8_t Buffer[NET_TCP_TX_BUFFER_SIZE];
    STATUS Status = MmCopyFromUser(Buffer, (const void *)Args[1], Length);
    if (!HZ_SUCCESS(Status))
        return Status;
    NET_TCP_CONNECTION *Connection = ExpLookupTcpHandle((HZ_TCP_HANDLE)Args[0]);
    if (!Connection)
        return STATUS_INVALID_PARAMETER;
    uint32_t Sent = 0;
    Status = NetTcpSend(Connection, Buffer, Length, &Sent, NET_SOCKET_DEFAULT_TIMEOUT_US);
    if (Args[3]) {
        STATUS CopyStatus = MmCopyToUser((void *)Args[3], &Sent, sizeof(Sent));
        if (!HZ_SUCCESS(CopyStatus))
            return CopyStatus;
    }
    return Status;
}

STATUS NtTcpReceive(const uint64_t *Args)
{
    uint32_t Zero = 0;
    if (Args[4]) {
        STATUS Probe = MmCopyToUser((void *)Args[4], &Zero, sizeof(Zero));
        if (!HZ_SUCCESS(Probe))
            return Probe;
    }
    uint32_t Capacity = (uint32_t)Args[2];
    if (Capacity > NET_TCP_RX_BUFFER_SIZE)
        return STATUS_BUFFER_TOO_SMALL;
    STATUS Status = ExpValidateOutput((void *)Args[1], Capacity);
    if (!HZ_SUCCESS(Status))
        return Status;
    NET_TCP_CONNECTION *Connection = ExpLookupTcpHandle((HZ_TCP_HANDLE)Args[0]);
    if (!Connection)
        return STATUS_INVALID_PARAMETER;
    uint8_t Buffer[NET_TCP_RX_BUFFER_SIZE];
    uint32_t Received = 0;
    Status = NetTcpReceive(Connection, Buffer, Capacity, &Received, Args[3]);
    if (HZ_SUCCESS(Status) && Received) {
        STATUS CopyStatus = MmCopyToUser((void *)Args[1], Buffer, Received);
        if (!HZ_SUCCESS(CopyStatus))
            return CopyStatus;
    }
    if (Args[4]) {
        STATUS CopyStatus = MmCopyToUser((void *)Args[4], &Received, sizeof(Received));
        if (!HZ_SUCCESS(CopyStatus))
            return CopyStatus;
    }
    return Status;
}

STATUS NtTcpClose(const uint64_t *Args)
{
    return ExpCloseTcpHandle((HZ_TCP_HANDLE)Args[0]);
}

enum {
    EXP_WSA_STARTUP = 1,
    EXP_WSA_SOCKET,
    EXP_WSA_CONNECT,
    EXP_WSA_SEND,
    EXP_WSA_RECV,
    EXP_WSA_CLOSESOCKET,
    EXP_WSA_CLEANUP,
    EXP_WSA_GETADDRINFO,
    EXP_WSA_FREEADDRINFO,
    EXP_WSA_GETADDRINFOW,
    EXP_WSA_GETNAMEINFOW,
};

typedef struct _EXP_WSA_DATA {
    uint16_t Version;
    uint16_t HighVersion;
    char Description[257];
    char SystemStatus[129];
    uint16_t MaxSockets;
    uint16_t MaxUdpDatagram;
    const char *VendorInfo;
} EXP_WSA_DATA;

_Static_assert(sizeof(EXP_WSA_DATA) == 408, "64-bit WSADATA layout");

static STATUS ExpWinSocketStartup(uint16_t Version, uint64_t UserData)
{
    if ((Version & 0xFFu) != 2 || (Version >> 8) > 2)
        return STATUS_NOT_SUPPORTED;

    EXP_WSA_DATA Data;
    memset(&Data, 0, sizeof(Data));
    Data.Version = 0x0202;
    Data.HighVersion = 0x0202;
    RtlStringCopy(Data.Description, sizeof(Data.Description), "Nexora Winsock 2.2");
    RtlStringCopy(Data.SystemStatus, sizeof(Data.SystemStatus), "Running");
    STATUS Status = MmCopyToUser((void *)(uintptr_t)UserData, &Data, sizeof(Data));
    if (!HZ_SUCCESS(Status))
        return Status;

    ExpInitializeTcpHandleLock();
    EPROCESS *Owner = PsGetCurrentProcess();
    KIRQL OldIrql;
    KeAcquireSpinLock(&ExpTcpHandleLock, &OldIrql);
    uint32_t Empty = NET_TCP_MAX_CONNECTIONS;
    for (uint32_t Index = 0; Index < NET_TCP_MAX_CONNECTIONS; ++Index) {
        if (ExpWinSockProcesses[Index].Owner == Owner) {
            if (ExpWinSockProcesses[Index].StartupReferences == UINT32_MAX) {
                KeReleaseSpinLock(&ExpTcpHandleLock, OldIrql);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            ++ExpWinSockProcesses[Index].StartupReferences;
            KeReleaseSpinLock(&ExpTcpHandleLock, OldIrql);
            return STATUS_SUCCESS;
        }
        if (!ExpWinSockProcesses[Index].Owner && Empty == NET_TCP_MAX_CONNECTIONS)
            Empty = Index;
    }
    if (Empty == NET_TCP_MAX_CONNECTIONS) {
        KeReleaseSpinLock(&ExpTcpHandleLock, OldIrql);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    ExpWinSockProcesses[Empty].Owner = Owner;
    ExpWinSockProcesses[Empty].StartupReferences = 1;
    KeReleaseSpinLock(&ExpTcpHandleLock, OldIrql);
    return STATUS_SUCCESS;
}

static STATUS ExpWinSocketCreate(uint64_t Domain, uint64_t Type, uint64_t Protocol)
{
    if (Domain != 2 || Type != 1 || (Protocol != 0 && Protocol != 6))
        return STATUS_NOT_SUPPORTED;

    ExpInitializeTcpHandleLock();
    uint64_t Token = 0;
    KIRQL OldIrql;
    KeAcquireSpinLock(&ExpTcpHandleLock, &OldIrql);
    EPROCESS *Owner = PsGetCurrentProcess();
    bool Started = false;
    for (uint32_t Index = 0; Index < NET_TCP_MAX_CONNECTIONS; ++Index)
        if (ExpWinSockProcesses[Index].Owner == Owner && ExpWinSockProcesses[Index].StartupReferences) {
            Started = true;
            break;
        }
    if (!Started) {
        KeReleaseSpinLock(&ExpTcpHandleLock, OldIrql);
        return STATUS_DEVICE_NOT_READY;
    }
    for (uint32_t Index = 0; Index < NET_TCP_MAX_CONNECTIONS; ++Index) {
        if (ExpWinSockets[Index].Socket)
            continue;
        bool Collision;
        do {
            ExpWinNextSocket = ExpWinNextSocket >= 0x7FFFFFFFFFFFFFFEu ?
                               0x50000000ull : ExpWinNextSocket + 1u;
            Token = ExpWinNextSocket;
            Collision = false;
            for (uint32_t Other = 0; Other < NET_TCP_MAX_CONNECTIONS; ++Other)
                if (ExpWinSockets[Other].Socket == Token)
                    Collision = true;
        } while (!Token || Collision);
        ExpWinSockets[Index].Socket = Token;
        ExpWinSockets[Index].Owner = Owner;
        ExpWinSockets[Index].TcpToken = HZ_TCP_INVALID_HANDLE;
        ExpWinSockets[Index].Connecting = false;
        break;
    }
    KeReleaseSpinLock(&ExpTcpHandleLock, OldIrql);
    if (!Token)
        return STATUS_INSUFFICIENT_RESOURCES;
    /* SOCKET fits in the positive STATUS range; the thunk returns it directly in RAX. */
    return (STATUS)Token;
}

static STATUS ExpWinSocketConnect(uint64_t Socket, uint64_t Address, uint64_t AddressLength)
{
    uint8_t Sockaddr[16];
    if (AddressLength < sizeof(Sockaddr))
        return STATUS_INVALID_PARAMETER;
    STATUS Status = MmCopyFromUser(Sockaddr, (const void *)(uintptr_t)Address, sizeof(Sockaddr));
    if (!HZ_SUCCESS(Status))
        return Status;
    if (Sockaddr[0] != 2 || Sockaddr[1] != 0)
        return STATUS_NOT_SUPPORTED; /* AF_INET only */

    ExpInitializeTcpHandleLock();
    KIRQL OldIrql;
    KeAcquireSpinLock(&ExpTcpHandleLock, &OldIrql);
    EPROCESS *Owner = PsGetCurrentProcess();
    bool Found = false;
    for (uint32_t Index = 0; Index < NET_TCP_MAX_CONNECTIONS; ++Index) {
        EXP_WINSOCK_ENTRY *Entry = &ExpWinSockets[Index];
        if (Entry->Socket != Socket || Entry->Owner != Owner)
            continue;
        Found = true;
        if (Entry->TcpToken || Entry->Connecting) {
            KeReleaseSpinLock(&ExpTcpHandleLock, OldIrql);
            return STATUS_INVALID_PARAMETER;
        }
        Entry->Connecting = true;
        break;
    }
    KeReleaseSpinLock(&ExpTcpHandleLock, OldIrql);
    if (!Found)
        return STATUS_INVALID_PARAMETER;

    uint32_t Ipv4 = ((uint32_t)Sockaddr[4] << 24) | ((uint32_t)Sockaddr[5] << 16) |
                    ((uint32_t)Sockaddr[6] << 8) | Sockaddr[7];
    uint16_t Port = (uint16_t)(((uint16_t)Sockaddr[2] << 8) | Sockaddr[3]);
    HZ_TCP_HANDLE TcpToken = HZ_TCP_INVALID_HANDLE;
    Status = ExpCreateTcpHandle(Ipv4, Port, &TcpToken);

    bool Keep = false;
    KeAcquireSpinLock(&ExpTcpHandleLock, &OldIrql);
    for (uint32_t Index = 0; Index < NET_TCP_MAX_CONNECTIONS; ++Index) {
        EXP_WINSOCK_ENTRY *Entry = &ExpWinSockets[Index];
        if (Entry->Socket != Socket || Entry->Owner != Owner)
            continue;
        Entry->Connecting = false;
        if (HZ_SUCCESS(Status)) {
            Entry->TcpToken = TcpToken;
            Keep = true;
        }
        break;
    }
    KeReleaseSpinLock(&ExpTcpHandleLock, OldIrql);
    if (HZ_SUCCESS(Status) && !Keep)
        (void)ExpCloseTcpHandle(TcpToken);
    return Keep ? STATUS_SUCCESS : (HZ_SUCCESS(Status) ? STATUS_INVALID_PARAMETER : Status);
}

static STATUS ExpWinSocketSend(uint64_t Socket, uint64_t UserBuffer, uint64_t Length, uint64_t Flags)
{
    if (Flags || Length > NET_TCP_TX_BUFFER_SIZE)
        return STATUS_INVALID_PARAMETER;
    HZ_TCP_HANDLE Token = ExpLookupWinSocket(Socket);
    NET_TCP_CONNECTION *Connection = ExpLookupTcpHandle(Token);
    if (!Connection)
        return STATUS_INVALID_PARAMETER;
    uint8_t Buffer[NET_TCP_TX_BUFFER_SIZE];
    STATUS Status = MmCopyFromUser(Buffer, (const void *)(uintptr_t)UserBuffer, (size_t)Length);
    if (!HZ_SUCCESS(Status))
        return Status;
    uint32_t Sent = 0;
    Status = NetTcpSend(Connection, Buffer, (uint32_t)Length, &Sent, NET_SOCKET_DEFAULT_TIMEOUT_US);
    return HZ_SUCCESS(Status) ? (STATUS)Sent : Status;
}

static STATUS ExpWinSocketReceive(uint64_t Socket, uint64_t UserBuffer, uint64_t Length, uint64_t Flags)
{
    if (Flags || Length > NET_TCP_RX_BUFFER_SIZE)
        return STATUS_INVALID_PARAMETER;
    STATUS Status = ExpValidateOutput((void *)(uintptr_t)UserBuffer, (uint32_t)Length);
    if (!HZ_SUCCESS(Status))
        return Status;
    HZ_TCP_HANDLE Token = ExpLookupWinSocket(Socket);
    NET_TCP_CONNECTION *Connection = ExpLookupTcpHandle(Token);
    if (!Connection)
        return STATUS_INVALID_PARAMETER;
    uint8_t Buffer[NET_TCP_RX_BUFFER_SIZE];
    uint32_t Received = 0;
    Status = NetTcpReceive(Connection, Buffer, (uint32_t)Length, &Received, NET_SOCKET_DEFAULT_TIMEOUT_US);
    if (!HZ_SUCCESS(Status))
        return Status;
    if (Received) {
        Status = MmCopyToUser((void *)(uintptr_t)UserBuffer, Buffer, Received);
        if (!HZ_SUCCESS(Status))
            return Status;
    }
    return (STATUS)Received;
}

static STATUS ExpWinSocketClose(uint64_t Socket)
{
    HZ_TCP_HANDLE TcpToken = HZ_TCP_INVALID_HANDLE;
    bool Found = false;
    KIRQL OldIrql;
    KeAcquireSpinLock(&ExpTcpHandleLock, &OldIrql);
    EPROCESS *Owner = PsGetCurrentProcess();
    for (uint32_t Index = 0; Index < NET_TCP_MAX_CONNECTIONS; ++Index) {
        EXP_WINSOCK_ENTRY *Entry = &ExpWinSockets[Index];
        if (Entry->Socket == Socket && Entry->Owner == Owner) {
            Found = true;
            TcpToken = Entry->TcpToken;
            memset(Entry, 0, sizeof(*Entry));
            break;
        }
    }
    KeReleaseSpinLock(&ExpTcpHandleLock, OldIrql);
    if (!Found)
        return STATUS_INVALID_PARAMETER;
    return TcpToken ? ExpCloseTcpHandle(TcpToken) : STATUS_SUCCESS;
}

static bool ExpParseIpv4A(const char *Text, uint8_t Address[4])
{
    if (!Text || !*Text)
        return false;
    for (uint32_t Part = 0; Part < 4; ++Part) {
        uint32_t Value = 0, Digits = 0;
        while (*Text >= '0' && *Text <= '9') {
            Value = Value * 10u + (uint32_t)(*Text++ - '0');
            if (Value > 255 || ++Digits > 3)
                return false;
        }
        if (!Digits)
            return false;
        Address[Part] = (uint8_t)Value;
        if (Part != 3) {
            if (*Text++ != '.')
                return false;
        } else if (*Text) {
            return false;
        }
    }
    return true;
}

static STATUS ExpCopyWinSocketString(uint64_t UserString, char *Output, size_t Capacity, bool Wide)
{
    if (!Wide)
        return MmCopyStringFromUser(Output, Capacity, (const char *)(uintptr_t)UserString);
    if (!UserString || Capacity < 2)
        return STATUS_INVALID_PARAMETER;
    for (size_t Index = 0; Index + 1 < Capacity; ++Index) {
        uint16_t Character;
        STATUS Status = MmCopyFromUser(&Character,
                                       (const void *)(uintptr_t)(UserString + Index * sizeof(Character)),
                                       sizeof(Character));
        if (!HZ_SUCCESS(Status))
            return Status;
        if (!Character) {
            Output[Index] = 0;
            return STATUS_SUCCESS;
        }
        if (Character > 0x7F)
            return STATUS_NOT_SUPPORTED; /* IDN/UTF-8 conversion is not implemented yet. */
        Output[Index] = (char)Character;
    }
    Output[0] = 0;
    return STATUS_NAME_TOO_LONG;
}

static STATUS ExpWinGetAddrInfo(uint64_t NodeAddress, uint64_t ServiceAddress,
                                uint64_t HintsAddress, uint64_t ResultAddress, bool Wide)
{
    uint64_t Empty = 0;
    STATUS Status = MmCopyToUser((void *)(uintptr_t)ResultAddress, &Empty, sizeof(Empty));
    if (!HZ_SUCCESS(Status))
        return Status;
    if (!NodeAddress || !ResultAddress)
        return STATUS_INVALID_PARAMETER;

    char Node[NET_DNS_MAX_NAME_LENGTH + 1];
    Status = ExpCopyWinSocketString(NodeAddress, Node, sizeof(Node), Wide);
    if (!HZ_SUCCESS(Status))
        return Status;
    if (!Node[0])
        return STATUS_INVALID_PARAMETER;

    uint16_t Port = 0;
    if (ServiceAddress) {
        char Service[8];
        Status = ExpCopyWinSocketString(ServiceAddress, Service, sizeof(Service), Wide);
        if (!HZ_SUCCESS(Status))
            return Status;
        uint32_t Value = 0;
        if (!Service[0])
            return STATUS_INVALID_PARAMETER;
        for (const char *Digit = Service; *Digit; ++Digit) {
            if (*Digit < '0' || *Digit > '9')
                return STATUS_NOT_SUPPORTED;
            Value = Value * 10u + (uint32_t)(*Digit - '0');
            if (Value > UINT16_MAX)
                return STATUS_INVALID_PARAMETER;
        }
        Port = (uint16_t)Value;
    }

    int32_t Flags = 0, Family = 0, Type = 0, Protocol = 0;
    if (HintsAddress) {
        EXP_WSA_ADDRINFOA Hints;
        Status = MmCopyFromUser(&Hints, (const void *)(uintptr_t)HintsAddress, sizeof(Hints));
        if (!HZ_SUCCESS(Status))
            return Status;
        Flags = Hints.Flags;
        Family = Hints.Family;
        Type = Hints.SocketType;
        Protocol = Hints.Protocol;
        if ((Family != 0 && Family != 2) || (Type != 0 && Type != 1) ||
            (Protocol != 0 && Protocol != 6) || (Flags & ~0x0406))
            return STATUS_NOT_SUPPORTED;
    }

    uint8_t Address[4];
    if (!ExpParseIpv4A(Node, Address)) {
        if (Flags & 0x0004) /* AI_NUMERICHOST */
            return STATUS_NOT_SUPPORTED;
        Status = NetDnsResolveA(Node, Address);
        if (!HZ_SUCCESS(Status))
            return Status;
    } else if (HintsAddress && (Flags & 0x0004)) {
        /* AI_NUMERICHOST requires a numeric address; it already parsed successfully. */
    }

    EPROCESS *Owner = PsGetCurrentProcess();
    void *Base = NULL;
    Status = MmAllocateVirtualMemory(Owner->AddressSpace, &Base, PAGE_SIZE,
                                     MEM_RESERVE | MEM_COMMIT, MM_PROT_READWRITE);
    if (!HZ_SUCCESS(Status))
        return Status;
    KIRQL OldIrql;
    uint32_t Slot;
    ExpInitializeTcpHandleLock();
    KeAcquireSpinLock(&ExpTcpHandleLock, &OldIrql);
    for (Slot = 0; Slot < EXP_WSA_ADDRINFO_ALLOCATIONS; ++Slot)
        if (!ExpWinAddrInfo[Slot].Base)
            break;
    if (Slot < EXP_WSA_ADDRINFO_ALLOCATIONS) {
        ExpWinAddrInfo[Slot].Base = (uint64_t)Base;
        ExpWinAddrInfo[Slot].Owner = Owner;
    }
    KeReleaseSpinLock(&ExpTcpHandleLock, OldIrql);
    if (Slot == EXP_WSA_ADDRINFO_ALLOCATIONS) {
        MmFreeVirtualMemory(Owner->AddressSpace, Base);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    EXP_WSA_ADDRINFOA Info;
    memset(&Info, 0, sizeof(Info));
    Info.Flags = Flags;
    Info.Family = 2;
    Info.SocketType = 1;
    Info.Protocol = 6;
    Info.AddressLength = sizeof(EXP_WSA_SOCKADDR_IN);
    Info.Address = (uint8_t *)Base + sizeof(Info);
    EXP_WSA_SOCKADDR_IN Sockaddr;
    memset(&Sockaddr, 0, sizeof(Sockaddr));
    Sockaddr.Family = 2;
    Sockaddr.Port = (uint16_t)((Port >> 8) | (Port << 8));
    memcpy(Sockaddr.Address, Address, sizeof(Address));
    Status = MmCopyToUser(Base, &Info, sizeof(Info));
    if (HZ_SUCCESS(Status))
        Status = MmCopyToUser(Info.Address, &Sockaddr, sizeof(Sockaddr));
    if (HZ_SUCCESS(Status) && (Flags & 0x0002)) { /* AI_CANONNAME */
        Info.CanonicalName = (char *)Base + sizeof(Info) + sizeof(Sockaddr);
        Status = MmCopyToUser(Base, &Info, sizeof(Info));
        if (HZ_SUCCESS(Status) && Wide) {
            uint16_t Canonical[NET_DNS_MAX_NAME_LENGTH + 1];
            size_t Length = strlen(Node);
            for (size_t Index = 0; Index < Length; ++Index)
                Canonical[Index] = (uint16_t)(uint8_t)Node[Index];
            Canonical[Length] = 0;
            Status = MmCopyToUser((void *)(uintptr_t)Info.CanonicalName, Canonical,
                                  (Length + 1) * sizeof(uint16_t));
        } else if (HZ_SUCCESS(Status)) {
            Status = MmCopyToUser((void *)(uintptr_t)Info.CanonicalName, Node, strlen(Node) + 1);
        }
    }
    if (HZ_SUCCESS(Status))
        Status = MmCopyToUser((void *)(uintptr_t)ResultAddress, &Base, sizeof(Base));
    if (!HZ_SUCCESS(Status)) {
        KeAcquireSpinLock(&ExpTcpHandleLock, &OldIrql);
        memset(&ExpWinAddrInfo[Slot], 0, sizeof(ExpWinAddrInfo[Slot]));
        KeReleaseSpinLock(&ExpTcpHandleLock, OldIrql);
        MmFreeVirtualMemory(Owner->AddressSpace, Base);
    }
    return Status;
}

static STATUS ExpWinFreeAddrInfo(uint64_t UserAddress)
{
    if (!UserAddress)
        return STATUS_SUCCESS;
    ExpInitializeTcpHandleLock();
    KIRQL OldIrql;
    uint64_t Base = 0;
    KeAcquireSpinLock(&ExpTcpHandleLock, &OldIrql);
    EPROCESS *Owner = PsGetCurrentProcess();
    for (uint32_t Index = 0; Index < EXP_WSA_ADDRINFO_ALLOCATIONS; ++Index) {
        if (ExpWinAddrInfo[Index].Base == UserAddress && ExpWinAddrInfo[Index].Owner == Owner) {
            Base = ExpWinAddrInfo[Index].Base;
            memset(&ExpWinAddrInfo[Index], 0, sizeof(ExpWinAddrInfo[Index]));
            break;
        }
    }
    KeReleaseSpinLock(&ExpTcpHandleLock, OldIrql);
    return Base ? MmFreeVirtualMemory(Owner->AddressSpace, (void *)(uintptr_t)Base) :
                  STATUS_INVALID_PARAMETER;
}

static STATUS ExpWinSocketCleanup(bool ProcessExit)
{
    HZ_TCP_HANDLE Tokens[NET_TCP_MAX_CONNECTIONS];
    uint32_t Count = 0;
    ExpInitializeTcpHandleLock();
    KIRQL OldIrql;
    KeAcquireSpinLock(&ExpTcpHandleLock, &OldIrql);
    EPROCESS *Owner = PsGetCurrentProcess();
    uint32_t ProcessSlot = NET_TCP_MAX_CONNECTIONS;
    for (uint32_t Index = 0; Index < NET_TCP_MAX_CONNECTIONS; ++Index)
        if (ExpWinSockProcesses[Index].Owner == Owner) {
            ProcessSlot = Index;
            break;
        }
    if (ProcessSlot == NET_TCP_MAX_CONNECTIONS) {
        KeReleaseSpinLock(&ExpTcpHandleLock, OldIrql);
        return STATUS_INVALID_PARAMETER;
    }
    if (!ProcessExit && ExpWinSockProcesses[ProcessSlot].StartupReferences > 1) {
        --ExpWinSockProcesses[ProcessSlot].StartupReferences;
        KeReleaseSpinLock(&ExpTcpHandleLock, OldIrql);
        return STATUS_SUCCESS;
    }
    memset(&ExpWinSockProcesses[ProcessSlot], 0, sizeof(ExpWinSockProcesses[ProcessSlot]));
    for (uint32_t Index = 0; Index < NET_TCP_MAX_CONNECTIONS; ++Index) {
        EXP_WINSOCK_ENTRY *Entry = &ExpWinSockets[Index];
        if (!Entry->Socket || Entry->Owner != Owner)
            continue;
        if (Entry->TcpToken)
            Tokens[Count++] = Entry->TcpToken;
        memset(Entry, 0, sizeof(*Entry));
    }
    KeReleaseSpinLock(&ExpTcpHandleLock, OldIrql);
    STATUS Result = STATUS_SUCCESS;
    for (uint32_t Index = 0; Index < Count; ++Index) {
        STATUS Status = ExpCloseTcpHandle(Tokens[Index]);
        if (!HZ_SUCCESS(Status))
            Result = Status;
    }
    return Result;
}

void ExCleanupWinSocketsForCurrentProcess(void)
{
    (void)ExpWinSocketCleanup(true);
    ExpInitializeTcpHandleLock();
    KIRQL OldIrql;
    EPROCESS *Owner = PsGetCurrentProcess();
    uint64_t AddressInfo[EXP_WSA_ADDRINFO_ALLOCATIONS];
    uint32_t Count = 0;
    KeAcquireSpinLock(&ExpTcpHandleLock, &OldIrql);
    for (uint32_t Index = 0; Index < EXP_WSA_ADDRINFO_ALLOCATIONS; ++Index) {
        if (!ExpWinAddrInfo[Index].Base || ExpWinAddrInfo[Index].Owner != Owner)
            continue;
        AddressInfo[Count++] = ExpWinAddrInfo[Index].Base;
        memset(&ExpWinAddrInfo[Index], 0, sizeof(ExpWinAddrInfo[Index]));
    }
    KeReleaseSpinLock(&ExpTcpHandleLock, OldIrql);
    for (uint32_t Index = 0; Index < Count; ++Index)
        (void)MmFreeVirtualMemory(Owner->AddressSpace, (void *)(uintptr_t)AddressInfo[Index]);
}

STATUS NtWinSocketApi(const uint64_t *Args)
{
    switch (Args[0]) {
    case EXP_WSA_STARTUP:
        return ExpWinSocketStartup((uint16_t)Args[1], Args[2]);
    case EXP_WSA_SOCKET:
        return ExpWinSocketCreate(Args[1], Args[2], Args[3]);
    case EXP_WSA_CONNECT:
        return ExpWinSocketConnect(Args[1], Args[2], Args[3]);
    case EXP_WSA_SEND:
        return ExpWinSocketSend(Args[1], Args[2], Args[3], Args[4]);
    case EXP_WSA_RECV:
        return ExpWinSocketReceive(Args[1], Args[2], Args[3], Args[4]);
    case EXP_WSA_CLOSESOCKET:
        return ExpWinSocketClose(Args[1]);
    case EXP_WSA_CLEANUP:
        return ExpWinSocketCleanup(false);
    case EXP_WSA_GETADDRINFO:
        return ExpWinGetAddrInfo(Args[1], Args[2], Args[3], Args[4], false);
    case EXP_WSA_FREEADDRINFO:
        return ExpWinFreeAddrInfo(Args[1]);
    case EXP_WSA_GETADDRINFOW:
        return ExpWinGetAddrInfo(Args[1], Args[2], Args[3], Args[4], true);
    case EXP_WSA_GETNAMEINFOW: {
        enum { WSAEFAULT = 10014, WSAEINVAL = 10022, WSAEAI_NONAME = 11001,
               WSAEAFNOSUPPORT = 10047, NI_NAMEREQD = 0x04u };
        if (Args[2] < sizeof(EXP_WSA_SOCKADDR_IN) || !Args[1])
            return WSAEFAULT;
        EXP_WSA_SOCKADDR_IN Address;
        if (!HZ_SUCCESS(MmCopyFromUser(&Address, (const void *)Args[1], sizeof(Address))))
            return WSAEFAULT;
        if (Address.Family != 2)
            return WSAEAFNOSUPPORT;
        if (Args[7] & ~0x1Fu)
            return WSAEINVAL;
        /* Reverse DNS is not implemented. Without NI_NAMEREQD, Windows permits numeric fallback. */
        if (Args[7] & NI_NAMEREQD)
            return WSAEAI_NONAME;
        char Host[16];
        RtlFormatString(Host, sizeof(Host), "%u.%u.%u.%u", (uint32_t)Address.Address[0],
                        (uint32_t)Address.Address[1], (uint32_t)Address.Address[2],
                        (uint32_t)Address.Address[3]);
        uint16_t Wide[16];
        size_t HostChars = strlen(Host);
        for (size_t I = 0; I <= HostChars; ++I)
            Wide[I] = (uint8_t)Host[I];
        if (Args[3]) {
            if (Args[4] <= HostChars ||
                !HZ_SUCCESS(MmCopyToUser((void *)Args[3], Wide, (HostChars + 1) * sizeof(uint16_t))))
                return WSAEFAULT;
        }
        if (Args[5]) {
            uint16_t Port = (uint16_t)((Address.Port >> 8) | (Address.Port << 8));
            char Service[6];
            RtlFormatString(Service, sizeof(Service), "%u", (uint32_t)Port);
            size_t ServiceChars = strlen(Service);
            for (size_t I = 0; I <= ServiceChars; ++I)
                Wide[I] = (uint8_t)Service[I];
            if (Args[6] <= ServiceChars ||
                !HZ_SUCCESS(MmCopyToUser((void *)Args[5], Wide, (ServiceChars + 1) * sizeof(uint16_t))))
                return WSAEFAULT;
        }
        return 0;
    }
    default:
        return STATUS_INVALID_PARAMETER;
    }
}
