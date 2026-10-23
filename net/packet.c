/*
 * Nexora OS - net/packet.c
 * Stateless validation and field extraction for Ethernet, ARP, IPv4, and UDP packets.
 */
#include "packet.h"
#include "ke.h"
#include "rtl.h"

static uint16_t NetpReadBe16(const uint8_t *Data)
{
    return (uint16_t)(((uint16_t)Data[0] << 8) | Data[1]);
}

uint16_t NetIpv4Checksum(const void *Data, uint32_t Length)
{
    if (!Data)
        return 0xFFFF;
    const uint8_t *Bytes = Data;
    uint32_t Sum = 0;
    while (Length >= 2) {
        Sum += NetpReadBe16(Bytes);
        Bytes += 2;
        Length -= 2;
    }
    if (Length)
        Sum += (uint16_t)Bytes[0] << 8;
    while (Sum >> 16)
        Sum = (Sum & 0xFFFF) + (Sum >> 16);
    return (uint16_t)~Sum;
}

STATUS NetParseEthernet(const void *Frame, uint32_t Length, NET_ETHERNET_VIEW *View)
{
    if (!Frame || !View || Length < NET_ETHERNET_HEADER_SIZE || Length > NETIF_MAX_FRAME_SIZE)
        return STATUS_INVALID_PARAMETER;
    const uint8_t *Bytes = Frame;
    View->Destination = Bytes;
    View->Source = Bytes + 6;
    View->EtherType = NetpReadBe16(Bytes + 12);
    View->Payload = Bytes + NET_ETHERNET_HEADER_SIZE;
    View->PayloadLength = Length - NET_ETHERNET_HEADER_SIZE;
    return STATUS_SUCCESS;
}

STATUS NetParseArp(const NET_ETHERNET_VIEW *Ethernet, NET_ARP_VIEW *View)
{
    if (!Ethernet || !View || Ethernet->EtherType != NET_ETHERTYPE_ARP || !Ethernet->Payload ||
        Ethernet->PayloadLength < 28)
        return STATUS_DEVICE_DATA_ERROR;
    const uint8_t *Arp = Ethernet->Payload;
    if (NetpReadBe16(Arp) != 1 || NetpReadBe16(Arp + 2) != NET_ETHERTYPE_IPV4 || Arp[4] != 6 || Arp[5] != 4)
        return STATUS_DEVICE_DATA_ERROR;
    uint16_t Operation = NetpReadBe16(Arp + 6);
    if (Operation != 1 && Operation != 2)
        return STATUS_DEVICE_DATA_ERROR;
    View->Operation = Operation;
    View->SenderMac = Arp + 8;
    View->SenderIpv4 = Arp + 14;
    View->TargetMac = Arp + 18;
    View->TargetIpv4 = Arp + 24;
    return STATUS_SUCCESS;
}

STATUS NetParseIpv4(const NET_ETHERNET_VIEW *Ethernet, NET_IPV4_VIEW *View)
{
    if (!Ethernet || !View || Ethernet->EtherType != NET_ETHERTYPE_IPV4 || !Ethernet->Payload ||
        Ethernet->PayloadLength < NET_IPV4_MIN_HEADER_SIZE)
        return STATUS_DEVICE_DATA_ERROR;
    const uint8_t *Ip = Ethernet->Payload;
    uint8_t Version = Ip[0] >> 4;
    uint32_t HeaderLength = (uint32_t)(Ip[0] & 0x0F) * 4;
    uint16_t TotalLength = NetpReadBe16(Ip + 2);
    if (Version != 4 || HeaderLength < NET_IPV4_MIN_HEADER_SIZE || HeaderLength > Ethernet->PayloadLength ||
        TotalLength < HeaderLength || TotalLength > Ethernet->PayloadLength ||
        NetIpv4Checksum(Ip, HeaderLength) != 0)
        return STATUS_DEVICE_DATA_ERROR;
    uint16_t Fragment = NetpReadBe16(Ip + 6);
    if (Fragment & 0x3FFF)
        return STATUS_NOT_SUPPORTED;
    if (Ip[8] == 0)
        return STATUS_DEVICE_DATA_ERROR;
    View->Ttl = Ip[8];
    View->Protocol = Ip[9];
    View->Source = Ip + 12;
    View->Destination = Ip + 16;
    View->Payload = Ip + HeaderLength;
    View->PayloadLength = (uint16_t)(TotalLength - HeaderLength);
    return STATUS_SUCCESS;
}

STATUS NetParseUdp(const NET_IPV4_VIEW *Ipv4, NET_UDP_VIEW *View)
{
    if (!Ipv4 || !View || Ipv4->Protocol != NET_IP_PROTOCOL_UDP || !Ipv4->Payload ||
        Ipv4->PayloadLength < NET_UDP_HEADER_SIZE)
        return STATUS_DEVICE_DATA_ERROR;
    uint16_t Length = NetpReadBe16(Ipv4->Payload + 4);
    if (Length < NET_UDP_HEADER_SIZE || Length > Ipv4->PayloadLength)
        return STATUS_DEVICE_DATA_ERROR;
    View->SourcePort = NetpReadBe16(Ipv4->Payload);
    View->DestinationPort = NetpReadBe16(Ipv4->Payload + 2);
    View->Payload = Ipv4->Payload + NET_UDP_HEADER_SIZE;
    View->PayloadLength = (uint16_t)(Length - NET_UDP_HEADER_SIZE);
    return STATUS_SUCCESS;
}

static bool NetpTest(bool Condition, const char *Name, uint32_t *Checks, uint32_t *Failures)
{
    (*Checks)++;
    if (Condition)
        return true;
    (*Failures)++;
    DbgPrintEx(DPFLTR_ERROR_LEVEL, "Network self-test: FAIL %s\n", Name);
    return false;
}

bool NetRunProtocolSelfTests(void)
{
    uint32_t Checks = 0, Failures = 0;
    uint8_t Frame[64] = {0};
    NET_ETHERNET_VIEW Ethernet = {0};
    STATUS Status = NetParseEthernet(Frame, 42, &Ethernet);
    NetpTest(Status == STATUS_SUCCESS && Ethernet.PayloadLength == 28, "ethernet bounds", &Checks, &Failures);
    NetpTest(NetParseEthernet(Frame, 13, &Ethernet) == STATUS_INVALID_PARAMETER, "short ethernet frame", &Checks,
             &Failures);

    Frame[12] = 0x08;
    Frame[13] = 0x06;
    uint8_t *Arp = Frame + NET_ETHERNET_HEADER_SIZE;
    Arp[1] = 1;
    Arp[2] = 0x08;
    Arp[3] = 0x00;
    Arp[4] = 6;
    Arp[5] = 4;
    Arp[7] = 1;
    NetParseEthernet(Frame, 42, &Ethernet);
    NET_ARP_VIEW ArpView = {0};
    NetpTest(NetParseArp(&Ethernet, &ArpView) == STATUS_SUCCESS && ArpView.Operation == 1, "valid ARP request",
             &Checks, &Failures);
    Arp[4] = 5;
    NetpTest(NetParseArp(&Ethernet, &ArpView) == STATUS_DEVICE_DATA_ERROR, "invalid ARP address size", &Checks,
             &Failures);

    memset(Frame, 0, sizeof(Frame));
    Frame[12] = 0x08;
    Frame[13] = 0x00;
    uint8_t *Ip = Frame + NET_ETHERNET_HEADER_SIZE;
    Ip[0] = 0x45;
    Ip[2] = 0;
    Ip[3] = 28;
    Ip[8] = 64;
    Ip[9] = NET_IP_PROTOCOL_UDP;
    Ip[12] = 192;
    Ip[14] = 2;
    Ip[15] = 1;
    Ip[16] = 198;
    Ip[17] = 51;
    Ip[18] = 100;
    Ip[19] = 2;
    uint16_t Checksum = NetIpv4Checksum(Ip, NET_IPV4_MIN_HEADER_SIZE);
    Ip[10] = (uint8_t)(Checksum >> 8);
    Ip[11] = (uint8_t)Checksum;
    uint8_t *Udp = Ip + NET_IPV4_MIN_HEADER_SIZE;
    Udp[0] = 0x30;
    Udp[1] = 0x39;
    Udp[2] = 0;
    Udp[3] = 53;
    Udp[5] = NET_UDP_HEADER_SIZE;
    NetParseEthernet(Frame, NET_ETHERNET_HEADER_SIZE + 28, &Ethernet);
    NET_IPV4_VIEW IpView = {0};
    Status = NetParseIpv4(&Ethernet, &IpView);
    NetpTest(Status == STATUS_SUCCESS && IpView.Protocol == NET_IP_PROTOCOL_UDP && IpView.PayloadLength == 8,
             "valid IPv4 datagram", &Checks, &Failures);
    NET_UDP_VIEW UdpView = {0};
    NetpTest(Status == STATUS_SUCCESS && NetParseUdp(&IpView, &UdpView) == STATUS_SUCCESS &&
                 UdpView.SourcePort == 12345 && UdpView.DestinationPort == 53 && UdpView.PayloadLength == 0,
             "valid UDP datagram", &Checks, &Failures);

    Ip[0] = 0x65;
    NetpTest(NetParseIpv4(&Ethernet, &IpView) == STATUS_DEVICE_DATA_ERROR, "invalid IPv4 version", &Checks,
             &Failures);
    Ip[0] = 0x45;
    Ip[10] ^= 0x01;
    NetpTest(NetParseIpv4(&Ethernet, &IpView) == STATUS_DEVICE_DATA_ERROR, "invalid IPv4 checksum", &Checks,
             &Failures);

    DbgPrintEx(Failures ? DPFLTR_ERROR_LEVEL : DPFLTR_INFO_LEVEL, "Network protocol self-tests: %u/%u passed\n",
               Checks - Failures, Checks);
    return Failures == 0;
}
