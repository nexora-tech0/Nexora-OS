/*
 * Nexora OS - fs/ntfs/stream.c
 * Bounded byte reads from decoded NTFS data-stream runs.
 */
#include "disk.h"
#include "ex.h"
#include "ntfs.h"
#include "rtl.h"

#define NTFS_STREAM_TAG HZ_POOL_TAG('N', 't', 'S', 'r')

STATUS NtfsReadDataStream(DEVICE_OBJECT *Storage, const NTFS_BOOT_INFORMATION *Geometry,
                          const NTFS_RUN *Runs, size_t RunCount, uint64_t DataBytes,
                          uint64_t InitializedBytes, uint64_t Offset, uint32_t Length, void *Buffer)
{
    if (!Storage || !Geometry || (!Runs && RunCount) || (Length && !Buffer) ||
        !Geometry->BytesPerSector || !Geometry->SectorsPerCluster ||
        Geometry->BytesPerCluster != (uint64_t)Geometry->BytesPerSector * Geometry->SectorsPerCluster ||
        Geometry->BytesPerCluster > 2u * 1024u * 1024u ||
        Storage->SectorSize != Geometry->BytesPerSector || Geometry->TotalSectors > Storage->SectorCount ||
        Geometry->TotalSectors > UINT64_MAX / Geometry->BytesPerSector || InitializedBytes > DataBytes ||
        Offset > DataBytes || Length > DataBytes - Offset)
        return STATUS_INVALID_PARAMETER;
    if (!Length)
        return STATUS_SUCCESS;

    uint8_t *SectorBuffer = ExAllocatePoolWithTag(NonPagedPool, Geometry->BytesPerSector, NTFS_STREAM_TAG);
    if (!SectorBuffer)
        return STATUS_NO_MEMORY;

    uint8_t *Output = (uint8_t *)Buffer;
    uint64_t Position = Offset;
    uint32_t Remaining = Length;
    STATUS Status = STATUS_SUCCESS;
    uint64_t ClusterCount = Geometry->TotalSectors / Geometry->SectorsPerCluster;
    while (Remaining) {
        uint64_t Vcn = Position / Geometry->BytesPerCluster;
        uint32_t ClusterOffset = (uint32_t)(Position % Geometry->BytesPerCluster);
        const NTFS_RUN *Run = NULL;
        for (size_t Index = 0; Index < RunCount; ++Index) {
            if (Vcn >= Runs[Index].Vcn && Vcn - Runs[Index].Vcn < Runs[Index].Length) {
                Run = &Runs[Index];
                break;
            }
        }
        if (!Run || !Run->Length || Run->Vcn > UINT64_MAX - Run->Length) {
            Status = STATUS_DISK_CORRUPT_ERROR;
            break;
        }
        uint64_t RunEndVcn = Run->Vcn + Run->Length;
        if (RunEndVcn > UINT64_MAX / Geometry->BytesPerCluster) {
            Status = STATUS_DISK_CORRUPT_ERROR;
            break;
        }
        uint64_t RunEndByte = RunEndVcn * (uint64_t)Geometry->BytesPerCluster;
        uint64_t Available = RunEndByte - Position;
        uint32_t Chunk = Available < Remaining ? (uint32_t)Available : Remaining;
        if (!Chunk) {
            Status = STATUS_DISK_CORRUPT_ERROR;
            break;
        }

        uint32_t InitializedChunk = 0;
        if (Position < InitializedBytes) {
            uint64_t InitializedAvailable = InitializedBytes - Position;
            InitializedChunk = InitializedAvailable < Chunk ? (uint32_t)InitializedAvailable : Chunk;
        }
        if (Run->Sparse)
            memset(Output, 0, Chunk);
        else if (InitializedChunk < Chunk)
            memset(Output + InitializedChunk, 0, Chunk - InitializedChunk);
        if (InitializedChunk && !Run->Sparse) {
            uint64_t ClusterDelta = Vcn - Run->Vcn;
            if (Run->Lcn < 0 || (uint64_t)Run->Lcn >= ClusterCount ||
                ClusterDelta > ClusterCount - (uint64_t)Run->Lcn ||
                (uint64_t)Run->Lcn + ClusterDelta >= ClusterCount) {
                Status = STATUS_DISK_CORRUPT_ERROR;
                break;
            }
            uint64_t Physical = ((uint64_t)Run->Lcn + ClusterDelta) * Geometry->BytesPerCluster + ClusterOffset;
            uint32_t ToRead = InitializedChunk;
            uint32_t Done = 0;
            while (ToRead) {
                uint64_t Lba = Physical / Geometry->BytesPerSector;
                uint32_t InSector = (uint32_t)(Physical % Geometry->BytesPerSector);
                uint32_t CopyBytes = Geometry->BytesPerSector - InSector;
                if (CopyBytes > ToRead)
                    CopyBytes = ToRead;
                if (Lba >= Storage->SectorCount) {
                    Status = STATUS_DISK_CORRUPT_ERROR;
                    break;
                }
                Status = IoReadSectors(Storage, Lba, 1, SectorBuffer);
                if (!HZ_SUCCESS(Status))
                    break;
                memcpy(Output + Done, SectorBuffer + InSector, CopyBytes);
                Physical += CopyBytes;
                Done += CopyBytes;
                ToRead -= CopyBytes;
            }
            if (!HZ_SUCCESS(Status))
                break;
        }
        Position += Chunk;
        Output += Chunk;
        Remaining -= Chunk;
    }

    ExFreePoolWithTag(SectorBuffer, NTFS_STREAM_TAG);
    return Status;
}
