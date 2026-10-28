/*
 * Nexora OS - net/udp.c
 * User Datagram Protocol: a bounded set of port bindings, each owning a small queue of received
 * datagrams, layered on the IPv4 send/receive path in net/ipv4.c. Nothing above this file can reach
 * the network until it exists (DNS name lookups are the first caller).
 *
 * Inbound datagrams are dispatched by the sole NetReceiveWorker into the bounded port queues.
 */
#include "ke.h"
#include "net.h"
#include "rtl.h"

#define NETP_UDP_EPHEMERAL_MIN 49152u
#define NETP_UDP_EPHEMERAL_MAX 65535u
#define NETP_UDP_POLL_US 2000ull

typedef struct _NETP_UDP_DATAGRAM {
    uint32_t Length;
    uint8_t Source[4];
    uint16_t SourcePort;
    uint8_t Data[NET_UDP_MAX_DATAGRAM];
} NETP_UDP_DATAGRAM;

/* The opaque NET_UDP_BINDING from include/net.h: one local port and a bounded receive queue. */
struct _NET_UDP_BINDING {
    uint16_t Port;
    uint32_t Head; /* index of the oldest queued datagram */
    uint32_t Count;
    NETP_UDP_DATAGRAM Queue[NET_UDP_QUEUE_DEPTH];
};

static NET_UDP_BINDING NetpUdpBindings[NET_UDP_MAX_BINDINGS];
static KSPIN_LOCK NetpUdpLock;
static bool NetpUdpInitialized;
static uint16_t NetpUdpEphemeral;

static void NetpWriteBe16(uint8_t *Bytes, uint16_t Value)
{
    Bytes[0] = (uint8_t)(Value >> 8);
    Bytes[1] = (uint8_t)Value;
}

/* Caller holds NetpUdpLock. */
static bool NetpUdpPortInUse(uint16_t Port)
{
    for (uint32_t i = 0; i < NET_UDP_MAX_BINDINGS; ++i)
        if (NetpUdpBindings[i].Port == Port)
            return true;
    return false;
}

/* Sets up the lock and the ephemeral port seed once; never clears a live binding. */
static void NetpUdpEnsureInitialized(void)
{
    if (NetpUdpInitialized)
        return;
    KeInitializeSpinLock(&NetpUdpLock);
    NetpUdpEphemeral = (uint16_t)(NETP_UDP_EPHEMERAL_MIN + (KeQueryUptimeMicroseconds() & 0x1FFFu));
    NetpUdpInitialized = true;
}

STATUS NetUdpInitialize(void)
{
    NetpUdpEnsureInitialized();

    KIRQL OldIrql;
    KeAcquireSpinLock(&NetpUdpLock, &OldIrql);
    for (uint32_t i = 0; i < NET_UDP_MAX_BINDINGS; ++i) {
        NetpUdpBindings[i].Port = 0;
        NetpUdpBindings[i].Head = 0;
        NetpUdpBindings[i].Count = 0;
    }
    KeReleaseSpinLock(&NetpUdpLock, OldIrql);
    return STATUS_SUCCESS;
}

STATUS NetUdpBind(uint16_t Port, NET_UDP_BINDING **Binding)
{
    if (!Binding)
        return STATUS_INVALID_PARAMETER;
    *Binding = NULL;
    NetpUdpEnsureInitialized();

    KIRQL OldIrql;
    KeAcquireSpinLock(&NetpUdpLock, &OldIrql);
    if (!Port) {
        /* Port 0 asks for an ephemeral port in 49152..65535 (RFC 6335). */
        for (uint32_t Attempt = 0; Attempt < 0x4000u && !Port; ++Attempt) {
            NetpUdpEphemeral++;
            if (NetpUdpEphemeral < NETP_UDP_EPHEMERAL_MIN)
                NetpUdpEphemeral = NETP_UDP_EPHEMERAL_MIN;
            if (!NetpUdpPortInUse(NetpUdpEphemeral))
                Port = NetpUdpEphemeral;
        }
        if (!Port) {
            KeReleaseSpinLock(&NetpUdpLock, OldIrql);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    } else if (NetpUdpPortInUse(Port)) {
        KeReleaseSpinLock(&NetpUdpLock, OldIrql);
        return STATUS_OBJECT_NAME_COLLISION;
    }

    for (uint32_t i = 0; i < NET_UDP_MAX_BINDINGS; ++i) {
        if (NetpUdpBindings[i].Port)
            continue;
        NetpUdpBindings[i].Port = Port;
        NetpUdpBindings[i].Head = 0;
        NetpUdpBindings[i].Count = 0;
        *Binding = &NetpUdpBindings[i];
        break;
    }
    KeReleaseSpinLock(&NetpUdpLock, OldIrql);

    if (!*Binding)
        return STATUS_INSUFFICIENT_RESOURCES;
    DbgPrint("Network: UDP bound port %u\n", Port);
    return STATUS_SUCCESS;
}

void NetUdpUnbind(NET_UDP_BINDING *Binding)
{
    if (!Binding || !NetpUdpInitialized)
        return;
    KIRQL OldIrql;
    KeAcquireSpinLock(&NetpUdpLock, &OldIrql);
    Binding->Port = 0;
    Binding->Head = 0;
    Binding->Count = 0;
    KeReleaseSpinLock(&NetpUdpLock, OldIrql);
}

uint16_t NetUdpBindingPort(NET_UDP_BINDING *Binding)
{
    return Binding ? Binding->Port : 0;
}

STATUS NetUdpSend(NET_INTERFACE *Interface, const uint8_t Source[4], uint16_t SourcePort,
                  const uint8_t Destination[4], uint16_t DestinationPort, const void *Payload,
                  uint32_t Length, uint64_t TimeoutUs)
{
    if (!Source || !Destination || (!Payload && Length))
        return STATUS_INVALID_PARAMETER;
    if (Length > NET_UDP_MAX_DATAGRAM)
        return STATUS_BUFFER_TOO_SMALL;
    if (!Interface)
        Interface = NetFindConfiguredInterface();
    if (!Interface || !Interface->QueryLink)
        return STATUS_DEVICE_NOT_READY;
    if (!Interface->QueryLink(Interface))
        return STATUS_DEVICE_NOT_READY;

    uint8_t Segment[NET_UDP_HEADER_SIZE + NET_UDP_MAX_DATAGRAM];
    uint32_t SegmentLength = NET_UDP_HEADER_SIZE + Length;
    uint8_t LocalSource[4];
    memcpy(LocalSource, Source, 4);
    if (NetAddressZero(LocalSource))
        memcpy(LocalSource, Interface->Ipv4Address, 4); /* DHCP sends from 0.0.0.0 */

    NetpWriteBe16(Segment, SourcePort);
    NetpWriteBe16(Segment + 2, DestinationPort);
    NetpWriteBe16(Segment + 4, (uint16_t)SegmentLength);
    NetpWriteBe16(Segment + 6, 0);
    if (Length)
        memcpy(Segment + NET_UDP_HEADER_SIZE, Payload, Length);

    uint16_t Checksum = NetUdpChecksum(LocalSource, Destination, Segment, SegmentLength);
    if (!Checksum)
        Checksum = 0xFFFFu; /* a computed zero is transmitted as all ones (RFC 768) */
    NetpWriteBe16(Segment + 6, Checksum);

    return NetIpResolveAndSend(Interface, LocalSource, Destination, NET_IP_PROTOCOL_UDP, Segment,
                               SegmentLength, TimeoutUs);
}

void NetUdpDeliver(NET_INTERFACE *Interface, const NET_IPV4_VIEW *Ipv4, const NET_UDP_VIEW *View)
{
    if (!Interface || !Ipv4 || !View || !View->Payload || !NetpUdpInitialized)
        return;
    if (View->PayloadLength > NET_UDP_MAX_DATAGRAM)
        return;
    uint32_t SegmentLength = NET_UDP_HEADER_SIZE + View->PayloadLength;
    if (SegmentLength > Ipv4->PayloadLength)
        return;
    /* A datagram with a bad checksum is not a datagram, whatever its port says. */
    if (!NetUdpChecksumValid(Ipv4->Source, Ipv4->Destination, Ipv4->Payload, SegmentLength))
        return;

    KIRQL OldIrql;
    KeAcquireSpinLock(&NetpUdpLock, &OldIrql);
    for (uint32_t i = 0; i < NET_UDP_MAX_BINDINGS; ++i) {
        NET_UDP_BINDING *Binding = &NetpUdpBindings[i];
        if (Binding->Port != View->DestinationPort)
            continue;
        if (Binding->Count >= NET_UDP_QUEUE_DEPTH) {
            KeReleaseSpinLock(&NetpUdpLock, OldIrql);
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "Network: UDP port %u queue full, %u bytes dropped\n",
                       Binding->Port, View->PayloadLength);
            return;
        }
        NETP_UDP_DATAGRAM *Slot = &Binding->Queue[(Binding->Head + Binding->Count) % NET_UDP_QUEUE_DEPTH];
        Slot->Length = View->PayloadLength;
        memcpy(Slot->Source, Ipv4->Source, 4);
        Slot->SourcePort = View->SourcePort;
        if (View->PayloadLength)
            memcpy(Slot->Data, View->Payload, View->PayloadLength);
        Binding->Count++;
        KeReleaseSpinLock(&NetpUdpLock, OldIrql);
        return;
    }
    KeReleaseSpinLock(&NetpUdpLock, OldIrql); /* unbound port: dropped */
}

STATUS NetUdpReceive(NET_UDP_BINDING *Binding, void *Buffer, uint32_t Capacity, uint8_t Source[4],
                     uint16_t *SourcePort, uint32_t *Received, uint64_t TimeoutUs)
{
    if (!Binding || (!Buffer && Capacity) || !Received)
        return STATUS_INVALID_PARAMETER;
    *Received = 0;
    if (!NetpUdpInitialized)
        return STATUS_DEVICE_NOT_READY;

    uint64_t Elapsed = 0;
    while (Elapsed < TimeoutUs) {
        bool Dequeued = false;
        KIRQL OldIrql;
        KeAcquireSpinLock(&NetpUdpLock, &OldIrql);
        if (Binding->Count) {
            NETP_UDP_DATAGRAM *Slot = &Binding->Queue[Binding->Head];
            Binding->Head = (Binding->Head + 1) % NET_UDP_QUEUE_DEPTH;
            Binding->Count--;
            if (Capacity < Slot->Length) {
                KeReleaseSpinLock(&NetpUdpLock, OldIrql);
                return STATUS_BUFFER_TOO_SMALL; /* the datagram is dropped, not truncated */
            }
            if (Slot->Length)
                memcpy(Buffer, Slot->Data, Slot->Length);
            if (Source)
                memcpy(Source, Slot->Source, 4);
            if (SourcePort)
                *SourcePort = Slot->SourcePort;
            *Received = Slot->Length;
            Dequeued = true;
        }
        KeReleaseSpinLock(&NetpUdpLock, OldIrql);
        if (Dequeued)
            return STATUS_SUCCESS;

        KeDelayExecutionThread(NETP_UDP_POLL_US);
        Elapsed += NETP_UDP_POLL_US;
    }
    return STATUS_IO_TIMEOUT;
}
