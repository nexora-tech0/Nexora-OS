/*
 * Nexora OS - fs/fat32/fattable.c
 * Sector caches, FAT entries, cluster chains and the FSInfo sector.
 *
 * Two one-sector caches avoid most repeated reads: one for the FAT (sequential chain walks and
 * allocation scans stay in one sector for 128 clusters) and one for directory sectors. The
 * directory cache is write-through. The FAT cache is write-back since M11: changes collect in the
 * cached sector and go to every FAT copy (or only the active one when the volume disabled
 * mirroring) when another FAT sector is needed or when the operation ends (FatFlushFsInfo), so
 * allocating a run of clusters writes each FAT sector once instead of once per cluster.
 */
#include "fatp.h"

STATUS FatReadMeta(FAT_VOLUME *Vol, uint64_t Lba, uint8_t **Sector)
{
    if (Vol->MetaCacheLba != Lba) {
        Vol->MetaCacheLba = UINT64_MAX;
        STATUS Status = IoReadSectors(Vol->Storage, Lba, 1, Vol->MetaCache);
        if (!HZ_SUCCESS(Status))
            return Status;
        Vol->MetaCacheLba = Lba;
    }
    *Sector = Vol->MetaCache;
    return STATUS_SUCCESS;
}

STATUS FatWriteMeta(FAT_VOLUME *Vol, uint64_t Lba)
{
    if (Vol->MetaCacheLba != Lba)
        return STATUS_INVALID_PARAMETER;
    STATUS Status = IoWriteSectors(Vol->Storage, Lba, 1, Vol->MetaCache);
    if (!HZ_SUCCESS(Status))
        Vol->MetaCacheLba = UINT64_MAX;
    return Status;
}

static uint64_t FatFatSector(FAT_VOLUME *Vol, uint32_t Copy, uint32_t Index)
{
    return Vol->ReservedSectors + (uint64_t)Copy * Vol->SectorsPerFat + Index;
}

static STATUS FatWriteFatCache(FAT_VOLUME *Vol)
{
    if (!Vol->FatCacheDirty)
        return STATUS_SUCCESS;
    for (uint32_t Copy = 0; Copy < Vol->FatCount; Copy++) {
        if (Vol->ActiveFat != UINT32_MAX && Copy != Vol->ActiveFat)
            continue;
        STATUS Status = IoWriteSectors(Vol->Storage, FatFatSector(Vol, Copy, Vol->FatCacheIndex), 1, Vol->FatCache);
        if (!HZ_SUCCESS(Status)) {
            Vol->FatCacheLba = UINT64_MAX; /* re-read: the disk may hold either version */
            Vol->FatCacheDirty = false;
            return Status;
        }
    }
    Vol->FatCacheDirty = false;
    return STATUS_SUCCESS;
}

static STATUS FatLoadFatSector(FAT_VOLUME *Vol, uint32_t Index)
{
    uint64_t Lba = FatFatSector(Vol, Vol->ActiveFat == UINT32_MAX ? 0 : Vol->ActiveFat, Index);
    if (Vol->FatCacheLba == Lba)
        return STATUS_SUCCESS;
    STATUS Status = FatWriteFatCache(Vol);
    if (!HZ_SUCCESS(Status))
        return Status;
    Vol->FatCacheLba = UINT64_MAX;
    Status = IoReadSectors(Vol->Storage, Lba, 1, Vol->FatCache);
    if (HZ_SUCCESS(Status)) {
        Vol->FatCacheLba = Lba;
        Vol->FatCacheIndex = Index;
    }
    return Status;
}

STATUS FatGetEntry(FAT_VOLUME *Vol, uint32_t Cluster, uint32_t *Value)
{
    if (!FatIsValidCluster(Vol, Cluster))
        return STATUS_FILE_CORRUPT_ERROR;
    uint64_t Byte = (uint64_t)Cluster * 4;
    STATUS Status = FatLoadFatSector(Vol, (uint32_t)(Byte / Vol->BytesPerSector));
    if (!HZ_SUCCESS(Status))
        return Status;
    uint32_t Raw;
    memcpy(&Raw, Vol->FatCache + Byte % Vol->BytesPerSector, 4);
    *Value = Raw & FAT_ENTRY_MASK;
    return STATUS_SUCCESS;
}

STATUS FatSetEntry(FAT_VOLUME *Vol, uint32_t Cluster, uint32_t Value)
{
    if (!FatIsValidCluster(Vol, Cluster))
        return STATUS_FILE_CORRUPT_ERROR;
    uint64_t Byte = (uint64_t)Cluster * 4;
    uint32_t Index = (uint32_t)(Byte / Vol->BytesPerSector);
    STATUS Status = FatLoadFatSector(Vol, Index);
    if (!HZ_SUCCESS(Status))
        return Status;
    uint32_t Raw;
    uint8_t *Slot = Vol->FatCache + Byte % Vol->BytesPerSector;
    memcpy(&Raw, Slot, 4);
    Raw = (Raw & ~FAT_ENTRY_MASK) | (Value & FAT_ENTRY_MASK); /* the top 4 bits are reserved */
    memcpy(Slot, &Raw, 4);
    Vol->FatCacheDirty = true;
    return STATUS_SUCCESS;
}

STATUS FatSeek(FAT_VOLUME *Vol, FAT_CURSOR *Cursor, uint32_t Index, uint32_t *Cluster)
{
    if (!FatIsValidCluster(Vol, Cursor->First))
        return STATUS_END_OF_FILE;
    if (Cursor->Cluster == 0 || Index < Cursor->Index) {
        Cursor->Index = 0;
        Cursor->Cluster = Cursor->First;
    }
    while (Cursor->Index < Index) {
        if (Cursor->Index > Vol->ClusterCount)
            return STATUS_FILE_CORRUPT_ERROR; /* a cycle */
        uint32_t Next;
        STATUS Status = FatGetEntry(Vol, Cursor->Cluster, &Next);
        if (!HZ_SUCCESS(Status))
            return Status;
        if (Next >= FAT_EOC_MIN)
            return STATUS_END_OF_FILE;
        if (!FatIsValidCluster(Vol, Next))
            return STATUS_FILE_CORRUPT_ERROR;
        Cursor->Cluster = Next;
        Cursor->Index++;
    }
    *Cluster = Cursor->Cluster;
    return STATUS_SUCCESS;
}

STATUS FatAllocateCluster(FAT_VOLUME *Vol, uint32_t Previous, bool Zero, uint32_t *Out)
{
    if (Vol->FreeClusters == 0)
        return STATUS_DISK_FULL;
    uint32_t Cluster = FatIsValidCluster(Vol, Vol->NextFree) ? Vol->NextFree : 2;
    for (uint32_t Scanned = 0; Scanned < Vol->ClusterCount; Scanned++) {
        uint32_t Value;
        STATUS Status = FatGetEntry(Vol, Cluster, &Value);
        if (!HZ_SUCCESS(Status))
            return Status;
        if (Value == 0) {
            if (Zero) {
                Status = IoWriteSectors(Vol->Storage, FatClusterToSector(Vol, Cluster), Vol->SectorsPerCluster,
                                        Vol->ClusterBuffer);
                if (!HZ_SUCCESS(Status))
                    return Status;
            }
            Status = FatSetEntry(Vol, Cluster, FAT_EOC);
            if (HZ_SUCCESS(Status) && Previous)
                Status = FatSetEntry(Vol, Previous, Cluster);
            if (!HZ_SUCCESS(Status))
                return Status;
            Vol->FreeClusters--;
            Vol->NextFree = Cluster + 1;
            Vol->FsInfoDirty = true;
            *Out = Cluster;
            return STATUS_SUCCESS;
        }
        Cluster = Cluster >= Vol->ClusterCount + 1 ? 2 : Cluster + 1;
    }
    Vol->FreeClusters = 0;
    return STATUS_DISK_FULL;
}

STATUS FatFreeChain(FAT_VOLUME *Vol, uint32_t First)
{
    uint32_t Cluster = First;
    for (uint32_t Guard = 0; FatIsValidCluster(Vol, Cluster) && Guard <= Vol->ClusterCount; Guard++) {
        uint32_t Next;
        STATUS Status = FatGetEntry(Vol, Cluster, &Next);
        if (HZ_SUCCESS(Status))
            Status = FatSetEntry(Vol, Cluster, 0);
        if (!HZ_SUCCESS(Status))
            return Status;
        Vol->FreeClusters++;
        if (Cluster < Vol->NextFree)
            Vol->NextFree = Cluster;
        Vol->FsInfoDirty = true;
        if (Next >= FAT_EOC_MIN)
            break;
        Cluster = Next;
    }
    return STATUS_SUCCESS;
}

STATUS FatCountChain(FAT_VOLUME *Vol, uint32_t First, uint32_t *Count)
{
    uint32_t n = 0;
    uint32_t Cluster = First;
    while (FatIsValidCluster(Vol, Cluster)) {
        if (++n > Vol->ClusterCount)
            return STATUS_FILE_CORRUPT_ERROR;
        uint32_t Next;
        STATUS Status = FatGetEntry(Vol, Cluster, &Next);
        if (!HZ_SUCCESS(Status))
            return Status;
        if (Next >= FAT_EOC_MIN)
            break;
        Cluster = Next;
    }
    *Count = n;
    return STATUS_SUCCESS;
}

STATUS FatCountFreeClusters(FAT_VOLUME *Vol, uint32_t *Free)
{
    uint32_t Count = 0;
    uint32_t PerSector = Vol->BytesPerSector / 4;
    uint32_t Last = Vol->ClusterCount + 1;
    for (uint32_t Index = 0; Index * PerSector <= Last; Index++) {
        STATUS Status = FatLoadFatSector(Vol, Index);
        if (!HZ_SUCCESS(Status))
            return Status;
        for (uint32_t i = 0; i < PerSector; i++) {
            uint32_t Cluster = Index * PerSector + i;
            if (Cluster < 2 || Cluster > Last)
                continue;
            uint32_t Raw;
            memcpy(&Raw, Vol->FatCache + i * 4, 4);
            if ((Raw & FAT_ENTRY_MASK) == 0)
                Count++;
        }
    }
    *Free = Count;
    return STATUS_SUCCESS;
}

STATUS FatFlushFsInfo(FAT_VOLUME *Vol)
{
    STATUS FatStatus = FatWriteFatCache(Vol);
    if (!HZ_SUCCESS(FatStatus))
        return FatStatus;
    if (!Vol->FsInfoDirty || Vol->ReadOnly || Vol->FsInfoSector == 0 || Vol->FsInfoSector >= Vol->ReservedSectors)
        return STATUS_SUCCESS;
    uint8_t *Sector;
    STATUS Status = FatReadMeta(Vol, Vol->FsInfoSector, &Sector);
    if (!HZ_SUCCESS(Status))
        return Status;
    uint32_t Lead, Struct, Trail;
    memcpy(&Lead, Sector + 0, 4);
    memcpy(&Struct, Sector + 484, 4);
    memcpy(&Trail, Sector + 508, 4);
    if (Lead != 0x41615252 || Struct != 0x61417272 || Trail != 0xAA550000)
        return STATUS_SUCCESS; /* no valid FSInfo: nothing to maintain */
    memcpy(Sector + 488, &Vol->FreeClusters, 4);
    memcpy(Sector + 492, &Vol->NextFree, 4);
    Status = FatWriteMeta(Vol, Vol->FsInfoSector);
    if (HZ_SUCCESS(Status))
        Vol->FsInfoDirty = false;
    return Status;
}
