/*
 * Nexora OS - net/dns.c
 * Bounded DNS A-query encoding and response parsing for the first IPv4 resolver.
 */
#include "netdns.h"
#include "ke.h"
#include "net.h"
#include "netif.h"
#include "rtl.h"

#define DNS_HEADER_SIZE 12
#define DNS_TYPE_A 1
#define DNS_TYPE_CNAME 5
#define DNS_CLASS_IN 1
#define DNS_MAX_ANSWERS 32
#define DNS_PORT 53
#define DNS_TIMEOUT_US 2000000ull
#define DNS_RETRIES 2

/* Serialize identifiers and preserve the existing bounded DNS request behavior. */
static KMUTEX NetpDnsMutex;
static bool NetpDnsInitialized;
static uint16_t NetpDnsPacketId;

static uint16_t NetDnsReadBe16(const uint8_t *Data);
static void NetDnsWriteBe16(uint8_t *Data, uint16_t Value);

static uint16_t NetDnsReadBe16(const uint8_t *Data)
{
    return (uint16_t)(((uint16_t)Data[0] << 8) | Data[1]);
}

static void NetDnsWriteBe16(uint8_t *Data, uint16_t Value)
{
    Data[0] = (uint8_t)(Value >> 8);
    Data[1] = (uint8_t)Value;
}

void NetDnsInitialize(void)
{
    KeInitializeMutex(&NetpDnsMutex);
    NetpDnsPacketId = (uint16_t)KeQueryUptimeMicroseconds();
    NetpDnsInitialized = true;
}

STATUS NetDnsResolveA(const char *Name, uint8_t Address[4])
{
    if (!Name || !Address)
        return STATUS_INVALID_PARAMETER;
    if (!NetpDnsInitialized)
        return STATUS_DEVICE_NOT_READY;

    uint8_t Query[NET_DNS_MAX_MESSAGE_SIZE];
    uint32_t QueryLength = 0;
    STATUS Status = NetDnsBuildAQuery(0, Name, Query, sizeof(Query), &QueryLength);
    if (!HZ_SUCCESS(Status))
        return Status;

    NET_INTERFACE *Interface = NetGetInterfaceList();
    while (Interface && __atomic_load_n(&Interface->ConfigurationState, __ATOMIC_ACQUIRE) != 2)
        Interface = Interface->Next;
    if (!Interface)
        return STATUS_DEVICE_NOT_READY;
    if (!Interface->QueryLink(Interface))
        return STATUS_DEVICE_NOT_READY;

    Status = KeWaitForSingleObject(&NetpDnsMutex, NULL);
    if (!HZ_SUCCESS(Status))
        return Status;

    if (NetAddressZero(Interface->DnsServer)) {
        Status = STATUS_DEVICE_NOT_READY;
        goto Exit;
    }
    NET_UDP_BINDING *Binding = NULL;
    Status = NetUdpBind(0, &Binding);
    if (!HZ_SUCCESS(Status))
        goto Exit;
    uint16_t Identifier = ++NetpDnsPacketId;
    uint16_t SourcePort = NetUdpBindingPort(Binding);
    NetDnsWriteBe16(Query, Identifier);
    for (uint32_t Retry = 0; Retry < DNS_RETRIES; ++Retry) {
        Status = NetUdpSend(Interface, Interface->Ipv4Address, SourcePort, Interface->DnsServer, DNS_PORT, Query,
                            QueryLength, DNS_TIMEOUT_US);
        if (!HZ_SUCCESS(Status))
            break;
        uint8_t Response[NET_DNS_MAX_MESSAGE_SIZE], Source[4];
        uint16_t ResponsePort = 0;
        uint32_t ResponseLength = 0;
        Status = NetUdpReceive(Binding, Response, sizeof(Response), Source, &ResponsePort, &ResponseLength,
                              DNS_TIMEOUT_US);
        if (HZ_SUCCESS(Status) && ResponsePort == DNS_PORT &&
            NetAddressEqual(Source, Interface->DnsServer)) {
            Status = NetDnsParseAResponse(Identifier, Name, Response, ResponseLength, Address);
            if (Status != STATUS_DEVICE_DATA_ERROR)
                break;
        } else if (HZ_SUCCESS(Status)) {
            Status = STATUS_DEVICE_DATA_ERROR;
        }
        if (Status != STATUS_IO_TIMEOUT && Status != STATUS_DEVICE_DATA_ERROR)
            break;
    }
    if (Status == STATUS_IO_TIMEOUT)
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "DNS: response timed out for %s via %u.%u.%u.%u\n", Name,
                   Interface->DnsServer[0], Interface->DnsServer[1], Interface->DnsServer[2],
                   Interface->DnsServer[3]);
    NetUdpUnbind(Binding);

Exit:
    KeReleaseMutex(&NetpDnsMutex);
    return Status;
}

static char NetDnsLower(char Value)
{
    return Value >= 'A' && Value <= 'Z' ? (char)(Value + ('a' - 'A')) : Value;
}

static bool NetDnsIsHostCharacter(unsigned char Value)
{
    return (Value >= 'a' && Value <= 'z') || (Value >= 'A' && Value <= 'Z') ||
           (Value >= '0' && Value <= '9') || Value == '-';
}

static bool NetDnsNameEqual(const char *Left, const char *Right)
{
    while (*Left && *Right && NetDnsLower(*Left) == NetDnsLower(*Right)) {
        ++Left;
        ++Right;
    }
    return !*Left && !*Right;
}

static STATUS NetDnsEncodeName(const char *Name, uint8_t *Buffer, uint32_t Capacity, uint32_t *Length)
{
    if (!Name || !*Name || !Buffer || !Length)
        return STATUS_INVALID_PARAMETER;

    uint32_t Output = 0, NameLength = 0;
    while (Name[NameLength]) {
        if (NameLength >= NET_DNS_MAX_NAME_LENGTH)
            return STATUS_NAME_TOO_LONG;
        ++NameLength;
    }
    if (Name[NameLength - 1] == '.')
        return STATUS_INVALID_PARAMETER;

    uint32_t Start = 0;
    for (uint32_t i = 0; i <= NameLength; ++i) {
        if (i != NameLength && Name[i] != '.') {
            unsigned char C = (unsigned char)Name[i];
            if (!NetDnsIsHostCharacter(C))
                return STATUS_INVALID_PARAMETER;
            continue;
        }
        uint32_t LabelLength = i - Start;
        if (!LabelLength)
            return STATUS_INVALID_PARAMETER;
        if (Name[Start] == '-' || Name[i - 1] == '-')
            return STATUS_INVALID_PARAMETER;
        if (LabelLength > 63 || Output + LabelLength + 2 > NET_DNS_MAX_NAME_LENGTH + 2)
            return STATUS_NAME_TOO_LONG;
        if (Output + LabelLength + 2 > Capacity)
            return STATUS_BUFFER_TOO_SMALL;
        Buffer[Output++] = (uint8_t)LabelLength;
        for (uint32_t j = Start; j < i; ++j)
            Buffer[Output++] = (uint8_t)Name[j];
        Start = i + 1;
    }
    if (Output + 1 > Capacity)
        return STATUS_BUFFER_TOO_SMALL;
    Buffer[Output++] = 0;
    *Length = Output;
    return STATUS_SUCCESS;
}

STATUS NetDnsBuildAQuery(uint16_t Identifier, const char *Name, void *Buffer, uint32_t Capacity, uint32_t *Length)
{
    if (!Buffer || !Length)
        return STATUS_INVALID_PARAMETER;
    if (Capacity < DNS_HEADER_SIZE)
        return STATUS_BUFFER_TOO_SMALL;

    uint8_t *Bytes = Buffer;
    memset(Bytes, 0, DNS_HEADER_SIZE);
    NetDnsWriteBe16(Bytes, Identifier);
    NetDnsWriteBe16(Bytes + 2, 0x0100); /* standard query with recursion desired */
    NetDnsWriteBe16(Bytes + 4, 1);

    uint32_t NameLength = 0;
    STATUS Status = NetDnsEncodeName(Name, Bytes + DNS_HEADER_SIZE, Capacity - DNS_HEADER_SIZE, &NameLength);
    if (!HZ_SUCCESS(Status))
        return Status;
    if (NameLength > Capacity - DNS_HEADER_SIZE - 4)
        return STATUS_BUFFER_TOO_SMALL;
    NetDnsWriteBe16(Bytes + DNS_HEADER_SIZE + NameLength, DNS_TYPE_A);
    NetDnsWriteBe16(Bytes + DNS_HEADER_SIZE + NameLength + 2, DNS_CLASS_IN);
    *Length = DNS_HEADER_SIZE + NameLength + 4;
    return STATUS_SUCCESS;
}

static STATUS NetDnsDecodeName(const uint8_t *Message, uint32_t Length, uint32_t *Offset, char *Output,
                              uint32_t Capacity)
{
    if (!Message || !Offset || !Output || !Capacity || *Offset >= Length)
        return STATUS_DEVICE_DATA_ERROR;

    uint32_t Position = *Offset, Written = 0, Resume = 0, PointerCount = 0;
    bool Jumped = false;
    while (Position < Length) {
        uint8_t LabelLength = Message[Position++];
        if ((LabelLength & 0xC0) == 0xC0) {
            if (Position >= Length || ++PointerCount > 16)
                return STATUS_DEVICE_DATA_ERROR;
            uint32_t Target = ((uint32_t)(LabelLength & 0x3F) << 8) | Message[Position++];
            if (Target >= Length)
                return STATUS_DEVICE_DATA_ERROR;
            if (!Jumped) {
                Resume = Position;
                Jumped = true;
            }
            Position = Target;
            continue;
        }
        if (LabelLength & 0xC0)
            return STATUS_DEVICE_DATA_ERROR;
        if (!LabelLength) {
            if (!Jumped)
                Resume = Position;
            *Offset = Resume;
            Output[Written] = 0;
            return STATUS_SUCCESS;
        }
        if (LabelLength > 63 || LabelLength > Length - Position)
            return STATUS_DEVICE_DATA_ERROR;
        if (Written && Written + 1 >= Capacity)
            return STATUS_NAME_TOO_LONG;
        if (Written)
            Output[Written++] = '.';
        if (LabelLength >= Capacity - Written)
            return STATUS_NAME_TOO_LONG;
        for (uint32_t i = 0; i < LabelLength; ++i) {
            uint8_t Character = Message[Position++];
            if (!NetDnsIsHostCharacter(Character))
                return STATUS_DEVICE_DATA_ERROR;
            Output[Written++] = NetDnsLower((char)Character);
        }
        if (Output[Written - LabelLength] == '-' || Output[Written - 1] == '-')
            return STATUS_DEVICE_DATA_ERROR;
    }
    return STATUS_DEVICE_DATA_ERROR;
}

STATUS NetDnsParseAResponse(uint16_t Identifier, const char *Name, const void *Message, uint32_t Length,
                            uint8_t Address[4])
{
    if (!Name || !Message || !Address || Length < DNS_HEADER_SIZE || Length > NET_DNS_MAX_MESSAGE_SIZE)
        return STATUS_INVALID_PARAMETER;
    const uint8_t *Bytes = Message;
    uint16_t Flags = NetDnsReadBe16(Bytes + 2);
    uint16_t Questions = NetDnsReadBe16(Bytes + 4);
    uint16_t Answers = NetDnsReadBe16(Bytes + 6);
    if (NetDnsReadBe16(Bytes) != Identifier || !(Flags & 0x8000) || (Flags & 0x7840) || (Flags & 0x0200) ||
        (Flags & 0x000F) || Questions != 1 || Answers > DNS_MAX_ANSWERS)
        return STATUS_DEVICE_DATA_ERROR;

    uint32_t Offset = DNS_HEADER_SIZE;
    char Decoded[NET_DNS_MAX_NAME_LENGTH + 1];
    STATUS Status = NetDnsDecodeName(Bytes, Length, &Offset, Decoded, sizeof(Decoded));
    if (!HZ_SUCCESS(Status) || Offset > Length || Length - Offset < 4 ||
        !NetDnsNameEqual(Decoded, Name) || NetDnsReadBe16(Bytes + Offset) != DNS_TYPE_A ||
        NetDnsReadBe16(Bytes + Offset + 2) != DNS_CLASS_IN)
        return STATUS_DEVICE_DATA_ERROR;
    Offset += 4;

    char CurrentName[NET_DNS_MAX_NAME_LENGTH + 1];
    memcpy(CurrentName, Decoded, strlen(Decoded) + 1);
    uint32_t AnswerStart = Offset;
    for (uint16_t Hop = 0; Hop <= Answers; ++Hop) {
        bool FoundCname = false;
        char CanonicalName[NET_DNS_MAX_NAME_LENGTH + 1];
        uint32_t RecordOffset = AnswerStart;
        for (uint16_t i = 0; i < Answers; ++i) {
            Status = NetDnsDecodeName(Bytes, Length, &RecordOffset, Decoded, sizeof(Decoded));
            if (!HZ_SUCCESS(Status) || RecordOffset > Length || Length - RecordOffset < 10)
                return STATUS_DEVICE_DATA_ERROR;
            uint16_t Type = NetDnsReadBe16(Bytes + RecordOffset);
            uint16_t Class = NetDnsReadBe16(Bytes + RecordOffset + 2);
            uint16_t DataLength = NetDnsReadBe16(Bytes + RecordOffset + 8);
            RecordOffset += 10;
            if (DataLength > Length - RecordOffset)
                return STATUS_DEVICE_DATA_ERROR;
            uint32_t DataEnd = RecordOffset + DataLength;
            if (Class == DNS_CLASS_IN && NetDnsNameEqual(Decoded, CurrentName)) {
                if (Type == DNS_TYPE_A && DataLength == 4) {
                    memcpy(Address, Bytes + RecordOffset, 4);
                    return STATUS_SUCCESS;
                }
                if (Type == DNS_TYPE_CNAME) {
                    if (FoundCname)
                        return STATUS_DEVICE_DATA_ERROR;
                    uint32_t TargetOffset = RecordOffset;
                    Status = NetDnsDecodeName(Bytes, Length, &TargetOffset, CanonicalName,
                                              sizeof(CanonicalName));
                    if (!HZ_SUCCESS(Status) || TargetOffset != DataEnd || !CanonicalName[0])
                        return STATUS_DEVICE_DATA_ERROR;
                    FoundCname = true;
                }
            }
            RecordOffset = DataEnd;
        }
        if (!FoundCname)
            return STATUS_NOT_FOUND;
        if (Hop == Answers)
            return STATUS_DEVICE_DATA_ERROR; /* the answer contains a CNAME loop */
        memcpy(CurrentName, CanonicalName, strlen(CanonicalName) + 1);
    }
    return STATUS_DEVICE_DATA_ERROR;
}

static bool NetDnsTest(bool Condition, const char *Name, uint32_t *Checks, uint32_t *Failures)
{
    (*Checks)++;
    if (Condition)
        return true;
    (*Failures)++;
    DbgPrintEx(DPFLTR_ERROR_LEVEL, "DNS self-test: FAIL %s\n", Name);
    return false;
}

bool NetDnsRunSelfTests(void)
{
    uint32_t Checks = 0, Failures = 0, QueryLength = 0;
    uint8_t Query[NET_DNS_MAX_MESSAGE_SIZE] = {0};
    STATUS Status = NetDnsBuildAQuery(0x1234, "nexora.test", Query, sizeof(Query), &QueryLength);
    NetDnsTest(Status == STATUS_SUCCESS && QueryLength == 29 && NetDnsReadBe16(Query) == 0x1234 &&
                   NetDnsReadBe16(Query + 2) == 0x0100 && NetDnsReadBe16(Query + 4) == 1 &&
                   Query[12] == 6 && memcmp(Query + 13, "nexora", 6) == 0,
               "A query encoding", &Checks, &Failures);
    NetDnsTest(NetDnsBuildAQuery(1, "bad..name", Query, sizeof(Query), &QueryLength) == STATUS_INVALID_PARAMETER,
               "empty label rejected", &Checks, &Failures);
    NetDnsTest(NetDnsBuildAQuery(1, "bad_name.test", Query, sizeof(Query), &QueryLength) == STATUS_INVALID_PARAMETER,
               "invalid hostname character rejected", &Checks, &Failures);
    NetDnsTest(NetDnsBuildAQuery(1, "nexora.test", Query, 12, &QueryLength) == STATUS_BUFFER_TOO_SMALL,
               "short query buffer", &Checks, &Failures);
    NetDnsTest(NetDnsBuildAQuery(1, "nexora.test", Query, 28, &QueryLength) == STATUS_BUFFER_TOO_SMALL,
               "query record buffer bound", &Checks, &Failures);

    uint8_t Response[128] = {0};
    NetDnsWriteBe16(Response, 0x1234);
    NetDnsWriteBe16(Response + 2, 0x8180);
    NetDnsWriteBe16(Response + 4, 1);
    NetDnsWriteBe16(Response + 6, 1);
    uint32_t Offset = 12;
    Response[Offset++] = 6;
    memcpy(Response + Offset, "nexora", 6);
    Offset += 6;
    Response[Offset++] = 4;
    memcpy(Response + Offset, "test", 4);
    Offset += 4;
    Response[Offset++] = 0;
    NetDnsWriteBe16(Response + Offset, DNS_TYPE_A);
    NetDnsWriteBe16(Response + Offset + 2, DNS_CLASS_IN);
    Offset += 4;
    Response[Offset++] = 0xC0;
    Response[Offset++] = 0x0C;
    NetDnsWriteBe16(Response + Offset, DNS_TYPE_A);
    NetDnsWriteBe16(Response + Offset + 2, DNS_CLASS_IN);
    Response[Offset + 7] = 60;
    NetDnsWriteBe16(Response + Offset + 8, 4);
    Offset += 10;
    Response[Offset++] = 203;
    Response[Offset++] = 0;
    Response[Offset++] = 113;
    Response[Offset++] = 7;
    uint8_t Address[4] = {0};
    Status = NetDnsParseAResponse(0x1234, "NEXORA.test", Response, Offset, Address);
    NetDnsTest(Status == STATUS_SUCCESS && Address[0] == 203 && Address[1] == 0 && Address[2] == 113 &&
                   Address[3] == 7,
               "compressed A response", &Checks, &Failures);
    NetDnsTest(NetDnsParseAResponse(0x4321, "nexora.test", Response, Offset, Address) == STATUS_DEVICE_DATA_ERROR,
               "response identifier checked", &Checks, &Failures);

    memset(Response, 0, sizeof(Response));
    NetDnsWriteBe16(Response, 0x1234);
    NetDnsWriteBe16(Response + 2, 0x8180);
    NetDnsWriteBe16(Response + 4, 1);
    NetDnsWriteBe16(Response + 6, 2);
    Offset = 12;
    Response[Offset++] = 3;
    memcpy(Response + Offset, "www", 3);
    Offset += 3;
    Response[Offset++] = 6;
    memcpy(Response + Offset, "nexora", 6);
    Offset += 6;
    Response[Offset++] = 4;
    memcpy(Response + Offset, "test", 4);
    Offset += 4;
    Response[Offset++] = 0;
    NetDnsWriteBe16(Response + Offset, DNS_TYPE_A);
    NetDnsWriteBe16(Response + Offset + 2, DNS_CLASS_IN);
    Offset += 4;
    Response[Offset++] = 0xC0;
    Response[Offset++] = 0x0C;
    NetDnsWriteBe16(Response + Offset, DNS_TYPE_CNAME);
    NetDnsWriteBe16(Response + Offset + 2, DNS_CLASS_IN);
    NetDnsWriteBe16(Response + Offset + 8, 13);
    Offset += 10;
    uint32_t CanonicalOffset = Offset;
    Response[Offset++] = 6;
    memcpy(Response + Offset, "portal", 6);
    Offset += 6;
    Response[Offset++] = 4;
    memcpy(Response + Offset, "test", 4);
    Offset += 4;
    Response[Offset++] = 0;
    Response[Offset++] = (uint8_t)(0xC0 | (CanonicalOffset >> 8));
    Response[Offset++] = (uint8_t)CanonicalOffset;
    NetDnsWriteBe16(Response + Offset, DNS_TYPE_A);
    NetDnsWriteBe16(Response + Offset + 2, DNS_CLASS_IN);
    NetDnsWriteBe16(Response + Offset + 8, 4);
    Offset += 10;
    Response[Offset++] = 198;
    Response[Offset++] = 51;
    Response[Offset++] = 100;
    Response[Offset++] = 9;
    Status = NetDnsParseAResponse(0x1234, "www.nexora.test", Response, Offset, Address);
    NetDnsTest(Status == STATUS_SUCCESS && Address[0] == 198 && Address[1] == 51 && Address[2] == 100 &&
                   Address[3] == 9,
               "CNAME chain to compressed A record", &Checks, &Failures);

    Response[12] = 0xC0;
    Response[13] = 0x0C;
    NetDnsTest(NetDnsParseAResponse(0x1234, "nexora.test", Response, Offset, Address) == STATUS_DEVICE_DATA_ERROR,
               "compression loop rejected", &Checks, &Failures);
    DbgPrintEx(Failures ? DPFLTR_ERROR_LEVEL : DPFLTR_INFO_LEVEL, "DNS self-tests: %u/%u passed\n",
               Checks - Failures, Checks);
    return Failures == 0;
}
