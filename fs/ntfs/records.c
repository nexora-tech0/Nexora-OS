/*
 * Nexora OS - fs/ntfs/records.c
 * NTFS multi-sector record repair and mapping-pairs decoding.
 */
#include "ntfs.h"
#include "rtl.h"

static uint16_t NtfsRecordRead16(const uint8_t *Bytes)
{
    return (uint16_t)Bytes[0] | ((uint16_t)Bytes[1] << 8);
}

static uint32_t NtfsRecordRead32(const uint8_t *Bytes)
{
    return (uint32_t)Bytes[0] | ((uint32_t)Bytes[1] << 8) |
           ((uint32_t)Bytes[2] << 16) | ((uint32_t)Bytes[3] << 24);
}

static uint64_t NtfsRecordReadUnsigned(const uint8_t *Bytes, unsigned Width)
{
    uint64_t Value = 0;
    for (unsigned Index = 0; Index < Width; ++Index)
        Value |= (uint64_t)Bytes[Index] << (Index * 8);
    return Value;
}

STATUS NtfsFindAttribute(const void *FixedFileRecord, size_t RecordBytes, uint32_t Type,
                         const uint16_t *Name, size_t NameCharacters, NTFS_ATTRIBUTE_VIEW *View)
{
    if (!FixedFileRecord || !View || (NameCharacters && !Name) || RecordBytes < 48)
        return STATUS_INVALID_PARAMETER;
    const uint8_t *Record = (const uint8_t *)FixedFileRecord;
    if (memcmp(Record, "FILE", 4) != 0)
        return STATUS_DISK_CORRUPT_ERROR;
    uint16_t FirstAttribute = NtfsRecordRead16(Record + 20);
    uint32_t BytesInUse = NtfsRecordRead32(Record + 24);
    if (FirstAttribute < 48 || FirstAttribute > BytesInUse || BytesInUse > RecordBytes)
        return STATUS_DISK_CORRUPT_ERROR;

    size_t Offset = FirstAttribute;
    while (Offset + 4 <= BytesInUse) {
        const uint8_t *Attribute = Record + Offset;
        uint32_t AttributeType = NtfsRecordRead32(Attribute);
        if (AttributeType == 0xFFFFFFFFu)
            break;
        if (Offset + 16 > BytesInUse)
            return STATUS_DISK_CORRUPT_ERROR;
        uint32_t AttributeBytes = NtfsRecordRead32(Attribute + 4);
        uint8_t NonResident = Attribute[8];
        uint8_t AttributeNameLength = Attribute[9];
        uint16_t AttributeNameOffset = NtfsRecordRead16(Attribute + 10);
        if (AttributeBytes < 16 || AttributeBytes > BytesInUse - Offset || NonResident > 1)
            return STATUS_DISK_CORRUPT_ERROR;
        uint64_t NameBytes = (uint64_t)AttributeNameLength * sizeof(uint16_t);
        if (AttributeNameLength && (AttributeNameOffset < 16 || AttributeNameOffset > AttributeBytes ||
                                   NameBytes > AttributeBytes - AttributeNameOffset))
            return STATUS_DISK_CORRUPT_ERROR;
        bool NameMatches = AttributeNameLength == NameCharacters;
        if (NameMatches && NameCharacters) {
            const uint8_t *StoredName = Attribute + AttributeNameOffset;
            for (size_t Index = 0; Index < NameCharacters; ++Index) {
                if (NtfsRecordRead16(StoredName + Index * sizeof(uint16_t)) != Name[Index]) {
                    NameMatches = false;
                    break;
                }
            }
        }
        if (AttributeType == Type && NameMatches) {
            NTFS_ATTRIBUTE_VIEW Candidate = {0};
            Candidate.Type = AttributeType;
            Candidate.Flags = NtfsRecordRead16(Attribute + 12);
            Candidate.NonResident = NonResident != 0;
            Candidate.Attribute = Attribute;
            Candidate.AttributeBytes = AttributeBytes;
            if (!NonResident) {
                if (AttributeBytes < 24)
                    return STATUS_DISK_CORRUPT_ERROR;
                uint32_t ValueBytes = NtfsRecordRead32(Attribute + 16);
                uint16_t ValueOffset = NtfsRecordRead16(Attribute + 20);
                if (ValueOffset < 24 || ValueOffset > AttributeBytes || ValueBytes > AttributeBytes - ValueOffset)
                    return STATUS_DISK_CORRUPT_ERROR;
                Candidate.ResidentValue = Attribute + ValueOffset;
                Candidate.ResidentValueBytes = ValueBytes;
            } else {
                if (AttributeBytes < 64)
                    return STATUS_DISK_CORRUPT_ERROR;
                uint16_t PairsOffset = NtfsRecordRead16(Attribute + 32);
                Candidate.LowestVcn = NtfsRecordReadUnsigned(Attribute + 16, 8);
                Candidate.HighestVcn = NtfsRecordReadUnsigned(Attribute + 24, 8);
                Candidate.DataBytes = NtfsRecordReadUnsigned(Attribute + 48, 8);
                Candidate.InitializedBytes = NtfsRecordReadUnsigned(Attribute + 56, 8);
                if (PairsOffset < 64 || PairsOffset >= AttributeBytes ||
                    Candidate.HighestVcn < Candidate.LowestVcn ||
                    Candidate.InitializedBytes > Candidate.DataBytes)
                    return STATUS_DISK_CORRUPT_ERROR;
                Candidate.MappingPairs = Attribute + PairsOffset;
                Candidate.MappingPairsBytes = AttributeBytes - PairsOffset;
            }
            *View = Candidate;
            return STATUS_SUCCESS;
        }
        Offset += AttributeBytes;
    }
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

STATUS NtfsApplyUpdateSequenceArray(void *Record, size_t RecordBytes, uint16_t BytesPerSector)
{
    if (!Record || BytesPerSector < 512 || (BytesPerSector & (BytesPerSector - 1)) ||
        RecordBytes < BytesPerSector || RecordBytes % BytesPerSector)
        return STATUS_INVALID_PARAMETER;

    uint8_t *Bytes = (uint8_t *)Record;
    uint16_t UsaOffset = NtfsRecordRead16(Bytes + 4);
    uint16_t UsaCount = NtfsRecordRead16(Bytes + 6);
    size_t ExpectedCount = RecordBytes / BytesPerSector + 1;
    size_t UsaBytes = (size_t)UsaCount * sizeof(uint16_t);
    if (UsaCount != ExpectedCount || UsaOffset < 8 || UsaOffset > RecordBytes ||
        UsaBytes > RecordBytes - UsaOffset)
        return STATUS_DISK_CORRUPT_ERROR;
    size_t UsaEnd = UsaOffset + UsaBytes;
    for (size_t SectorIndex = 1; SectorIndex < UsaCount; ++SectorIndex) {
        size_t TailOffset = SectorIndex * BytesPerSector - sizeof(uint16_t);
        if (UsaOffset < TailOffset + sizeof(uint16_t) && TailOffset < UsaEnd)
            return STATUS_DISK_CORRUPT_ERROR;
    }

    uint16_t UpdateSequence = NtfsRecordRead16(Bytes + UsaOffset);
    if (!UpdateSequence)
        return STATUS_DISK_CORRUPT_ERROR;
    for (size_t SectorIndex = 1; SectorIndex < UsaCount; ++SectorIndex) {
        size_t TailOffset = SectorIndex * BytesPerSector - sizeof(uint16_t);
        if (NtfsRecordRead16(Bytes + TailOffset) != UpdateSequence)
            return STATUS_DISK_CORRUPT_ERROR;
    }
    for (size_t SectorIndex = 1; SectorIndex < UsaCount; ++SectorIndex) {
        size_t TailOffset = SectorIndex * BytesPerSector - sizeof(uint16_t);
        memcpy(Bytes + TailOffset, Bytes + UsaOffset + SectorIndex * sizeof(uint16_t), sizeof(uint16_t));
    }
    return STATUS_SUCCESS;
}

STATUS NtfsDecodeRunlist(const void *MappingPairs, size_t MappingBytes, uint64_t StartingVcn,
                         NTFS_RUN *Runs, size_t RunCapacity, size_t *RunsDecoded)
{
    if (RunsDecoded)
        *RunsDecoded = 0;
    if (!MappingPairs || !MappingBytes || !Runs || !RunCapacity || !RunsDecoded)
        return STATUS_INVALID_PARAMETER;

    const uint8_t *Pairs = (const uint8_t *)MappingPairs;
    size_t Offset = 0, Count = 0;
    uint64_t Vcn = StartingVcn;
    int64_t PreviousLcn = 0;
    bool Terminated = false;
    while (Offset < MappingBytes) {
        uint8_t Header = Pairs[Offset++];
        if (!Header) {
            Terminated = true;
            break;
        }
        unsigned LengthBytes = Header & 0x0F;
        unsigned OffsetBytes = Header >> 4;
        if (!LengthBytes || LengthBytes > 8 || OffsetBytes > 8 ||
            (size_t)LengthBytes + OffsetBytes > MappingBytes - Offset)
            return STATUS_DISK_CORRUPT_ERROR;
        uint64_t RunLength = NtfsRecordReadUnsigned(Pairs + Offset, LengthBytes);
        Offset += LengthBytes;
        if (!RunLength || RunLength > UINT64_MAX - Vcn)
            return STATUS_DISK_CORRUPT_ERROR;

        NTFS_RUN Run = {0};
        Run.Vcn = Vcn;
        Run.Length = RunLength;
        Run.Sparse = OffsetBytes == 0;
        if (!Run.Sparse) {
            uint64_t Encoded = NtfsRecordReadUnsigned(Pairs + Offset, OffsetBytes);
            if (OffsetBytes < 8 && (Pairs[Offset + OffsetBytes - 1] & 0x80))
                Encoded |= UINT64_MAX << (OffsetBytes * 8);
            int64_t Delta = (int64_t)Encoded;
            if ((Delta > 0 && PreviousLcn > INT64_MAX - Delta) ||
                (Delta < 0 && PreviousLcn < INT64_MIN - Delta))
                return STATUS_DISK_CORRUPT_ERROR;
            int64_t NewLcn = PreviousLcn + Delta;
            if (NewLcn < 0 || (uint64_t)NewLcn > INT64_MAX - RunLength)
                return STATUS_DISK_CORRUPT_ERROR;
            Run.Lcn = NewLcn;
            PreviousLcn = NewLcn;
        }
        Offset += OffsetBytes;
        if (Count == RunCapacity)
            return STATUS_BUFFER_TOO_SMALL;
        Runs[Count++] = Run;
        Vcn += RunLength;
    }
    if (!Terminated)
        return STATUS_DISK_CORRUPT_ERROR;
    *RunsDecoded = Count;
    return STATUS_SUCCESS;
}

static void NtfsRecordWrite16(uint8_t *Bytes, uint16_t Value)
{
    Bytes[0] = (uint8_t)Value;
    Bytes[1] = (uint8_t)(Value >> 8);
}

static void NtfsRecordWrite32(uint8_t *Bytes, uint32_t Value)
{
    for (unsigned Index = 0; Index < 4; ++Index)
        Bytes[Index] = (uint8_t)(Value >> (Index * 8));
}

static void NtfsRecordWrite64(uint8_t *Bytes, uint64_t Value)
{
    NtfsRecordWrite32(Bytes, (uint32_t)Value);
    NtfsRecordWrite32(Bytes + 4, (uint32_t)(Value >> 32));
}

bool NtfsRunRecordSelfTests(uint32_t *Passed, uint32_t *Total)
{
    enum { TestCount = 18 };
    uint8_t Record[1024] = {0};
    uint8_t Pairs[] = {0x11, 3, 5, 0x11, 2, 0xFE, 0x01, 4, 0x00};
    uint8_t NoTerminator[] = {0x11, 1, 1};
    uint8_t BadLength[] = {0x01, 0, 0};
    uint8_t NegativeLcn[] = {0x11, 1, 0xFF, 0};
    NTFS_RUN Runs[3];
    NTFS_ATTRIBUTE_VIEW Attribute;
    uint8_t AttributeRecord[128] = {0};
    uint8_t IndexValue[132] = {0};
    size_t Decoded = 0;
    uint32_t Pass = 0;

    NtfsRecordWrite16(Record + 4, 48);
    NtfsRecordWrite16(Record + 6, 3);
    NtfsRecordWrite16(Record + 48, 0xA55A);
    NtfsRecordWrite16(Record + 510, 0xA55A);
    NtfsRecordWrite16(Record + 1022, 0xA55A);
    NtfsRecordWrite16(Record + 50, 0x1234);
    NtfsRecordWrite16(Record + 52, 0x5678);
    Pass += HZ_SUCCESS(NtfsApplyUpdateSequenceArray(Record, sizeof(Record), 512)) &&
            NtfsRecordRead16(Record + 510) == 0x1234 && NtfsRecordRead16(Record + 1022) == 0x5678;

    NtfsRecordWrite16(Record + 510, 0xA55A);
    NtfsRecordWrite16(Record + 1022, 0x0000);
    Pass += NtfsApplyUpdateSequenceArray(Record, sizeof(Record), 512) == STATUS_DISK_CORRUPT_ERROR;
    NtfsRecordWrite16(Record + 4, 510);
    Pass += NtfsApplyUpdateSequenceArray(Record, sizeof(Record), 512) == STATUS_DISK_CORRUPT_ERROR;
    NtfsRecordWrite16(Record + 4, 48);
    Pass += NtfsApplyUpdateSequenceArray(Record, 1023, 512) == STATUS_INVALID_PARAMETER;
    NtfsRecordWrite16(Record + 6, 2);
    Pass += NtfsApplyUpdateSequenceArray(Record, sizeof(Record), 512) == STATUS_DISK_CORRUPT_ERROR;

    STATUS Status = NtfsDecodeRunlist(Pairs, sizeof(Pairs), 7, Runs, 3, &Decoded);
    Pass += HZ_SUCCESS(Status) && Decoded == 3 && Runs[0].Vcn == 7 && Runs[0].Length == 3 &&
            Runs[0].Lcn == 5 && Runs[1].Vcn == 10 && Runs[1].Length == 2 && Runs[1].Lcn == 3 &&
            Runs[2].Vcn == 12 && Runs[2].Length == 4 && Runs[2].Sparse;
    Pass += NtfsDecodeRunlist(Pairs, sizeof(Pairs), 0, Runs, 2, &Decoded) == STATUS_BUFFER_TOO_SMALL;
    Pass += NtfsDecodeRunlist(NoTerminator, sizeof(NoTerminator), 0, Runs, 3, &Decoded) == STATUS_DISK_CORRUPT_ERROR;
    Pass += NtfsDecodeRunlist(BadLength, sizeof(BadLength), 0, Runs, 3, &Decoded) == STATUS_DISK_CORRUPT_ERROR;
    Pass += NtfsDecodeRunlist(NegativeLcn, sizeof(NegativeLcn), 0, Runs, 3, &Decoded) == STATUS_DISK_CORRUPT_ERROR;

    memcpy(AttributeRecord, "FILE", 4);
    NtfsRecordWrite16(AttributeRecord + 20, 56);
    NtfsRecordWrite32(AttributeRecord + 24, 88);
    NtfsRecordWrite32(AttributeRecord + 56, 0x80); /* unnamed $DATA */
    NtfsRecordWrite32(AttributeRecord + 60, 32);
    AttributeRecord[64] = 0; /* resident */
    NtfsRecordWrite32(AttributeRecord + 72, 4); /* value length */
    NtfsRecordWrite16(AttributeRecord + 76, 24);
    memcpy(AttributeRecord + 80, "data", 4);
    NtfsRecordWrite32(AttributeRecord + 88, 0xFFFFFFFFu);
    STATUS AttributeStatus = NtfsFindAttribute(AttributeRecord, sizeof(AttributeRecord), 0x80, NULL, 0, &Attribute);
    Pass += HZ_SUCCESS(AttributeStatus) && !Attribute.NonResident && Attribute.ResidentValueBytes == 4 &&
            memcmp(Attribute.ResidentValue, "data", 4) == 0;
    Pass += NtfsFindAttribute(AttributeRecord, sizeof(AttributeRecord), 0x80, NULL, 1, &Attribute) ==
            STATUS_INVALID_PARAMETER;
    Pass += NtfsFindAttribute(AttributeRecord, sizeof(AttributeRecord), 0x90, NULL, 0, &Attribute) ==
            STATUS_OBJECT_NAME_NOT_FOUND;
    NtfsRecordWrite32(AttributeRecord + 60, 0xFFFFFFFFu);
    Pass += NtfsFindAttribute(AttributeRecord, sizeof(AttributeRecord), 0x80, NULL, 0, &Attribute) ==
            STATUS_DISK_CORRUPT_ERROR;
    memset(AttributeRecord, 0, sizeof(AttributeRecord));
    memcpy(AttributeRecord, "FILE", 4);
    NtfsRecordWrite16(AttributeRecord + 20, 56);
    NtfsRecordWrite32(AttributeRecord + 24, 128);
    NtfsRecordWrite32(AttributeRecord + 56, 0x80);
    NtfsRecordWrite32(AttributeRecord + 60, 72);
    AttributeRecord[64] = 1;
    NtfsRecordWrite64(AttributeRecord + 72, 0); /* lowest VCN */
    NtfsRecordWrite64(AttributeRecord + 80, 7); /* highest VCN */
    NtfsRecordWrite16(AttributeRecord + 88, 64); /* mapping pairs offset */
    NtfsRecordWrite64(AttributeRecord + 104, 32768); /* data bytes */
    NtfsRecordWrite64(AttributeRecord + 112, 32768); /* initialized bytes */
    AttributeRecord[120] = 0x11; AttributeRecord[121] = 8; AttributeRecord[122] = 4;
    AttributeRecord[123] = 0x00;
    AttributeStatus = NtfsFindAttribute(AttributeRecord, sizeof(AttributeRecord), 0x80, NULL, 0, &Attribute);
    Pass += HZ_SUCCESS(AttributeStatus) && Attribute.NonResident && Attribute.LowestVcn == 0 &&
            Attribute.HighestVcn == 7 && Attribute.DataBytes == 32768 && Attribute.MappingPairsBytes == 8;

    NtfsRecordWrite32(IndexValue + 16, 16);  /* first entry relative to index header */
    NtfsRecordWrite32(IndexValue + 20, 116); /* first entry, one key, and end marker */
    NtfsRecordWrite32(IndexValue + 24, 116);
    NtfsRecordWrite64(IndexValue + 32, 0x0001000000000007ull);
    NtfsRecordWrite16(IndexValue + 40, 84);
    NtfsRecordWrite16(IndexValue + 42, 68);
    NtfsRecordWrite64(IndexValue + 32 + 16 + 48, 1234);
    NtfsRecordWrite32(IndexValue + 32 + 16 + 56, 0x10);
    IndexValue[32 + 16 + 64] = 1;
    IndexValue[32 + 16 + 65] = 1;
    NtfsRecordWrite16(IndexValue + 32 + 16 + 66, 'A');
    NtfsRecordWrite16(IndexValue + 116 + 8, 16);
    NtfsRecordWrite16(IndexValue + 116 + 12, 2);
    NTFS_ATTRIBUTE_VIEW IndexRoot = {.Type = 0x90, .ResidentValue = IndexValue,
                                     .ResidentValueBytes = sizeof(IndexValue)};
    NTFS_INDEX_ENTRY_VIEW IndexEntry;
    uint32_t IndexCursor = 0;
    STATUS IndexStatus = NtfsIndexRootNext(&IndexRoot, &IndexCursor, &IndexEntry);
    Pass += HZ_SUCCESS(IndexStatus) && IndexEntry.FileReference == 0x0001000000000007ull &&
            IndexEntry.NameCharacters == 1 && IndexEntry.NameNamespace == 1 &&
            IndexEntry.DataBytes == 1234 && IndexEntry.FileAttributes == 0x10 &&
            NtfsRecordRead16(IndexEntry.NameUtf16Le) == 'A' && !IndexEntry.HasChildNode;
    Pass += NtfsIndexRootNext(&IndexRoot, &IndexCursor, &IndexEntry) == STATUS_NO_MORE_FILES;
    IndexValue[32 + 16 + 64] = 255;
    IndexCursor = 0;
    Pass += NtfsIndexRootNext(&IndexRoot, &IndexCursor, &IndexEntry) == STATUS_DISK_CORRUPT_ERROR;

    if (Passed) *Passed = Pass;
    if (Total) *Total = TestCount;
    return Pass == TestCount;
}
