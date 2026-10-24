/*
 * Nexora OS - include/netdns.h
 * Bounded DNS A-query encoding and response parsing.
 */
#ifndef NETDNS_H
#define NETDNS_H

#include "hzbase.h"

HZ_EXTERN_C_BEGIN

#define NET_DNS_MAX_MESSAGE_SIZE 512
#define NET_DNS_MAX_NAME_LENGTH 253

STATUS NetDnsBuildAQuery(uint16_t Identifier, const char *Name, void *Buffer, uint32_t Capacity, uint32_t *Length);
STATUS NetDnsParseAResponse(uint16_t Identifier, const char *Name, const void *Message, uint32_t Length,
                            uint8_t Address[4]);
void NetDnsInitialize(void);
STATUS NetDnsResolveA(const char *Name, uint8_t Address[4]);
bool NetDnsRunSelfTests(void);

HZ_EXTERN_C_END

#endif /* NETDNS_H */
