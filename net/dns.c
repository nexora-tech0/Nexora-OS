/*
 * Nexora OS - net/dns.c
 * Bounded DNS A-query encoding and response parsing for the first IPv4 resolver.
 */
#include "netdns.h"
#include "ke.h"
#include "netif.h"
#include "packet.h"
#include "rtl.h"

#define DNS_HEADER_SIZE 12
#define DNS_TYPE_A 1
#define DNS_CLASS_IN 1
#define DNS_MAX_ANSWERS 32
#define DNS_PORT 53
#define DNS_SOURCE_PORT_MIN 49152
#define DNS_POLL_US 10000ull
#define DNS_TIMEOUT_US 2000000ull
#define DNS_RETRIES 2

/* DNS lookups serialize access to the NIC receive ring after DHCP finishes. */
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

static bool NetDnsAddressZero(const uint8_t Address[4])
{
    return !(Address[0] | Address[1] | Address[2] | Address[3]);
}

static bool NetDnsSameSubnet(const uint8_t Left[4], const uint8_t Right[4], const uint8_t Mask[4])
{
    for (uint32_t i = 0; i < 4; ++i)
        if ((Left[i] & Mask[i]) != (Right[i] & Mask[i]))
            return false;
    return true;
}

static void NetDnsBuildArpRequest(NET_INTERFACE *Interface, const uint8_t Target[4], uint8_t Frame[42])
{
    static const uint8_t Broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    memset(Frame, 0, 42);
    memcpy(Frame, Broadcast, sizeof(Broadcast));
    memcpy(Frame + 6, Interface->MacAddress, 6);
    NetDnsWriteBe16(Frame + 12, NET_ETHERTYPE_ARP);
    uint8_t *Arp = Frame + NET_ETHERNET_HEADER_SIZE;
    NetDnsWriteBe16(Arp, 1);
    NetDnsWriteBe16(Arp + 2, NET_ETHERTYPE_IPV4);
    Arp[4] = 6;
    Arp[5] = 4;
    NetDnsWriteBe16(Arp + 6, 1);
    memcpy(Arp + 8, Interface->MacAddress, 6);
    memcpy(Arp + 14, Interface->Ipv4Address, 4);
    memcpy(Arp + 24, Target, 4);
}

static STATUS NetDnsWaitForArp(NET_INTERFACE *Interface, const uint8_t Target[4], uint8_t Mac[6])
{
    uint8_t Frame[NETIF_MAX_FRAME_SIZE];
    uint64_t Elapsed = 0;
    while (Elapsed < DNS_TIMEOUT_US) {
        uint32_t Length = 0;
        STATUS Status = Interface->ReceiveFrame(Interface, Frame, sizeof(Frame), &Length);
        if (HZ_SUCCESS(Status)) {
            NET_ETHERNET_VIEW Ethernet;
            NET_ARP_VIEW Arp;
            if (HZ_SUCCESS(NetParseEthernet(Frame, Length, &Ethernet)) &&
                memcmp(Ethernet.Destination, Interface->MacAddress, 6) == 0 &&
                HZ_SUCCESS(NetParseArp(&Ethernet, &Arp)) && Arp.Operation == 2 &&
                memcmp(Arp.SenderIpv4, Target, 4) == 0 &&
                memcmp(Arp.TargetIpv4, Interface->Ipv4Address, 4) == 0 &&
                memcmp(Arp.TargetMac, Interface->MacAddress, 6) == 0 &&
                memcmp(Arp.SenderMac, Ethernet.Source, 6) == 0 && !(Arp.SenderMac[0] & 1)) {
                memcpy(Mac, Arp.SenderMac, 6);
                return STATUS_SUCCESS;
            }
        } else if (Status != STATUS_NO_MORE_ENTRIES && Status != STATUS_DEVICE_DATA_ERROR) {
            return Status;
        }
        KeDelayExecutionThread(DNS_POLL_US);
        Elapsed += DNS_POLL_US;
    }
    return STATUS_IO_TIMEOUT;
}

static STATUS NetDnsSendQuery(NET_INTERFACE *Interface, const uint8_t DestinationMac[6],
                              const uint8_t DestinationIp[4], uint16_t SourcePort,
                              const uint8_t *Payload, uint32_t PayloadLength)
{
    uint8_t Frame[NETIF_MAX_FRAME_SIZE] = {0};
    uint32_t FrameLength = NET_ETHERNET_HEADER_SIZE + NET_IPV4_MIN_HEADER_SIZE + NET_UDP_HEADER_SIZE + PayloadLength;
    if (FrameLength > sizeof(Frame) || PayloadLength > NET_DNS_MAX_MESSAGE_SIZE)
        return STATUS_BUFFER_TOO_SMALL;

    memcpy(Frame, DestinationMac, 6);
    memcpy(Frame + 6, Interface->MacAddress, 6);
    NetDnsWriteBe16(Frame + 12, NET_ETHERTYPE_IPV4);
    uint8_t *Ip = Frame + NET_ETHERNET_HEADER_SIZE;
    Ip[0] = 0x45;
    NetDnsWriteBe16(Ip + 2, (uint16_t)(NET_IPV4_MIN_HEADER_SIZE + NET_UDP_HEADER_SIZE + PayloadLength));
    NetDnsWriteBe16(Ip + 4, ++NetpDnsPacketId);
    NetDnsWriteBe16(Ip + 6, 0x4000);
    Ip[8] = 64;
    Ip[9] = NET_IP_PROTOCOL_UDP;
    memcpy(Ip + 12, Interface->Ipv4Address, 4);
    memcpy(Ip + 16, DestinationIp, 4);
    NetDnsWriteBe16(Ip + 10, NetIpv4Checksum(Ip, NET_IPV4_MIN_HEADER_SIZE));
    uint8_t *Udp = Ip + NET_IPV4_MIN_HEADER_SIZE;
    NetDnsWriteBe16(Udp, SourcePort);
    NetDnsWriteBe16(Udp + 2, DNS_PORT);
    NetDnsWriteBe16(Udp + 4, (uint16_t)(NET_UDP_HEADER_SIZE + PayloadLength));
    memcpy(Udp + NET_UDP_HEADER_SIZE, Payload, PayloadLength);

    for (uint32_t Retry = 0; Retry < 10; ++Retry) {
        STATUS Status = Interface->SendFrame(Interface, Frame, FrameLength);
        if (Status != STATUS_DEVICE_BUSY)
            return Status;
        KeDelayExecutionThread(DNS_POLL_US);
    }
    return STATUS_DEVICE_BUSY;
}

static STATUS NetDnsWaitForResponse(NET_INTERFACE *Interface, uint16_t Identifier, const char *Name,
                                    uint16_t SourcePort, uint8_t Address[4])
{
    uint8_t Frame[NETIF_MAX_FRAME_SIZE];
    uint64_t Elapsed = 0;
    while (Elapsed < DNS_TIMEOUT_US) {
        uint32_t Length = 0;
        STATUS Status = Interface->ReceiveFrame(Interface, Frame, sizeof(Frame), &Length);
        if (HZ_SUCCESS(Status)) {
            NET_ETHERNET_VIEW Ethernet;
            NET_IPV4_VIEW Ipv4;
            NET_UDP_VIEW Udp;
            if (HZ_SUCCESS(NetParseEthernet(Frame, Length, &Ethernet)) &&
                memcmp(Ethernet.Destination, Interface->MacAddress, 6) == 0 &&
                HZ_SUCCESS(NetParseIpv4(&Ethernet, &Ipv4)) && HZ_SUCCESS(NetParseUdp(&Ipv4, &Udp)) &&
                Ipv4.Protocol == NET_IP_PROTOCOL_UDP &&
                memcmp(Ipv4.Source, Interface->DnsServer, 4) == 0 &&
                memcmp(Ipv4.Destination, Interface->Ipv4Address, 4) == 0 &&
                Udp.SourcePort == DNS_PORT && Udp.DestinationPort == SourcePort) {
                Status = NetDnsParseAResponse(Identifier, Name, Udp.Payload, Udp.PayloadLength, Address);
                if (Status != STATUS_DEVICE_DATA_ERROR)
                    return Status;
            }
        } else if (Status != STATUS_NO_MORE_ENTRIES && Status != STATUS_DEVICE_DATA_ERROR) {
            return Status;
        }
        KeDelayExecutionThread(DNS_POLL_US);
        Elapsed += DNS_POLL_US;
    }
    return STATUS_IO_TIMEOUT;
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

    uint8_t NextHop[4], DestinationMac[6];
    memcpy(NextHop, NetDnsSameSubnet(Interface->DnsServer, Interface->Ipv4Address, Interface->SubnetMask)
                        ? Interface->DnsServer
                        : Interface->Gateway,
           sizeof(NextHop));
    if (NetDnsAddressZero(Interface->DnsServer) || NetDnsAddressZero(NextHop)) {
        Status = STATUS_DEVICE_NOT_READY;
        goto Exit;
    }
    uint8_t ArpFrame[42];
    NetDnsBuildArpRequest(Interface, NextHop, ArpFrame);
    for (uint32_t Retry = 0; Retry < 10; ++Retry) {
        Status = Interface->SendFrame(Interface, ArpFrame, sizeof(ArpFrame));
        if (Status != STATUS_DEVICE_BUSY)
            break;
        KeDelayExecutionThread(DNS_POLL_US);
    }
    if (!HZ_SUCCESS(Status))
        goto Exit;
    Status = NetDnsWaitForArp(Interface, NextHop, DestinationMac);
    if (!HZ_SUCCESS(Status)) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "DNS: ARP resolution failed for %u.%u.%u.%u (0x%08X)\n",
                   NextHop[0], NextHop[1], NextHop[2], NextHop[3], (uint32_t)Status);
        goto Exit;
    }

    uint16_t Identifier = ++NetpDnsPacketId;
    uint16_t SourcePort = (uint16_t)(DNS_SOURCE_PORT_MIN + (Identifier & 0x3FFF));
    NetDnsWriteBe16(Query, Identifier);
    for (uint32_t Retry = 0; Retry < DNS_RETRIES; ++Retry) {
        Status = NetDnsSendQuery(Interface, DestinationMac, Interface->DnsServer, SourcePort, Query, QueryLength);
        if (!HZ_SUCCESS(Status))
            break;
        Status = NetDnsWaitForResponse(Interface, Identifier, Name, SourcePort, Address);
        if (Status != STATUS_IO_TIMEOUT)
            break;
    }
    if (Status == STATUS_IO_TIMEOUT)
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "DNS: response timed out for %s via %u.%u.%u.%u\n", Name,
                   Interface->DnsServer[0], Interface->DnsServer[1], Interface->DnsServer[2],
                   Interface->DnsServer[3]);

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

    for (uint16_t i = 0; i < Answers; ++i) {
        Status = NetDnsDecodeName(Bytes, Length, &Offset, Decoded, sizeof(Decoded));
        if (!HZ_SUCCESS(Status) || Offset > Length || Length - Offset < 10)
            return STATUS_DEVICE_DATA_ERROR;
        uint16_t Type = NetDnsReadBe16(Bytes + Offset);
        uint16_t Class = NetDnsReadBe16(Bytes + Offset + 2);
        uint16_t DataLength = NetDnsReadBe16(Bytes + Offset + 8);
        Offset += 10;
        if (DataLength > Length - Offset)
            return STATUS_DEVICE_DATA_ERROR;
        if (Type == DNS_TYPE_A && Class == DNS_CLASS_IN && DataLength == 4 && NetDnsNameEqual(Decoded, Name)) {
            memcpy(Address, Bytes + Offset, 4);
            return STATUS_SUCCESS;
        }
        Offset += DataLength;
    }
    return STATUS_NOT_FOUND;
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

    uint8_t Response[64] = {0};
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

    Response[12] = 0xC0;
    Response[13] = 0x0C;
    NetDnsTest(NetDnsParseAResponse(0x1234, "nexora.test", Response, Offset, Address) == STATUS_DEVICE_DATA_ERROR,
               "compression loop rejected", &Checks, &Failures);
    DbgPrintEx(Failures ? DPFLTR_ERROR_LEVEL : DPFLTR_INFO_LEVEL, "DNS self-tests: %u/%u passed\n",
               Checks - Failures, Checks);
    return Failures == 0;
}
