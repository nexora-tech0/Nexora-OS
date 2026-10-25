/*
 * Nexora OS - net/arp.c
 * Address Resolution Protocol: a bounded cache with expiry, request/reply handling, and
 * address resolution used by IPv4 send. Resolution is not serialized with the rest of the stack:
 * each request owns one pending slot whose event is signaled by the receive worker, so a sender
 * can resolve while frames are being dispatched.
 */
#include "ex.h"
#include "ke.h"
#include "net.h"
#include "rtl.h"

typedef struct _NET_ARP_ENTRY {
    bool Valid;
    uint8_t Address[4];
    uint8_t Mac[6];
    uint64_t ExpiryUs;
} NET_ARP_ENTRY;

typedef struct _NET_ARP_PENDING {
    bool InUse;
    uint8_t Address[4];
    KEVENT Event;
} NET_ARP_PENDING;

static NET_ARP_ENTRY NetpArpCache[NET_ARP_CACHE_SIZE];
static NET_ARP_PENDING NetpArpPending[NET_ARP_MAX_PENDING];
static KSPIN_LOCK NetpArpLock;
static bool NetpArpInitialized;

static void NetpWriteBe16(uint8_t *Bytes, uint16_t Value)
{
    Bytes[0] = (uint8_t)(Value >> 8);
    Bytes[1] = (uint8_t)Value;
}

static bool NetpMacUnicast(const uint8_t Mac[6])
{
    return !(Mac[0] & 1) && (Mac[0] | Mac[1] | Mac[2] | Mac[3] | Mac[4] | Mac[5]) != 0;
}

void NetArpInitialize(void)
{
    memset(NetpArpCache, 0, sizeof(NetpArpCache));
    memset(NetpArpPending, 0, sizeof(NetpArpPending));
    KeInitializeSpinLock(&NetpArpLock);
    NetpArpInitialized = true;
}

/* Caller holds NetpArpLock. */
static NET_ARP_ENTRY *NetpFindEntry(const uint8_t Address[4])
{
    for (uint32_t i = 0; i < NET_ARP_CACHE_SIZE; ++i)
        if (NetpArpCache[i].Valid && NetAddressEqual(NetpArpCache[i].Address, Address))
            return &NetpArpCache[i];
    return NULL;
}

/* Caller holds NetpArpLock. Learns Address -> Mac, replacing the oldest entry when full. */
static void NetpLearn(const uint8_t Address[4], const uint8_t Mac[6])
{
    if (!NetpMacUnicast(Mac) || NetAddressZero(Address) || NetAddressBroadcast(Address))
        return;
    NET_ARP_ENTRY *Entry = NetpFindEntry(Address);
    if (!Entry) {
        Entry = &NetpArpCache[0];
        for (uint32_t i = 0; i < NET_ARP_CACHE_SIZE; ++i) {
            if (!NetpArpCache[i].Valid) {
                Entry = &NetpArpCache[i];
                break;
            }
            if (NetpArpCache[i].ExpiryUs < Entry->ExpiryUs)
                Entry = &NetpArpCache[i];
        }
    }
    Entry->Valid = true;
    memcpy(Entry->Address, Address, 4);
    memcpy(Entry->Mac, Mac, 6);
    Entry->ExpiryUs = KeQueryUptimeMicroseconds() + NET_ARP_ENTRY_LIFETIME_US;
}

/* Caller holds NetpArpLock. */
static void NetpSignalPending(const uint8_t Address[4])
{
    for (uint32_t i = 0; i < NET_ARP_MAX_PENDING; ++i)
        if (NetpArpPending[i].InUse && NetAddressEqual(NetpArpPending[i].Address, Address))
            KeSetEvent(&NetpArpPending[i].Event);
}

static void NetpBuildArpFrame(NET_INTERFACE *Interface, uint16_t Operation, const uint8_t DestinationMac[6],
                              const uint8_t TargetIp[4], uint8_t Frame[42])
{
    static const uint8_t Broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    static const uint8_t NoMac[6] = {0};
    memset(Frame, 0, 42);
    memcpy(Frame, Operation == 1 ? Broadcast : DestinationMac, 6);
    memcpy(Frame + 6, Interface->MacAddress, 6);
    NetpWriteBe16(Frame + 12, NET_ETHERTYPE_ARP);
    uint8_t *Arp = Frame + NET_ETHERNET_HEADER_SIZE;
    NetpWriteBe16(Arp, 1);
    NetpWriteBe16(Arp + 2, NET_ETHERTYPE_IPV4);
    Arp[4] = 6;
    Arp[5] = 4;
    NetpWriteBe16(Arp + 6, Operation);
    memcpy(Arp + 8, Interface->MacAddress, 6);
    memcpy(Arp + 14, Interface->Ipv4Address, 4);
    memcpy(Arp + 18, Operation == 1 ? NoMac : DestinationMac, 6);
    memcpy(Arp + 24, TargetIp, 4);
}

static void NetpArpSendRequest(NET_INTERFACE *Interface, const uint8_t Address[4])
{
    static const uint8_t NoMac[6] = {0};
    uint8_t Frame[42];
    NetpBuildArpFrame(Interface, 1, NoMac, Address, Frame);
    NetTransmitFrame(Interface, Frame, sizeof(Frame));
}

STATUS NetArpResolve(NET_INTERFACE *Interface, const uint8_t Address[4], uint8_t Mac[6], uint64_t TimeoutUs)
{
    if (!Interface || !Address || !Mac || !NetpArpInitialized)
        return STATUS_INVALID_PARAMETER;
    if (NetAddressZero(Address) || NetAddressBroadcast(Address))
        return STATUS_INVALID_PARAMETER;

    KIRQL OldIrql;
    KeAcquireSpinLock(&NetpArpLock, &OldIrql);
    NET_ARP_ENTRY *Entry = NetpFindEntry(Address);
    if (Entry && Entry->ExpiryUs > KeQueryUptimeMicroseconds()) {
        memcpy(Mac, Entry->Mac, 6);
        KeReleaseSpinLock(&NetpArpLock, OldIrql);
        return STATUS_SUCCESS;
    }
    NET_ARP_PENDING *Pending = NULL;
    for (uint32_t i = 0; i < NET_ARP_MAX_PENDING; ++i) {
        if (!NetpArpPending[i].InUse) {
            Pending = &NetpArpPending[i];
            break;
        }
    }
    if (!Pending) {
        KeReleaseSpinLock(&NetpArpLock, OldIrql);
        return STATUS_DEVICE_BUSY;
    }
    Pending->InUse = true;
    memcpy(Pending->Address, Address, 4);
    KeResetEvent(&Pending->Event);
    KeReleaseSpinLock(&NetpArpLock, OldIrql);

    NetpArpSendRequest(Interface, Address);

    uint64_t Deadline = KeQueryUptimeMicroseconds() + TimeoutUs;
    STATUS Result = STATUS_IO_TIMEOUT;
    for (;;) {
        KeAcquireSpinLock(&NetpArpLock, &OldIrql);
        Entry = NetpFindEntry(Address);
        if (Entry && Entry->ExpiryUs > KeQueryUptimeMicroseconds()) {
            memcpy(Mac, Entry->Mac, 6);
            KeReleaseSpinLock(&NetpArpLock, OldIrql);
            Result = STATUS_SUCCESS;
            break;
        }
        KeReleaseSpinLock(&NetpArpLock, OldIrql);
        uint64_t Now = KeQueryUptimeMicroseconds();
        if (Now >= Deadline)
            break;
        uint64_t Slice = HZ_MIN(Deadline - Now, NET_ARP_REQUEST_TIMEOUT_US / 2);
        KeWaitForSingleObject(&Pending->Event, &Slice);
    }

    KeAcquireSpinLock(&NetpArpLock, &OldIrql);
    Pending->InUse = false;
    KeReleaseSpinLock(&NetpArpLock, OldIrql);
    return Result;
}

void NetArpReceive(NET_INTERFACE *Interface, const NET_ETHERNET_VIEW *Ethernet, const NET_ARP_VIEW *View)
{
    (void)Ethernet;
    if (!Interface || !View)
        return;
    KIRQL OldIrql;
    KeAcquireSpinLock(&NetpArpLock, &OldIrql);
    NetpLearn(View->SenderIpv4, View->SenderMac);
    NetpSignalPending(View->SenderIpv4);
    bool ForUs = View->Operation == 1 && NetAddressEqual(View->TargetIpv4, Interface->Ipv4Address) &&
                 !NetAddressZero(Interface->Ipv4Address);
    KeReleaseSpinLock(&NetpArpLock, OldIrql);

    if (ForUs) {
        uint8_t Frame[42];
        NetpBuildArpFrame(Interface, 2, View->SenderMac, View->SenderIpv4, Frame);
        NetTransmitFrame(Interface, Frame, sizeof(Frame));
    }
}

void NetArpTick(void)
{
    if (!NetpArpInitialized)
        return;
    uint64_t Now = KeQueryUptimeMicroseconds();
    KIRQL OldIrql;
    KeAcquireSpinLock(&NetpArpLock, &OldIrql);
    for (uint32_t i = 0; i < NET_ARP_CACHE_SIZE; ++i)
        if (NetpArpCache[i].Valid && NetpArpCache[i].ExpiryUs <= Now)
            NetpArpCache[i].Valid = false;
    KeReleaseSpinLock(&NetpArpLock, OldIrql);
}
