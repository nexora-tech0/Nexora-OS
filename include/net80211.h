/*
 * Nexora OS - include/net80211.h
 * The shared 802.11 layer: the one contract every wireless driver plugs into, so that "support many
 * cards" is a matter of adding a driver that implements these operations, not of writing the
 * wireless logic again each time. This mirrors what Linux splits into cfg80211 (this) and the
 * per-vendor drivers (iwlwifi, ath9k, mt76, ...).
 *
 * Division of labour, deliberately chosen so the hard security code exists once:
 *   - The DRIVER owns the hardware: firmware upload if the card needs it, DMA rings, channel
 *     tuning, and the raw 802.11 management/data frame transmit and receive.
 *   - THIS LAYER owns the portable parts: scanning and BSS bookkeeping, the association state
 *     machine, and the WPA2-PSK/WPA3 supplicant (4-way handshake, PBKDF2, CCMP/AES). A card that
 *     only moves frames therefore needs no security code of its own.
 *   - The driver presents its link to the rest of Nexora as an ordinary NET_INTERFACE (netif.h), so
 *     IP, ARP, DHCP and everything above them are unchanged whether the link is wired or wireless.
 *
 * Status: this header is the contract only. net/net80211.c (the shared layer and the supplicant) and
 * the first card driver (net/80211/<card>.c) are not written yet, so no wireless driver exists.
 */
#ifndef NET80211_H
#define NET80211_H

#include "netif.h"

HZ_EXTERN_C_BEGIN

#define NET80211_SSID_MAX 32
#define NET80211_BSSID_SIZE 6
#define NET80211_MAX_SCAN_RESULTS 32

typedef enum _NET80211_SECURITY {
    Net80211SecurityOpen = 0,  /* no authentication */
    Net80211SecurityWpa2Psk,   /* WPA2-Personal: PBKDF2 + 4-way handshake + CCMP/AES */
    Net80211SecurityWpa3Sae,   /* WPA3-Personal: SAE handshake (not planned before WPA2 works) */
} NET80211_SECURITY;

typedef enum _NET80211_STATE {
    Net80211StateDown = 0,
    Net80211StateScanning,
    Net80211StateAssociating,
    Net80211StateHandshaking,
    Net80211StateConnected,
} NET80211_STATE;

/* One discovered network. SignalDbm is what the card reports (typically -30..-90). */
typedef struct _NET80211_BSS {
    uint8_t Bssid[NET80211_BSSID_SIZE];
    uint8_t Ssid[NET80211_SSID_MAX];
    uint8_t SsidLength;
    uint8_t Channel;
    int8_t SignalDbm;
    uint8_t Security; /* NET80211_SECURITY the beacon advertised */
} NET80211_BSS;

typedef struct _NET80211_DEVICE NET80211_DEVICE;

/* ---- Driver operations (the hardware half) --------------------------------------------------- */

typedef STATUS (*NET80211_START)(NET80211_DEVICE *Device);   /* power on, upload firmware if needed */
typedef void (*NET80211_STOP)(NET80211_DEVICE *Device);
typedef STATUS (*NET80211_SET_CHANNEL)(NET80211_DEVICE *Device, uint8_t Channel);
/* Active scan; the driver may block for a bounded time. Fills Results, sets *Count. */
typedef STATUS (*NET80211_SCAN)(NET80211_DEVICE *Device, NET80211_BSS *Results, uint32_t Capacity,
                                uint32_t *Count);
/* Sends one association request and reports whether the AP accepted it. */
typedef STATUS (*NET80211_JOIN)(NET80211_DEVICE *Device, const NET80211_BSS *Bss, uint64_t TimeoutUs);
typedef void (*NET80211_LEAVE)(NET80211_DEVICE *Device);
/* Sends an EAPOL frame (the supplicant owns its contents). */
typedef STATUS (*NET80211_SEND_EAPOL)(NET80211_DEVICE *Device, const void *Frame, uint32_t Length);
/* Sends one IEEE 802.3 frame (the card wraps it in an 802.11 data frame). */
typedef STATUS (*NET80211_SEND_DATA)(NET80211_DEVICE *Device, const void *Frame, uint32_t Length);
typedef bool (*NET80211_QUERY_LINK)(NET80211_DEVICE *Device);

struct _NET80211_DEVICE {
    NET80211_DEVICE *Next;
    char Name[NETIF_NAME_MAX];
    uint8_t MacAddress[6];
    uint32_t Mtu;
    void *Context;
    uint8_t SupportedSecurity; /* bitmask of NET80211_SECURITY the driver can run */
    volatile uint32_t State;   /* NET80211_STATE */
    NET80211_BSS Current;      /* the associated network, valid while Connected */

    NET80211_START Start;
    NET80211_STOP Stop;
    NET80211_SET_CHANNEL SetChannel;
    NET80211_SCAN Scan;
    NET80211_JOIN Join;
    NET80211_LEAVE Leave;
    NET80211_SEND_EAPOL SendEapol;
    NET80211_SEND_DATA SendData;
    NET80211_QUERY_LINK QueryLink;

    /* The Ethernet-like view of this card; the shared layer fills it in and registers it. */
    NET_INTERFACE Interface;
};

/* ---- Shared-layer entry points (the portable half) ------------------------------------------- */

/* Registers a wireless device and publishes its NET_INTERFACE to the IP stack. Driver init time. */
STATUS Net80211RegisterDevice(NET80211_DEVICE *Device);
/* Scans and fills Results (bounded by Capacity). */
STATUS Net80211Scan(NET80211_DEVICE *Device, NET80211_BSS *Results, uint32_t Capacity, uint32_t *Count);
/* Associates and, for WPA2/WPA3, runs the supplicant. Passphrase is empty for an open network. */
STATUS Net80211Connect(NET80211_DEVICE *Device, const NET80211_BSS *Bss, const char *Passphrase,
                       uint64_t TimeoutUs);
STATUS Net80211Disconnect(NET80211_DEVICE *Device);
/* Calls the supplicant with an inbound EAPOL frame; wired to the driver's receive path. */
void Net80211ReceiveEapol(NET80211_DEVICE *Device, const void *Frame, uint32_t Length);
/* Expires supplicant timers (rekey, handshake retries). */
void Net80211Tick(void);

HZ_EXTERN_C_END

#endif /* NET80211_H */
