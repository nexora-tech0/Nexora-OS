/*
 * Nexora OS - ex/netapi.c
 * Application-facing network services.
 */
#include "ex.h"
#include "mm.h"
#include "netdns.h"

STATUS NtResolveHostA(const uint64_t *Args)
{
    char HostName[NET_DNS_MAX_NAME_LENGTH + 1];
    uint8_t Address[4] = {0};
    STATUS Status = MmCopyStringFromUser(HostName, sizeof(HostName), (const char *)Args[0]);
    if (!HZ_SUCCESS(Status))
        return Status;

    Status = MmCopyToUser((void *)Args[1], Address, sizeof(Address));
    if (!HZ_SUCCESS(Status))
        return Status;

    Status = NetDnsResolveA(HostName, Address);
    if (!HZ_SUCCESS(Status))
        return Status;
    return MmCopyToUser((void *)Args[1], Address, sizeof(Address));
}
