/*
 * Nexora OS - net/netcore.c
 * The glue include/net.h declares but nothing implemented: frame submission with retry, the
 * receive worker that drains each interface's ring, and the dispatch that routes a received frame
 * to ARP or IPv4. arp.c already calls NetTransmitFrame, so without this file the kernel net layer
 * cannot link.
 *
 * What this file deliberately does NOT do (kept honest):
 *   - It does not start the worker for the user yet. NetStart() (net/dhcp.c) already polls the same
 *     ring while it configures the interface, and two readers of one ring would steal each other's
 *     frames. NetInitializeStack()/NetReceiveWorker() are the contract the transports will use; a
 *     later step starts the worker once UDP/TCP exist and DHCP is done with the ring.
 *   - NetInitializeStack resets the ARP and UDP tables. The TCP and socket tables it is documented
 *     to reset do not exist yet (net/tcp.c and net/socket.c are not written), so calling their
 *     initializers would not link.
 */
#include "ex.h"
#include "ke.h"
#include "net.h"
#include "rtl.h"

#define NETP_TRANSMIT_RETRIES 20u
#define NETP_TRANSMIT_BACKOFF_US 50u

STATUS NetTransmitFrame(NET_INTERFACE *Interface, const void *Frame, uint32_t Length)
{
    if (!Interface || !Interface->SendFrame || !Frame || !Length || Length > NETIF_MAX_FRAME_SIZE)
        return STATUS_INVALID_PARAMETER;

    STATUS Status = STATUS_DEVICE_BUSY;
    for (uint32_t Attempt = 0; Attempt < NETP_TRANSMIT_RETRIES; ++Attempt) {
        Status = Interface->SendFrame(Interface, Frame, Length);
        if (Status != STATUS_DEVICE_BUSY)
            return Status;
        KeDelayExecutionThread(NETP_TRANSMIT_BACKOFF_US);
    }
    return Status;
}

void NetProcessFrame(NET_INTERFACE *Interface, const void *Frame, uint32_t Length)
{
    if (!Interface || !Frame)
        return;
    NET_ETHERNET_VIEW Ethernet;
    if (!HZ_SUCCESS(NetParseEthernet(Frame, Length, &Ethernet)))
        return;

    switch (Ethernet.EtherType) {
    case NET_ETHERTYPE_ARP: {
        NET_ARP_VIEW Arp;
        if (HZ_SUCCESS(NetParseArp(&Ethernet, &Arp)))
            NetArpReceive(Interface, &Ethernet, &Arp);
        break;
    }
    case NET_ETHERTYPE_IPV4: {
        NET_IPV4_VIEW Ipv4;
        if (HZ_SUCCESS(NetParseIpv4(&Ethernet, &Ipv4)))
            NetIpReceive(Interface, &Ethernet, &Ipv4);
        break;
    }
    default:
        break; /* not a protocol this build carries */
    }
}

void NetReceiveWorker(void *Context)
{
    NET_INTERFACE *Single = Context;
    uint8_t Frame[NETIF_MAX_FRAME_SIZE];
    bool Reported = false;

    for (;;) {
        bool Drained = false;
        for (NET_INTERFACE *Current = Single ? Single : NetGetInterfaceList(); Current;
             Current = Single ? NULL : Current->Next) {
            uint32_t Length = 0;
            STATUS Status = Current->ReceiveFrame(Current, Frame, sizeof(Frame), &Length);
            if (HZ_SUCCESS(Status) && Length) {
                NetProcessFrame(Current, Frame, Length);
                Drained = true;
                continue;
            }
            if (Status != STATUS_NO_MORE_ENTRIES && !Reported) {
                DbgPrintEx(DPFLTR_WARNING_LEVEL, "Network: receive error on %s (0x%08X)\n", Current->Name,
                           (uint32_t)Status);
                Reported = true;
            }
        }
        if (!Drained)
            KeDelayExecutionThread(NET_POLL_INTERVAL_US);
    }
}

void NetInitializeStack(void)
{
    NetArpInitialize();
    NetUdpInitialize();
    DbgPrint("Network: stack initialized (ARP and UDP; TCP is not implemented yet)\n");
}

void NetStackTick(void)
{
    NetArpTick();
}
