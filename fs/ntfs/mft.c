/*
 * Nexora OS - fs/ntfs/mft.c
 * Reads fixed-size MFT records through the decoded $MFT data runlist and repairs USA fixups.
 */
#include "ntfs.h"
#include "disk.h"
#include "ex.h"
#include "rtl.h"

#define NTFS_MFT_TAG HZ_POOL_TAG('N', 't', 'M', 'f')

static uint16_t NtfsMftRead16(const uint8_t *Bytes)
{
    return (uint16_t)Bytes[0] | ((uint16_t)Bytes[1] << 8);
}

static uint32_t NtfsMftRead32(const uint8_t *Bytes)
{
    return (uint32_t)Bytes[0] | ((uint32_t)Bytes[1] << 8) |
           ((uint32_t)Bytes[2] << 16) | ((uint32_t)Bytes[3] << 24);
}

static uint64_t NtfsMftRead64(const uint8_t *Bytes)
{
    return (uint64_t)NtfsMftRead32(Bytes) | ((uint64_t)NtfsMftRead32(Bytes + 4) << 32);
}

static STATUS NtfsIndexNodeNext(const uint8_t *Value, size_t ValueBytes, uint32_t HeaderOffset,
                                uint32_t *Cursor, NTFS_INDEX_ENTRY_VIEW *Entry)
{
    if (!Value || !Cursor || !Entry || ValueBytes < HeaderOffset + 16)
        return STATUS_INVALID_PARAMETER;
    uint32_t FirstEntry = NtfsMftRead32(Value + HeaderOffset);
    uint32_t TotalEntries = NtfsMftRead32(Value + HeaderOffset + 4);
    uint32_t AllocatedEntries = NtfsMftRead32(Value + HeaderOffset + 8);
    if (FirstEntry < 16 || TotalEntries < FirstEntry || AllocatedEntries < TotalEntries ||
        AllocatedEntries > ValueBytes - HeaderOffset || TotalEntries > ValueBytes - HeaderOffset)
        return STATUS_DISK_CORRUPT_ERROR;
    uint32_t End = HeaderOffset + TotalEntries;
    uint32_t Offset = *Cursor ? *Cursor : HeaderOffset + FirstEntry;
    if (Offset > End || End - Offset < 16)
        return STATUS_DISK_CORRUPT_ERROR;
    const uint8_t *Record = Value + Offset;
    uint16_t RecordBytes = NtfsMftRead16(Record + 8);
    uint16_t KeyBytes = NtfsMftRead16(Record + 10);
    uint16_t Flags = NtfsMftRead16(Record + 12);
    if (RecordBytes < 16 || RecordBytes > End - Offset)
        return STATUS_DISK_CORRUPT_ERROR;
    if (Flags & 0x0002) {
        NTFS_INDEX_ENTRY_VIEW Candidate = {0};
        Candidate.HasChildNode = (Flags & 0x0001) != 0;
        if (Candidate.HasChildNode) {
            if (RecordBytes < 24)
                return STATUS_DISK_CORRUPT_ERROR;
            Candidate.ChildVcn = NtfsMftRead64(Record + RecordBytes - sizeof(uint64_t));
        }
        *Entry = Candidate;
        *Cursor = 0;
        return STATUS_NO_MORE_FILES;
    }

    bool HasChildNode = (Flags & 0x0001) != 0;
    if (RecordBytes < 16 + (HasChildNode ? sizeof(uint64_t) : 0))
        return STATUS_DISK_CORRUPT_ERROR;
    size_t KeyLimit = RecordBytes - 16 - (HasChildNode ? sizeof(uint64_t) : 0);
    if (KeyBytes < 66 || KeyBytes > KeyLimit)
        return STATUS_DISK_CORRUPT_ERROR;
    const uint8_t *Key = Record + 16;
    uint8_t NameCharacters = Key[64];
    uint64_t NameBytes = (uint64_t)NameCharacters * sizeof(uint16_t);
    if (NameBytes > KeyBytes - 66)
        return STATUS_DISK_CORRUPT_ERROR;

    NTFS_INDEX_ENTRY_VIEW Candidate = {0};
    Candidate.FileReference = NtfsMftRead64(Record);
    Candidate.NameUtf16Le = Key + 66;
    Candidate.NameCharacters = NameCharacters;
    Candidate.NameNamespace = Key[65];
    Candidate.DataBytes = NtfsMftRead64(Key + 48);
    Candidate.FileAttributes = NtfsMftRead32(Key + 56);
    Candidate.HasChildNode = HasChildNode;
    if (HasChildNode)
        Candidate.ChildVcn = NtfsMftRead64(Record + RecordBytes - sizeof(uint64_t));
    *Entry = Candidate;
    *Cursor = Offset + RecordBytes;
    return STATUS_SUCCESS;
}

STATUS NtfsIndexRootNext(const NTFS_ATTRIBUTE_VIEW *IndexRoot, uint32_t *Cursor,
                         NTFS_INDEX_ENTRY_VIEW *Entry)
{
    if (!IndexRoot || IndexRoot->NonResident || IndexRoot->Type != 0x90)
        return STATUS_INVALID_PARAMETER;
    return NtfsIndexNodeNext(IndexRoot->ResidentValue, IndexRoot->ResidentValueBytes, 16,
                             Cursor, Entry);
}

static STATUS NtfsValidateIndexRoot(const NTFS_ATTRIBUTE_VIEW *IndexRoot)
{
    uint32_t Cursor = 0;
    uint32_t Entries = 0;
    for (;;) {
        NTFS_INDEX_ENTRY_VIEW Entry;
        STATUS Status = NtfsIndexRootNext(IndexRoot, &Cursor, &Entry);
        if (Status == STATUS_NO_MORE_FILES)
            return STATUS_SUCCESS;
        if (!HZ_SUCCESS(Status))
            return Status;
        if (++Entries > 65536)
            return STATUS_DISK_CORRUPT_ERROR;
    }
}

STATUS NtfsInitializeVolume(DEVICE_OBJECT *Storage, NTFS_VOLUME_CONTEXT *Volume)
{
    if (!Storage || !Volume || Storage->SectorSize < 512 || Storage->SectorSize > 4096 ||
        Storage->SectorCount < 16)
        return STATUS_INVALID_PARAMETER;
    memset(Volume, 0, sizeof(*Volume));
    NTFS_BOOT_INFORMATION *Geometry = &Volume->Geometry;
    uint8_t *Boot = ExAllocatePoolWithTag(NonPagedPool, Storage->SectorSize, NTFS_MFT_TAG);
    if (!Boot)
        return STATUS_NO_MEMORY;
    STATUS Status = IoReadSectors(Storage, 0, 1, Boot);
    if (HZ_SUCCESS(Status))
        Status = NtfsDecodeBootRecord(Boot, Storage->SectorSize, Storage->SectorCount, Geometry);
    ExFreePoolWithTag(Boot, NTFS_MFT_TAG);
    if (!HZ_SUCCESS(Status))
        return Status;
    if (Geometry->BytesPerSector != Storage->SectorSize ||
        Geometry->FileRecordBytes % Geometry->BytesPerSector ||
        Geometry->FileRecordBytes > UINT32_MAX)
        return STATUS_UNRECOGNIZED_VOLUME;

    uint64_t MftLba = Geometry->MftCluster * Geometry->SectorsPerCluster;
    uint32_t RecordSectors = Geometry->FileRecordBytes / Geometry->BytesPerSector;
    if (!RecordSectors || MftLba >= Storage->SectorCount ||
        RecordSectors > Storage->SectorCount - MftLba)
        return STATUS_DISK_CORRUPT_ERROR;
    uint8_t *MftRecord = ExAllocatePoolWithTag(NonPagedPool, Geometry->FileRecordBytes, NTFS_MFT_TAG);
    if (!MftRecord)
        return STATUS_NO_MEMORY;
    Status = IoReadSectors(Storage, MftLba, RecordSectors, MftRecord);
    if (HZ_SUCCESS(Status))
        Status = NtfsApplyUpdateSequenceArray(MftRecord, Geometry->FileRecordBytes,
                                               Geometry->BytesPerSector);
    NTFS_ATTRIBUTE_VIEW DataAttribute = {0};
    if (HZ_SUCCESS(Status))
        Status = NtfsFindAttribute(MftRecord, Geometry->FileRecordBytes, 0x80, NULL, 0, &DataAttribute);
    if (HZ_SUCCESS(Status) && (!DataAttribute.NonResident || DataAttribute.LowestVcn != 0 ||
                               DataAttribute.Flags & 0xC001 ||
                               DataAttribute.DataBytes < (uint64_t)Geometry->FileRecordBytes * 6 ||
                               DataAttribute.InitializedBytes < (uint64_t)Geometry->FileRecordBytes * 6))
        Status = STATUS_NOT_SUPPORTED;

    NTFS_RUN *Runs = NULL;
    size_t RunCount = 0;
    size_t RunCapacity = HZ_SUCCESS(Status) ? (size_t)DataAttribute.MappingPairsBytes / 2 + 1 : 0;
    if (HZ_SUCCESS(Status) && (!RunCapacity || RunCapacity > SIZE_MAX / sizeof(*Runs)))
        Status = STATUS_DISK_CORRUPT_ERROR;
    if (HZ_SUCCESS(Status)) {
        Runs = ExAllocatePoolZero(NonPagedPool, RunCapacity * sizeof(*Runs), NTFS_MFT_TAG);
        if (!Runs)
            Status = STATUS_NO_MEMORY;
    }
    if (HZ_SUCCESS(Status))
        Status = NtfsDecodeRunlist(DataAttribute.MappingPairs, DataAttribute.MappingPairsBytes,
                                   DataAttribute.LowestVcn, Runs, RunCapacity, &RunCount);
    ExFreePoolWithTag(MftRecord, NTFS_MFT_TAG);

    if (HZ_SUCCESS(Status)) {
        uint8_t *RootRecord = ExAllocatePoolWithTag(NonPagedPool, Geometry->FileRecordBytes, NTFS_MFT_TAG);
        if (!RootRecord) {
            Status = STATUS_NO_MEMORY;
        } else {
            Status = NtfsReadMftRecord(Storage, Geometry, Runs, RunCount, DataAttribute.DataBytes,
                                       DataAttribute.InitializedBytes, 5, RootRecord,
                                       Geometry->FileRecordBytes);
            if (HZ_SUCCESS(Status) && !(NtfsMftRead16(RootRecord + 22) & 0x0002))
                Status = STATUS_DISK_CORRUPT_ERROR;
            static const uint16_t I30Name[] = {'$', 'I', '3', '0'};
            NTFS_ATTRIBUTE_VIEW IndexRoot = {0};
            if (HZ_SUCCESS(Status))
                Status = NtfsFindAttribute(RootRecord, Geometry->FileRecordBytes, 0x90,
                                           I30Name, HZ_ARRAYSIZE(I30Name), &IndexRoot);
            if (HZ_SUCCESS(Status))
                Status = NtfsValidateIndexRoot(&IndexRoot);
            ExFreePoolWithTag(RootRecord, NTFS_MFT_TAG);
        }
    }
    if (HZ_SUCCESS(Status)) {
        Volume->Storage = Storage;
        Volume->MftRuns = Runs;
        Volume->MftRunCount = RunCount;
        Volume->MftDataBytes = DataAttribute.DataBytes;
        Volume->MftInitializedBytes = DataAttribute.InitializedBytes;
        DbgPrint("NTFS: boot, $MFT data, and root-directory metadata validated (serial %08X-%08X)\n",
                 (uint32_t)(Geometry->VolumeSerial >> 32), (uint32_t)Geometry->VolumeSerial);
    } else if (Runs) {
        ExFreePoolWithTag(Runs, NTFS_MFT_TAG);
    }
    return Status;
}

void NtfsReleaseVolume(NTFS_VOLUME_CONTEXT *Volume)
{
    if (!Volume)
        return;
    if (Volume->MftRuns)
        ExFreePoolWithTag(Volume->MftRuns, NTFS_MFT_TAG);
    memset(Volume, 0, sizeof(*Volume));
}

STATUS NtfsReadVolumeMftRecord(const NTFS_VOLUME_CONTEXT *Volume, uint64_t RecordNumber,
                               void *Record, size_t RecordBytes)
{
    if (!Volume || !Volume->Storage || !Volume->MftRuns || !Volume->MftRunCount)
        return STATUS_INVALID_PARAMETER;
    return NtfsReadMftRecord(Volume->Storage, &Volume->Geometry, Volume->MftRuns,
                             Volume->MftRunCount, Volume->MftDataBytes,
                             Volume->MftInitializedBytes, RecordNumber, Record, RecordBytes);
}

static uint16_t NtfsFoldAscii(uint16_t Character)
{
    return Character >= 'a' && Character <= 'z' ? Character - ('a' - 'A') : Character;
}

static int NtfsCompareIndexName(const NTFS_INDEX_ENTRY_VIEW *Entry, const uint16_t *Name,
                                size_t NameCharacters)
{
    size_t SharedCharacters = Entry->NameCharacters < NameCharacters ? Entry->NameCharacters : NameCharacters;
    for (size_t Index = 0; Index < SharedCharacters; ++Index) {
        uint16_t EntryCharacter = (uint16_t)Entry->NameUtf16Le[Index * 2] |
                                  ((uint16_t)Entry->NameUtf16Le[Index * 2 + 1] << 8);
        EntryCharacter = NtfsFoldAscii(EntryCharacter);
        uint16_t NameCharacter = NtfsFoldAscii(Name[Index]);
        if (EntryCharacter != NameCharacter)
            return EntryCharacter < NameCharacter ? -1 : 1;
    }
    if (Entry->NameCharacters == NameCharacters)
        return 0;
    return Entry->NameCharacters < NameCharacters ? -1 : 1;
}

static STATUS NtfsDecodeMappingPairs(const NTFS_ATTRIBUTE_VIEW *Attribute,
                                     NTFS_RUN **Runs, size_t *RunCount)
{
    *Runs = NULL;
    *RunCount = 0;
    if (!Attribute->NonResident || (Attribute->Flags & 0x4001))
        return STATUS_NOT_SUPPORTED;
    size_t Capacity = (size_t)Attribute->MappingPairsBytes / 2 + 1;
    if (!Capacity || Capacity > SIZE_MAX / sizeof(NTFS_RUN))
        return STATUS_DISK_CORRUPT_ERROR;
    NTFS_RUN *Decoded = ExAllocatePoolZero(NonPagedPool, Capacity * sizeof(*Decoded), NTFS_MFT_TAG);
    if (!Decoded)
        return STATUS_NO_MEMORY;
    STATUS Status = NtfsDecodeRunlist(Attribute->MappingPairs, Attribute->MappingPairsBytes,
                                      Attribute->LowestVcn, Decoded, Capacity, RunCount);
    if (!HZ_SUCCESS(Status)) {
        ExFreePoolWithTag(Decoded, NTFS_MFT_TAG);
        return Status;
    }
    *Runs = Decoded;
    return STATUS_SUCCESS;
}

static STATUS NtfsSearchIndexNode(const NTFS_VOLUME_CONTEXT *Volume, const NTFS_ATTRIBUTE_VIEW *IndexRoot,
                                  const uint8_t *NodeBuffer, const uint16_t *Name, size_t NameCharacters,
                                  uint32_t *Cursor, bool RootNode, uint64_t *ChildVcn,
                                  NTFS_INDEX_ENTRY_VIEW *FoundEntry)
{
    uint32_t IndexBytes;
    uint32_t HeaderOffset;
    if (RootNode) {
        IndexBytes = IndexRoot->ResidentValueBytes;
        HeaderOffset = 16;
    } else {
        IndexBytes = Volume->Geometry.IndexRecordBytes;
        HeaderOffset = 24;
    }
    for (;;) {
        NTFS_INDEX_ENTRY_VIEW Entry;
        STATUS Status = RootNode
                            ? NtfsIndexRootNext(IndexRoot, Cursor, &Entry)
                            : NtfsIndexNodeNext(NodeBuffer, IndexBytes, HeaderOffset, Cursor, &Entry);
        if (Status == STATUS_NO_MORE_FILES) {
            *ChildVcn = Entry.HasChildNode ? Entry.ChildVcn : 0;
            if (Entry.HasChildNode)
                return STATUS_SUCCESS;
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        if (!HZ_SUCCESS(Status))
            return Status;

        int Compare = NtfsCompareIndexName(&Entry, Name, NameCharacters);
        if (Compare == 0) {
            *FoundEntry = Entry;
            *ChildVcn = 0;
            return STATUS_SUCCESS;
        }
        if (Compare > 0) {
            *ChildVcn = Entry.HasChildNode ? Entry.ChildVcn : 0;
            return Entry.HasChildNode ? STATUS_SUCCESS : STATUS_OBJECT_NAME_NOT_FOUND;
        }
        *ChildVcn = Entry.HasChildNode ? Entry.ChildVcn : 0;
    }
}

STATUS NtfsLookupDirectoryEntry(const NTFS_VOLUME_CONTEXT *Volume, uint64_t DirectoryRecordNumber,
                               const uint16_t *Name, size_t NameCharacters,
                               NTFS_FILE_INFORMATION *Information)
{
    if (!Volume || !Volume->Storage || !Name || !NameCharacters || NameCharacters > 255 || !Information)
        return STATUS_INVALID_PARAMETER;
    uint8_t *Record = ExAllocatePoolWithTag(NonPagedPool, Volume->Geometry.FileRecordBytes, NTFS_MFT_TAG);
    if (!Record)
        return STATUS_NO_MEMORY;
    STATUS Status = NtfsReadVolumeMftRecord(Volume, DirectoryRecordNumber, Record,
                                            Volume->Geometry.FileRecordBytes);
    if (HZ_SUCCESS(Status) && !(NtfsMftRead16(Record + 22) & 0x0002))
        Status = STATUS_NOT_A_DIRECTORY;
    static const uint16_t I30Name[] = {'$', 'I', '3', '0'};
    NTFS_ATTRIBUTE_VIEW IndexRoot = {0};
    if (HZ_SUCCESS(Status))
        Status = NtfsFindAttribute(Record, Volume->Geometry.FileRecordBytes, 0x90,
                                   I30Name, HZ_ARRAYSIZE(I30Name), &IndexRoot);

    NTFS_ATTRIBUTE_VIEW IndexAllocation = {0};
    NTFS_RUN *AllocationRuns = NULL;
    size_t AllocationRunCount = 0;
    if (HZ_SUCCESS(Status)) {
        STATUS AllocationStatus = NtfsFindAttribute(Record, Volume->Geometry.FileRecordBytes,
                                                    0xA0, I30Name, HZ_ARRAYSIZE(I30Name),
                                                    &IndexAllocation);
        if (AllocationStatus != STATUS_OBJECT_NAME_NOT_FOUND)
            Status = AllocationStatus;
    }
    if (HZ_SUCCESS(Status) && IndexAllocation.Attribute)
        Status = NtfsDecodeMappingPairs(&IndexAllocation, &AllocationRuns, &AllocationRunCount);

    uint32_t Cursor = 0;
    uint64_t ChildVcn = 0;
    bool RootNode = true;
    uint8_t *IndexBuffer = NULL;
    uint64_t Visited[32];
    size_t VisitedCount = 0;
    NTFS_INDEX_ENTRY_VIEW FoundEntry = {0};
    if (HZ_SUCCESS(Status)) {
        for (unsigned Depth = 0; Depth < HZ_ARRAYSIZE(Visited); ++Depth) {
            Status = NtfsSearchIndexNode(Volume, &IndexRoot, IndexBuffer, Name, NameCharacters, &Cursor,
                                         RootNode, &ChildVcn, &FoundEntry);
            if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
                break;
            if (!HZ_SUCCESS(Status) || FoundEntry.FileReference)
                break;
            if (!IndexAllocation.Attribute || !AllocationRuns || !AllocationRunCount) {
                Status = STATUS_DISK_CORRUPT_ERROR;
                break;
            }
            for (size_t Index = 0; Index < VisitedCount; ++Index) {
                if (Visited[Index] == ChildVcn) {
                    Status = STATUS_DISK_CORRUPT_ERROR;
                    break;
                }
            }
            if (!HZ_SUCCESS(Status))
                break;
            Visited[VisitedCount++] = ChildVcn;
            uint32_t IndexBytes = Volume->Geometry.IndexRecordBytes;
            uint32_t VcnUnitBytes = IndexBytes >= Volume->Geometry.BytesPerCluster
                                        ? Volume->Geometry.BytesPerCluster
                                        : Volume->Geometry.BytesPerSector;
            if (!VcnUnitBytes || IndexBytes % VcnUnitBytes ||
                ChildVcn > UINT64_MAX / VcnUnitBytes) {
                Status = STATUS_NOT_SUPPORTED;
                break;
            }
            uint64_t Offset = ChildVcn * VcnUnitBytes;
            if (Offset > IndexAllocation.DataBytes ||
                IndexBytes > IndexAllocation.DataBytes - Offset) {
                Status = STATUS_DISK_CORRUPT_ERROR;
                break;
            }
            if (IndexBuffer) {
                ExFreePoolWithTag(IndexBuffer, NTFS_MFT_TAG);
                IndexBuffer = NULL;
            }
            IndexBuffer = ExAllocatePoolWithTag(NonPagedPool, IndexBytes, NTFS_MFT_TAG);
            if (!IndexBuffer) {
                Status = STATUS_NO_MEMORY;
                break;
            }
            Status = NtfsReadDataStream(Volume->Storage, &Volume->Geometry,
                                        AllocationRuns, AllocationRunCount,
                                        IndexAllocation.DataBytes, IndexAllocation.InitializedBytes,
                                        Offset, IndexBytes, IndexBuffer);
            if (HZ_SUCCESS(Status) && memcmp(IndexBuffer, "INDX", 4) != 0)
                Status = STATUS_DISK_CORRUPT_ERROR;
            if (HZ_SUCCESS(Status))
                Status = NtfsApplyUpdateSequenceArray(IndexBuffer, IndexBytes,
                                                       Volume->Geometry.BytesPerSector);
            if (HZ_SUCCESS(Status) && NtfsMftRead64(IndexBuffer + 16) != ChildVcn)
                Status = STATUS_DISK_CORRUPT_ERROR;
            if (!HZ_SUCCESS(Status)) {
                ExFreePoolWithTag(IndexBuffer, NTFS_MFT_TAG);
                IndexBuffer = NULL;
                break;
            }
            RootNode = false;
            Cursor = 0;
        }
    }
    if (HZ_SUCCESS(Status) && ChildVcn && !FoundEntry.FileReference)
        Status = STATUS_DISK_CORRUPT_ERROR;

    if (HZ_SUCCESS(Status) && FoundEntry.FileReference) {
        NTFS_FILE_INFORMATION Candidate = {0};
        Candidate.FileReference = FoundEntry.FileReference;
        Candidate.RecordNumber = FoundEntry.FileReference & 0x0000FFFFFFFFFFFFull;
        Candidate.DataBytes = FoundEntry.DataBytes;
        Candidate.Attributes = FoundEntry.FileAttributes;
        Candidate.IsDirectory = (FoundEntry.FileAttributes & 0x10) != 0;
        *Information = Candidate;
    }
    if (AllocationRuns)
        ExFreePoolWithTag(AllocationRuns, NTFS_MFT_TAG);
    if (IndexBuffer)
        ExFreePoolWithTag(IndexBuffer, NTFS_MFT_TAG);
    ExFreePoolWithTag(Record, NTFS_MFT_TAG);
    return Status;
}

STATUS NtfsReadFileData(const NTFS_VOLUME_CONTEXT *Volume, const NTFS_FILE_INFORMATION *Information,
                        uint64_t Offset, uint32_t Length, void *Buffer, uint32_t *BytesRead)
{
    if (BytesRead)
        *BytesRead = 0;
    if (!Volume || !Volume->Storage || !Information || (Length && !Buffer) || !BytesRead)
        return STATUS_INVALID_PARAMETER;
    if (Information->IsDirectory)
        return STATUS_FILE_IS_A_DIRECTORY;
    if (!Length)
        return STATUS_SUCCESS;
    if (Offset >= Information->DataBytes)
        return STATUS_END_OF_FILE;
    uint32_t ReadLength = (uint32_t)HZ_MIN((uint64_t)Length, Information->DataBytes - Offset);
    uint8_t *Record = ExAllocatePoolWithTag(NonPagedPool, Volume->Geometry.FileRecordBytes, NTFS_MFT_TAG);
    if (!Record)
        return STATUS_NO_MEMORY;
    STATUS Status = NtfsReadVolumeMftRecord(Volume, Information->RecordNumber, Record,
                                            Volume->Geometry.FileRecordBytes);
    uint16_t ReferenceSequence = (uint16_t)(Information->FileReference >> 48);
    if (HZ_SUCCESS(Status) && ReferenceSequence && NtfsMftRead16(Record + 16) != ReferenceSequence)
        Status = STATUS_OBJECT_NAME_NOT_FOUND;
    NTFS_ATTRIBUTE_VIEW Data = {0};
    if (HZ_SUCCESS(Status))
        Status = NtfsFindAttribute(Record, Volume->Geometry.FileRecordBytes, 0x80, NULL, 0, &Data);
    if (HZ_SUCCESS(Status) && !Data.NonResident) {
        if (Offset >= Data.ResidentValueBytes) {
            Status = STATUS_END_OF_FILE;
        } else {
            ReadLength = (uint32_t)HZ_MIN((uint64_t)ReadLength, Data.ResidentValueBytes - Offset);
            memcpy(Buffer, Data.ResidentValue + Offset, ReadLength);
            *BytesRead = ReadLength;
        }
    } else if (HZ_SUCCESS(Status)) {
        if (Data.Flags & 0x4001) {
            Status = STATUS_NOT_SUPPORTED;
        } else {
            if (Offset >= Data.DataBytes) {
                Status = STATUS_END_OF_FILE;
            } else {
                ReadLength = (uint32_t)HZ_MIN((uint64_t)ReadLength, Data.DataBytes - Offset);
            }
            size_t RunCapacity = HZ_SUCCESS(Status) ? (size_t)Data.MappingPairsBytes / 2 + 1 : 0;
            if (HZ_SUCCESS(Status) && (!RunCapacity || RunCapacity > SIZE_MAX / sizeof(NTFS_RUN))) {
                Status = STATUS_DISK_CORRUPT_ERROR;
            } else if (HZ_SUCCESS(Status)) {
                NTFS_RUN *Runs = ExAllocatePoolZero(NonPagedPool, RunCapacity * sizeof(*Runs), NTFS_MFT_TAG);
                if (!Runs) {
                    Status = STATUS_NO_MEMORY;
                } else {
                    size_t RunCount = 0;
                    Status = NtfsDecodeRunlist(Data.MappingPairs, Data.MappingPairsBytes,
                                               Data.LowestVcn, Runs, RunCapacity, &RunCount);
                    if (HZ_SUCCESS(Status))
                        Status = NtfsReadDataStream(Volume->Storage, &Volume->Geometry, Runs, RunCount,
                                                    Data.DataBytes, Data.InitializedBytes, Offset,
                                                    ReadLength, Buffer);
                    if (HZ_SUCCESS(Status))
                        *BytesRead = ReadLength;
                    ExFreePoolWithTag(Runs, NTFS_MFT_TAG);
                }
            }
        }
    }
    ExFreePoolWithTag(Record, NTFS_MFT_TAG);
    return Status;
}

STATUS NtfsValidateVolumeMetadata(DEVICE_OBJECT *Storage, NTFS_BOOT_INFORMATION *Geometry)
{
    if (!Geometry)
        return STATUS_INVALID_PARAMETER;
    NTFS_VOLUME_CONTEXT Volume = {0};
    STATUS Status = NtfsInitializeVolume(Storage, &Volume);
    if (HZ_SUCCESS(Status))
        *Geometry = Volume.Geometry;
    NtfsReleaseVolume(&Volume);
    return Status;
}

STATUS NtfsReadMftRecord(DEVICE_OBJECT *Storage, const NTFS_BOOT_INFORMATION *Geometry,
                         const NTFS_RUN *MftRuns, size_t MftRunCount, uint64_t MftDataBytes,
                         uint64_t MftInitializedBytes, uint64_t RecordNumber,
                         void *Record, size_t RecordBytes)
{
    if (!Storage || !Geometry || !Record || RecordBytes != Geometry->FileRecordBytes ||
        !RecordBytes || RecordBytes > UINT32_MAX || !Geometry->BytesPerSector ||
        RecordBytes % Geometry->BytesPerSector ||
        RecordNumber > UINT64_MAX / RecordBytes)
        return STATUS_INVALID_PARAMETER;
    uint64_t Offset = RecordNumber * RecordBytes;
    if (Offset > MftDataBytes || RecordBytes > MftDataBytes - Offset ||
        Offset > MftInitializedBytes || RecordBytes > MftInitializedBytes - Offset)
        return STATUS_END_OF_FILE;

    STATUS Status = NtfsReadDataStream(Storage, Geometry, MftRuns, MftRunCount, MftDataBytes,
                                       MftInitializedBytes, Offset, (uint32_t)RecordBytes, Record);
    if (!HZ_SUCCESS(Status))
        return Status;
    if (memcmp(Record, "FILE", 4) != 0)
        return STATUS_DISK_CORRUPT_ERROR;
    return NtfsApplyUpdateSequenceArray(Record, RecordBytes, Geometry->BytesPerSector);
}
