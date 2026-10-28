/*
 * Nexora OS - include/net.h
 * Kernel network protocol stack layered on the NET_INTERFACE boundary (netif.h): ARP, IPv4 with
 * ICMP echo, UDP with bounded receive queues, TCP with bounded state and timers, and a bounded
 * socket interface. One receive worker per configured interface polls its device and dispatches
 * complete frames; senders submit frames directly through the interface.
 *
 * Locking model: the stack never blocks while holding the TCP or UDP locks. ARP has its own
 * spin lock and per-request events, so a sender may resolve an address while a peer frame is
 * being dispatched. Sockets are owned by one thread at a time (no concurrent use of one socket).
 */
#ifndef NET_H
#define NET_H

#include "packet.h"

HZ_EXTERN_C_BEGIN

/* ---- Host addresses ------------------------------------------------------------------------ */

static inline bool NetAddressZero(const uint8_t Address[4])
{
    return !(Address[0] | Address[1] | Address[2] | Address[3]);
}

static inline bool NetAddressEqual(const uint8_t Left[4], const uint8_t Right[4])
{
    return Left[0] == Right[0] && Left[1] == Right[1] && Left[2] == Right[2] && Left[3] == Right[3];
}

static inline bool NetAddressBroadcast(const uint8_t Address[4])
{
    return Address[0] == 255 && Address[1] == 255 && Address[2] == 255 && Address[3] == 255;
}

/* Receive worker poll interval: the driver ring is drained this often. */
#define NET_POLL_INTERVAL_US 2000ull

/* ---- Stack initialization and the receive path -------------------------------------------- */

/* Phase 1 (after the interfaces are registered): resets the ARP, UDP, TCP and socket tables. */
void NetInitializeStack(void);
/* The receive worker: polls the configured interfaces and dispatches frames (net/netif.c). */
void NetReceiveWorker(void *Context);
/* Parses and dispatches one complete Ethernet frame. */
void NetProcessFrame(NET_INTERFACE *Interface, const void *Frame, uint32_t Length);
/* Submits a complete Ethernet frame, retrying while the ring is busy. */
STATUS NetTransmitFrame(NET_INTERFACE *Interface, const void *Frame, uint32_t Length);
/* Expires ARP entries, retransmits TCP segments and reaps TIME_WAIT connections. */
void NetStackTick(void);
/* Protocol self-tests (net/nettest.c). */
bool NetRunStackSelfTests(void);

/* ---- IPv4 --------------------------------------------------------------------------------- */

/* Builds and sends a datagram to a known next-hop MAC. The payload is a complete transport
   segment. Sets the IPv4 checksum and the don't-fragment bit; never fragments. */
STATUS NetIpSend(NET_INTERFACE *Interface, const uint8_t DestinationMac[6], const uint8_t Source[4],
                 const uint8_t Destination[4], uint8_t Protocol, const void *Payload, uint32_t Length);
/* Resolves the next hop (destination or gateway) through ARP and sends. Call without the TCP or
   UDP lock held. */
STATUS NetIpResolveAndSend(NET_INTERFACE *Interface, const uint8_t Source[4], const uint8_t Destination[4],
                           uint8_t Protocol, const void *Payload, uint32_t Length, uint64_t TimeoutUs);
/* Receives one IPv4 datagram (ICMP, UDP or TCP). */
STATUS NetIpReceive(NET_INTERFACE *Interface, const NET_ETHERNET_VIEW *Ethernet, const NET_IPV4_VIEW *Ipv4);
/* Next hop for a destination: the destination itself or the gateway (zero on failure). */
STATUS NetResolveNextHop(NET_INTERFACE *Interface, const uint8_t Destination[4], uint8_t NextHop[4]);
/* True if the destination is on the interface's subnet. */
bool NetAddressInSubnet(NET_INTERFACE *Interface, const uint8_t Address[4]);
/* Chooses the first configured interface (used by senders given no interface). */
NET_INTERFACE *NetFindConfiguredInterface(void);

/* ---- ARP ---------------------------------------------------------------------------------- */

#define NET_ARP_CACHE_SIZE 16
#define NET_ARP_MAX_PENDING 4
#define NET_ARP_ENTRY_LIFETIME_US 120000000ull /* 120 s */
#define NET_ARP_REQUEST_TIMEOUT_US 1000000ull

void NetArpInitialize(void);
/* Resolves Address to a unicast MAC, sending a request and waiting for the reply. */
STATUS NetArpResolve(NET_INTERFACE *Interface, const uint8_t Address[4], uint8_t Mac[6], uint64_t TimeoutUs);
/* Handles an inbound ARP frame: learns the sender and answers requests for our address. */
void NetArpReceive(NET_INTERFACE *Interface, const NET_ETHERNET_VIEW *Ethernet, const NET_ARP_VIEW *View);
/* Drops expired entries. */
void NetArpTick(void);

/* ---- UDP ---------------------------------------------------------------------------------- */

#define NET_UDP_MAX_BINDINGS 8
#define NET_UDP_QUEUE_DEPTH 3
#define NET_UDP_MAX_DATAGRAM 1472

typedef struct _NET_UDP_BINDING NET_UDP_BINDING;

STATUS NetUdpInitialize(void);
/* Binds a local port (0 = pick an ephemeral port in 49152..65535). */
STATUS NetUdpBind(uint16_t Port, NET_UDP_BINDING **Binding);
void NetUdpUnbind(NET_UDP_BINDING *Binding);
uint16_t NetUdpBindingPort(NET_UDP_BINDING *Binding);
/* Sends a datagram from SourcePort to Destination:DestPort, resolving the next hop. */
STATUS NetUdpSend(NET_INTERFACE *Interface, const uint8_t Source[4], uint16_t SourcePort,
                  const uint8_t Destination[4], uint16_t DestinationPort, const void *Payload,
                  uint32_t Length, uint64_t TimeoutUs);
/* Waits for a datagram on Binding and copies it out. */
STATUS NetUdpReceive(NET_UDP_BINDING *Binding, void *Buffer, uint32_t Capacity, uint8_t Source[4],
                     uint16_t *SourcePort, uint32_t *Received, uint64_t TimeoutUs);
/* Delivers an inbound UDP datagram to the bound port (drops if unbound or the queue is full). */
void NetUdpDeliver(NET_INTERFACE *Interface, const NET_IPV4_VIEW *Ipv4, const NET_UDP_VIEW *View);

/* ---- TCP ---------------------------------------------------------------------------------- */

#define NET_TCP_MAX_CONNECTIONS 8
#define NET_TCP_BACKLOG 4
#define NET_TCP_RX_BUFFER_SIZE 4096
#define NET_TCP_TX_BUFFER_SIZE 4096
#define NET_TCP_MSS 1460
#define NET_TCP_RETRANSMIT_US 500000ull
#define NET_TCP_MAX_RETRANSMITS 5
#define NET_TCP_TIME_WAIT_US 4000000ull
#define NET_TCP_CONNECT_TIMEOUT_US 6000000ull
#define NET_TCP_CLOSE_TIMEOUT_US 8000000ull

typedef enum _NET_TCP_STATE {
    NetTcpStateClosed = 0,
    NetTcpStateListen,
    NetTcpStateSynSent,
    NetTcpStateSynReceived,
    NetTcpStateEstablished,
    NetTcpStateFinWait1,
    NetTcpStateFinWait2,
    NetTcpStateCloseWait,
    NetTcpStateClosing,
    NetTcpStateLastAck,
    NetTcpStateTimeWait,
} NET_TCP_STATE;

typedef struct _NET_TCP_CONNECTION NET_TCP_CONNECTION;

STATUS NetTcpInitialize(void);
/* Allocates a connection slot (NetTcpStateClosed). */
STATUS NetTcpCreate(NET_TCP_CONNECTION **Connection);
/* Opens a connection to Address:Port; blocks up to TimeoutUs. */
STATUS NetTcpConnect(NET_TCP_CONNECTION *Connection, const uint8_t Address[4], uint16_t Port, uint64_t TimeoutUs);
/* Puts the connection in the LISTEN state on LocalPort. */
STATUS NetTcpListen(NET_TCP_CONNECTION *Connection, uint16_t Port);
/* Pops one established child connection from a listener. */
STATUS NetTcpAccept(NET_TCP_CONNECTION *Listener, NET_TCP_CONNECTION **Accepted, uint64_t TimeoutUs);
/* Queues and sends up to Length bytes. */
STATUS NetTcpSend(NET_TCP_CONNECTION *Connection, const void *Buffer, uint32_t Length, uint32_t *Sent,
                  uint64_t TimeoutUs);
/* Copies in-order received bytes out. *Received == 0 with STATUS_SUCCESS means the peer closed. */
STATUS NetTcpReceive(NET_TCP_CONNECTION *Connection, void *Buffer, uint32_t Capacity, uint32_t *Received,
                     uint64_t TimeoutUs);
/* Sends FIN (or RST from a half-open state) and starts the closing handshake. */
STATUS NetTcpClose(NET_TCP_CONNECTION *Connection);
void NetTcpDestroy(NET_TCP_CONNECTION *Connection);
NET_TCP_STATE NetTcpState(NET_TCP_CONNECTION *Connection);
/* Called by NetStackTick from the receive worker. */
void NetTcpTick(void);
/* Delivers an inbound segment. */
void NetTcpDeliver(NET_INTERFACE *Interface, const uint8_t SourceMac[6], const NET_IPV4_VIEW *Ipv4,
                   const NET_TCP_VIEW *View);

/* ---- Sockets ------------------------------------------------------------------------------ */

#define NET_SOCKET_MAX 16
#define NET_SOCKET_TYPE_STREAM 0
#define NET_SOCKET_TYPE_DATAGRAM 1
#define NET_SOCKET_DEFAULT_TIMEOUT_US 6000000ull

#define NET_SOCKET_EVENT_READABLE 0x1
#define NET_SOCKET_EVENT_WRITABLE 0x2
#define NET_SOCKET_EVENT_CLOSED 0x4

typedef struct _NET_SOCKET NET_SOCKET;

void NetSocketInitialize(void);
STATUS NetSocketCreate(uint32_t Type, NET_SOCKET **Socket);
STATUS NetSocketBind(NET_SOCKET *Socket, const uint8_t Address[4], uint16_t Port);
STATUS NetSocketConnect(NET_SOCKET *Socket, const uint8_t Address[4], uint16_t Port, uint64_t TimeoutUs);
STATUS NetSocketListen(NET_SOCKET *Socket, uint16_t Port, uint32_t Backlog);
STATUS NetSocketAccept(NET_SOCKET *Socket, uint8_t Address[4], uint16_t *Port, uint64_t TimeoutUs,
                       NET_SOCKET **Accepted);
STATUS NetSocketSend(NET_SOCKET *Socket, const void *Buffer, uint32_t Length, uint32_t *Sent, uint64_t TimeoutUs);
STATUS NetSocketReceive(NET_SOCKET *Socket, void *Buffer, uint32_t Capacity, uint32_t *Received,
                        uint64_t TimeoutUs);
STATUS NetSocketClose(NET_SOCKET *Socket);
STATUS NetSocketDestroy(NET_SOCKET *Socket);
uint32_t NetSocketPoll(NET_SOCKET *Socket, uint32_t Events, uint32_t *Ready);

HZ_EXTERN_C_END

#endif /* NET_H */
