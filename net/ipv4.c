/*
 * Nexora OS - net/ipv4.c
 * IPv4: the send path, next-hop routing, and the receive dispatch declared by include/net.h.
 * This is the layer every transport above it (UDP, TCP) builds on, and the reason a browser can
 * exist at all: without it nothing can leave the machine.
 *
 * Scope of this file, stated honestly:
 *   - NetIpSend builds one datagram (never fragments; the don't-fragment bit is set) and hands the
 *     complete Ethernet frame to NetTransmitFrame.
 *   - NetIpResolveAndSend resolves the next hop through ARP and then sends.
 *   - NetIpReceive answers ICMP echo requests (so the machine is pingable, a real reachability
 *     check) and hands UDP datagrams to NetUdpDeliver() (net/udp.c). TCP returns
 *     STATUS_NOT_SUPPORTED until net/tcp.c exists; this function must not call NetTcpDeliver()
 *     before then, because a call to an unimplemented function would not link.
 */
#include "ex.h"
#include "ke.h"
#include "net.h"
#include "rtl.h"

#define NETP_IPV4_VERSION_IHL 0x45u
#define NETP_IPV4_TTL 64u
#define NETP_IPV4_FLAG_DF 0x4000u
#define NETP_ICMP_ECHO_REQUEST 8u
#define NETP_ICMP_ECHO_REPLY 0u
#define NETP_ICMP_HEADER_SIZE 8u

static void NetpWriteBe16(uint8_t *Bytes, uint16_t Value)
{
    Bytes[0] = (uint8_t)(Value >> 8);
    Bytes[1] = (uint8_t)Value;
}

bool NetAddressInSubnet(NET_INTERFACE *Interface, const uint8_t Address[4])
{
    if (!Interface || !Address)
        return false;
    /* An interface with no address or no mask has no subnet yet (DHCP has not finished). */
    if (NetAddressZero(Interface->Ipv4Address) || NetAddressZero(Interface->SubnetMask))
        return false;
    for (uint32_t i = 0; i < 4; ++i)
        if ((Address[i] & Interface->SubnetMask[i]) != (Interface->Ipv4Address[i] & Interface->SubnetMask[i]))
            return false;
    return true;
}

STATUS NetResolveNextHop(NET_INTERFACE *Interface, const uint8_t Destination[4], uint8_t NextHop[4])
{
    if (!Interface || !Destination || !NextHop || NetAddressZero(Destination))
        return STATUS_INVALID_PARAMETER;
    if (NetAddressBroadcast(Destination)) {
        memset(NextHop, 0xFF, 4);
        return STATUS_SUCCESS;
    }
    if (NetAddressInSubnet(Interface, Destination)) {
        memcpy(NextHop, Destination, 4);
        return STATUS_SUCCESS;
    }
    if (NetAddressZero(Interface->Gateway))
        return STATUS_UNSUCCESSFUL; /* off-subnet and no gateway: unreachable */
    memcpy(NextHop, Interface->Gateway, 4);
    return STATUS_SUCCESS;
}

NET_INTERFACE *NetFindConfiguredInterface(void)
{
    for (NET_INTERFACE *Current = NetGetInterfaceList(); Current; Current = Current->Next)
        if (!NetAddressZero(Current->Ipv4Address))
            return Current;
    return NULL;
}

STATUS NetIpSend(NET_INTERFACE *Interface, const uint8_t DestinationMac[6], const uint8_t Source[4],
                 const uint8_t Destination[4], uint8_t Protocol, const void *Payload, uint32_t Length)
{
    if (!Interface || !DestinationMac || !Source || !Destination || (!Payload && Length))
        return STATUS_INVALID_PARAMETER;
    if (Length > Interface->Mtu - NET_IPV4_MIN_HEADER_SIZE)
        return STATUS_BUFFER_TOO_SMALL; /* this build never fragments; the caller must fit the MTU */

    uint8_t Frame[NETIF_MAX_FRAME_SIZE];
    uint32_t Total = NET_ETHERNET_HEADER_SIZE + NET_IPV4_MIN_HEADER_SIZE + Length;
    if (Total > sizeof(Frame))
        return STATUS_BUFFER_TOO_SMALL;
    if (Length)
        memcpy(Frame + NET_ETHERNET_HEADER_SIZE + NET_IPV4_MIN_HEADER_SIZE, Payload, Length);

    memcpy(Frame, DestinationMac, 6);
    memcpy(Frame + 6, Interface->MacAddress, 6);
    NetpWriteBe16(Frame + 12, NET_ETHERTYPE_IPV4);

    uint8_t *Ip = Frame + NET_ETHERNET_HEADER_SIZE;
    Ip[0] = NETP_IPV4_VERSION_IHL;
    Ip[1] = 0;
    NetpWriteBe16(Ip + 2, (uint16_t)(NET_IPV4_MIN_HEADER_SIZE + Length));
    static volatile uint32_t Identification;
    NetpWriteBe16(Ip + 4, (uint16_t)__atomic_add_fetch(&Identification, 1, __ATOMIC_RELAXED));
    NetpWriteBe16(Ip + 6, NETP_IPV4_FLAG_DF);
    Ip[8] = NETP_IPV4_TTL;
    Ip[9] = Protocol;
    NetpWriteBe16(Ip + 10, 0);
    memcpy(Ip + 12, Source, 4);
    memcpy(Ip + 16, Destination, 4);
    NetpWriteBe16(Ip + 10, NetIpv4Checksum(Ip, NET_IPV4_MIN_HEADER_SIZE));

    return NetTransmitFrame(Interface, Frame, Total);
}

STATUS NetIpResolveAndSend(NET_INTERFACE *Interface, const uint8_t Source[4], const uint8_t Destination[4],
                           uint8_t Protocol, const void *Payload, uint32_t Length, uint64_t TimeoutUs)
{
    if (!Interface)
        return STATUS_INVALID_PARAMETER;
    uint8_t NextHop[4];
    STATUS Status = NetResolveNextHop(Interface, Destination, NextHop);
    if (!HZ_SUCCESS(Status))
        return Status;

    uint8_t Mac[6];
    if (NetAddressBroadcast(NextHop)) {
        memset(Mac, 0xFF, 6);
    } else {
        Status = NetArpResolve(Interface, NextHop, Mac, TimeoutUs);
        if (!HZ_SUCCESS(Status))
            return Status;
    }
    return NetIpSend(Interface, Mac, Source, Destination, Protocol, Payload, Length);
}

/* Builds the echo reply in place inside Payload and sends it back to the sender. */
static STATUS NetpReplyEcho(NET_INTERFACE *Interface, const NET_ETHERNET_VIEW *Ethernet,
                            const NET_IPV4_VIEW *Ipv4)
{
    uint32_t Length = Ipv4->PayloadLength;
    if (Length < NETP_ICMP_HEADER_SIZE || Length > NET_UDP_MAX_DATAGRAM + NETP_ICMP_HEADER_SIZE)
        return STATUS_DEVICE_DATA_ERROR;
    if (Ipv4->Payload[0] != NETP_ICMP_ECHO_REQUEST || Ipv4->Payload[1] != 0)
        return STATUS_NOT_SUPPORTED;

    uint8_t Message[NETIF_MAX_FRAME_SIZE];
    memcpy(Message, Ipv4->Payload, Length);
    Message[0] = NETP_ICMP_ECHO_REPLY;
    Message[1] = 0;
    NetpWriteBe16(Message + 2, 0);
    NetpWriteBe16(Message + 2, NetIpv4Checksum(Message, Length));

    return NetIpSend(Interface, Ethernet->Source, Ipv4->Destination, Ipv4->Source, NET_IP_PROTOCOL_ICMP,
                     Message, Length);
}

STATUS NetIpReceive(NET_INTERFACE *Interface, const NET_ETHERNET_VIEW *Ethernet, const NET_IPV4_VIEW *Ipv4)
{
    if (!Interface || !Ethernet || !Ipv4)
        return STATUS_INVALID_PARAMETER;

    /* Only traffic addressed to us (or broadcast) is ours to answer. */
    if (!NetAddressEqual(Ipv4->Destination, Interface->Ipv4Address) && !NetAddressBroadcast(Ipv4->Destination))
        return STATUS_SUCCESS;

    switch (Ipv4->Protocol) {
    case NET_IP_PROTOCOL_ICMP:
        return NetpReplyEcho(Interface, Ethernet, Ipv4);
    case NET_IP_PROTOCOL_UDP: {
        NET_UDP_VIEW Udp;
        if (HZ_SUCCESS(NetParseUdp(Ipv4, &Udp)))
            NetUdpDeliver(Interface, Ipv4, &Udp);
        return STATUS_SUCCESS;
    }
    case NET_IP_PROTOCOL_TCP:
        /* Arrives when net/tcp.c is written; dropped cleanly until then. */
        return STATUS_NOT_SUPPORTED;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}
