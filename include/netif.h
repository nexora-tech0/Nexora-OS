/*
 * Nexora OS - include/netif.h
 * Kernel network-interface boundary. Protocol stacks poll received Ethernet frames and submit
 * complete frames through this interface; device drivers own their DMA rings.
 */
#ifndef NETIF_H
#define NETIF_H

#include "hzbase.h"

HZ_EXTERN_C_BEGIN

#define NETIF_NAME_MAX 16
#define NETIF_MAX_FRAME_SIZE 1514

typedef struct _NET_INTERFACE NET_INTERFACE;

typedef STATUS (*NETIF_SEND_FRAME)(NET_INTERFACE *Interface, const void *Frame, uint32_t Length);
typedef STATUS (*NETIF_RECEIVE_FRAME)(NET_INTERFACE *Interface, void *Frame, uint32_t Capacity, uint32_t *Length);
typedef bool (*NETIF_QUERY_LINK)(NET_INTERFACE *Interface);

struct _NET_INTERFACE {
    NET_INTERFACE *Next;
    char Name[NETIF_NAME_MAX];
    uint8_t MacAddress[6];
    uint32_t Mtu;
    void *Context;
    NETIF_SEND_FRAME SendFrame;
    NETIF_RECEIVE_FRAME ReceiveFrame;
    NETIF_QUERY_LINK QueryLink;
};

/* Called by a bus/device driver during I/O initialization. Interfaces remain registered thereafter. */
STATUS NetRegisterInterface(NET_INTERFACE *Interface);
NET_INTERFACE *NetGetInterfaceList(void);

HZ_EXTERN_C_END

#endif /* NETIF_H */
