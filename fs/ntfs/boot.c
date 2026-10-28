/*
 * Nexora OS - fs/ntfs/boot.c
 * Strict NTFS volume boot-record validation. This is the first piece of the native NTFS driver;
 * it deliberately does not register or claim a mountable file system until MFT and I/O support
 * are implemented.
 */
#include "ntfs.h"
#include "rtl.h"

static uint16_t NtfsRead16(const uint8_t *Bytes)
{
    return (uint16_t)Bytes[0] | ((uint16_t)Bytes[1] << 8);
}

static uint32_t NtfsRead32(const uint8_t *Bytes)
{
    return (uint32_t)Bytes[0] | ((uint32_t)Bytes[1] << 8) |
           ((uint32_t)Bytes[2] << 16) | ((uint32_t)Bytes[3] << 24);
}

static uint64_t NtfsRead64(const uint8_t *Bytes)
{
    return (uint64_t)NtfsRead32(Bytes) | ((uint64_t)NtfsRead32(Bytes + 4) << 32);
}

static bool NtfsPowerOfTwo(uint32_t Value)
{
    return Value && !(Value & (Value - 1));
}

static bool NtfsDecodeRecordBytes(int8_t Encoded, uint32_t ClusterBytes, uint32_t *Bytes)
{
    if (!Encoded)
        return false;
    if (Encoded > 0) {
        if ((uint32_t)Encoded > UINT32_MAX / ClusterBytes)
            return false;
        *Bytes = (uint32_t)Encoded * ClusterBytes;
    } else {
        /* NTFS stores a negative log2 byte size for sub-cluster MFT/index records. */
        int Exponent = -(int)Encoded;
        if (Exponent < 9 || Exponent > 20)
            return false;
        *Bytes = 1u << Exponent;
    }
    return *Bytes >= 512 && *Bytes <= 1024u * 1024u && NtfsPowerOfTwo(*Bytes);
}

STATUS NtfsDecodeBootRecord(const void *Sector, size_t SectorBytes, uint64_t DeviceSectors,
                            NTFS_BOOT_INFORMATION *Information)
{
    if (!Sector || !Information || SectorBytes < 512 || DeviceSectors == 0)
        return STATUS_INVALID_PARAMETER;
    const uint8_t *Bytes = (const uint8_t *)Sector;
    if (memcmp(Bytes + 3, "NTFS    ", 8) != 0 || NtfsRead16(Bytes + 510) != 0xAA55)
        return STATUS_UNRECOGNIZED_VOLUME;

    uint16_t BytesPerSector = NtfsRead16(Bytes + 11);
    uint8_t SectorsPerCluster = Bytes[13];
    if (!NtfsPowerOfTwo(BytesPerSector) || BytesPerSector < 512 || BytesPerSector > 4096 ||
        BytesPerSector > SectorBytes || !NtfsPowerOfTwo(SectorsPerCluster) ||
        SectorsPerCluster > 128)
        return STATUS_UNRECOGNIZED_VOLUME;
    uint32_t ClusterBytes = (uint32_t)BytesPerSector * SectorsPerCluster;
    if (ClusterBytes > 2u * 1024u * 1024u)
        return STATUS_UNRECOGNIZED_VOLUME;

    uint64_t TotalSectors = NtfsRead64(Bytes + 40);
    uint64_t MftCluster = NtfsRead64(Bytes + 48);
    uint64_t MftMirrorCluster = NtfsRead64(Bytes + 56);
    if (!TotalSectors || TotalSectors > DeviceSectors)
        return STATUS_UNRECOGNIZED_VOLUME;
    uint64_t ClusterCount = TotalSectors / SectorsPerCluster;
    if (MftCluster >= ClusterCount || MftMirrorCluster >= ClusterCount)
        return STATUS_UNRECOGNIZED_VOLUME;

    NTFS_BOOT_INFORMATION Candidate = {0};
    Candidate.BytesPerSector = BytesPerSector;
    Candidate.SectorsPerCluster = SectorsPerCluster;
    Candidate.BytesPerCluster = ClusterBytes;
    Candidate.TotalSectors = TotalSectors;
    Candidate.MftCluster = MftCluster;
    Candidate.MftMirrorCluster = MftMirrorCluster;
    Candidate.VolumeSerial = NtfsRead64(Bytes + 72);
    if (!NtfsDecodeRecordBytes((int8_t)Bytes[64], ClusterBytes, &Candidate.FileRecordBytes) ||
        !NtfsDecodeRecordBytes((int8_t)Bytes[68], ClusterBytes, &Candidate.IndexRecordBytes))
        return STATUS_UNRECOGNIZED_VOLUME;

    *Information = Candidate;
    return STATUS_SUCCESS;
}

static void NtfsWrite16(uint8_t *Bytes, uint16_t Value)
{
    Bytes[0] = (uint8_t)Value;
    Bytes[1] = (uint8_t)(Value >> 8);
}

static void NtfsWrite32(uint8_t *Bytes, uint32_t Value)
{
    for (unsigned I = 0; I < 4; ++I)
        Bytes[I] = (uint8_t)(Value >> (I * 8));
}

static void NtfsWrite64(uint8_t *Bytes, uint64_t Value)
{
    NtfsWrite32(Bytes, (uint32_t)Value);
    NtfsWrite32(Bytes + 4, (uint32_t)(Value >> 32));
}

bool NtfsRunBootRecordSelfTests(uint32_t *Passed, uint32_t *Total)
{
    enum { TestCount = 10 };
    uint8_t Sector[512] = {0};
    NTFS_BOOT_INFORMATION Information;
    uint32_t Pass = 0;

    memcpy(Sector + 3, "NTFS    ", 8);
    NtfsWrite16(Sector + 11, 512);
    Sector[13] = 8;
    NtfsWrite64(Sector + 40, 8192);
    NtfsWrite64(Sector + 48, 4);
    NtfsWrite64(Sector + 56, 8);
    Sector[64] = (uint8_t)-10; /* 1 KiB FILE records */
    Sector[68] = (uint8_t)-10; /* 1 KiB index records */
    NtfsWrite64(Sector + 72, 0xAABBCCDDEEFF0011ull);
    NtfsWrite16(Sector + 510, 0xAA55);

    Pass += HZ_SUCCESS(NtfsDecodeBootRecord(Sector, sizeof(Sector), 8192, &Information)) &&
            Information.BytesPerSector == 512 && Information.SectorsPerCluster == 8 &&
            Information.BytesPerCluster == 4096 && Information.FileRecordBytes == 1024 &&
            Information.IndexRecordBytes == 1024 && Information.MftCluster == 4 &&
            Information.VolumeSerial == 0xAABBCCDDEEFF0011ull;

    Sector[510] = 0;
    Pass += NtfsDecodeBootRecord(Sector, sizeof(Sector), 8192, &Information) == STATUS_UNRECOGNIZED_VOLUME;
    Sector[510] = 0x55; Sector[511] = 0xAA;
    Sector[3] = 'F';
    Pass += NtfsDecodeBootRecord(Sector, sizeof(Sector), 8192, &Information) == STATUS_UNRECOGNIZED_VOLUME;
    Sector[3] = 'N';
    NtfsWrite16(Sector + 11, 768);
    Pass += NtfsDecodeBootRecord(Sector, sizeof(Sector), 8192, &Information) == STATUS_UNRECOGNIZED_VOLUME;
    NtfsWrite16(Sector + 11, 512);
    Sector[13] = 3;
    Pass += NtfsDecodeBootRecord(Sector, sizeof(Sector), 8192, &Information) == STATUS_UNRECOGNIZED_VOLUME;
    Sector[13] = 8;
    NtfsWrite64(Sector + 40, 8193);
    Pass += NtfsDecodeBootRecord(Sector, sizeof(Sector), 8192, &Information) == STATUS_UNRECOGNIZED_VOLUME;
    NtfsWrite64(Sector + 40, 8192);
    NtfsWrite64(Sector + 48, 1024);
    Pass += NtfsDecodeBootRecord(Sector, sizeof(Sector), 8192, &Information) == STATUS_UNRECOGNIZED_VOLUME;
    NtfsWrite64(Sector + 48, 4);
    Sector[64] = 0;
    Pass += NtfsDecodeBootRecord(Sector, sizeof(Sector), 8192, &Information) == STATUS_UNRECOGNIZED_VOLUME;
    Sector[64] = (uint8_t)-10;
    Sector[68] = 21;
    Pass += NtfsDecodeBootRecord(Sector, sizeof(Sector), 8192, &Information) == STATUS_UNRECOGNIZED_VOLUME;
    Sector[68] = (uint8_t)-10;
    Pass += NtfsDecodeBootRecord(Sector, sizeof(Sector), 0, &Information) == STATUS_INVALID_PARAMETER;

    if (Passed) *Passed = Pass;
    if (Total) *Total = TestCount;
    return Pass == TestCount;
}
