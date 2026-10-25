/*
 * Nexora OS - net/packet.h
 * Bounds-checked views of common Ethernet and IPv4 packet headers.
 */
#ifndef NET_PACKET_H
#define NET_PACKET_H

#include "netif.h"

HZ_EXTERN_C_BEGIN

#define NET_ETHERNET_HEADER_SIZE 14
#define NET_IPV4_MIN_HEADER_SIZE 20
#define NET_UDP_HEADER_SIZE 8
#define NET_TCP_HEADER_MIN_SIZE 20
#define NET_ETHERTYPE_IPV4 0x0800
#define NET_ETHERTYPE_ARP 0x0806
#define NET_IP_PROTOCOL_ICMP 1
#define NET_IP_PROTOCOL_UDP 17
#define NET_IP_PROTOCOL_TCP 6

/* TCP control bits (NET_TCP_VIEW.Flags). */
#define NET_TCP_FLAG_FIN 0x01
#define NET_TCP_FLAG_SYN 0x02
#define NET_TCP_FLAG_RST 0x04
#define NET_TCP_FLAG_PSH 0x08
#define NET_TCP_FLAG_ACK 0x10
#define NET_TCP_FLAG_URG 0x20

typedef struct _NET_ETHERNET_VIEW {
    const uint8_t *Destination;
    const uint8_t *Source;
    uint16_t EtherType;
    const uint8_t *Payload;
    uint32_t PayloadLength;
} NET_ETHERNET_VIEW;

typedef struct _NET_ARP_VIEW {
    uint16_t Operation;
    const uint8_t *SenderMac;
    const uint8_t *SenderIpv4;
    const uint8_t *TargetMac;
    const uint8_t *TargetIpv4;
} NET_ARP_VIEW;

typedef struct _NET_IPV4_VIEW {
    uint8_t Protocol;
    uint8_t Ttl;
    const uint8_t *Source;
    const uint8_t *Destination;
    const uint8_t *Payload;
    uint16_t PayloadLength;
} NET_IPV4_VIEW;

typedef struct _NET_UDP_VIEW {
    uint16_t SourcePort;
    uint16_t DestinationPort;
    const uint8_t *Payload;
    uint16_t PayloadLength;
} NET_UDP_VIEW;

typedef struct _NET_TCP_VIEW {
    uint16_t SourcePort;
    uint16_t DestinationPort;
    uint32_t Sequence;
    uint32_t Acknowledgement;
    uint8_t DataOffset; /* header length in bytes */
    uint8_t Flags;      /* NET_TCP_FLAG_* */
    uint16_t Window;
    const uint8_t *Segment; /* start of the header (needed for the checksum) */
    uint16_t SegmentLength;
    const uint8_t *Payload;
    uint16_t PayloadLength;
} NET_TCP_VIEW;

STATUS NetParseEthernet(const void *Frame, uint32_t Length, NET_ETHERNET_VIEW *View);
STATUS NetParseArp(const NET_ETHERNET_VIEW *Ethernet, NET_ARP_VIEW *View);
STATUS NetParseIpv4(const NET_ETHERNET_VIEW *Ethernet, NET_IPV4_VIEW *View);
STATUS NetParseUdp(const NET_IPV4_VIEW *Ipv4, NET_UDP_VIEW *View);
STATUS NetParseTcp(const NET_IPV4_VIEW *Ipv4, NET_TCP_VIEW *View);
uint16_t NetIpv4Checksum(const void *Data, uint32_t Length);

/* Ones-complement checksum building blocks (used for the UDP and TCP pseudo-headers). */
uint32_t NetChecksumPartial(uint32_t Sum, const void *Data, uint32_t Length);
uint16_t NetChecksumFold(uint32_t Sum);
uint16_t NetPseudoHeaderSum(const uint8_t Source[4], const uint8_t Destination[4], uint8_t Protocol,
                            uint16_t Length);
uint16_t NetUdpChecksum(const uint8_t Source[4], const uint8_t Destination[4], const void *Segment,
                        uint32_t Length);
bool NetUdpChecksumValid(const uint8_t Source[4], const uint8_t Destination[4], const void *Segment,
                         uint32_t Length);
uint16_t NetTcpChecksum(const uint8_t Source[4], const uint8_t Destination[4], const void *Segment,
                        uint32_t Length);
bool NetTcpChecksumValid(const uint8_t Source[4], const uint8_t Destination[4], const void *Segment,
                         uint32_t Length);

HZ_EXTERN_C_END

#endif /* NET_PACKET_H */
