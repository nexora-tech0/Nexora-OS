/*
 * Nexora OS - net/dhcp.c
 * Bounded DHCPv4 client for the first registered Ethernet interface. The worker retries a
 * discover/request exchange and publishes only a fully validated ACK configuration.
 */
#include "hal.h"
#include "ke.h"
#include "packet.h"
#include "netif.h"
#include "ps.h"
#include "rtl.h"

#define NET_DHCP_PORT_CLIENT 68
#define NET_DHCP_PORT_SERVER 67
#define NET_DHCP_MAGIC 0x63825363u
#define NET_DHCP_FIXED_SIZE 240
#define NET_DHCP_MAX_SIZE 576
#define NET_DHCP_MIN_PACKET_SIZE 300
#define NET_DHCP_RETRIES 4
#define NET_DHCP_TIMEOUT_US 2000000ull
#define NET_DHCP_POLL_US 10000ull

enum {
    DhcpOptionSubnetMask = 1,
    DhcpOptionRouter = 3,
    DhcpOptionDns = 6,
    DhcpOptionRequestedIp = 50,
    DhcpOptionMessageType = 53,
    DhcpOptionServerId = 54,
    DhcpOptionParameterList = 55,
    DhcpOptionClientId = 61,
    DhcpOptionEnd = 255
};

enum {
    DhcpDiscover = 1,
    DhcpOffer = 2,
    DhcpRequest = 3,
    DhcpAck = 5,
    DhcpNak = 6
};

typedef struct _NET_DHCP_RESULT {
    uint8_t MessageType;
    uint8_t YourAddress[4];
    uint8_t ServerAddress[4];
    uint8_t SubnetMask[4];
    uint8_t Router[4];
    uint8_t DnsServer[4];
    bool HasSubnetMask;
    bool HasRouter;
    bool HasDnsServer;
    bool HasServerAddress;
} NET_DHCP_RESULT;

static uint32_t NetpReadBe32(const uint8_t *Bytes)
{
    return ((uint32_t)Bytes[0] << 24) | ((uint32_t)Bytes[1] << 16) | ((uint32_t)Bytes[2] << 8) | Bytes[3];
}

static void NetpWriteBe16(uint8_t *Bytes, uint16_t Value)
{
    Bytes[0] = (uint8_t)(Value >> 8);
    Bytes[1] = (uint8_t)Value;
}

static void NetpWriteBe32(uint8_t *Bytes, uint32_t Value)
{
    Bytes[0] = (uint8_t)(Value >> 24);
    Bytes[1] = (uint8_t)(Value >> 16);
    Bytes[2] = (uint8_t)(Value >> 8);
    Bytes[3] = (uint8_t)Value;
}

static uint8_t *NetpDhcpOption(uint8_t *Options, uint32_t *Length, uint8_t Code, const void *Value,
                               uint8_t ValueLength)
{
    if (*Length > NET_DHCP_MAX_SIZE - NET_DHCP_FIXED_SIZE - (uint32_t)ValueLength - 2)
        return NULL;
    Options[(*Length)++] = Code;
    Options[(*Length)++] = ValueLength;
    memcpy(Options + *Length, Value, ValueLength);
    *Length += ValueLength;
    return Options + *Length;
}

static uint32_t NetpBuildDhcpRequest(NET_INTERFACE *Interface, uint8_t *Buffer, uint8_t MessageType, uint32_t Xid,
                                     const uint8_t *RequestedIp, const uint8_t *ServerId)
{
    memset(Buffer, 0, NET_DHCP_MAX_SIZE);
    Buffer[0] = 1;
    Buffer[1] = 1;
    Buffer[2] = 6;
    NetpWriteBe32(Buffer + 4, Xid);
    NetpWriteBe16(Buffer + 10, 0x8000);
    memcpy(Buffer + 28, Interface->MacAddress, 6);
    NetpWriteBe32(Buffer + 236, NET_DHCP_MAGIC);

    uint8_t *Options = Buffer + NET_DHCP_FIXED_SIZE;
    uint32_t Length = 0;
    uint8_t Type = MessageType;
    if (!NetpDhcpOption(Options, &Length, DhcpOptionMessageType, &Type, 1))
        return 0;
    uint8_t ClientId[7] = {1};
    memcpy(ClientId + 1, Interface->MacAddress, 6);
    if (!NetpDhcpOption(Options, &Length, DhcpOptionClientId, ClientId, sizeof(ClientId)))
        return 0;
    if (RequestedIp && !NetpDhcpOption(Options, &Length, DhcpOptionRequestedIp, RequestedIp, 4))
        return 0;
    if (ServerId && !NetpDhcpOption(Options, &Length, DhcpOptionServerId, ServerId, 4))
        return 0;
    static const uint8_t RequestedOptions[] = {DhcpOptionSubnetMask, DhcpOptionRouter, DhcpOptionDns};
    if (!NetpDhcpOption(Options, &Length, DhcpOptionParameterList, RequestedOptions, sizeof(RequestedOptions)))
        return 0;
    Options[Length++] = DhcpOptionEnd;
    uint32_t PacketLength = NET_DHCP_FIXED_SIZE + Length;
    return PacketLength < NET_DHCP_MIN_PACKET_SIZE ? NET_DHCP_MIN_PACKET_SIZE : PacketLength;
}

static void NetpBuildUdpFrame(NET_INTERFACE *Interface, uint8_t *Frame, const uint8_t *Payload, uint32_t PayloadLength,
                              uint32_t Identification)
{
    static const uint8_t BroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    static const uint8_t AnyIpv4[4] = {0};
    static const uint8_t BroadcastIpv4[4] = {255, 255, 255, 255};
    memset(Frame, 0, NET_ETHERNET_HEADER_SIZE + NET_IPV4_MIN_HEADER_SIZE + NET_UDP_HEADER_SIZE + PayloadLength);
    memcpy(Frame, BroadcastMac, sizeof(BroadcastMac));
    memcpy(Frame + 6, Interface->MacAddress, sizeof(Interface->MacAddress));
    NetpWriteBe16(Frame + 12, NET_ETHERTYPE_IPV4);

    uint8_t *Ip = Frame + NET_ETHERNET_HEADER_SIZE;
    Ip[0] = 0x45;
    NetpWriteBe16(Ip + 2, (uint16_t)(NET_IPV4_MIN_HEADER_SIZE + NET_UDP_HEADER_SIZE + PayloadLength));
    NetpWriteBe16(Ip + 4, (uint16_t)Identification);
    NetpWriteBe16(Ip + 6, 0x4000);
    Ip[8] = 64;
    Ip[9] = NET_IP_PROTOCOL_UDP;
    memcpy(Ip + 12, AnyIpv4, sizeof(AnyIpv4));
    memcpy(Ip + 16, BroadcastIpv4, sizeof(BroadcastIpv4));
    uint16_t Checksum = NetIpv4Checksum(Ip, NET_IPV4_MIN_HEADER_SIZE);
    NetpWriteBe16(Ip + 10, Checksum);

    uint8_t *Udp = Ip + NET_IPV4_MIN_HEADER_SIZE;
    NetpWriteBe16(Udp, NET_DHCP_PORT_CLIENT);
    NetpWriteBe16(Udp + 2, NET_DHCP_PORT_SERVER);
    NetpWriteBe16(Udp + 4, (uint16_t)(NET_UDP_HEADER_SIZE + PayloadLength));
    memcpy(Udp + NET_UDP_HEADER_SIZE, Payload, PayloadLength);
}

static bool NetpCopyDhcpOption(const uint8_t *Options, uint32_t Length, uint8_t Wanted, uint8_t *Output,
                               uint8_t OutputLength)
{
    uint32_t Offset = 0;
    while (Offset < Length) {
        uint8_t Code = Options[Offset++];
        if (Code == 0)
            continue;
        if (Code == DhcpOptionEnd)
            return false;
        if (Offset >= Length)
            return false;
        uint8_t OptionLength = Options[Offset++];
        if (OptionLength > Length - Offset)
            return false;
        if (Code == Wanted) {
            if (OptionLength != OutputLength)
                return false;
            memcpy(Output, Options + Offset, OutputLength);
            return true;
        }
        Offset += OptionLength;
    }
    return false;
}

static bool NetpParseDhcp(const NET_UDP_VIEW *Udp, uint32_t Xid, const uint8_t Mac[6], NET_DHCP_RESULT *Result)
{
    if (!Udp || !Udp->Payload || !Mac || !Result || Udp->SourcePort != NET_DHCP_PORT_SERVER ||
        Udp->DestinationPort != NET_DHCP_PORT_CLIENT || Udp->PayloadLength < NET_DHCP_FIXED_SIZE)
        return false;
    const uint8_t *Packet = Udp->Payload;
    if (Packet[0] != 2 || Packet[1] != 1 || Packet[2] != 6 || NetpReadBe32(Packet + 4) != Xid ||
        NetpReadBe32(Packet + 236) != NET_DHCP_MAGIC || memcmp(Packet + 28, Mac, 6) != 0)
        return false;
    uint32_t OptionLength = Udp->PayloadLength - NET_DHCP_FIXED_SIZE;
    const uint8_t *Options = Packet + NET_DHCP_FIXED_SIZE;
    uint8_t MessageType;
    if (!NetpCopyDhcpOption(Options, OptionLength, DhcpOptionMessageType, &MessageType, 1))
        return false;
    memset(Result, 0, sizeof(*Result));
    Result->MessageType = MessageType;
    memcpy(Result->YourAddress, Packet + 16, 4);
    Result->HasServerAddress = NetpCopyDhcpOption(Options, OptionLength, DhcpOptionServerId, Result->ServerAddress, 4);
    Result->HasSubnetMask = NetpCopyDhcpOption(Options, OptionLength, DhcpOptionSubnetMask, Result->SubnetMask, 4);
    Result->HasRouter = NetpCopyDhcpOption(Options, OptionLength, DhcpOptionRouter, Result->Router, 4);
    Result->HasDnsServer = NetpCopyDhcpOption(Options, OptionLength, DhcpOptionDns, Result->DnsServer, 4);
    if (!Result->HasServerAddress)
        memcpy(Result->ServerAddress, Packet + 20, 4);
    return true;
}

static bool NetpDhcpTest(bool Condition, const char *Name, uint32_t *Checks, uint32_t *Failures)
{
    (*Checks)++;
    if (Condition)
        return true;
    (*Failures)++;
    DbgPrintEx(DPFLTR_ERROR_LEVEL, "DHCP self-test: FAIL %s\n", Name);
    return false;
}

bool NetDhcpRunSelfTests(void)
{
    uint32_t Checks = 0, Failures = 0;
    NET_INTERFACE Interface = {0};
    const uint8_t Mac[6] = {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0x01};
    memcpy(Interface.MacAddress, Mac, sizeof(Mac));

    uint8_t Packet[NET_DHCP_MAX_SIZE];
    const uint32_t Xid = 0x4E58524Fu;
    uint32_t Length = NetpBuildDhcpRequest(&Interface, Packet, DhcpDiscover, Xid, NULL, NULL);
    bool RequestValid = Length == NET_DHCP_MIN_PACKET_SIZE && Packet[0] == 1 && Packet[1] == 1 && Packet[2] == 6 &&
                        NetpReadBe32(Packet + 4) == Xid && memcmp(Packet + 28, Mac, sizeof(Mac)) == 0;
    uint8_t MessageType = 0, ClientId[7] = {0};
    RequestValid = RequestValid &&
                   NetpCopyDhcpOption(Packet + NET_DHCP_FIXED_SIZE, Length - NET_DHCP_FIXED_SIZE,
                                      DhcpOptionMessageType, &MessageType, sizeof(MessageType)) &&
                   MessageType == DhcpDiscover &&
                   NetpCopyDhcpOption(Packet + NET_DHCP_FIXED_SIZE, Length - NET_DHCP_FIXED_SIZE, DhcpOptionClientId,
                                      ClientId, sizeof(ClientId)) &&
                   ClientId[0] == 1 && memcmp(ClientId + 1, Mac, sizeof(Mac)) == 0;
    NetpDhcpTest(RequestValid, "DISCOVER includes transaction and client identifiers", &Checks, &Failures);

    const uint8_t RequestedIp[4] = {10, 0, 2, 15}, ServerId[4] = {10, 0, 2, 2};
    Length = NetpBuildDhcpRequest(&Interface, Packet, DhcpRequest, Xid, RequestedIp, ServerId);
    uint8_t ParsedIp[4] = {0}, ParsedServer[4] = {0};
    bool SelectValid = Length == NET_DHCP_MIN_PACKET_SIZE &&
                       NetpCopyDhcpOption(Packet + NET_DHCP_FIXED_SIZE, Length - NET_DHCP_FIXED_SIZE,
                                          DhcpOptionRequestedIp, ParsedIp, sizeof(ParsedIp)) &&
                       NetpCopyDhcpOption(Packet + NET_DHCP_FIXED_SIZE, Length - NET_DHCP_FIXED_SIZE,
                                          DhcpOptionServerId, ParsedServer, sizeof(ParsedServer)) &&
                       memcmp(ParsedIp, RequestedIp, sizeof(ParsedIp)) == 0 &&
                       memcmp(ParsedServer, ServerId, sizeof(ParsedServer)) == 0;
    NetpDhcpTest(SelectValid, "REQUEST selects offered address and server", &Checks, &Failures);

    memset(Packet, 0, sizeof(Packet));
    Packet[0] = 2;
    Packet[1] = 1;
    Packet[2] = 6;
    NetpWriteBe32(Packet + 4, Xid);
    const uint8_t OfferedIp[4] = {10, 0, 2, 15}, ServerIp[4] = {10, 0, 2, 2};
    memcpy(Packet + 16, OfferedIp, sizeof(OfferedIp));
    memcpy(Packet + 20, ServerIp, sizeof(ServerIp));
    memcpy(Packet + 28, Mac, sizeof(Mac));
    NetpWriteBe32(Packet + 236, NET_DHCP_MAGIC);
    uint32_t OptionLength = 0;
    uint8_t OfferType = DhcpAck;
    const uint8_t Mask[4] = {255, 255, 255, 0}, Gateway[4] = {10, 0, 2, 2}, Dns[4] = {10, 0, 2, 3};
    bool BuiltOptions = NetpDhcpOption(Packet + NET_DHCP_FIXED_SIZE, &OptionLength, DhcpOptionMessageType,
                                       &OfferType, 1) &&
                        NetpDhcpOption(Packet + NET_DHCP_FIXED_SIZE, &OptionLength, DhcpOptionServerId, ServerIp, 4) &&
                        NetpDhcpOption(Packet + NET_DHCP_FIXED_SIZE, &OptionLength, DhcpOptionSubnetMask, Mask, 4) &&
                        NetpDhcpOption(Packet + NET_DHCP_FIXED_SIZE, &OptionLength, DhcpOptionRouter, Gateway, 4) &&
                        NetpDhcpOption(Packet + NET_DHCP_FIXED_SIZE, &OptionLength, DhcpOptionDns, Dns, 4);
    Packet[NET_DHCP_FIXED_SIZE + OptionLength++] = DhcpOptionEnd;
    NET_UDP_VIEW Udp = {NET_DHCP_PORT_SERVER, NET_DHCP_PORT_CLIENT, Packet,
                        (uint16_t)(NET_DHCP_FIXED_SIZE + OptionLength)};
    NET_DHCP_RESULT Result = {0};
    bool AckValid = BuiltOptions && NetpParseDhcp(&Udp, Xid, Mac, &Result) &&
                    Result.MessageType == DhcpAck && Result.HasServerAddress && Result.HasSubnetMask &&
                    Result.HasRouter && Result.HasDnsServer && memcmp(Result.YourAddress, OfferedIp, 4) == 0 &&
                    memcmp(Result.SubnetMask, Mask, 4) == 0 && memcmp(Result.Router, Gateway, 4) == 0 &&
                    memcmp(Result.DnsServer, Dns, 4) == 0;
    NetpDhcpTest(AckValid, "ACK validates and extracts network configuration", &Checks, &Failures);
    NetpDhcpTest(!NetpParseDhcp(&Udp, Xid + 1, Mac, &Result), "rejects a different transaction", &Checks, &Failures);
    const uint8_t OtherMac[6] = {0x02, 0, 0, 0, 0, 2};
    NetpDhcpTest(!NetpParseDhcp(&Udp, Xid, OtherMac, &Result), "rejects another client MAC", &Checks, &Failures);

    const uint8_t Malformed[] = {DhcpOptionMessageType, 3, DhcpAck, DhcpOptionEnd};
    Udp.Payload = Packet;
    Udp.PayloadLength = NET_DHCP_FIXED_SIZE + sizeof(Malformed);
    memcpy(Packet + NET_DHCP_FIXED_SIZE, Malformed, sizeof(Malformed));
    NetpDhcpTest(!NetpParseDhcp(&Udp, Xid, Mac, &Result), "rejects a malformed message-type option", &Checks,
                 &Failures);

    DbgPrint("DHCP self-tests: %u/%u passed\n", Checks - Failures, Checks);
    return Failures == 0;
}

static bool NetpWaitDhcpReply(NET_INTERFACE *Interface, uint32_t Xid, NET_DHCP_RESULT *Result,
                              uint64_t TimeoutUs)
{
    uint8_t Frame[NETIF_MAX_FRAME_SIZE];
    uint64_t Elapsed = 0;
    bool ReceiveErrorReported = false;
    while (Elapsed < TimeoutUs) {
        uint32_t Length = 0;
        STATUS Status = Interface->ReceiveFrame(Interface, Frame, sizeof(Frame), &Length);
        if (HZ_SUCCESS(Status)) {
            NET_ETHERNET_VIEW Ethernet;
            NET_IPV4_VIEW Ipv4;
            NET_UDP_VIEW Udp;
            if (HZ_SUCCESS(NetParseEthernet(Frame, Length, &Ethernet)) &&
                HZ_SUCCESS(NetParseIpv4(&Ethernet, &Ipv4)) && HZ_SUCCESS(NetParseUdp(&Ipv4, &Udp)) &&
                NetpParseDhcp(&Udp, Xid, Interface->MacAddress, Result))
                return true;
        } else if (Status != STATUS_NO_MORE_ENTRIES && !ReceiveErrorReported) {
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "DHCP: receive error on %s (0x%08X)\n", Interface->Name,
                       (uint32_t)Status);
            ReceiveErrorReported = true;
        }
        KeDelayExecutionThread(NET_DHCP_POLL_US);
        Elapsed += NET_DHCP_POLL_US;
    }
    return false;
}

static void NetpDhcpWorker(void *Context)
{
    NET_INTERFACE *Interface = Context;
    if (!Interface)
        return;

    __atomic_store_n(&Interface->ConfigurationState, 1, __ATOMIC_RELEASE);
    uint64_t Seed = KeQueryUptimeMicroseconds();
    uint32_t Xid = (uint32_t)(Seed ^ ((uint64_t)Interface->MacAddress[2] << 24) ^
                              ((uint64_t)Interface->MacAddress[4] << 8) ^ Interface->MacAddress[5]);
    if (!Xid)
        Xid = 0x4E58524Fu;

    uint8_t Dhcp[NET_DHCP_MAX_SIZE], Frame[NETIF_MAX_FRAME_SIZE];
    NET_DHCP_RESULT Offer = {0}, Ack = {0};
    bool Configured = false;
    for (uint32_t Retry = 0; Retry < NET_DHCP_RETRIES && !Configured; Retry++) {
        if (!Interface->QueryLink(Interface)) {
            KeDelayExecutionThread(250000);
            continue;
        }
        uint32_t Length = NetpBuildDhcpRequest(Interface, Dhcp, DhcpDiscover, Xid, NULL, NULL);
        if (!Length)
            break;
        NetpBuildUdpFrame(Interface, Frame, Dhcp, Length, Retry + 1);
        STATUS Status = Interface->SendFrame(Interface, Frame,
                                             NET_ETHERNET_HEADER_SIZE + NET_IPV4_MIN_HEADER_SIZE +
                                                 NET_UDP_HEADER_SIZE + Length);
        if (!HZ_SUCCESS(Status)) {
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "DHCP: DISCOVER send failed on %s (0x%08X)\n", Interface->Name,
                       (uint32_t)Status);
            KeDelayExecutionThread(NET_DHCP_POLL_US);
            continue;
        }
        if (!NetpWaitDhcpReply(Interface, Xid, &Offer, NET_DHCP_TIMEOUT_US) ||
            Offer.MessageType != DhcpOffer || !Offer.HasServerAddress)
            continue;

        Length = NetpBuildDhcpRequest(Interface, Dhcp, DhcpRequest, Xid, Offer.YourAddress, Offer.ServerAddress);
        if (!Length)
            break;
        NetpBuildUdpFrame(Interface, Frame, Dhcp, Length, Retry + NET_DHCP_RETRIES + 1);
        Status = Interface->SendFrame(Interface, Frame,
                                      NET_ETHERNET_HEADER_SIZE + NET_IPV4_MIN_HEADER_SIZE +
                                          NET_UDP_HEADER_SIZE + Length);
        if (!HZ_SUCCESS(Status)) {
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "DHCP: REQUEST send failed on %s (0x%08X)\n", Interface->Name,
                       (uint32_t)Status);
            continue;
        }
        if (!NetpWaitDhcpReply(Interface, Xid, &Ack, NET_DHCP_TIMEOUT_US))
            continue;
        if (Ack.MessageType == DhcpNak)
            continue;
        if (Ack.MessageType != DhcpAck || !(Ack.YourAddress[0] | Ack.YourAddress[1] | Ack.YourAddress[2] |
                                            Ack.YourAddress[3]))
            continue;
        if (!Ack.HasSubnetMask || !Ack.HasRouter || !Ack.HasDnsServer)
            continue;

        memcpy(Interface->Ipv4Address, Ack.YourAddress, sizeof(Interface->Ipv4Address));
        memcpy(Interface->SubnetMask, Ack.SubnetMask, sizeof(Interface->SubnetMask));
        memcpy(Interface->Gateway, Ack.Router, sizeof(Interface->Gateway));
        memcpy(Interface->DnsServer, Ack.DnsServer, sizeof(Interface->DnsServer));
        __atomic_store_n(&Interface->ConfigurationState, 2, __ATOMIC_RELEASE);
        Configured = true;
    }

    if (Configured) {
        DbgPrint("DHCP: %s configured %u.%u.%u.%u, gateway %u.%u.%u.%u, DNS %u.%u.%u.%u\n",
                 Interface->Name, Interface->Ipv4Address[0], Interface->Ipv4Address[1], Interface->Ipv4Address[2],
                 Interface->Ipv4Address[3], Interface->Gateway[0], Interface->Gateway[1], Interface->Gateway[2],
                 Interface->Gateway[3], Interface->DnsServer[0], Interface->DnsServer[1], Interface->DnsServer[2],
                 Interface->DnsServer[3]);
    } else {
        __atomic_store_n(&Interface->ConfigurationState, 3, __ATOMIC_RELEASE);
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "DHCP: configuration failed for %s after %u attempts\n", Interface->Name,
                   NET_DHCP_RETRIES);
    }
}

STATUS NetStart(void)
{
    NET_INTERFACE *Interface = NetGetInterfaceList();
    if (!Interface)
        return STATUS_NO_SUCH_DEVICE;
    ETHREAD *Thread = NULL;
    STATUS Status = PsCreateSystemThread(NetpDhcpWorker, Interface, 1, "DhcpClient", &Thread);
    if (HZ_SUCCESS(Status) && Thread)
        ObDereferenceObject(Thread);
    return Status;
}
