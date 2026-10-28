/*
 * Nexora OS - include/ntfs.h
 * Bounds-checked NTFS 3.x volume boot-record decoding shared by the driver and its self-tests.
 */
#ifndef NTFS_H
#define NTFS_H

#include "io.h"

HZ_EXTERN_C_BEGIN

typedef struct _NTFS_BOOT_INFORMATION {
    uint16_t BytesPerSector;
    uint8_t SectorsPerCluster;
    uint32_t BytesPerCluster;
    uint64_t TotalSectors;
    uint64_t MftCluster;
    uint64_t MftMirrorCluster;
    uint32_t FileRecordBytes;
    uint32_t IndexRecordBytes;
    uint64_t VolumeSerial;
} NTFS_BOOT_INFORMATION;

typedef struct _NTFS_RUN {
    uint64_t Vcn;
    uint64_t Length;
    int64_t Lcn;
    bool Sparse;
} NTFS_RUN;

typedef struct _NTFS_ATTRIBUTE_VIEW {
    uint32_t Type;
    uint16_t Flags;
    bool NonResident;
    const uint8_t *Attribute;
    uint32_t AttributeBytes;
    const uint8_t *ResidentValue;
    uint32_t ResidentValueBytes;
    uint64_t LowestVcn;
    uint64_t HighestVcn;
    uint64_t DataBytes;
    uint64_t InitializedBytes;
    const uint8_t *MappingPairs;
    uint32_t MappingPairsBytes;
} NTFS_ATTRIBUTE_VIEW;

/* Persistent read-side volume state used by the NTFS mount and file-control layers. */
typedef struct _NTFS_VOLUME_CONTEXT {
    DEVICE_OBJECT *Storage;
    NTFS_BOOT_INFORMATION Geometry;
    NTFS_RUN *MftRuns;
    size_t MftRunCount;
    uint64_t MftDataBytes;
    uint64_t MftInitializedBytes;
} NTFS_VOLUME_CONTEXT;

typedef struct _NTFS_INDEX_ENTRY_VIEW {
    uint64_t FileReference;
    const uint8_t *NameUtf16Le;
    uint8_t NameCharacters;
    uint8_t NameNamespace;
    uint32_t FileAttributes;
    uint64_t DataBytes;
    uint64_t ChildVcn;
    bool HasChildNode;
} NTFS_INDEX_ENTRY_VIEW;

typedef struct _NTFS_FILE_INFORMATION {
    uint64_t FileReference;
    uint64_t RecordNumber;
    uint64_t DataBytes;
    uint32_t Attributes;
    bool IsDirectory;
} NTFS_FILE_INFORMATION;

/* Decode a 512-byte NTFS boot record and reject geometry outside the supplied device bounds. */
STATUS NtfsDecodeBootRecord(const void *Sector, size_t SectorBytes, uint64_t DeviceSectors,
                            NTFS_BOOT_INFORMATION *Information);

/* Pure parser tests, also called by the kernel file-system self-test suite. */
bool NtfsRunBootRecordSelfTests(uint32_t *Passed, uint32_t *Total);

/* Validate and apply the Update Sequence Array (USA) fixups in one FILE/INDX record. */
STATUS NtfsApplyUpdateSequenceArray(void *Record, size_t RecordBytes, uint16_t BytesPerSector);

/* Decode NTFS mapping pairs into VCN runs. The function returns the number of decoded runs. */
STATUS NtfsDecodeRunlist(const void *MappingPairs, size_t MappingBytes, uint64_t StartingVcn,
                         NTFS_RUN *Runs, size_t RunCapacity, size_t *RunsDecoded);
STATUS NtfsFindAttribute(const void *FixedFileRecord, size_t RecordBytes, uint32_t Type,
                         const uint16_t *Name, size_t NameCharacters, NTFS_ATTRIBUTE_VIEW *View);
STATUS NtfsIndexRootNext(const NTFS_ATTRIBUTE_VIEW *IndexRoot, uint32_t *Cursor,
                         NTFS_INDEX_ENTRY_VIEW *Entry);
STATUS NtfsReadDataStream(DEVICE_OBJECT *Storage, const NTFS_BOOT_INFORMATION *Geometry,
                          const NTFS_RUN *Runs, size_t RunCount, uint64_t DataBytes,
                          uint64_t InitializedBytes, uint64_t Offset, uint32_t Length, void *Buffer);
STATUS NtfsReadMftRecord(DEVICE_OBJECT *Storage, const NTFS_BOOT_INFORMATION *Geometry,
                         const NTFS_RUN *MftRuns, size_t MftRunCount, uint64_t MftDataBytes,
                         uint64_t MftInitializedBytes, uint64_t RecordNumber,
                         void *Record, size_t RecordBytes);
STATUS NtfsInitializeVolume(DEVICE_OBJECT *Storage, NTFS_VOLUME_CONTEXT *Volume);
void NtfsReleaseVolume(NTFS_VOLUME_CONTEXT *Volume);
STATUS NtfsReadVolumeMftRecord(const NTFS_VOLUME_CONTEXT *Volume, uint64_t RecordNumber,
                               void *Record, size_t RecordBytes);
STATUS NtfsLookupDirectoryEntry(const NTFS_VOLUME_CONTEXT *Volume, uint64_t DirectoryRecordNumber,
                               const uint16_t *Name, size_t NameCharacters,
                               NTFS_FILE_INFORMATION *Information);
STATUS NtfsReadFileData(const NTFS_VOLUME_CONTEXT *Volume, const NTFS_FILE_INFORMATION *Information,
                        uint64_t Offset, uint32_t Length, void *Buffer, uint32_t *BytesRead);
/* Read the boot sector, $MFT record zero, its data runlist, and root directory record five. */
STATUS NtfsValidateVolumeMetadata(DEVICE_OBJECT *Storage, NTFS_BOOT_INFORMATION *Geometry);
bool NtfsRunRecordSelfTests(uint32_t *Passed, uint32_t *Total);

HZ_EXTERN_C_END

#endif /* NTFS_H */
