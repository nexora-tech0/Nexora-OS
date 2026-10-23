/*
 * Nexora OS - include/disk.h
 * Disk class driver (drivers/disk): turns a storage port driver's sector-transfer routine into
 * \Device\HarddiskN\DR0 (the whole disk; \Device\HarddiskN\Partition0 links to it) and one
 * device per partition, \Device\HarddiskN\PartitionK, from a GPT (CRC-checked) or an MBR.
 * Port drivers (AHCI today) call DiskCreateDisk once per attached disk.
 */
#ifndef DISK_H
#define DISK_H

#include "io.h"

HZ_EXTERN_C_BEGIN

/* Transfers Count sectors (at most MaxTransferSectors) at Lba; PASSIVE_LEVEL, may block. */
typedef STATUS (*DISK_TRANSFER_ROUTINE)(void *Context, uint64_t Lba, uint32_t Count, void *Buffer, bool Write);

typedef struct _DISK_DESCRIPTION {
    char Model[41];
    uint32_t SectorSize;
    uint64_t SectorCount;
    uint32_t MaxTransferSectors;
    bool ReadOnly;
    DISK_TRANSFER_ROUTINE Transfer;
    void *Context;
} DISK_DESCRIPTION;

STATUS DiskCreateDisk(const DISK_DESCRIPTION *Description, uint32_t *DiskNumber);

/* GPT type of Nexora OS test scratch partitions: the only place the disk self-test writes raw
   sectors. 7A1F5C3E-2B4D-4E8A-9F60-485A54455354 in on-disk byte order. */
extern const uint8_t DiskTestScratchType[16];
/* Finds the first partition of that type; returns a referenced device or NULL. */
DEVICE_OBJECT *DiskFindPartitionByType(const uint8_t TypeGuid[16]);
/* Parses "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX" into GPT on-disk byte order. */
bool DiskParseGuid(const char *Text, uint8_t Guid[16]);
/* The hz.system GUID as text, "" if none was given. */
void DiskDescribeSystemGuid(char *Out, size_t Size);

HZ_EXTERN_C_END

#endif /* DISK_H */
