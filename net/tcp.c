/*
 * Bounded TCP/IPv4 active connections. This intentionally implements the small stream subset
 * needed by clients: active open, in-order data, ACK/retransmit, peer FIN and graceful close.
 */
#include "ex.h"
#include "ke.h"
#include "net.h"
#include "rtl.h"

#define NETP_TCP_EPHEMERAL_MIN 49152u
#define NETP_TCP_POLL_US 2000ull
#define NETP_TCP_ACK_TIMEOUT_US 1000000ull
#define NETP_TCP_TX_UNSENT_TIMESTAMP UINT64_MAX

struct _NET_TCP_CONNECTION {
    bool Allocated;
    bool PeerClosed;
    bool PeerReset;
    bool TxPending;
    bool TxFailed;
    NET_TCP_STATE State;
    NET_INTERFACE *Interface;
    uint8_t LocalAddress[4];
    uint8_t RemoteAddress[4];
    uint8_t RemoteMac[6];
    uint16_t LocalPort;
    uint16_t RemotePort;
    uint16_t PeerMss;
    uint32_t SendUnacknowledged;
    uint32_t SendNext;
    uint32_t PeerWindowRightEdge;
    uint32_t PeerWindowSequence;
    uint32_t PeerWindowAcknowledgement;
    uint32_t ReceiveNext;
    uint32_t TxSequenceEnd;
    uint64_t TxTimestamp;
    uint64_t TimeWaitTimestamp;
    uint32_t TxRetries;
    uint16_t TxLength;
    uint8_t TxSegment[NET_TCP_HEADER_MIN_SIZE + NET_TCP_MSS];
    uint32_t ReceiveHead;
    uint32_t ReceiveCount;
    uint8_t ReceiveBuffer[NET_TCP_RX_BUFFER_SIZE];
};

typedef NET_TCP_CONNECTION NETP_TCP_CONNECTION;

static NETP_TCP_CONNECTION NetpTcpConnections[NET_TCP_MAX_CONNECTIONS];
static KSPIN_LOCK NetpTcpLock;
static bool NetpTcpInitialized;
static uint16_t NetpTcpEphemeral;

static bool NetpTcpSequenceAfter(uint32_t Left, uint32_t Right)
{
    return (int32_t)(Left - Right) > 0;
}

static bool NetpTcpSequenceAtOrAfter(uint32_t Left, uint32_t Right)
{
    return !NetpTcpSequenceAfter(Right, Left);
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

static uint16_t NetpTcpPeerMss(const NET_TCP_VIEW *View)
{
    uint16_t Mss = 536;
    for (uint32_t Offset = NET_TCP_HEADER_MIN_SIZE; Offset < View->DataOffset;) {
        uint8_t Kind = View->Segment[Offset];
        if (Kind == 0)
            break;
        if (Kind == 1) {
            ++Offset;
            continue;
        }
        if (Offset + 2 > View->DataOffset)
            break;
        uint8_t Length = View->Segment[Offset + 1];
        if (Length < 2 || Length > View->DataOffset - Offset)
            break;
        if (Kind == 2 && Length == 4) {
            uint16_t Advertised = (uint16_t)(((uint16_t)View->Segment[Offset + 2] << 8) |
                                             View->Segment[Offset + 3]);
            if (Advertised)
                Mss = (uint16_t)HZ_MIN(Advertised, NET_TCP_MSS);
        }
        Offset += Length;
    }
    return Mss;
}

static bool NetpTcpValidConnection(NET_TCP_CONNECTION *Connection)
{
    for (uint32_t i = 0; i < NET_TCP_MAX_CONNECTIONS; ++i)
        if (Connection == (NET_TCP_CONNECTION *)&NetpTcpConnections[i])
            return NetpTcpConnections[i].Allocated;
    return false;
}

static void NetpTcpInitializeOnce(void)
{
    if (NetpTcpInitialized)
        return;
    KeInitializeSpinLock(&NetpTcpLock);
    NetpTcpEphemeral = (uint16_t)(NETP_TCP_EPHEMERAL_MIN +
                                  (KeQueryUptimeMicroseconds() & 0x3FFFu));
    NetpTcpInitialized = true;
}

STATUS NetTcpInitialize(void)
{
    NetpTcpInitializeOnce();
    KIRQL OldIrql;
    KeAcquireSpinLock(&NetpTcpLock, &OldIrql);
    memset(NetpTcpConnections, 0, sizeof(NetpTcpConnections));
    KeReleaseSpinLock(&NetpTcpLock, OldIrql);
    return STATUS_SUCCESS;
}

STATUS NetTcpCreate(NET_TCP_CONNECTION **Connection)
{
    if (!Connection)
        return STATUS_INVALID_PARAMETER;
    *Connection = NULL;
    NetpTcpInitializeOnce();
    KIRQL OldIrql;
    KeAcquireSpinLock(&NetpTcpLock, &OldIrql);
    for (uint32_t i = 0; i < NET_TCP_MAX_CONNECTIONS; ++i) {
        if (NetpTcpConnections[i].Allocated)
            continue;
        memset(&NetpTcpConnections[i], 0, sizeof(NetpTcpConnections[i]));
        NetpTcpConnections[i].Allocated = true;
        NetpTcpConnections[i].State = NetTcpStateClosed;
        *Connection = (NET_TCP_CONNECTION *)&NetpTcpConnections[i];
        break;
    }
    KeReleaseSpinLock(&NetpTcpLock, OldIrql);
    return *Connection ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

static uint16_t NetpTcpPickPort(void)
{
    for (uint32_t attempt = 0; attempt < 0x4000u; ++attempt) {
        bool Used = false;
        NetpTcpEphemeral++;
        if (NetpTcpEphemeral < NETP_TCP_EPHEMERAL_MIN)
            NetpTcpEphemeral = NETP_TCP_EPHEMERAL_MIN;
        for (uint32_t i = 0; i < NET_TCP_MAX_CONNECTIONS; ++i)
            if (NetpTcpConnections[i].Allocated && NetpTcpConnections[i].LocalPort == NetpTcpEphemeral)
                Used = true;
        if (!Used)
            return NetpTcpEphemeral;
    }
    return 0;
}

static uint16_t NetpTcpBuild(uint8_t *Segment, const uint8_t Source[4], const uint8_t Destination[4],
                             uint16_t SourcePort, uint16_t DestinationPort, uint32_t Sequence,
                             uint32_t Acknowledgement, uint8_t Flags, uint16_t Window,
                             const void *Payload, uint16_t PayloadLength)
{
    uint8_t HeaderLength = (Flags & NET_TCP_FLAG_SYN) ? 24 : NET_TCP_HEADER_MIN_SIZE;
    uint16_t Length = (uint16_t)(HeaderLength + PayloadLength);
    memset(Segment, 0, HeaderLength);
    NetpWriteBe16(Segment, SourcePort);
    NetpWriteBe16(Segment + 2, DestinationPort);
    NetpWriteBe32(Segment + 4, Sequence);
    NetpWriteBe32(Segment + 8, Acknowledgement);
    Segment[12] = (uint8_t)((HeaderLength / 4) << 4);
    Segment[13] = Flags;
    NetpWriteBe16(Segment + 14, Window);
    if (HeaderLength == 24) {
        Segment[20] = 2;
        Segment[21] = 4;
        NetpWriteBe16(Segment + 22, NET_TCP_MSS);
    }
    if (PayloadLength)
        memcpy(Segment + HeaderLength, Payload, PayloadLength);
    NetpWriteBe16(Segment + 16, NetTcpChecksum(Source, Destination, Segment, Length));
    return Length;
}

static STATUS NetpTcpSendRaw(NET_INTERFACE *Interface, const uint8_t Source[4], const uint8_t Destination[4],
                             const uint8_t DestinationMac[6], uint16_t SourcePort, uint16_t DestinationPort,
                             uint32_t Sequence, uint32_t Acknowledgement, uint8_t Flags, uint16_t Window,
                             const void *Payload, uint16_t PayloadLength)
{
    uint8_t Segment[NET_TCP_HEADER_MIN_SIZE + NET_TCP_MSS];
    uint16_t Length = NetpTcpBuild(Segment, Source, Destination, SourcePort, DestinationPort, Sequence,
                                   Acknowledgement, Flags, Window, Payload, PayloadLength);
    if (DestinationMac)
        return NetIpSend(Interface, DestinationMac, Source, Destination, NET_IP_PROTOCOL_TCP, Segment, Length);
    return NetIpResolveAndSend(Interface, Source, Destination, NET_IP_PROTOCOL_TCP, Segment, Length,
                               NETP_TCP_ACK_TIMEOUT_US);
}

static bool NetpTcpAbortSyn(NET_TCP_CONNECTION *Connection, STATUS *SendStatus)
{
    NET_INTERFACE *Interface;
    uint8_t Source[4], Destination[4], DestinationMac[6];
    uint16_t SourcePort, DestinationPort;
    uint32_t Sequence;
    KIRQL OldIrql;
    KeAcquireSpinLock(&NetpTcpLock, &OldIrql);
    if (!NetpTcpValidConnection(Connection) || Connection->State != NetTcpStateSynSent) {
        KeReleaseSpinLock(&NetpTcpLock, OldIrql);
        return false;
    }
    Interface = Connection->Interface;
    memcpy(Source, Connection->LocalAddress, 4);
    memcpy(Destination, Connection->RemoteAddress, 4);
    memcpy(DestinationMac, Connection->RemoteMac, 6);
    SourcePort = Connection->LocalPort;
    DestinationPort = Connection->RemotePort;
    Sequence = Connection->SendNext;
    Connection->State = NetTcpStateClosed;
    Connection->TxPending = false;
    KeReleaseSpinLock(&NetpTcpLock, OldIrql);

    bool HasMac = false;
    for (uint32_t i = 0; i < sizeof(DestinationMac); ++i)
        HasMac = HasMac || DestinationMac[i] != 0;
    HasMac = HasMac && !(DestinationMac[0] & 1);
    *SendStatus = NetpTcpSendRaw(Interface, Source, Destination, HasMac ? DestinationMac : NULL,
                                 SourcePort, DestinationPort, Sequence, 0, NET_TCP_FLAG_RST, 0, NULL, 0);
    return true;
}

static STATUS NetpTcpTransmitStored(NETP_TCP_CONNECTION *Connection)
{
    uint8_t Segment[NET_TCP_HEADER_MIN_SIZE + NET_TCP_MSS];
    uint8_t Source[4], Destination[4], DestinationMac[6];
    uint16_t Length;
    NET_INTERFACE *Interface;
    bool HasMac = false;
    KIRQL OldIrql;
    KeAcquireSpinLock(&NetpTcpLock, &OldIrql);
    if (!NetpTcpValidConnection((NET_TCP_CONNECTION *)Connection) || !Connection->TxPending) {
        KeReleaseSpinLock(&NetpTcpLock, OldIrql);
        return STATUS_INVALID_PARAMETER;
    }
    Length = Connection->TxLength;
    memcpy(Segment, Connection->TxSegment, Length);
    memcpy(Source, Connection->LocalAddress, 4);
    memcpy(Destination, Connection->RemoteAddress, 4);
    memcpy(DestinationMac, Connection->RemoteMac, 6);
    for (uint32_t i = 0; i < sizeof(DestinationMac); ++i)
        HasMac = HasMac || DestinationMac[i] != 0;
    HasMac = HasMac && !(DestinationMac[0] & 1);
    Interface = Connection->Interface;
    KeReleaseSpinLock(&NetpTcpLock, OldIrql);
    /* Retransmits run on the sole receive worker; use its learned MAC to avoid blocking on ARP there. */
    STATUS Status = HasMac
                        ? NetIpSend(Interface, DestinationMac, Source, Destination, NET_IP_PROTOCOL_TCP, Segment,
                                    Length)
                        : NetIpResolveAndSend(Interface, Source, Destination, NET_IP_PROTOCOL_TCP, Segment, Length,
                                              NETP_TCP_ACK_TIMEOUT_US);
    if (HZ_SUCCESS(Status)) {
        KeAcquireSpinLock(&NetpTcpLock, &OldIrql);
        if (NetpTcpValidConnection((NET_TCP_CONNECTION *)Connection) && Connection->TxPending)
            Connection->TxTimestamp = KeQueryUptimeMicroseconds();
        KeReleaseSpinLock(&NetpTcpLock, OldIrql);
    }
    return Status;
}

static void NetpTcpSetPending(NETP_TCP_CONNECTION *Connection, uint8_t Flags, uint32_t Sequence,
                              const void *Payload, uint16_t PayloadLength)
{
    Connection->TxLength = NetpTcpBuild(Connection->TxSegment, Connection->LocalAddress,
                                        Connection->RemoteAddress, Connection->LocalPort,
                                        Connection->RemotePort, Sequence, Connection->ReceiveNext, Flags,
                                        NET_TCP_RX_BUFFER_SIZE - Connection->ReceiveCount, Payload,
                                        PayloadLength);
    Connection->TxSequenceEnd = Sequence + PayloadLength +
                                ((Flags & NET_TCP_FLAG_SYN) ? 1u : 0u) +
                                ((Flags & NET_TCP_FLAG_FIN) ? 1u : 0u);
    Connection->TxPending = true;
    Connection->TxFailed = false;
    /* The caller may need ARP resolution; don't let the receive worker retransmit before it completes. */
    Connection->TxTimestamp = NETP_TCP_TX_UNSENT_TIMESTAMP;
    Connection->TxRetries = 0;
}

STATUS NetTcpConnect(NET_TCP_CONNECTION *Connection, const uint8_t Address[4], uint16_t Port,
                     uint64_t TimeoutUs)
{
    if (!Connection || !Address || !Port || NetAddressZero(Address) || NetAddressBroadcast(Address) ||
        Address[0] >= 224)
        return STATUS_INVALID_PARAMETER;
    NET_INTERFACE *Interface = NetFindConfiguredInterface();
    if (!Interface || __atomic_load_n(&Interface->ConfigurationState, __ATOMIC_ACQUIRE) != 2 ||
        !Interface->QueryLink || !Interface->QueryLink(Interface))
        return STATUS_DEVICE_NOT_READY;

    KIRQL OldIrql;
    KeAcquireSpinLock(&NetpTcpLock, &OldIrql);
    if (!NetpTcpValidConnection(Connection) || Connection->State != NetTcpStateClosed) {
        KeReleaseSpinLock(&NetpTcpLock, OldIrql);
        return STATUS_INVALID_PARAMETER;
    }
    uint16_t LocalPort = NetpTcpPickPort();
    if (!LocalPort) {
        KeReleaseSpinLock(&NetpTcpLock, OldIrql);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Connection->Interface = Interface;
    memcpy(Connection->LocalAddress, Interface->Ipv4Address, 4);
    memcpy(Connection->RemoteAddress, Address, 4);
    Connection->LocalPort = LocalPort;
    Connection->RemotePort = Port;
    Connection->PeerMss = 536;
    uint32_t InitialSequence = (uint32_t)(KeQueryUptimeMicroseconds() ^
                                          ((uint64_t)LocalPort << 16) ^ (uintptr_t)Connection);
    Connection->SendUnacknowledged = InitialSequence;
    Connection->SendNext = InitialSequence + 1;
    Connection->State = NetTcpStateSynSent;
    NetpTcpSetPending(Connection, NET_TCP_FLAG_SYN, InitialSequence, NULL, 0);
    KeReleaseSpinLock(&NetpTcpLock, OldIrql);

    STATUS Status = NetpTcpTransmitStored((NETP_TCP_CONNECTION *)Connection);
    if (!HZ_SUCCESS(Status)) {
        KeAcquireSpinLock(&NetpTcpLock, &OldIrql);
        Connection->State = NetTcpStateClosed;
        Connection->TxPending = false;
        KeReleaseSpinLock(&NetpTcpLock, OldIrql);
        return Status;
    }
    uint64_t Elapsed = 0;
    while (Elapsed < TimeoutUs) {
        KeAcquireSpinLock(&NetpTcpLock, &OldIrql);
        NET_TCP_STATE State = Connection->State;
        bool TimedOut = Connection->TxFailed;
        KeReleaseSpinLock(&NetpTcpLock, OldIrql);
        if (State == NetTcpStateEstablished)
            return STATUS_SUCCESS;
        if (State == NetTcpStateClosed)
            return TimedOut ? STATUS_IO_TIMEOUT : STATUS_UNSUCCESSFUL;
        KeDelayExecutionThread(NETP_TCP_POLL_US);
        Elapsed += NETP_TCP_POLL_US;
    }
    STATUS ResetStatus;
    if (!NetpTcpAbortSyn(Connection, &ResetStatus)) {
        KeAcquireSpinLock(&NetpTcpLock, &OldIrql);
        bool Established = NetpTcpValidConnection(Connection) &&
                           Connection->State == NetTcpStateEstablished;
        KeReleaseSpinLock(&NetpTcpLock, OldIrql);
        if (Established)
            return STATUS_SUCCESS;
    }
    return STATUS_IO_TIMEOUT;
}

STATUS NetTcpListen(NET_TCP_CONNECTION *Connection, uint16_t Port)
{
    (void)Connection;
    (void)Port;
    return STATUS_NOT_SUPPORTED;
}

STATUS NetTcpAccept(NET_TCP_CONNECTION *Listener, NET_TCP_CONNECTION **Accepted, uint64_t TimeoutUs)
{
    (void)Listener;
    (void)Accepted;
    (void)TimeoutUs;
    return STATUS_NOT_SUPPORTED;
}

STATUS NetTcpSend(NET_TCP_CONNECTION *Connection, const void *Buffer, uint32_t Length, uint32_t *Sent,
                  uint64_t TimeoutUs)
{
    if (!Connection || (!Buffer && Length) || !Sent)
        return STATUS_INVALID_PARAMETER;
    *Sent = 0;
    uint64_t Elapsed = 0;
    const uint8_t *Bytes = Buffer;
    while (*Sent < Length) {
        KIRQL OldIrql;
        KeAcquireSpinLock(&NetpTcpLock, &OldIrql);
        if (!NetpTcpValidConnection(Connection)) {
            KeReleaseSpinLock(&NetpTcpLock, OldIrql);
            return STATUS_DEVICE_NOT_READY;
        }
        if (Connection->TxFailed) {
            KeReleaseSpinLock(&NetpTcpLock, OldIrql);
            return STATUS_IO_TIMEOUT;
        }
        if (Connection->State != NetTcpStateEstablished) {
            KeReleaseSpinLock(&NetpTcpLock, OldIrql);
            return STATUS_DEVICE_NOT_READY;
        }
        uint32_t AvailableWindow = NetpTcpSequenceAfter(Connection->PeerWindowRightEdge,
                                                         Connection->SendNext)
                                       ? Connection->PeerWindowRightEdge - Connection->SendNext
                                       : 0;
        if (Connection->TxPending || !AvailableWindow) {
            KeReleaseSpinLock(&NetpTcpLock, OldIrql);
            if (Elapsed >= TimeoutUs)
                return STATUS_IO_TIMEOUT;
            KeDelayExecutionThread(NETP_TCP_POLL_US);
            Elapsed += NETP_TCP_POLL_US;
            continue;
        }
        uint32_t Remaining = Length - *Sent;
        uint16_t Chunk = (uint16_t)HZ_MIN(Remaining, Connection->PeerMss);
        Chunk = (uint16_t)HZ_MIN(Chunk, AvailableWindow);
        uint32_t Sequence = Connection->SendNext;
        Connection->SendNext += Chunk;
        NetpTcpSetPending(Connection, NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH, Sequence, Bytes + *Sent, Chunk);
        KeReleaseSpinLock(&NetpTcpLock, OldIrql);

        STATUS Status = NetpTcpTransmitStored((NETP_TCP_CONNECTION *)Connection);
        if (!HZ_SUCCESS(Status)) {
            KeAcquireSpinLock(&NetpTcpLock, &OldIrql);
            if (Connection->TxPending && Connection->TxSequenceEnd == Sequence + Chunk) {
                Connection->TxPending = false;
                Connection->SendNext = Sequence;
            }
            KeReleaseSpinLock(&NetpTcpLock, OldIrql);
            return Status;
        }
        *Sent += Chunk;
        while (Elapsed < TimeoutUs) {
            KeAcquireSpinLock(&NetpTcpLock, &OldIrql);
            bool Pending = Connection->TxPending;
            bool Failed = Connection->TxFailed;
            NET_TCP_STATE State = Connection->State;
            if (!Pending && !Failed && State == NetTcpStateEstablished) {
                KeReleaseSpinLock(&NetpTcpLock, OldIrql);
                break;
            }
            KeReleaseSpinLock(&NetpTcpLock, OldIrql);
            if (Failed || State != NetTcpStateEstablished)
                return STATUS_UNSUCCESSFUL;
            KeDelayExecutionThread(NETP_TCP_POLL_US);
            Elapsed += NETP_TCP_POLL_US;
        }
        if (*Sent < Length && Elapsed >= TimeoutUs)
            return STATUS_IO_TIMEOUT;
    }
    return STATUS_SUCCESS;
}

STATUS NetTcpReceive(NET_TCP_CONNECTION *Connection, void *Buffer, uint32_t Capacity, uint32_t *Received,
                     uint64_t TimeoutUs)
{
    if (!Connection || !Buffer || !Capacity || !Received)
        return STATUS_INVALID_PARAMETER;
    *Received = 0;
    uint64_t Elapsed = 0;
    while (Elapsed < TimeoutUs || TimeoutUs == 0) {
        KIRQL OldIrql;
        KeAcquireSpinLock(&NetpTcpLock, &OldIrql);
        if (!NetpTcpValidConnection(Connection)) {
            KeReleaseSpinLock(&NetpTcpLock, OldIrql);
            return STATUS_INVALID_PARAMETER;
        }
        if (Connection->ReceiveCount) {
            uint32_t Count = HZ_MIN(Capacity, Connection->ReceiveCount);
            uint32_t First = HZ_MIN(Count, NET_TCP_RX_BUFFER_SIZE - Connection->ReceiveHead);
            memcpy(Buffer, Connection->ReceiveBuffer + Connection->ReceiveHead, First);
            if (Count > First)
                memcpy((uint8_t *)Buffer + First, Connection->ReceiveBuffer, Count - First);
            Connection->ReceiveHead = (Connection->ReceiveHead + Count) % NET_TCP_RX_BUFFER_SIZE;
            Connection->ReceiveCount -= Count;
            *Received = Count;
            uint8_t Local[4], Remote[4], Mac[6];
            memcpy(Local, Connection->LocalAddress, 4);
            memcpy(Remote, Connection->RemoteAddress, 4);
            memcpy(Mac, Connection->RemoteMac, 6);
            uint16_t LocalPort = Connection->LocalPort;
            uint16_t RemotePort = Connection->RemotePort;
            uint16_t Window = (uint16_t)(NET_TCP_RX_BUFFER_SIZE - Connection->ReceiveCount);
            uint32_t Sequence = Connection->SendNext;
            uint32_t Acknowledgement = Connection->ReceiveNext;
            NET_INTERFACE *Interface = Connection->Interface;
            KeReleaseSpinLock(&NetpTcpLock, OldIrql);
            NetpTcpSendRaw(Interface, Local, Remote, Mac, LocalPort, RemotePort, Sequence, Acknowledgement,
                           NET_TCP_FLAG_ACK, Window, NULL, 0);
            return STATUS_SUCCESS;
        }
        bool Closed = Connection->PeerClosed || Connection->State == NetTcpStateClosed ||
                      Connection->State == NetTcpStateCloseWait;
        bool Reset = Connection->PeerReset;
        bool TimedOut = Connection->TxFailed;
        KeReleaseSpinLock(&NetpTcpLock, OldIrql);
        if (Closed)
            return Reset ? STATUS_UNSUCCESSFUL : TimedOut ? STATUS_IO_TIMEOUT : STATUS_SUCCESS;
        if (TimeoutUs && Elapsed >= TimeoutUs)
            break;
        KeDelayExecutionThread(NETP_TCP_POLL_US);
        Elapsed += NETP_TCP_POLL_US;
    }
    return STATUS_IO_TIMEOUT;
}

STATUS NetTcpClose(NET_TCP_CONNECTION *Connection)
{
    if (!Connection)
        return STATUS_INVALID_PARAMETER;
    STATUS ResetStatus;
    if (NetpTcpAbortSyn(Connection, &ResetStatus))
        return ResetStatus;
    KIRQL OldIrql;
    uint64_t Elapsed = 0;
    for (;;) {
        KeAcquireSpinLock(&NetpTcpLock, &OldIrql);
        if (!NetpTcpValidConnection(Connection)) {
            KeReleaseSpinLock(&NetpTcpLock, OldIrql);
            return STATUS_INVALID_PARAMETER;
        }
        bool Pending = Connection->TxPending;
        KeReleaseSpinLock(&NetpTcpLock, OldIrql);
        if (!Pending)
            break;
        if (Elapsed >= NET_TCP_CLOSE_TIMEOUT_US)
            return STATUS_IO_TIMEOUT;
        KeDelayExecutionThread(NETP_TCP_POLL_US);
        Elapsed += NETP_TCP_POLL_US;
    }
    KeAcquireSpinLock(&NetpTcpLock, &OldIrql);
    if (!NetpTcpValidConnection(Connection)) {
        KeReleaseSpinLock(&NetpTcpLock, OldIrql);
        return STATUS_INVALID_PARAMETER;
    }
    if (Connection->State == NetTcpStateClosed) {
        bool TimedOut = Connection->TxFailed;
        KeReleaseSpinLock(&NetpTcpLock, OldIrql);
        return TimedOut ? STATUS_IO_TIMEOUT : STATUS_SUCCESS;
    }
    if (Connection->State != NetTcpStateEstablished && Connection->State != NetTcpStateCloseWait) {
        Connection->State = NetTcpStateClosed;
        Connection->TxPending = false;
        KeReleaseSpinLock(&NetpTcpLock, OldIrql);
        return STATUS_SUCCESS;
    }
    uint8_t Flags = NET_TCP_FLAG_FIN | NET_TCP_FLAG_ACK;
    uint32_t Sequence = Connection->SendNext++;
    Connection->State = Connection->State == NetTcpStateCloseWait ? NetTcpStateLastAck : NetTcpStateFinWait1;
    NetpTcpSetPending(Connection, Flags, Sequence, NULL, 0);
    KeReleaseSpinLock(&NetpTcpLock, OldIrql);
    STATUS Status = NetpTcpTransmitStored((NETP_TCP_CONNECTION *)Connection);
    if (!HZ_SUCCESS(Status)) {
        KeAcquireSpinLock(&NetpTcpLock, &OldIrql);
        Connection->State = NetTcpStateClosed;
        Connection->TxPending = false;
        KeReleaseSpinLock(&NetpTcpLock, OldIrql);
        return Status;
    }

    Elapsed = 0;
    while (Elapsed < NET_TCP_CLOSE_TIMEOUT_US) {
        KeAcquireSpinLock(&NetpTcpLock, &OldIrql);
        NET_TCP_STATE State = Connection->State;
        KeReleaseSpinLock(&NetpTcpLock, OldIrql);
        if (State == NetTcpStateClosed)
            return STATUS_SUCCESS;
        KeDelayExecutionThread(NETP_TCP_POLL_US);
        Elapsed += NETP_TCP_POLL_US;
    }
    return STATUS_IO_TIMEOUT;
}

void NetTcpDestroy(NET_TCP_CONNECTION *Connection)
{
    if (!Connection || !NetpTcpInitialized)
        return;
    KIRQL OldIrql;
    KeAcquireSpinLock(&NetpTcpLock, &OldIrql);
    if (NetpTcpValidConnection(Connection))
        memset(Connection, 0, sizeof(NETP_TCP_CONNECTION));
    KeReleaseSpinLock(&NetpTcpLock, OldIrql);
}

NET_TCP_STATE NetTcpState(NET_TCP_CONNECTION *Connection)
{
    if (!Connection || !NetpTcpInitialized)
        return NetTcpStateClosed;
    KIRQL OldIrql;
    KeAcquireSpinLock(&NetpTcpLock, &OldIrql);
    NET_TCP_STATE State = NetpTcpValidConnection(Connection) ? Connection->State : NetTcpStateClosed;
    KeReleaseSpinLock(&NetpTcpLock, OldIrql);
    return State;
}

void NetTcpDeliver(NET_INTERFACE *Interface, const uint8_t SourceMac[6], const NET_IPV4_VIEW *Ipv4,
                   const NET_TCP_VIEW *View)
{
    if (!Interface || !SourceMac || !Ipv4 || !View || !NetpTcpInitialized)
        return;
    NETP_TCP_CONNECTION *Connection = NULL;
    bool SendAck = false;
    uint32_t AckSequence = 0, AckNumber = 0;
    uint16_t AckPort = 0, LocalPort = 0, AckWindow = 0;
    uint8_t Local[4], Remote[4], Mac[6];
    KIRQL OldIrql;
    KeAcquireSpinLock(&NetpTcpLock, &OldIrql);
    for (uint32_t i = 0; i < NET_TCP_MAX_CONNECTIONS; ++i) {
        NETP_TCP_CONNECTION *Candidate = &NetpTcpConnections[i];
        if (Candidate->Allocated && Candidate->Interface == Interface &&
            Candidate->LocalPort == View->DestinationPort && Candidate->RemotePort == View->SourcePort &&
            NetAddressEqual(Candidate->LocalAddress, Ipv4->Destination) &&
            NetAddressEqual(Candidate->RemoteAddress, Ipv4->Source)) {
            Connection = Candidate;
            break;
        }
    }
    if (!Connection) {
        KeReleaseSpinLock(&NetpTcpLock, OldIrql);
        return;
    }
    if (Connection->State == NetTcpStateSynSent) {
        if (View->Flags & NET_TCP_FLAG_RST) {
            if ((View->Flags & NET_TCP_FLAG_ACK) &&
                View->Acknowledgement == Connection->SendNext) {
                memcpy(Connection->RemoteMac, SourceMac, 6);
                Connection->State = NetTcpStateClosed;
                Connection->TxPending = false;
            }
        } else if ((View->Flags & (NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK)) ==
                (NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK) &&
            View->Acknowledgement == Connection->SendNext) {
            memcpy(Connection->RemoteMac, SourceMac, 6);
            Connection->SendUnacknowledged = View->Acknowledgement;
            Connection->ReceiveNext = View->Sequence + 1;
            Connection->PeerMss = NetpTcpPeerMss(View);
            Connection->PeerWindowRightEdge = View->Acknowledgement + View->Window;
            Connection->PeerWindowSequence = View->Sequence;
            Connection->PeerWindowAcknowledgement = View->Acknowledgement;
            Connection->TxPending = false;
            Connection->State = NetTcpStateEstablished;
            SendAck = true;
        }
    } else {
        if (Connection->State == NetTcpStateClosed) {
            KeReleaseSpinLock(&NetpTcpLock, OldIrql);
            return;
        }
        if (View->Flags & NET_TCP_FLAG_RST) {
            if (Connection->State != NetTcpStateTimeWait &&
                View->Sequence == Connection->ReceiveNext) {
                memcpy(Connection->RemoteMac, SourceMac, 6);
                Connection->State = NetTcpStateClosed;
                Connection->TxPending = false;
                Connection->PeerReset = true;
            } else if (Connection->State != NetTcpStateTimeWait) {
                SendAck = true;
            }
        } else if (Connection->State == NetTcpStateTimeWait) {
            uint32_t IncomingSequence = View->Sequence;
            if ((View->Flags & NET_TCP_FLAG_FIN) &&
                IncomingSequence + View->PayloadLength + 1 == Connection->ReceiveNext) {
                Connection->TimeWaitTimestamp = KeQueryUptimeMicroseconds();
            }
            SendAck = true;
        } else {
            memcpy(Connection->RemoteMac, SourceMac, 6);
            bool ValidAck = (View->Flags & NET_TCP_FLAG_ACK) &&
                            NetpTcpSequenceAtOrAfter(View->Acknowledgement,
                                                     Connection->SendUnacknowledged) &&
                            !NetpTcpSequenceAfter(View->Acknowledgement, Connection->SendNext);
            if (ValidAck) {
                bool NewerWindow = NetpTcpSequenceAfter(View->Sequence, Connection->PeerWindowSequence) ||
                                   (View->Sequence == Connection->PeerWindowSequence &&
                                    NetpTcpSequenceAtOrAfter(View->Acknowledgement,
                                                             Connection->PeerWindowAcknowledgement));
                if (NewerWindow) {
                    Connection->PeerWindowRightEdge = View->Acknowledgement + View->Window;
                    Connection->PeerWindowSequence = View->Sequence;
                    Connection->PeerWindowAcknowledgement = View->Acknowledgement;
                }
                if (NetpTcpSequenceAfter(View->Acknowledgement, Connection->SendUnacknowledged)) {
                    Connection->SendUnacknowledged = View->Acknowledgement;
                    if (Connection->TxPending &&
                        NetpTcpSequenceAtOrAfter(View->Acknowledgement, Connection->TxSequenceEnd))
                        Connection->TxPending = false;
                    if (Connection->State == NetTcpStateFinWait1 && !Connection->TxPending)
                        Connection->State = NetTcpStateFinWait2;
                    else if (Connection->State == NetTcpStateLastAck && !Connection->TxPending)
                        Connection->State = NetTcpStateClosed;
                    else if (Connection->State == NetTcpStateClosing && !Connection->TxPending) {
                        Connection->State = NetTcpStateTimeWait;
                        Connection->TimeWaitTimestamp = KeQueryUptimeMicroseconds();
                    }
                }
            }

            uint32_t IncomingSequence = View->Sequence;
            bool CanReceive = Connection->State == NetTcpStateEstablished ||
                              Connection->State == NetTcpStateFinWait1 ||
                              Connection->State == NetTcpStateFinWait2;
            if (View->PayloadLength && CanReceive && IncomingSequence == Connection->ReceiveNext &&
                View->PayloadLength <= NET_TCP_RX_BUFFER_SIZE - Connection->ReceiveCount) {
                uint32_t Tail = (Connection->ReceiveHead + Connection->ReceiveCount) % NET_TCP_RX_BUFFER_SIZE;
                uint32_t First = HZ_MIN((uint32_t)View->PayloadLength, NET_TCP_RX_BUFFER_SIZE - Tail);
                memcpy(Connection->ReceiveBuffer + Tail, View->Payload, First);
                if (View->PayloadLength > First)
                    memcpy(Connection->ReceiveBuffer, View->Payload + First, View->PayloadLength - First);
                Connection->ReceiveCount += View->PayloadLength;
                Connection->ReceiveNext += View->PayloadLength;
                SendAck = true;
            } else if (View->PayloadLength) {
                SendAck = true;
            }
            if (View->Flags & NET_TCP_FLAG_FIN) {
                if (!Connection->PeerClosed &&
                    IncomingSequence + View->PayloadLength == Connection->ReceiveNext) {
                    Connection->ReceiveNext++;
                    Connection->PeerClosed = true;
                    if (Connection->State == NetTcpStateEstablished)
                        Connection->State = NetTcpStateCloseWait;
                    else if (Connection->State == NetTcpStateFinWait1)
                        Connection->State = Connection->TxPending ? NetTcpStateClosing : NetTcpStateTimeWait;
                    else if (Connection->State == NetTcpStateFinWait2)
                        Connection->State = NetTcpStateTimeWait;
                    if (Connection->State == NetTcpStateTimeWait)
                        Connection->TimeWaitTimestamp = KeQueryUptimeMicroseconds();
                }
                SendAck = true;
            }
        }
    }
    if (SendAck) {
        AckSequence = Connection->SendNext;
        AckNumber = Connection->ReceiveNext;
        AckWindow = (uint16_t)(NET_TCP_RX_BUFFER_SIZE - Connection->ReceiveCount);
        AckPort = Connection->RemotePort;
        LocalPort = Connection->LocalPort;
        memcpy(Local, Connection->LocalAddress, 4);
        memcpy(Remote, Connection->RemoteAddress, 4);
        memcpy(Mac, SourceMac, 6);
    }
    KeReleaseSpinLock(&NetpTcpLock, OldIrql);
    if (SendAck)
        NetpTcpSendRaw(Interface, Local, Remote, Mac, LocalPort, AckPort, AckSequence, AckNumber,
                       NET_TCP_FLAG_ACK, AckWindow, NULL, 0);
}

void NetTcpTick(void)
{
    if (!NetpTcpInitialized)
        return;
    uint64_t Now = KeQueryUptimeMicroseconds();
    NETP_TCP_CONNECTION *Retransmit[NET_TCP_MAX_CONNECTIONS];
    uint32_t RetransmitCount = 0;
    KIRQL OldIrql;
    KeAcquireSpinLock(&NetpTcpLock, &OldIrql);
    for (uint32_t i = 0; i < NET_TCP_MAX_CONNECTIONS; ++i) {
        NETP_TCP_CONNECTION *Connection = &NetpTcpConnections[i];
        if (!Connection->Allocated)
            continue;
        if (Connection->State == NetTcpStateTimeWait &&
            Now - Connection->TimeWaitTimestamp >= NET_TCP_TIME_WAIT_US) {
            Connection->State = NetTcpStateClosed;
            continue;
        }
        if (!Connection->TxPending || Connection->TxTimestamp == NETP_TCP_TX_UNSENT_TIMESTAMP ||
            Now - Connection->TxTimestamp < NET_TCP_RETRANSMIT_US)
            continue;
        if (Connection->TxRetries >= NET_TCP_MAX_RETRANSMITS) {
            Connection->TxPending = false;
            Connection->TxFailed = true;
            Connection->State = NetTcpStateClosed;
            continue;
        }
        Connection->TxTimestamp = Now;
        Connection->TxRetries++;
        Retransmit[RetransmitCount++] = Connection;
    }
    KeReleaseSpinLock(&NetpTcpLock, OldIrql);
    for (uint32_t i = 0; i < RetransmitCount; ++i)
        NetpTcpTransmitStored(Retransmit[i]);
}
