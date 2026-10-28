/*
 * Nexora OS - io/iotest.c
 * I/O and file system self-tests (every boot, after the process tests).
 *
 *   - C: (the live image, or the installed system volume): read an image header, list
 *     \Nexora\System; the live image (\Device\Ramdisk0, C: or not) refuses writes;
 *   - raw disk: sector round trips on a GPT partition of the Nexora OS test-scratch type only
 *     (never on other partitions), including transfers larger than one AHCI command;
 *   - FAT32 on a volume labeled HZTEST (the test disk built by scripts/mkdisk.sh): known files
 *     (8.3, long names, a 300 KB pattern), then create/write/read/truncate/extend/rename/delete
 *     in a scratch directory, directory growth, and the free-cluster count afterwards.
 * The user-mode halves of these tests ran as fstest.exe, which was removed with the beta apps.
 * Missing hardware is not a failure: without a test disk the disk and FAT32 parts are skipped.
 */
#include "disk.h"
#include "bootinfo.h"
#include "iop.h"
#include "ntfs.h"
#include "ps.h"

#define IOT_TAG HZ_POOL_TAG('I', 'o', 'T', 's')
#define IOT_PATTERN_SIZE 300000u

static uint8_t IotPattern(uint64_t i)
{
    return (uint8_t)((i * 31) ^ (i >> 7));
}

static uint32_t IotPassed, IotTotal;

static bool IotReport(bool Ok, const char *What)
{
    IotTotal++;
    IotPassed += Ok;
    DbgPrintEx(Ok ? DPFLTR_TRACE_LEVEL : DPFLTR_ERROR_LEVEL, "IoTest: %s: %s\n", What, Ok ? "OK" : "FAILED");
    return Ok;
}

static STATUS IotOpen(const char *Path, ACCESS_MASK Access, uint32_t Disposition, uint32_t Options, FILE_OBJECT **File)
{
    return IoCreateFile(Path, Access, Disposition, Options, KernelMode, File, NULL);
}

static bool IotFileEquals(const char *Path, const char *Expected)
{
    void *Data;
    uint64_t Size;
    if (!HZ_SUCCESS(IoReadWholeFile(Path, &Data, &Size, 1 << 20)))
        return false;
    bool Ok = Size == strlen(Expected) && memcmp(Data, Expected, Size) == 0;
    ExFreePool(Data);
    return Ok;
}

static bool IotDirectoryContains(const char *Path, const char *Name, uint32_t *Count)
{
    FILE_OBJECT *Dir;
    if (!HZ_SUCCESS(IotOpen(Path, FILE_LIST_DIRECTORY, FILE_OPEN, FILE_DIRECTORY_FILE, &Dir)))
        return false;
    HZ_FILE_DIRECTORY_INFORMATION *Entries = ExAllocatePoolWithTag(NonPagedPool, 8 * sizeof(*Entries), IOT_TAG);
    bool Found = false;
    uint32_t Total = 0, Returned;
    uint32_t Flags = HZ_DIRECTORY_RESTART_SCAN;
    while (Entries && HZ_SUCCESS(IoQueryDirectoryFile(Dir, Entries, 8, Flags, &Returned))) {
        Flags = 0;
        for (uint32_t i = 0; i < Returned; i++)
            Found |= Name && _stricmp(Entries[i].Name, Name) == 0;
        Total += Returned;
    }
    if (Entries)
        ExFreePoolWithTag(Entries, IOT_TAG);
    ObDereferenceObject(Dir);
    if (Count)
        *Count = Total;
    return Name ? Found : true;
}

/* ---- C: ---------------------------------------------------------------------------------------- */

static void IotTestBootVolume(void)
{
    FILE_OBJECT *File;
    uint8_t Header[2] = {0, 0};
    uint32_t Got = 0;
    bool Ok = HZ_SUCCESS(IotOpen("\\??\\C:\\Nexora\\System\\shell.exe", FILE_READ_DATA, FILE_OPEN, 0, &File));
    if (Ok) {
        uint64_t Zero = 0;
        Ok = HZ_SUCCESS(IoReadFile(File, Header, 2, &Zero, KernelMode, &Got)) && Got == 2 && Header[0] == 'M' &&
             Header[1] == 'Z';
        ObDereferenceObject(File);
    }
    IotReport(Ok, "C: reads an image header");

    uint32_t Count = 0;
    Ok = IotDirectoryContains("\\??\\C:\\Nexora\\System", "shell.exe", &Count) &&
         IotDirectoryContains("\\??\\C:\\Nexora\\System", "files.exe", NULL) && Count >= 4;
    IotReport(Ok, "C: lists \\Nexora\\System");

    Ok = IotOpen("\\Device\\Ramdisk0\\new.txt", FILE_WRITE_DATA, FILE_CREATE, 0, &File) == STATUS_MEDIA_WRITE_PROTECTED &&
         IotOpen("\\Device\\Ramdisk0\\Nexora\\System\\shell.exe", FILE_WRITE_DATA, FILE_OPEN, 0, &File) ==
             STATUS_MEDIA_WRITE_PROTECTED &&
         IotOpen("\\Device\\Ramdisk0\\Nexora\\Nope\\x", FILE_READ_DATA, FILE_OPEN, 0, &File) ==
             STATUS_OBJECT_PATH_NOT_FOUND &&
         IotOpen("\\??\\C:\\Nexora\\Nope\\x", FILE_READ_DATA, FILE_OPEN, 0, &File) == STATUS_OBJECT_PATH_NOT_FOUND;
    IotReport(Ok, "the live image is read-only, missing paths fail cleanly");
}

/* ---- Raw sectors ----------------------------------------------------------------------------------- */

static void IotTestNtfsRunRead(DEVICE_OBJECT *Storage)
{
    if (Storage->SectorSize != 512 || Storage->SectorCount < 407) {
        DbgPrint("IoTest: scratch geometry cannot run the NTFS stream-read test\n");
        return;
    }
    uint8_t ExtentA[1024], ExtentB[1024], Readback[512];
    for (uint32_t Index = 0; Index < sizeof(ExtentA); ++Index) {
        ExtentA[Index] = IotPattern(Index + 17001);
        ExtentB[Index] = IotPattern(Index + 88003);
    }
    NTFS_BOOT_INFORMATION Geometry = {0};
    Geometry.BytesPerSector = 512;
    Geometry.SectorsPerCluster = 1;
    Geometry.BytesPerCluster = 512;
    Geometry.TotalSectors = Storage->SectorCount;
    NTFS_RUN Runs[] = {
        {.Vcn = 0, .Length = 2, .Lcn = 400, .Sparse = false},
        {.Vcn = 2, .Length = 2, .Lcn = 0, .Sparse = true},
        {.Vcn = 4, .Length = 2, .Lcn = 405, .Sparse = false},
    };
    STATUS WriteA = IoWriteSectors(Storage, 400, 2, ExtentA);
    STATUS WriteB = IoWriteSectors(Storage, 405, 2, ExtentB);
    bool Ok = HZ_SUCCESS(WriteA) && HZ_SUCCESS(WriteB);
    STATUS ReadA = Ok ? NtfsReadDataStream(Storage, &Geometry, Runs, HZ_ARRAYSIZE(Runs),
                                            6 * 512, 5 * 512 + 17, 480, 80, Readback) : STATUS_UNSUCCESSFUL;
    bool ReadAContent = HZ_SUCCESS(ReadA) && memcmp(Readback, ExtentA + 480, 32) == 0;
    Ok = Ok && ReadAContent;
    for (size_t Index = 32; Ok && Index < 80; ++Index)
        Ok = Readback[Index] == ExtentA[512 + Index - 32];
    STATUS ReadSparse = Ok ? NtfsReadDataStream(Storage, &Geometry, Runs, HZ_ARRAYSIZE(Runs),
                                                 6 * 512, 5 * 512 + 17, 2 * 512, 512, Readback) : STATUS_UNSUCCESSFUL;
    Ok = Ok && HZ_SUCCESS(ReadSparse);
    for (size_t Index = 0; Ok && Index < 512; ++Index)
        Ok = Readback[Index] == 0;
    STATUS ReadInitialized = Ok ? NtfsReadDataStream(Storage, &Geometry, Runs, HZ_ARRAYSIZE(Runs),
                                                      6 * 512, 5 * 512 + 17, 5 * 512, 32, Readback) : STATUS_UNSUCCESSFUL;
    bool InitializedContent = HZ_SUCCESS(ReadInitialized) && memcmp(Readback, ExtentB + 512, 17) == 0;
    Ok = Ok && InitializedContent;
    for (size_t Index = 17; Ok && Index < 32; ++Index)
        Ok = Readback[Index] == 0;
    DbgPrint("IoTest: NTFS stream stage status writes=%08X/%08X read=%08X sparse=%08X initialized=%08X data=%u/%u/%u\n",
             WriteA, WriteB, ReadA, ReadSparse, ReadInitialized, ReadAContent, InitializedContent, Ok);
    IotReport(Ok, "NTFS run-based stream reads across sectors, sparse runs, and initialized-data boundary");
}

static void IotTestNtfsMftRead(DEVICE_OBJECT *Storage)
{
    if (Storage->SectorSize != 512 || Storage->SectorCount < 413)
        return;
    uint8_t DiskRecord[1024] = {0}, Record[1024] = {0};
    memcpy(DiskRecord, "FILE", 4);
    DiskRecord[20] = 56; /* first attribute */
    DiskRecord[24] = 56; /* bytes in use */
    DiskRecord[4] = 48; /* USA offset */
    DiskRecord[6] = 3;  /* sequence plus two sector-tail words */
    DiskRecord[48] = 0x5A; DiskRecord[49] = 0xA5;
    DiskRecord[50] = 0x34; DiskRecord[51] = 0x12;
    DiskRecord[52] = 0x78; DiskRecord[53] = 0x56;
    DiskRecord[510] = 0x5A; DiskRecord[511] = 0xA5;
    DiskRecord[1022] = 0x5A; DiskRecord[1023] = 0xA5;
    NTFS_BOOT_INFORMATION Geometry = {0};
    Geometry.BytesPerSector = 512;
    Geometry.SectorsPerCluster = 1;
    Geometry.BytesPerCluster = 512;
    Geometry.TotalSectors = Storage->SectorCount;
    Geometry.FileRecordBytes = sizeof(DiskRecord);
    NTFS_RUN Run = {.Vcn = 0, .Length = 2, .Lcn = 411, .Sparse = false};
    STATUS WriteStatus = IoWriteSectors(Storage, 411, 2, DiskRecord);
    STATUS ReadStatus = HZ_SUCCESS(WriteStatus)
                            ? NtfsReadMftRecord(Storage, &Geometry, &Run, 1, sizeof(DiskRecord),
                                                sizeof(DiskRecord), 0, Record, sizeof(Record))
                            : STATUS_UNSUCCESSFUL;
    bool Ok = HZ_SUCCESS(ReadStatus) && memcmp(Record, "FILE", 4) == 0 &&
              Record[510] == 0x34 && Record[511] == 0x12 && Record[1022] == 0x78 && Record[1023] == 0x56;
    IotReport(Ok, "NTFS MFT record read and USA repair through storage sectors");
}

static void IotTestRawDisk(void)
{
    DEVICE_OBJECT *Scratch = DiskFindPartitionByType(DiskTestScratchType);
    if (!Scratch) {
        DbgPrintEx(DPFLTR_TRACE_LEVEL, "IoTest: no test scratch partition: raw disk test skipped\n");
        return;
    }
    const uint32_t Sectors = 300; /* more than one 128-sector AHCI command */
    uint32_t Bytes = Sectors * Scratch->SectorSize;
    uint8_t *Out = ExAllocatePoolWithTag(NonPagedPool, Bytes, IOT_TAG);
    uint8_t *In = ExAllocatePoolWithTag(NonPagedPool, Bytes, IOT_TAG);
    bool Ok = Out && In && Scratch->SectorCount >= Sectors + 8;
    uint64_t Seed = KeQueryUptimeMicroseconds();
    for (uint32_t i = 0; Ok && i < Bytes; i++)
        Out[i] = IotPattern(i + Seed);
    Ok = Ok && HZ_SUCCESS(IoWriteSectors(Scratch, 5, Sectors, Out)) && HZ_SUCCESS(IoReadSectors(Scratch, 5, Sectors, In)) &&
         memcmp(In, Out, Bytes) == 0;
    /* Requests that are not sector-aligned or cross the partition end are refused. */
    IRP Irp;
    IoInitializeIrp(&Irp, IRP_MJ_READ, NULL, In, 100);
    Ok = Ok && IoCallDriver(Scratch, &Irp) == STATUS_INVALID_PARAMETER;
    Ok = Ok && IoReadSectors(Scratch, Scratch->SectorCount - 1, 2, In) == STATUS_INVALID_PARAMETER;
    IotReport(Ok, "raw sector round trip on the scratch partition (300 sectors)");
    IotTestNtfsRunRead(Scratch);
    IotTestNtfsMftRead(Scratch);
    if (Out)
        ExFreePoolWithTag(Out, IOT_TAG);
    if (In)
        ExFreePoolWithTag(In, IOT_TAG);
    ObDereferenceObject(Scratch);
}

/* ---- FAT32 ---------------------------------------------------------------------------------- */

static bool IotFindTestVolume(char *Letter)
{
    for (char c = 'D'; c <= 'Z'; c++) {
        char Root[8];
        RtlFormatString(Root, sizeof(Root), "\\??\\%c:\\", c);
        FILE_OBJECT *Dir;
        if (!HZ_SUCCESS(IotOpen(Root, FILE_LIST_DIRECTORY, FILE_OPEN, FILE_DIRECTORY_FILE, &Dir)))
            continue;
        HZ_FILE_VOLUME_INFORMATION Info;
        bool Match = HZ_SUCCESS(IoQueryInformationFile(Dir, HZ_FILE_VOLUME_INFORMATION_CLASS, &Info, sizeof(Info))) &&
                     !strcmp(Info.FileSystem, "FAT32") && !strcmp(Info.Label, "HZTEST");
        ObDereferenceObject(Dir);
        if (Match) {
            *Letter = c;
            return true;
        }
    }
    return false;
}

static uint64_t IotFreeBytes(const char *Root)
{
    FILE_OBJECT *Dir;
    HZ_FILE_VOLUME_INFORMATION Info = {0};
    if (HZ_SUCCESS(IotOpen(Root, FILE_LIST_DIRECTORY, FILE_OPEN, FILE_DIRECTORY_FILE, &Dir))) {
        IoQueryInformationFile(Dir, HZ_FILE_VOLUME_INFORMATION_CLASS, &Info, sizeof(Info));
        ObDereferenceObject(Dir);
    }
    return Info.FreeBytes;
}

static STATUS IotDelete(const char *Path)
{
    FILE_OBJECT *File;
    STATUS Status = IotOpen(Path, DELETE, FILE_OPEN, 0, &File);
    if (!HZ_SUCCESS(Status))
        return Status;
    HZ_FILE_DISPOSITION_INFORMATION Disposition = {1, 0};
    Status = IoSetInformationFile(File, HZ_FILE_DISPOSITION_INFORMATION_CLASS, &Disposition, sizeof(Disposition));
    ObDereferenceObject(File);
    return Status;
}

/* Removes what an interrupted earlier run (VirtualBox keeps the disk) may have left. */
static void IotCleanScratch(const char *Base)
{
    char Path[96];
    static const char *const Leftovers[] = {"\\Kernel Test\\Write test with a long name.bin", "\\Kernel Test\\renamed.bin"};
    for (size_t i = 0; i < HZ_ARRAYSIZE(Leftovers); i++) {
        RtlFormatString(Path, sizeof(Path), "%s%s", Base, Leftovers[i]);
        IotDelete(Path);
    }
    for (int i = 0; i < 40; i++) {
        RtlFormatString(Path, sizeof(Path), "%s\\Kernel Many\\entry number %02d.dat", Base, i);
        IotDelete(Path);
    }
    RtlFormatString(Path, sizeof(Path), "%s\\Kernel Many", Base);
    IotDelete(Path);
    RtlFormatString(Path, sizeof(Path), "%s\\Kernel Test", Base);
    IotDelete(Path);
}

static bool IotVerifyPatternFile(const char *Path)
{
    FILE_OBJECT *File;
    if (!HZ_SUCCESS(IotOpen(Path, FILE_READ_DATA, FILE_OPEN, FILE_NON_DIRECTORY_FILE, &File)))
        return false;
    uint8_t *Buffer = ExAllocatePoolWithTag(NonPagedPool, 65536, IOT_TAG);
    bool Ok = Buffer != NULL;
    uint64_t Total = 0;
    uint32_t Got = 1;
    while (Ok && Got) {
        Got = 0;
        STATUS Status = IoReadFile(File, Buffer, 65536 - 13, NULL, KernelMode, &Got); /* odd size on purpose */
        if (Status == STATUS_END_OF_FILE)
            break;
        Ok = HZ_SUCCESS(Status);
        for (uint32_t i = 0; Ok && i < Got; i++)
            Ok = Buffer[i] == IotPattern(Total + i);
        Total += Got;
    }
    if (Buffer)
        ExFreePoolWithTag(Buffer, IOT_TAG);
    ObDereferenceObject(File);
    return Ok && Total == IOT_PATTERN_SIZE;
}

static void IotTestFat32(char Letter)
{
    char Base[8], Path[128], Path2[128];
    RtlFormatString(Base, sizeof(Base), "\\??\\%c:", Letter);
    IotCleanScratch(Base);

    RtlFormatString(Path, sizeof(Path), "%s\\README.TXT", Base);
    IotReport(IotFileEquals(Path, "Nexora OS FAT32 test volume\n"), "FAT32 reads an 8.3 file (case-insensitive)");
    RtlFormatString(Path, sizeof(Path), "%s\\Docs\\A long file name for testing.txt", Base);
    IotReport(IotFileEquals(Path, "long names work\n"), "FAT32 reads a long file name in a subdirectory");
    RtlFormatString(Path, sizeof(Path), "%s\\Data\\pattern.bin", Base);
    IotReport(IotVerifyPatternFile(Path), "FAT32 reads a 300 KB file in odd-sized chunks");

    uint64_t FreeBefore = IotFreeBytes(Base);
    FILE_OBJECT *Dir = NULL, *File = NULL;
    uint32_t Result = 0;
    RtlFormatString(Path, sizeof(Path), "%s\\Kernel Test", Base);
    bool Ok = HZ_SUCCESS(IoCreateFile(Path, FILE_LIST_DIRECTORY, FILE_CREATE, FILE_DIRECTORY_FILE, KernelMode, &Dir, &Result)) &&
              Result == FILE_CREATED;
    if (Dir)
        ObDereferenceObject(Dir);
    RtlFormatString(Path, sizeof(Path), "%s\\Kernel Test\\Write test with a long name.bin", Base);
    Ok = Ok && HZ_SUCCESS(IotOpen(Path, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_CREATE, FILE_NON_DIRECTORY_FILE, &File));

    /* Write 100000 bytes in unaligned 4099-byte pieces, then read back everything. */
    const uint32_t Size = 100000;
    uint8_t *Buffer = ExAllocatePoolWithTag(NonPagedPool, Size, IOT_TAG);
    Ok = Ok && Buffer;
    for (uint32_t i = 0; Ok && i < Size; i++)
        Buffer[i] = IotPattern(i * 7);
    for (uint32_t Done = 0; Ok && Done < Size;) {
        uint32_t Chunk = HZ_MIN(4099u, Size - Done), Wrote = 0;
        Ok = HZ_SUCCESS(IoWriteFile(File, Buffer + Done, Chunk, NULL, KernelMode, &Wrote)) && Wrote == Chunk;
        Done += Chunk;
    }
    if (Ok) {
        memset(Buffer, 0, Size);
        uint64_t Zero = 0;
        uint32_t Got = 0;
        Ok = HZ_SUCCESS(IoReadFile(File, Buffer, Size, &Zero, KernelMode, &Got)) && Got == Size;
        for (uint32_t i = 0; Ok && i < Size; i++)
            Ok = Buffer[i] == IotPattern(i * 7);
    }
    IotReport(Ok, "FAT32 creates a directory and a long-named file, writes and reads 100000 bytes");

    /* Truncate to 5000, extend to 70000: the new range must read as zeros. */
    HZ_FILE_END_OF_FILE_INFORMATION Eof = {5000};
    bool Ok2 = Ok && HZ_SUCCESS(IoSetInformationFile(File, HZ_FILE_END_OF_FILE_INFORMATION_CLASS, &Eof, sizeof(Eof)));
    Eof.EndOfFile = 70000;
    Ok2 = Ok2 && HZ_SUCCESS(IoSetInformationFile(File, HZ_FILE_END_OF_FILE_INFORMATION_CLASS, &Eof, sizeof(Eof)));
    if (Ok2) {
        uint64_t Zero = 0;
        uint32_t Got = 0;
        Ok2 = HZ_SUCCESS(IoReadFile(File, Buffer, 70000, &Zero, KernelMode, &Got)) && Got == 70000;
        for (uint32_t i = 0; Ok2 && i < 70000; i++)
            Ok2 = Buffer[i] == (i < 5000 ? IotPattern(i * 7) : 0);
        HZ_FILE_BASIC_INFORMATION Info;
        Ok2 = Ok2 && HZ_SUCCESS(IoQueryInformationFile(File, HZ_FILE_BASIC_INFORMATION_CLASS, &Info, sizeof(Info))) &&
              Info.EndOfFile == 70000 && Info.AllocationSize >= 70000 && Info.LastWriteTime != 0;
    }
    IotReport(Ok2, "FAT32 truncates and extends (zero-filled), reports size and times");

    /* Rename to a lowercase 8.3 name and list the directory. */
    HZ_FILE_RENAME_INFORMATION *Rename = ExAllocatePoolZero(NonPagedPool, sizeof(*Rename), IOT_TAG);
    bool Ok3 = Ok2 && Rename;
    if (Ok3) {
        RtlStringCopy(Rename->NewName, sizeof(Rename->NewName), "renamed.bin");
        Ok3 = HZ_SUCCESS(IoSetInformationFile(File, HZ_FILE_RENAME_INFORMATION_CLASS, Rename, sizeof(*Rename)));
    }
    if (File)
        ObDereferenceObject(File);
    File = NULL;
    RtlFormatString(Path, sizeof(Path), "%s\\Kernel Test", Base);
    uint32_t Count = 0;
    Ok3 = Ok3 && IotDirectoryContains(Path, "renamed.bin", &Count) && Count == 1;
    RtlFormatString(Path2, sizeof(Path2), "%s\\Kernel Test\\Write test with a long name.bin", Base);
    Ok3 = Ok3 && IotOpen(Path2, FILE_READ_DATA, FILE_OPEN, 0, &File) == STATUS_OBJECT_NAME_NOT_FOUND;
    IotReport(Ok3, "FAT32 renames and enumerates");

    /* Delete: a non-empty directory refuses, then the file and the directory go. */
    RtlFormatString(Path, sizeof(Path), "%s\\Kernel Test", Base);
    RtlFormatString(Path2, sizeof(Path2), "%s\\Kernel Test\\renamed.bin", Base);
    bool Ok4 = Ok3 && IotDelete(Path) == STATUS_DIRECTORY_NOT_EMPTY && HZ_SUCCESS(IotDelete(Path2)) &&
               HZ_SUCCESS(IotDelete(Path)) && IotOpen(Path, FILE_READ_DATA, FILE_OPEN, 0, &File) == STATUS_OBJECT_NAME_NOT_FOUND;
    IotReport(Ok4, "FAT32 deletes files and empty directories only");

    /* Forty long-named files: the directory grows over several clusters. */
    RtlFormatString(Path, sizeof(Path), "%s\\Kernel Many", Base);
    bool Ok5 = HZ_SUCCESS(IotOpen(Path, FILE_LIST_DIRECTORY, FILE_CREATE, FILE_DIRECTORY_FILE, &Dir));
    if (Ok5)
        ObDereferenceObject(Dir);
    for (int i = 0; Ok5 && i < 40; i++) {
        RtlFormatString(Path2, sizeof(Path2), "%s\\Kernel Many\\entry number %02d.dat", Base, i);
        Ok5 = HZ_SUCCESS(IotOpen(Path2, FILE_WRITE_DATA, FILE_CREATE, 0, &File));
        if (Ok5) {
            uint32_t Wrote;
            Ok5 = HZ_SUCCESS(IoWriteFile(File, Path2, (uint32_t)strlen(Path2), NULL, KernelMode, &Wrote));
            ObDereferenceObject(File);
        }
    }
    Ok5 = Ok5 && IotDirectoryContains(Path, "entry number 39.dat", &Count) && Count == 40;
    for (int i = 0; i < 40; i++) {
        RtlFormatString(Path2, sizeof(Path2), "%s\\Kernel Many\\entry number %02d.dat", Base, i);
        Ok5 = HZ_SUCCESS(IotDelete(Path2)) && Ok5;
    }
    Ok5 = HZ_SUCCESS(IotDelete(Path)) && Ok5;
    IotReport(Ok5, "FAT32 grows a directory to 40 long-named files and removes them");

    IotReport(IotFreeBytes(Base) == FreeBefore, "FAT32 free space is back to where it started");

    /* Leave a small known tree behind for host-side checks (fsck.fat, mtools in the smoke test):
       a long-named file and a directory moved into another one (its ".." must follow). */
    char Keep[96];
    RtlFormatString(Keep, sizeof(Keep), "%s\\Kept by Nexora OS", Base);
    bool Ok6 = HZ_SUCCESS(IotOpen(Keep, FILE_LIST_DIRECTORY, FILE_OPEN_IF, FILE_DIRECTORY_FILE, &Dir));
    if (Ok6)
        ObDereferenceObject(Dir);
    RtlFormatString(Path, sizeof(Path), "%s\\Written by the kernel.txt", Keep);
    Ok6 = Ok6 && HZ_SUCCESS(IotOpen(Path, FILE_WRITE_DATA, FILE_OVERWRITE_IF, 0, &File));
    if (Ok6) {
        static const char Text[] = "Nexora OS wrote this file\n";
        uint32_t Wrote;
        Ok6 = HZ_SUCCESS(IoWriteFile(File, Text, sizeof(Text) - 1, NULL, KernelMode, &Wrote));
        ObDereferenceObject(File);
    }
    RtlFormatString(Path, sizeof(Path), "%s\\Target directory", Keep);
    Ok6 = Ok6 && HZ_SUCCESS(IotOpen(Path, FILE_LIST_DIRECTORY, FILE_OPEN_IF, FILE_DIRECTORY_FILE, &Dir));
    if (Ok6)
        ObDereferenceObject(Dir);
    RtlFormatString(Path, sizeof(Path), "%s\\Target directory\\Moved directory\\inner.txt", Keep);
    IotDelete(Path);
    RtlFormatString(Path, sizeof(Path), "%s\\Target directory\\Moved directory", Keep);
    IotDelete(Path); /* from an earlier boot */
    RtlFormatString(Path, sizeof(Path), "%s\\Directory to move", Keep);
    Ok6 = Ok6 && HZ_SUCCESS(IotOpen(Path, FILE_LIST_DIRECTORY | DELETE, FILE_CREATE, FILE_DIRECTORY_FILE, &Dir));
    if (Ok6) {
        RtlFormatString(Path2, sizeof(Path2), "%s\\Directory to move\\inner.txt", Keep);
        FILE_OBJECT *Inner;
        if (HZ_SUCCESS(IotOpen(Path2, FILE_WRITE_DATA, FILE_CREATE, 0, &Inner))) {
            uint32_t Wrote;
            IoWriteFile(Inner, "inner\n", 6, NULL, KernelMode, &Wrote);
            ObDereferenceObject(Inner);
        }
        HZ_FILE_RENAME_INFORMATION *Move = ExAllocatePoolZero(NonPagedPool, sizeof(*Move), IOT_TAG);
        Ok6 = Move != NULL;
        if (Move) {
            RtlStringCopy(Move->NewName, sizeof(Move->NewName), "\\Kept by Nexora OS\\Target directory\\Moved directory");
            Ok6 = HZ_SUCCESS(IoSetInformationFile(Dir, HZ_FILE_RENAME_INFORMATION_CLASS, Move, sizeof(*Move)));
            ExFreePoolWithTag(Move, IOT_TAG);
        }
        ObDereferenceObject(Dir);
    }
    RtlFormatString(Path, sizeof(Path), "%s\\Target directory\\Moved directory\\inner.txt", Keep);
    Ok6 = Ok6 && IotFileEquals(Path, "inner\n");
    IotReport(Ok6, "FAT32 keeps a test tree (a long name, a directory moved into another)");
    if (Buffer)
        ExFreePoolWithTag(Buffer, IOT_TAG);
    if (Rename)
        ExFreePoolWithTag(Rename, IOT_TAG);
}

/* ---- User mode ------------------------------------------------------------------------------- */

/* The user-mode half of the file tests (fstest.exe) was removed with the beta apps; the kernel
   paths above now cover the same create/write/read/truncate/rename/delete on C: and the FAT32
   test volume. */

bool IoRunSelfTests(void)
{
    POOL_STATISTICS PoolBefore, PoolAfter;
    ExQueryPoolStatistics(&PoolBefore);
    uint64_t FilesBefore = IoFileObjectType->ObjectCount, ThreadsBefore = PsThreadType->ObjectCount;
    uint64_t Start = KeQueryUptimeMicroseconds();
    IotPassed = IotTotal = 0;

    uint32_t NtfsPassed = 0, NtfsTotal = 0;
    bool NtfsOk = NtfsRunBootRecordSelfTests(&NtfsPassed, &NtfsTotal);
    IotReport(NtfsOk, "NTFS boot-record geometry validation");
    DbgPrint("IoTest: NTFS boot-record cases %u/%u passed\n", NtfsPassed, NtfsTotal);
    NtfsOk = NtfsRunRecordSelfTests(&NtfsPassed, &NtfsTotal);
    IotReport(NtfsOk, "NTFS MFT record fixup and data-run decoding");
    DbgPrint("IoTest: NTFS record cases %u/%u passed\n", NtfsPassed, NtfsTotal);
    if (BiCommandLineHasOption("hz.ntfs-probe=1")) {
        DEVICE_OBJECT *NtfsDevice = NULL;
        STATUS OpenStatus = ObReferenceObjectByName("\\Device\\Harddisk0\\Partition1",
                                                     IoDeviceObjectType, (void **)&NtfsDevice);
        NTFS_VOLUME_CONTEXT NtfsVolume = {0};
        STATUS InitStatus = HZ_SUCCESS(OpenStatus) ? NtfsInitializeVolume(NtfsDevice, &NtfsVolume)
                                                    : OpenStatus;
        static const uint16_t ProbeName[] = {'N', 'E', 'X', 'O', 'R', 'A', '.', 'T', 'X', 'T'};
        NTFS_FILE_INFORMATION FileInformation = {0};
        STATUS LookupStatus = HZ_SUCCESS(InitStatus)
                                  ? NtfsLookupDirectoryEntry(&NtfsVolume, 5, ProbeName,
                                                             HZ_ARRAYSIZE(ProbeName), &FileInformation)
                                  : InitStatus;
        static const char ExpectedData[] = "Nexora NTFS data-read probe: 3.1\n";
        char ActualData[sizeof(ExpectedData)] = {0};
        uint32_t BytesRead = 0;
        STATUS ReadStatus = HZ_SUCCESS(LookupStatus)
                                ? NtfsReadFileData(&NtfsVolume, &FileInformation, 0,
                                                   sizeof(ExpectedData) - 1, ActualData, &BytesRead)
                                : LookupStatus;
        bool ProbeOk = HZ_SUCCESS(ReadStatus) && BytesRead == sizeof(ExpectedData) - 1 &&
                       memcmp(ActualData, ExpectedData, sizeof(ExpectedData) - 1) == 0;
        DbgPrint("IoTest: NTFS root-file read statuses init=%08X lookup=%08X read=%08X bytes=%u\n",
                 InitStatus, LookupStatus, ReadStatus, BytesRead);
        NtfsReleaseVolume(&NtfsVolume);
        if (NtfsDevice)
            ObDereferenceObject(NtfsDevice);
        IotReport(ProbeOk, "NTFS root index lookup and file-data read");

        /* Exercise the same fixture through the registered NTFS mount and ordinary file API. */
        FILE_OBJECT *MountedFile = NULL;
        STATUS MountedOpenStatus = IoCreateFile("\\Device\\Harddisk0\\Partition1\\NEXORA.TXT",
                                                 FILE_READ_DATA, FILE_OPEN, FILE_NON_DIRECTORY_FILE,
                                                 KernelMode, &MountedFile, NULL);
        char MountedData[sizeof(ExpectedData)] = {0};
        uint32_t MountedBytes = 0;
        STATUS MountedReadStatus = HZ_SUCCESS(MountedOpenStatus)
                                       ? IoReadFile(MountedFile, MountedData, sizeof(ExpectedData) - 1,
                                                    NULL, KernelMode, &MountedBytes)
                                       : MountedOpenStatus;
        bool MountedOk = HZ_SUCCESS(MountedReadStatus) && MountedBytes == sizeof(ExpectedData) - 1 &&
                         memcmp(MountedData, ExpectedData, sizeof(ExpectedData) - 1) == 0;
        if (MountedFile)
            ObDereferenceObject(MountedFile);
        DbgPrint("IoTest: mounted NTFS file open=%08X read=%08X bytes=%u\n",
                 MountedOpenStatus, MountedReadStatus, MountedBytes);
        IotReport(MountedOk, "NTFS mounted file open and read through I/O manager");
    }

    IotTestBootVolume();
    IotTestRawDisk();
    char Letter = 0;
    if (IotFindTestVolume(&Letter))
        IotTestFat32(Letter);
    else
        DbgPrint("IoTest: no FAT32 volume labeled HZTEST: FAT32 tests skipped\n");

    /* Objects are deleted on the worker thread after their last reference, and (M11) an exited
       thread on another processor is reaped there a moment after its process is signaled; give
       both time. A real leak never comes back, so waiting cannot hide one. */
    for (int Wait = 0; Wait < 200; Wait++) {
        ExQueryPoolStatistics(&PoolAfter);
        if (IoFileObjectType->ObjectCount == FilesBefore && PsProcessType->ObjectCount <= 1 &&
            PsThreadType->ObjectCount <= ThreadsBefore && PoolAfter.BytesInUse == PoolBefore.BytesInUse)
            break;
        KeDelayExecutionThread(10000);
    }
    ExQueryPoolStatistics(&PoolAfter);
    char Leak[96];
    RtlFormatString(Leak, sizeof(Leak), "no file objects or pool left behind (%lld files, %+lld bytes)",
                    (long long)(IoFileObjectType->ObjectCount - FilesBefore),
                    (long long)(PoolAfter.BytesInUse - PoolBefore.BytesInUse));
    IotReport(IoFileObjectType->ObjectCount == FilesBefore && PoolAfter.BytesInUse == PoolBefore.BytesInUse, Leak);

    DbgPrintEx(IotPassed == IotTotal ? DPFLTR_INFO_LEVEL : DPFLTR_ERROR_LEVEL,
               "File system self-tests: %u/%u passed (%llu ms)\n", IotPassed, IotTotal,
               (unsigned long long)((KeQueryUptimeMicroseconds() - Start) / 1000));
    return IotPassed == IotTotal;
}
