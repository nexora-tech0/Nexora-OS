/*
 * Nexora OS - user/apps/setup/installer.h
 * What Setup writes to a disk, kept apart from the window so it can be exercised on its own:
 *   - CRC-32 (IEEE, as GPT uses it) and GUIDs (random, version 4; text in the usual form, bytes
 *     in GPT on-disk order: the first three groups little-endian);
 *   - the disk layout: a GPT with 128 entries, an EFI system partition of 100 MiB at 1 MiB and a
 *     Nexora OS system partition over the rest (both 1 MiB aligned), and the GPT sectors
 *     (protective MBR, primary and backup headers, the entry array);
 *   - FAT32 parameters for a partition (the largest cluster size the size table allows that still
 *     leaves a valid FAT32 cluster count) and its boot sector, FSInfo and first FAT sector;
 *   - the boot loader configuration for the installed system.
 * The functions that touch a disk (formatting, copying) live in setup.cpp.
 */
#ifndef SETUP_INSTALLER_H
#define SETUP_INSTALLER_H

#include "hzcore.h"

namespace setup {

constexpr uint32_t kSectorSize = 512;
constexpr uint64_t kAlignment = 2048;                   /* sectors: 1 MiB */
constexpr uint64_t kEspSectors = 100ull * 2048;         /* 100 MiB */
constexpr uint64_t kMinimumSystemSectors = 128ull * 2048; /* 128 MiB */
constexpr uint32_t kGptEntries = 128, kGptEntrySize = 128;
constexpr uint32_t kGptEntrySectors = kGptEntries * kGptEntrySize / kSectorSize; /* 32 */
constexpr uint32_t kMinimumFat32Clusters = 65525 + 16;  /* a little margin over the FAT32 minimum */

uint32_t Crc32(const void *Data, size_t Length, uint32_t Crc = 0);

struct Guid {
    uint8_t Bytes[16] = {};
    bool operator==(const Guid &O) const { return memcmp(Bytes, O.Bytes, 16) == 0; }
};
extern const Guid kEfiSystemType;  /* C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
extern const Guid kBasicDataType;  /* EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 */
Guid NewGuid();
hz::String FormatGuid(const Guid &G);
bool ParseGuid(const char *Text, Guid *Out);

struct Partition {
    uint64_t FirstLba = 0, LastLba = 0;
    Guid Type, Unique;
    const char *Name = "";
    uint64_t Sectors() const { return LastLba - FirstLba + 1; }
};

struct Layout {
    uint64_t DiskSectors = 0;
    Guid DiskGuid;
    Partition Esp, System;
};

/* The minimum disk for an installation. */
uint64_t MinimumDiskSectors();
/* Plans the partitions of a disk of DiskSectors 512-byte sectors (GUIDs left zero). */
bool PlanLayout(uint64_t DiskSectors, Layout *Out);
/* Fills the GPT sectors: Mbr and the two headers are 512 bytes, Entries kGptEntrySectors sectors.
   The primary header goes to LBA 1 and its entries to LBA 2; the backup entries end just before
   the backup header, which is the last sector. */
void BuildGpt(const Layout &L, uint8_t *Mbr, uint8_t *Primary, uint8_t *Entries, uint8_t *Backup);

struct FatParameters {
    uint64_t TotalSectors = 0;
    uint32_t SectorsPerCluster = 0;
    uint32_t ReservedSectors = 32;
    uint32_t FatCount = 2;
    uint32_t FatSectors = 0;
    uint32_t Clusters = 0;
    uint32_t Serial = 0;
    char Label[12] = {};       /* 11 characters, space-padded */
    uint64_t FirstDataSector() const { return ReservedSectors + (uint64_t)FatCount * FatSectors; }
};
/* Chooses the FAT32 geometry for a partition; false if it is too small for FAT32. */
bool PlanFat32(uint64_t Sectors, const char *Label, uint32_t Serial, FatParameters *Out);
/* The boot sector (also written as the backup at sector 6), FSInfo (sector 1 and 7), and the
   first sector of each FAT (entries 0, 1 and the root directory's cluster 2). */
void BuildBootSector(const FatParameters &P, uint64_t HiddenSectors, uint8_t *Sector);
void BuildFsInfo(const FatParameters &P, uint8_t *Sector);
void BuildFirstFatSector(uint8_t *Sector);
/* The root directory's first sector: the volume label entry. */
void BuildRootSector(const FatParameters &P, uint8_t *Sector);

/* limine.conf for the installed system: boots C: from the partition with SystemGuid. */
hz::String BootConfiguration(const Guid &SystemGuid);

/* Validates a user name for first-boot setup (1..32 letters, digits, spaces, '-', '_', '.';
   not starting or ending with a space). */
bool IsValidUserName(const char *Name);

}  // namespace setup

#endif /* SETUP_INSTALLER_H */
