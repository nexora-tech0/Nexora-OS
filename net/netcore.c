/*
 * Nexora OS - net/netcore.c
 * Frame submission with retry, per-interface receive workers, and Ethernet dispatch.
 * DHCP owns each ring until that interface is configured; afterward its receive worker is the
 * only ring reader for that interface.
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
        NetStackTick();
        if (!Drained)
            KeDelayExecutionThread(NET_POLL_INTERVAL_US);
    }
}

void NetInitializeStack(void)
{
    NetArpInitialize();
    NetUdpInitialize();
    NetTcpInitialize();
    DbgPrint("Network: stack initialized (ARP, UDP and TCP)\n");
}

void NetStackTick(void)
{
    NetArpTick();
    NetTcpTick();
}
