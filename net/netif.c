/*
 * Nexora OS - net/netif.c
 * Registry for device-backed Ethernet interfaces. Registration happens during driver
 * initialization; the list is immutable afterwards and can be read without a lock.
 */
#include "ex.h"
#include "ke.h"
#include "netif.h"
#include "rtl.h"

static NET_INTERFACE *NetpInterfaces;
static KSPIN_LOCK NetpLock;
static bool NetpInitialized;

STATUS NetRegisterInterface(NET_INTERFACE *Interface)
{
    if (!Interface || !Interface->Name[0] || !Interface->Mtu || Interface->Mtu > NETIF_MAX_FRAME_SIZE - 14 ||
        !Interface->SendFrame || !Interface->ReceiveFrame || !Interface->QueryLink)
        return STATUS_INVALID_PARAMETER;

    if (!NetpInitialized) {
        KeInitializeSpinLock(&NetpLock);
        NetpInitialized = true;
    }

    KIRQL OldIrql;
    KeAcquireSpinLock(&NetpLock, &OldIrql);
    for (NET_INTERFACE *Current = NetpInterfaces; Current; Current = Current->Next) {
        if (_stricmp(Current->Name, Interface->Name) == 0) {
            KeReleaseSpinLock(&NetpLock, OldIrql);
            return STATUS_OBJECT_NAME_COLLISION;
        }
    }
    Interface->Next = NetpInterfaces;
    NetpInterfaces = Interface;
    KeReleaseSpinLock(&NetpLock, OldIrql);
    DbgPrint("Network: interface %s registered, MTU %u, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", Interface->Name,
             Interface->Mtu, Interface->MacAddress[0], Interface->MacAddress[1], Interface->MacAddress[2],
             Interface->MacAddress[3], Interface->MacAddress[4], Interface->MacAddress[5]);
    return STATUS_SUCCESS;
}

NET_INTERFACE *NetGetInterfaceList(void)
{
    return NetpInterfaces;
}
