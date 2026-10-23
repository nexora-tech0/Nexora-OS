/*
 * Nexora OS - user/tests/fstest/fstest.cpp
 * File and device services from user mode. The exit code is the number of failed checks.
 *
 *   fstest.exe X:   also exercise the FAT32 test volume X: (create, write, read, truncate, rename,
 *                   delete-on-close, large transfers)
 *   fstest.exe -    C: and devices only (no test disk attached)
 */
#include "hzsys.h"

namespace {

int g_Checks;
int g_Failures;

void Check(bool Ok, const char *What)
{
    g_Checks++;
    if (!Ok) {
        g_Failures++;
        HzPrint("fstest: FAIL %s", What);
    }
}

void CheckStatus(STATUS Got, STATUS Expected, const char *What)
{
    g_Checks++;
    if (Got != Expected) {
        g_Failures++;
        HzPrint("fstest: FAIL %s: status 0x%08X, expected 0x%08X", What, static_cast<uint32_t>(Got),
                static_cast<uint32_t>(Expected));
    }
}

STATUS Open(HANDLE *Handle, const char *Path, ACCESS_MASK Access, uint32_t Disposition, uint32_t Options,
            uint32_t *Result = nullptr)
{
    *Handle = nullptr;
    return NtCreateFile(Handle, Access, Path, Disposition, Options, Result);
}

bool ListContains(const char *Directory, const char *Name, uint32_t *Count)
{
    HANDLE Dir;
    if (!HZ_SUCCESS(Open(&Dir, Directory, FILE_LIST_DIRECTORY, FILE_OPEN, FILE_DIRECTORY_FILE)))
        return false;
    static HZ_FILE_DIRECTORY_INFORMATION Entries[4];
    bool Found = false;
    uint32_t Total = 0, Returned = 0;
    uint32_t Flags = HZ_DIRECTORY_RESTART_SCAN;
    while (HZ_SUCCESS(NtQueryDirectoryFile(Dir, Entries, sizeof(Entries), Flags, &Returned))) {
        Flags = 0;
        for (uint32_t i = 0; i < Returned; i++) {
            const char *a = Entries[i].Name;
            const char *b = Name;
            while (*a && *b && ((*a | 0x20) == (*b | 0x20)))
                a++, b++;
            Found |= *a == 0 && *b == 0;
        }
        Total += Returned;
    }
    NtClose(Dir);
    if (Count)
        *Count = Total;
    return Found;
}

STATUS Delete(const char *Path)
{
    HANDLE File;
    STATUS Status = Open(&File, Path, DELETE, FILE_OPEN, 0);
    if (!HZ_SUCCESS(Status))
        return Status;
    HZ_FILE_DISPOSITION_INFORMATION Disposition = {1, 0};
    Status = NtSetInformationFile(File, HZ_FILE_DISPOSITION_INFORMATION_CLASS, &Disposition, sizeof(Disposition));
    NtClose(File);
    return Status;
}

uint64_t FreeBytes(const char *Root)
{
    HANDLE Dir;
    HZ_FILE_VOLUME_INFORMATION Info = {};
    if (HZ_SUCCESS(Open(&Dir, Root, FILE_LIST_DIRECTORY, FILE_OPEN, FILE_DIRECTORY_FILE))) {
        NtQueryInformationFile(Dir, HZ_FILE_VOLUME_INFORMATION_CLASS, &Info, sizeof(Info));
        NtClose(Dir);
    }
    return Info.FreeBytes;
}

/* ---- C: and devices --------------------------------------------------------------------------------- */

void TestBootVolume()
{
    HANDLE File;
    uint32_t Result = 0;
    CheckStatus(Open(&File, "C:\\Nexora\\System\\hello.exe", GENERIC_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE, &Result),
                STATUS_SUCCESS, "open C:\\Nexora\\System\\hello.exe");
    Check(Result == FILE_OPENED, "create result FILE_OPENED");
    char Header[2] = {};
    uint32_t Got = 0;
    CheckStatus(NtReadFile(File, Header, 2, nullptr, &Got), STATUS_SUCCESS, "read the image header");
    Check(Got == 2 && Header[0] == 'M' && Header[1] == 'Z', "image starts with MZ");
    HZ_FILE_POSITION_INFORMATION Position = {};
    NtQueryInformationFile(File, HZ_FILE_POSITION_INFORMATION_CLASS, &Position, sizeof(Position));
    Check(Position.CurrentByteOffset == 2, "position advanced by the read");
    HZ_FILE_BASIC_INFORMATION Basic = {};
    CheckStatus(NtQueryInformationFile(File, HZ_FILE_BASIC_INFORMATION_CLASS, &Basic, sizeof(Basic)), STATUS_SUCCESS,
                "query basic information");
    Check(Basic.EndOfFile > 1024, "image size");
    uint64_t Beyond = Basic.EndOfFile;
    CheckStatus(NtReadFile(File, Header, 2, &Beyond, &Got), STATUS_END_OF_FILE, "read at the end of the file");
    CheckStatus(NtWriteFile(File, "x", 1, nullptr, nullptr), STATUS_ACCESS_DENIED, "write through a read-only handle");
    CheckStatus(NtReadFile(File, reinterpret_cast<void *>(0xFFFF800000000000ull), 2, nullptr, nullptr),
                STATUS_ACCESS_VIOLATION, "read into a kernel address");
    NtClose(File);

    /* The live image is read-only wherever it is mounted (C: on a live boot). */
    CheckStatus(Open(&File, "\\Device\\Ramdisk0\\Nexora\\System\\hello.exe", GENERIC_READ, FILE_OPEN,
                     FILE_NON_DIRECTORY_FILE),
                STATUS_SUCCESS, "open the live image through its device path");
    memset(&Basic, 0, sizeof(Basic));
    NtQueryInformationFile(File, HZ_FILE_BASIC_INFORMATION_CLASS, &Basic, sizeof(Basic));
    HZ_FILE_VOLUME_INFORMATION Volume = {};
    NtQueryInformationFile(File, HZ_FILE_VOLUME_INFORMATION_CLASS, &Volume, sizeof(Volume));
    Check((Basic.Attributes & FILE_ATTRIBUTE_READONLY) && Volume.ReadOnly && strcmp(Volume.FileSystem, "UstarFs") == 0,
          "the live image is a read-only UstarFs volume");
    NtClose(File);

    CheckStatus(Open(&File, "\\Device\\Ramdisk0\\new.txt", GENERIC_WRITE, FILE_CREATE, 0), STATUS_MEDIA_WRITE_PROTECTED,
                "create on the live image");
    CheckStatus(Open(&File, "relative.txt", GENERIC_READ, FILE_OPEN, 0), STATUS_OBJECT_PATH_SYNTAX_BAD, "relative path");
    CheckStatus(Open(&File, "C:\\Nexora\\System", GENERIC_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE),
                STATUS_FILE_IS_A_DIRECTORY, "directory opened as a file");
    CheckStatus(NtCreateFile(&File, GENERIC_READ, reinterpret_cast<const char *>(0x20), FILE_OPEN, 0, nullptr),
                STATUS_ACCESS_VIOLATION, "path at an unmapped address");
    CheckStatus(NtCreateFile(nullptr, GENERIC_READ, "C:\\Nexora", FILE_OPEN, 0, nullptr), STATUS_ACCESS_VIOLATION,
                "handle output to null");
    uint32_t Count = 0;
    Check(ListContains("C:\\Nexora\\System", "fstest.exe", &Count) && Count >= 4, "list C:\\Nexora\\System");

    HANDLE Event;
    NtCreateEvent(&Event, HZ_NOTIFICATION_EVENT, 0);
    CheckStatus(NtReadFile(Event, Header, 1, nullptr, nullptr), STATUS_OBJECT_TYPE_MISMATCH, "read from an event handle");
    NtClose(Event);
}

void TestDevices()
{
    HANDLE Keyboard;
    STATUS Status = Open(&Keyboard, "\\Device\\Keyboard0", FILE_READ_DATA, FILE_OPEN, 0);
    if (Status != STATUS_OBJECT_NAME_NOT_FOUND) {
        CheckStatus(Status, STATUS_SUCCESS, "open the keyboard device");
        HZ_KEY_EVENT Small[1];
        CheckStatus(NtReadFile(Keyboard, Small, 4, nullptr, nullptr), STATUS_BUFFER_TOO_SMALL,
                    "keyboard read smaller than an event");
        NtClose(Keyboard);
    }
    HANDLE Disk;
    Status = Open(&Disk, "\\Device\\Harddisk0\\DR0", FILE_READ_DATA, FILE_OPEN, 0);
    if (Status != STATUS_OBJECT_PATH_NOT_FOUND && Status != STATUS_OBJECT_NAME_NOT_FOUND) {
        CheckStatus(Status, STATUS_SUCCESS, "open \\Device\\Harddisk0\\DR0");
        HZ_DISK_GEOMETRY Geometry = {};
        uint32_t Returned = 0;
        CheckStatus(NtDeviceIoControlFile(Disk, IOCTL_DISK_GET_GEOMETRY, nullptr, 0, &Geometry, sizeof(Geometry), &Returned),
                    STATUS_SUCCESS, "disk geometry");
        Check(Returned == sizeof(Geometry) && Geometry.BytesPerSector == 512 && Geometry.SectorCount > 0,
              "disk geometry values");
        static uint8_t Sector[512];
        uint64_t Zero = 0;
        uint32_t Got = 0;
        CheckStatus(NtReadFile(Disk, Sector, 512, &Zero, &Got), STATUS_SUCCESS, "raw read of sector 0");
        CheckStatus(NtReadFile(Disk, Sector, 100, &Zero, &Got), STATUS_INVALID_PARAMETER, "unaligned raw read");

        /* Disk information and partition updates (M10). */
        HZ_DISK_INFORMATION Info = {};
        CheckStatus(NtDeviceIoControlFile(Disk, IOCTL_DISK_GET_INFO, nullptr, 0, &Info, sizeof(Info), &Returned),
                    STATUS_SUCCESS, "disk information");
        Check(Returned == sizeof(Info) && Info.Model[0] && Info.SectorCount == Geometry.SectorCount &&
                  Info.BytesPerSector == 512 && Info.DiskNumber == 0,
              "disk information values");
        Check(!(Info.Flags & HZ_DISK_SYSTEM) || (Info.Flags & HZ_DISK_IN_USE), "the system disk is in use");
        uint32_t Count = 0;
        CheckStatus(NtDeviceIoControlFile(Disk, IOCTL_DISK_UPDATE_PARTITIONS, nullptr, 0, &Count, sizeof(Count), nullptr),
                    STATUS_ACCESS_DENIED, "updating partitions needs write access");
        NtClose(Disk);
        if (Info.Flags & HZ_DISK_IN_USE) {
            CheckStatus(Open(&Disk, "\\Device\\Harddisk0\\DR0", FILE_READ_DATA | FILE_WRITE_DATA, FILE_OPEN, 0),
                        STATUS_SUCCESS, "open the disk for writing");
            CheckStatus(NtDeviceIoControlFile(Disk, IOCTL_DISK_UPDATE_PARTITIONS, nullptr, 0, &Count, sizeof(Count), nullptr),
                        STATUS_DEVICE_BUSY, "no partition update while a volume is mounted");
            NtClose(Disk);
        }
        if (Info.PartitionCount &&
            HZ_SUCCESS(Open(&Disk, "\\Device\\Harddisk0\\Partition1", FILE_READ_DATA, FILE_OPEN, 0))) {
            CheckStatus(NtDeviceIoControlFile(Disk, IOCTL_DISK_GET_INFO, nullptr, 0, &Info, sizeof(Info), nullptr),
                        STATUS_INVALID_DEVICE_REQUEST, "disk information is for whole disks");
            HZ_PARTITION_INFORMATION Part = {};
            CheckStatus(NtDeviceIoControlFile(Disk, IOCTL_DISK_GET_PARTITION_INFO, nullptr, 0, &Part, sizeof(Part), nullptr),
                        STATUS_SUCCESS, "partition information");
            static const uint8_t Zero16[16] = {};
            Check(Part.Number == 1 && Part.Style == HZ_PARTITION_STYLE_GPT && memcmp(Part.UniqueGuid, Zero16, 16) != 0,
                  "GPT partitions report their unique GUID");
            NtClose(Disk);
        }
    }
}

/* ---- The FAT32 test volume -------------------------------------------------------------------- */

void Path(char *Out, const char *Drive, const char *Rest)
{
    HzFormat(Out, 128, "%s%s", Drive, Rest);
}

void TestFatVolume(const char *Drive)
{
    char Dir[128], FileName[128], Other[128], Root[8];
    Path(Dir, Drive, "\\User Test");
    Path(FileName, Drive, "\\User Test\\data file.txt");
    Path(Other, Drive, "\\User Test\\moved.txt");
    HzFormat(Root, sizeof(Root), "%s\\", Drive);
    Delete(FileName);
    Delete(Other);
    Delete(Dir);
    uint64_t FreeBefore = FreeBytes(Root);

    HANDLE Handle, File;
    uint32_t Result = 0;
    CheckStatus(Open(&Handle, Dir, FILE_LIST_DIRECTORY, FILE_CREATE, FILE_DIRECTORY_FILE, &Result), STATUS_SUCCESS,
                "create a directory");
    Check(Result == FILE_CREATED, "directory result FILE_CREATED");
    NtClose(Handle);

    CheckStatus(Open(&File, FileName, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_CREATE, 0), STATUS_SUCCESS,
                "create a file");
    uint64_t At10 = 10, Zero = 0;
    uint32_t Moved = 0;
    CheckStatus(NtWriteFile(File, "hello", 5, &Zero, &Moved), STATUS_SUCCESS, "write at 0");
    CheckStatus(NtWriteFile(File, "world", 5, &At10, &Moved), STATUS_SUCCESS, "write at 10 (leaves a gap)");
    char Text[32] = {};
    CheckStatus(NtReadFile(File, Text, sizeof(Text), &Zero, &Moved), STATUS_SUCCESS, "read back");
    Check(Moved == 15 && memcmp(Text, "hello\0\0\0\0\0world", 15) == 0, "gap reads as zeros");
    HZ_FILE_END_OF_FILE_INFORMATION Eof = {3};
    CheckStatus(NtSetInformationFile(File, HZ_FILE_END_OF_FILE_INFORMATION_CLASS, &Eof, sizeof(Eof)), STATUS_SUCCESS,
                "truncate to 3 bytes");
    HZ_FILE_POSITION_INFORMATION Position = {3};
    NtSetInformationFile(File, HZ_FILE_POSITION_INFORMATION_CLASS, &Position, sizeof(Position));
    CheckStatus(NtWriteFile(File, "p!", 2, nullptr, &Moved), STATUS_SUCCESS, "write at the current position");
    memset(Text, 0, sizeof(Text));
    CheckStatus(NtReadFile(File, Text, sizeof(Text), &Zero, &Moved), STATUS_SUCCESS, "read the edited file");
    Check(Moved == 5 && memcmp(Text, "help!", 5) == 0, "contents after truncate and append");

    HZ_FILE_RENAME_INFORMATION Rename = {};
    HzFormat(Rename.NewName, sizeof(Rename.NewName), "\\User Test\\moved.txt");
    CheckStatus(NtSetInformationFile(File, HZ_FILE_RENAME_INFORMATION_CLASS, &Rename, sizeof(Rename)), STATUS_SUCCESS,
                "rename by volume path");
    NtClose(File);
    uint32_t Count = 0;
    Check(ListContains(Dir, "moved.txt", &Count) && Count == 1, "directory lists the renamed file only");

    CheckStatus(Open(&File, Other, GENERIC_READ, FILE_CREATE, 0), STATUS_OBJECT_NAME_COLLISION, "FILE_CREATE on an existing file");
    CheckStatus(Open(&File, Other, GENERIC_READ, FILE_OPEN_IF, 0, &Result), STATUS_SUCCESS, "FILE_OPEN_IF on an existing file");
    Check(Result == FILE_OPENED, "FILE_OPEN_IF result");
    NtClose(File);
    CheckStatus(Open(&File, Other, GENERIC_WRITE, FILE_OVERWRITE_IF, 0, &Result), STATUS_SUCCESS, "FILE_OVERWRITE_IF");
    Check(Result == FILE_OVERWRITTEN, "FILE_OVERWRITE_IF result");
    HZ_FILE_BASIC_INFORMATION Basic = {};
    NtQueryInformationFile(File, HZ_FILE_BASIC_INFORMATION_CLASS, &Basic, sizeof(Basic));
    Check(Basic.EndOfFile == 0, "overwritten file is empty");
    NtClose(File);
    CheckStatus(Open(&File, Other, GENERIC_READ, FILE_OPEN, FILE_DIRECTORY_FILE), STATUS_NOT_A_DIRECTORY,
                "file opened as a directory");

    char Bad[128];
    Path(Bad, Drive, "\\User Test\\bad*name");
    CheckStatus(Open(&File, Bad, GENERIC_WRITE, FILE_CREATE, 0), STATUS_OBJECT_NAME_INVALID, "invalid file name");
    Path(Bad, Drive, "\\No Such Dir\\x.txt");
    CheckStatus(Open(&File, Bad, GENERIC_WRITE, FILE_CREATE, 0), STATUS_OBJECT_PATH_NOT_FOUND, "missing parent directory");

    /* Delete-on-close */
    char Temp[128];
    Path(Temp, Drive, "\\User Test\\temporary file.tmp");
    CheckStatus(Open(&File, Temp, GENERIC_WRITE, FILE_CREATE, FILE_DELETE_ON_CLOSE), STATUS_SUCCESS,
                "create a delete-on-close file");
    NtWriteFile(File, "temp", 4, nullptr, nullptr);
    NtClose(File);
    CheckStatus(Open(&File, Temp, GENERIC_READ, FILE_OPEN, 0), STATUS_OBJECT_NAME_NOT_FOUND, "delete-on-close removed it");

    /* 1 MiB through user memory: several kernel bounce chunks. */
    const uint64_t Size = 1024 * 1024;
    void *Buffer = nullptr;
    uint64_t RegionSize = Size;
    CheckStatus(NtAllocateVirtualMemory(HZ_CURRENT_PROCESS, &Buffer, &RegionSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE),
                STATUS_SUCCESS, "allocate 1 MiB");
    if (Buffer) {
        auto *Bytes = static_cast<uint8_t *>(Buffer);
        for (uint64_t i = 0; i < Size; i++)
            Bytes[i] = static_cast<uint8_t>(i * 13 + (i >> 9));
        CheckStatus(Open(&File, Other, GENERIC_READ | GENERIC_WRITE, FILE_OPEN, 0), STATUS_SUCCESS, "reopen for 1 MiB");
        CheckStatus(NtWriteFile(File, Bytes, static_cast<uint32_t>(Size), &Zero, &Moved), STATUS_SUCCESS, "write 1 MiB");
        Check(Moved == Size, "1 MiB written");
        memset(Bytes, 0, Size);
        CheckStatus(NtReadFile(File, Bytes, static_cast<uint32_t>(Size), &Zero, &Moved), STATUS_SUCCESS, "read 1 MiB");
        bool Same = Moved == Size;
        for (uint64_t i = 0; Same && i < Size; i++)
            Same = Bytes[i] == static_cast<uint8_t>(i * 13 + (i >> 9));
        Check(Same, "1 MiB round trip");
        NtClose(File);
        NtFreeVirtualMemory(HZ_CURRENT_PROCESS, Buffer, MEM_RELEASE);
    }

    CheckStatus(Delete(Dir), STATUS_DIRECTORY_NOT_EMPTY, "delete a non-empty directory");
    CheckStatus(Delete(Other), STATUS_SUCCESS, "delete the file");
    CheckStatus(Delete(Dir), STATUS_SUCCESS, "delete the directory");
    Check(FreeBytes(Root) == FreeBefore, "free space restored");
}

}  // namespace

extern "C" int main(int argc, char **argv)
{
    TestBootVolume();
    TestDevices();
    bool Volume = argc > 1 && argv[1][0] != '-' && argv[1][1] == ':';
    if (Volume)
        TestFatVolume(argv[1]);
    uint64_t Time = 0;
    CheckStatus(NtQuerySystemTime(&Time), STATUS_SUCCESS, "system time");
    Check(Time > HZ_TIME_UNIX_EPOCH + 1600000000ull * HZ_TIME_UNITS_PER_SECOND, "system time is after 2020");
    HzPrint("fstest: %d checks, %d failed%s", g_Checks, g_Failures, Volume ? "" : " (no test volume)");
    return g_Failures;
}
