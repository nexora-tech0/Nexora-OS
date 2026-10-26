/*
 * Nexora OS - user/apps/setup/installer.cpp
 * GPT, FAT32 and boot configuration builders; see installer.h.
 */
#include "installer.h"
#include "hzfile.h"

namespace setup {

/* ---- CRC-32 and GUIDs --------------------------------------------------------------------------- */

uint32_t Crc32(const void *Data, size_t Length, uint32_t Crc)
{
    static uint32_t Table[256];
    static bool Ready = false;
    if (!Ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t C = i;
            for (int k = 0; k < 8; k++)
                C = (C & 1) ? 0xEDB88320u ^ (C >> 1) : C >> 1;
            Table[i] = C;
        }
        Ready = true;
    }
    const uint8_t *P = static_cast<const uint8_t *>(Data);
    Crc = ~Crc;
    for (size_t i = 0; i < Length; i++)
        Crc = Table[(Crc ^ P[i]) & 0xFF] ^ (Crc >> 8);
    return ~Crc;
}

const Guid kEfiSystemType = {{0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B}};
const Guid kBasicDataType = {{0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7}};

namespace {

uint64_t g_RandomState;

uint64_t SplitMix(uint64_t &State)
{
    uint64_t Z = (State += 0x9E3779B97F4A7C15ull);
    Z = (Z ^ (Z >> 30)) * 0xBF58476D1CE4E5B9ull;
    Z = (Z ^ (Z >> 27)) * 0x94D049BB133111EBull;
    return Z ^ (Z >> 31);
}

uint64_t ReadTsc()
{
    uint32_t Low, High;
    __asm__ volatile("rdtsc" : "=a"(Low), "=d"(High));
    return ((uint64_t)High << 32) | Low;
}

/* GPT byte order: groups 1-3 little-endian. Text index of each byte. */
const uint8_t kGuidOrder[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};

int HexValue(char C)
{
    if (C >= '0' && C <= '9')
        return C - '0';
    if (C >= 'a' && C <= 'f')
        return C - 'a' + 10;
    if (C >= 'A' && C <= 'F')
        return C - 'A' + 10;
    return -1;
}

void Put16(uint8_t *P, uint16_t V) { memcpy(P, &V, 2); }
void Put32(uint8_t *P, uint32_t V) { memcpy(P, &V, 4); }
void Put64(uint8_t *P, uint64_t V) { memcpy(P, &V, 8); }

}  // namespace

Guid NewGuid()
{
    uint64_t Time = 0, Up = 0;
    NtQuerySystemTime(&Time);
    NtQueryUptime(&Up);
    g_RandomState ^= ReadTsc() ^ (Time * 0x100000001B3ull) ^ (Up << 17) ^ (uint64_t)HzCurrentPeb()->ProcessId << 48;
    Guid G;
    uint64_t A = SplitMix(g_RandomState), B = SplitMix(g_RandomState) ^ ReadTsc();
    memcpy(G.Bytes, &A, 8);
    memcpy(G.Bytes + 8, &B, 8);
    G.Bytes[7] = (uint8_t)((G.Bytes[7] & 0x0F) | 0x40); /* version 4 (the high byte of group 3) */
    G.Bytes[8] = (uint8_t)((G.Bytes[8] & 0x3F) | 0x80); /* RFC 4122 variant */
    return G;
}

hz::String FormatGuid(const Guid &G)
{
    const uint8_t *B = G.Bytes;
    return hz::String::Format("%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", B[3], B[2], B[1],
                              B[0], B[5], B[4], B[7], B[6], B[8], B[9], B[10], B[11], B[12], B[13], B[14], B[15]);
}

bool ParseGuid(const char *Text, Guid *Out)
{
    uint8_t Bytes[16];
    uint32_t Count = 0;
    for (size_t i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (Text[i] != '-')
                return false;
            continue;
        }
        int High = HexValue(Text[i]), Low = HexValue(Text[i + 1]);
        if (High < 0 || Low < 0)
            return false;
        Bytes[Count++] = (uint8_t)(High * 16 + Low);
        i++;
    }
    if (Text[36])
        return false;
    for (uint32_t i = 0; i < 16; i++)
        Out->Bytes[kGuidOrder[i]] = Bytes[i];
    return true;
}

/* ---- GPT ---------------------------------------------------------------------------------------- */

uint64_t MinimumDiskSectors()
{
    return kAlignment + kEspSectors + kMinimumSystemSectors + kAlignment;
}

bool PlanLayout(uint64_t DiskSectors, Layout *Out)
{
    if (DiskSectors < MinimumDiskSectors())
        return false;
    *Out = Layout();
    Out->DiskSectors = DiskSectors;
    uint64_t LastUsable = DiskSectors - 1 - 1 - kGptEntrySectors; /* before the backup entries and header */
    Out->Esp.FirstLba = kAlignment;
    Out->Esp.LastLba = kAlignment + kEspSectors - 1;
    Out->Esp.Type = kEfiSystemType;
    Out->Esp.Name = "EFI system partition";
    Out->System.FirstLba = Out->Esp.LastLba + 1;
    Out->System.LastLba = (LastUsable + 1) / kAlignment * kAlignment - 1;
    Out->System.Type = kBasicDataType;
    Out->System.Name = "Nexora OS";
    return Out->System.LastLba >= Out->System.FirstLba && Out->System.Sectors() >= kMinimumSystemSectors;
}

static void BuildHeader(const Layout &L, uint32_t EntriesCrc, bool Backup, uint8_t *H)
{
    memset(H, 0, kSectorSize);
    uint64_t Last = L.DiskSectors - 1;
    memcpy(H, "EFI PART", 8);
    Put32(H + 8, 0x00010000);
    Put32(H + 12, 92);
    Put64(H + 24, Backup ? Last : 1);                       /* this header */
    Put64(H + 32, Backup ? 1 : Last);                       /* the other one */
    Put64(H + 40, 2 + kGptEntrySectors);                    /* first usable */
    Put64(H + 48, Last - 1 - kGptEntrySectors);             /* last usable */
    memcpy(H + 56, L.DiskGuid.Bytes, 16);
    Put64(H + 72, Backup ? Last - kGptEntrySectors : 2);    /* entry array */
    Put32(H + 80, kGptEntries);
    Put32(H + 84, kGptEntrySize);
    Put32(H + 88, EntriesCrc);
    Put32(H + 16, Crc32(H, 92));
}

static void BuildEntry(const Partition &P, uint8_t *E)
{
    memcpy(E, P.Type.Bytes, 16);
    memcpy(E + 16, P.Unique.Bytes, 16);
    Put64(E + 32, P.FirstLba);
    Put64(E + 40, P.LastLba);
    Put64(E + 48, 0);
    for (size_t i = 0; P.Name[i] && i < 36; i++)
        Put16(E + 56 + i * 2, (uint16_t)(uint8_t)P.Name[i]); /* ASCII names */
}

void BuildGpt(const Layout &L, uint8_t *Mbr, uint8_t *Primary, uint8_t *Entries, uint8_t *Backup)
{
    /* Protective MBR: one partition of type 0xEE over the whole disk (or as much as fits). */
    memset(Mbr, 0, kSectorSize);
    uint8_t *M = Mbr + 446;
    M[1] = 0x00;
    M[2] = 0x02; /* CHS 0/0/2 */
    M[4] = 0xEE;
    M[5] = M[6] = M[7] = 0xFF;
    Put32(M + 8, 1);
    uint64_t Covered = L.DiskSectors - 1;
    Put32(M + 12, Covered > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)Covered);
    Mbr[510] = 0x55;
    Mbr[511] = 0xAA;

    memset(Entries, 0, kGptEntrySectors * kSectorSize);
    BuildEntry(L.Esp, Entries);
    BuildEntry(L.System, Entries + kGptEntrySize);
    uint32_t EntriesCrc = Crc32(Entries, kGptEntries * kGptEntrySize);
    BuildHeader(L, EntriesCrc, false, Primary);
    BuildHeader(L, EntriesCrc, true, Backup);
}

/* ---- FAT32 ---------------------------------------------------------------------------------------- */

bool PlanFat32(uint64_t Sectors, const char *Label, uint32_t Serial, FatParameters *Out)
{
    if (Sectors > 0xFFFFFFFFull)
        Sectors = 0xFFFFFFFFull; /* FAT32's 32-bit sector count (2 TiB) */
    /* The usual cluster size for the volume size (up to 260 MiB: 512 bytes, 8 GiB: 4 KiB, 16 GiB:
       8 KiB, 32 GiB: 16 KiB, else 32 KiB), raised when that keeps enough clusters: fewer FAT
       updates when writing. Lowered when the count would be too small for FAT32. */
    uint32_t Bytes = Sectors <= 532480 ? 4096 : Sectors <= 16777216 ? 4096 : Sectors <= 33554432 ? 8192
                                                                            : Sectors <= 67108864 ? 16384 : 32768;
    for (; Bytes >= kSectorSize; Bytes /= 2) {
        uint32_t Spc = Bytes / kSectorSize;
        /* FAT size: enough entries for the clusters left after the FATs themselves. */
        uint64_t Fat = 1;
        for (int Iteration = 0; Iteration < 8; Iteration++) {
            uint64_t Data = Sectors - 32 - 2 * Fat;
            uint64_t Clusters = Data / Spc;
            uint64_t Needed = ((Clusters + 2) * 4 + kSectorSize - 1) / kSectorSize;
            if (Needed == Fat)
                break;
            Fat = Needed;
        }
        if (Sectors <= 32 + 2 * Fat)
            continue;
        uint64_t Clusters = (Sectors - 32 - 2 * Fat) / Spc;
        if (Clusters < kMinimumFat32Clusters || Clusters > 0x0FFFFFF5)
            continue;
        *Out = FatParameters();
        Out->TotalSectors = Sectors;
        Out->SectorsPerCluster = Spc;
        Out->FatSectors = (uint32_t)Fat;
        Out->Clusters = (uint32_t)Clusters;
        Out->Serial = Serial;
        size_t i = 0;
        for (; Label[i] && i < 11; i++) {
            char C = Label[i];
            Out->Label[i] = (C >= 'a' && C <= 'z') ? (char)(C - 32) : C;
        }
        for (; i < 11; i++)
            Out->Label[i] = ' ';
        Out->Label[11] = '\0';
        return true;
    }
    return false;
}

void BuildBootSector(const FatParameters &P, uint64_t HiddenSectors, uint8_t *S)
{
    memset(S, 0, kSectorSize);
    S[0] = 0xEB;
    S[1] = 0x58;
    S[2] = 0x90;
    memcpy(S + 3, "NEXORA  ", 8);
    Put16(S + 11, kSectorSize);
    S[13] = (uint8_t)P.SectorsPerCluster;
    Put16(S + 14, (uint16_t)P.ReservedSectors);
    S[16] = (uint8_t)P.FatCount;
    S[21] = 0xF8;                       /* fixed disk */
    Put16(S + 24, 63);                  /* sectors per track (legacy) */
    Put16(S + 26, 255);                 /* heads (legacy) */
    Put32(S + 28, (uint32_t)HiddenSectors);
    Put32(S + 32, (uint32_t)P.TotalSectors);
    Put32(S + 36, P.FatSectors);
    Put16(S + 40, 0);                   /* FATs mirrored */
    Put16(S + 42, 0);                   /* version 0.0 */
    Put32(S + 44, 2);                   /* root directory cluster */
    Put16(S + 48, 1);                   /* FSInfo sector */
    Put16(S + 50, 6);                   /* backup boot sector */
    S[64] = 0x80;
    S[66] = 0x29;
    Put32(S + 67, P.Serial);
    memcpy(S + 71, P.Label, 11);
    memcpy(S + 82, "FAT32   ", 8);
    /* Not bootable by BIOS: a tiny loop that halts (the firmware boots through UEFI). */
    static const uint8_t Code[] = {0xFA, 0xF4, 0xEB, 0xFD}; /* cli; hlt; jmp $-1 */
    memcpy(S + 90, Code, sizeof(Code));
    S[510] = 0x55;
    S[511] = 0xAA;
}

void BuildFsInfo(const FatParameters &P, uint8_t *S)
{
    memset(S, 0, kSectorSize);
    Put32(S, 0x41615252);
    Put32(S + 484, 0x61417272);
    Put32(S + 488, P.Clusters - 1); /* the root directory uses cluster 2 */
    Put32(S + 492, 3);
    Put32(S + 508, 0xAA550000);
}

void BuildFirstFatSector(uint8_t *S)
{
    memset(S, 0, kSectorSize);
    Put32(S, 0x0FFFFFF8);     /* media descriptor */
    Put32(S + 4, 0x0FFFFFFF); /* clean, no errors */
    Put32(S + 8, 0x0FFFFFFF); /* the root directory: one cluster */
}

void BuildRootSector(const FatParameters &P, uint8_t *S)
{
    memset(S, 0, kSectorSize);
    memcpy(S, P.Label, 11);
    S[11] = 0x08; /* volume label */
}

/* ---- Boot configuration and names ------------------------------------------------------------------ */

hz::String BootConfiguration(const Guid &SystemGuid)
{
    hz::String G = FormatGuid(SystemGuid);
    return hz::String::Format(
        "# Nexora OS boot menu, written by Setup (Limine). Paths are on this EFI system partition.\n"
        "timeout: 2\n"
        "interface_branding: Nexora OS Boot Manager\n"
        "interface_branding_colour: 6\n"
        "interface_help_hidden: yes\n"
        "term_background: 0010122A\n"
        "term_foreground: E8EAF6\n"
        "default_entry: /Nexora OS\n"
        "\n"
        "/Nexora OS\n"
        "    comment: Start Nexora OS from this disk.\n"
        "    protocol: limine\n"
        "    path: boot():/boot/hzkrnl.elf\n"
        "    cmdline: hz.system=%s\n"
        "    module_path: boot():/boot/ramdisk.tar\n"
        "    module_string: ramdisk\n"
        "    module_path: boot():/boot/hzkrnl.sym\n"
        "    module_string: symbols\n"
        "\n"
        "/Nexora OS (system image, read-only)\n"
        "    comment: Start the read-only system image kept on this disk, for repairs.\n"
        "    protocol: limine\n"
        "    path: boot():/boot/hzkrnl.elf\n"
        "    module_path: boot():/boot/ramdisk.tar\n"
        "    module_string: ramdisk\n"
        "    module_path: boot():/boot/hzkrnl.sym\n"
        "    module_string: symbols\n",
        G.CStr());
}

bool IsValidUserName(const char *Name)
{
    return hz::fs::IsValidUserName(Name); /* shared with Settings (M11) */
}

}  // namespace setup
