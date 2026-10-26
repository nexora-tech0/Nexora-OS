/*
 * Nexora OS - user/apps/setup/setup.cpp
 * Setup, in two modes:
 *   - "setup.exe" (the live system, Start's "Install Nexora OS"): a wizard that inspects the
 *     target disk and its partitions, requires typing ERASE, revalidates the selected disk before
 *     writes, and reads back raw-sector and copied-file writes before reporting success.
 *   - "setup.exe firstboot" (started by the shell on an installed system's first boot): the user
 *     name, an optional password (M11: a salted hash, hzcrypto.h) and a wallpaper, saved to
 *     C:\Nexora\Settings\shell.ini, and C:\Users\<name>\Documents.
 * Every step is logged ("setup: ...") for the smoke tests.
 */
#include "hzapps.h"
#include "hzcrypto.h"
#include "hzfile.h"
#include "hzui.h"
#include "hzversion.h"
#include "installer.h"

using namespace hzui;

namespace {

constexpr uint32_t kMsgProgress = HZ_WM_USER + 1; /* LParam = ProgressUpdate * */
constexpr uint32_t kMsgFinished = HZ_WM_USER + 2; /* WParam = STATUS, LParam = hz::String * */
constexpr uint32_t kMaxDisks = 16;
constexpr uint32_t kChunk = 64 * 1024;

/*
 * Installation is reported as named stages, and each stage owns a band of the progress bar. The bar
 * moves inside a band as the work of that stage completes, so the label names the step that is
 * actually running instead of a percentage that means nothing to the user.
 */
constexpr uint32_t kInstallStages = 6;
constexpr uint32_t kStageBand[kInstallStages + 1] = {0, 40, 90, 140, 900, 950, 1000};

struct ProgressUpdate {
    uint32_t PerMille = 0;
    uint32_t Stage = 0; /* 1-based */
    uint32_t StageCount = 0;
    uint64_t ElapsedMs = 0;
    hz::String StageName;
    hz::String Detail; /* the file or partition being worked on right now */
};

/* ---- Disks --------------------------------------------------------------------------------------- */

struct DiskEntry {
    uint32_t Number = 0;
    HZ_DISK_INFORMATION Info = {};
    hz::Vector<HZ_PARTITION_INFORMATION> Partitions;
    bool PartitionsValid = true;
    bool Eligible = false;
    hz::String Reason; /* why it cannot be used, or what will happen */
};

hz::String DiskPath(uint32_t Number)
{
    return hz::String::Format("\\Device\\Harddisk%u\\DR0", Number);
}

hz::String PartitionPath(uint32_t Disk, uint32_t Partition)
{
    return hz::String::Format("\\Device\\Harddisk%u\\Partition%u", Disk, Partition);
}

STATUS ReadPartitions(uint32_t Number, uint32_t Count, hz::Vector<HZ_PARTITION_INFORMATION> &Out)
{
    Out.Clear();
    if (Count > 128)
        return STATUS_DEVICE_DATA_ERROR;
    for (uint32_t N = 1; N <= Count; N++) {
        HANDLE Partition;
        hz::String Path = PartitionPath(Number, N);
        STATUS Status = NtCreateFile(&Partition, FILE_READ_DATA, Path.CStr(), FILE_OPEN, 0, nullptr);
        if (!HZ_SUCCESS(Status))
            return Status;
        HZ_PARTITION_INFORMATION Info = {};
        uint32_t Returned = 0;
        Status = NtDeviceIoControlFile(Partition, IOCTL_DISK_GET_PARTITION_INFO, nullptr, 0, &Info, sizeof(Info), &Returned);
        NtClose(Partition);
        if (!HZ_SUCCESS(Status))
            return Status;
        if (Returned != sizeof(Info) || Info.Number != N)
            return STATUS_DEVICE_DATA_ERROR;
        Out.Push(Info);
    }
    return STATUS_SUCCESS;
}

bool SamePartition(const HZ_PARTITION_INFORMATION &A, const HZ_PARTITION_INFORMATION &B)
{
    return A.StartingOffset == B.StartingOffset && A.Length == B.Length && A.Number == B.Number && A.Style == B.Style &&
           memcmp(A.TypeGuid, B.TypeGuid, sizeof(A.TypeGuid)) == 0 && memcmp(A.UniqueGuid, B.UniqueGuid, sizeof(A.UniqueGuid)) == 0 &&
           memcmp(A.Name, B.Name, sizeof(A.Name)) == 0;
}

bool SameDisk(const HZ_DISK_INFORMATION &A, const HZ_DISK_INFORMATION &B)
{
    return A.DiskNumber == B.DiskNumber && A.SectorCount == B.SectorCount && A.BytesPerSector == B.BytesPerSector &&
           A.Style == B.Style && A.PartitionCount == B.PartitionCount && memcmp(A.Model, B.Model, sizeof(A.Model)) == 0;
}

hz::Vector<DiskEntry> ListDisks()
{
    hz::Vector<DiskEntry> Out;
    for (uint32_t N = 0; N < kMaxDisks; N++) {
        HANDLE Disk;
        if (!HZ_SUCCESS(NtCreateFile(&Disk, FILE_READ_DATA, DiskPath(N).CStr(), FILE_OPEN, 0, nullptr)))
            continue;
        DiskEntry E;
        E.Number = N;
        uint32_t Returned = 0;
        STATUS Status = NtDeviceIoControlFile(Disk, IOCTL_DISK_GET_INFO, nullptr, 0, &E.Info, sizeof(E.Info), &Returned);
        NtClose(Disk);
        if (!HZ_SUCCESS(Status) || Returned != sizeof(E.Info))
            continue;
        Status = ReadPartitions(N, E.Info.PartitionCount, E.Partitions);
        E.PartitionsValid = HZ_SUCCESS(Status);
        uint64_t Bytes = E.Info.SectorCount * E.Info.BytesPerSector;
        if (!E.PartitionsValid)
            E.Reason = "existing partitions could not be inspected";
        else if (E.Info.Flags & HZ_DISK_SYSTEM)
            E.Reason = "holds the running system";
        else if (E.Info.Flags & HZ_DISK_READ_ONLY)
            E.Reason = "read-only";
        else if (E.Info.Flags & HZ_DISK_IN_USE)
            E.Reason = "in use (a volume on it has a drive letter or is open)";
        else if (E.Info.BytesPerSector != setup::kSectorSize)
            E.Reason = hz::String::Format("%u-byte sectors are not supported", E.Info.BytesPerSector);
        else if (E.Info.SectorCount < setup::MinimumDiskSectors())
            E.Reason = hz::String::Format("too small (at least %s)",
                                          hz::fs::FormatSize(setup::MinimumDiskSectors() * setup::kSectorSize).CStr());
        else {
            E.Eligible = true;
            E.Reason = E.Info.PartitionCount
                           ? hz::String::Format("%u partition(s) that will be erased", E.Info.PartitionCount)
                           : hz::String("empty");
        }
        HzPrint("setup: disk %u '%s', %s: %s", N, E.Info.Model, hz::fs::FormatSize(Bytes).CStr(), E.Reason.CStr());
        Out.Push(hz::Move(E));
    }
    return Out;
}

/* ---- The installation (worker thread) --------------------------------------------------------------- */

struct InstallJob {
    HWND Window = 0;
    uint32_t Disk = 0;
    HZ_DISK_INFORMATION ExpectedInfo = {};
    hz::Vector<HZ_PARTITION_INFORMATION> ExpectedPartitions;
    uint64_t TotalBytes = 0, DoneBytes = 0;
    uint32_t Files = 0;
    hz::String Error;
    setup::Guid SystemGuid;
    uint64_t StartUs = 0;
    uint32_t Stage = 0; /* 1-based; 0 before the first stage begins */
    hz::String StageName;

    uint64_t ElapsedMs() const
    {
        uint64_t Now = 0;
        NtQueryUptime(&Now);
        return Now > StartUs ? (Now - StartUs) / 1000 : 0;
    }
};

void Progress(InstallJob &Job, uint32_t PerMille, const hz::String &Detail)
{
    ProgressUpdate *Update = new ProgressUpdate();
    Update->PerMille = PerMille;
    Update->Stage = Job.Stage;
    Update->StageCount = kInstallStages;
    Update->ElapsedMs = Job.ElapsedMs();
    Update->StageName = Job.StageName;
    Update->Detail = Detail;
    if (!HZ_SUCCESS(NtUserPostMessage(Job.Window, kMsgProgress, 0, reinterpret_cast<uint64_t>(Update))))
        delete Update;
}

/* Begins a named stage: the label changes at once and the bar jumps to the start of the stage band. */
void BeginStage(InstallJob &Job, uint32_t Index, const char *Name)
{
    Job.Stage = Index + 1;
    Job.StageName = Name;
    Progress(Job, kStageBand[Index], Job.StageName);
}

/* The bar position inside the current stage's band for a stage that is Completed out of Total done. */
uint32_t StagePerMille(const InstallJob &Job, uint64_t Completed, uint64_t Total)
{
    uint32_t Index = Job.Stage ? Job.Stage - 1 : 0;
    if (Index >= kInstallStages)
        Index = kInstallStages - 1;
    uint32_t Start = kStageBand[Index], End = kStageBand[Index + 1];
    uint64_t Fraction = Total ? hz::Min<uint64_t>(Completed, Total) * 1000 / Total : 0;
    return Start + (uint32_t)((End - Start) * Fraction / 1000);
}

STATUS WriteSectors(HANDLE Device, uint64_t Lba, const void *Data, uint32_t Count)
{
    uint64_t Offset = Lba * setup::kSectorSize;
    uint32_t Bytes = Count * setup::kSectorSize, Written = 0;
    STATUS Status = NtWriteFile(Device, Data, Bytes, &Offset, &Written);
    if (HZ_SUCCESS(Status) && Written != Bytes)
        Status = STATUS_DISK_FULL;
    if (HZ_SUCCESS(Status)) {
        static uint8_t Verify[kChunk];
        uint64_t ReadOffset = Offset;
        uint32_t Read = 0;
        if (Bytes > sizeof(Verify))
            return STATUS_INVALID_PARAMETER;
        Status = NtReadFile(Device, Verify, Bytes, &ReadOffset, &Read);
        if (HZ_SUCCESS(Status) && (Read != Bytes || memcmp(Verify, Data, Bytes) != 0))
            Status = STATUS_DEVICE_DATA_ERROR;
    }
    return Status;
}

STATUS VerifyFileCopy(const char *SourcePath, const char *TargetPath, uint64_t Size)
{
    hz::fs::Entry TargetInfo;
    STATUS Status = hz::fs::Stat(hz::String(TargetPath), TargetInfo);
    if (!HZ_SUCCESS(Status))
        return Status;
    if (TargetInfo.Directory || TargetInfo.Size != Size)
        return STATUS_DEVICE_DATA_ERROR;
    HANDLE Source, Target;
    Status = NtCreateFile(&Source, FILE_READ_DATA, SourcePath, FILE_OPEN, 0, nullptr);
    if (!HZ_SUCCESS(Status))
        return Status;
    Status = NtCreateFile(&Target, FILE_READ_DATA, TargetPath, FILE_OPEN, 0, nullptr);
    if (!HZ_SUCCESS(Status)) {
        NtClose(Source);
        return Status;
    }
    static uint8_t SourceBuffer[kChunk], TargetBuffer[kChunk];
    uint64_t Offset = 0;
    while (Offset < Size) {
        uint32_t Bytes = (uint32_t)hz::Min<uint64_t>(Size - Offset, kChunk);
        uint32_t SourceRead = 0, TargetRead = 0;
        uint64_t SourceOffset = Offset, TargetOffset = Offset;
        Status = NtReadFile(Source, SourceBuffer, Bytes, &SourceOffset, &SourceRead);
        if (HZ_SUCCESS(Status))
            Status = NtReadFile(Target, TargetBuffer, Bytes, &TargetOffset, &TargetRead);
        if (!HZ_SUCCESS(Status))
            break;
        if (SourceRead != Bytes || TargetRead != Bytes || memcmp(SourceBuffer, TargetBuffer, Bytes) != 0) {
            Status = STATUS_DEVICE_DATA_ERROR;
            break;
        }
        Offset += Bytes;
    }
    NtClose(Target);
    NtClose(Source);
    return Status;
}

STATUS WriteZeros(HANDLE Device, uint64_t Lba, uint64_t Count)
{
    static uint8_t Zero[kChunk];
    const uint32_t PerChunk = kChunk / setup::kSectorSize;
    while (Count) {
        uint32_t N = (uint32_t)hz::Min<uint64_t>(Count, PerChunk);
        STATUS Status = WriteSectors(Device, Lba, Zero, N);
        if (!HZ_SUCCESS(Status))
            return Status;
        Lba += N;
        Count -= N;
    }
    return STATUS_SUCCESS;
}

STATUS Partition(InstallJob &Job, setup::Layout &Layout)
{
    HANDLE Disk;
    STATUS Status = NtCreateFile(&Disk, GENERIC_READ | GENERIC_WRITE, DiskPath(Job.Disk).CStr(), FILE_OPEN, 0, nullptr);
    if (!HZ_SUCCESS(Status)) {
        Job.Error = "The disk cannot be opened for writing.";
        return Status;
    }
    HZ_DISK_INFORMATION Info = {};
    uint32_t Returned = 0;
    Status = NtDeviceIoControlFile(Disk, IOCTL_DISK_GET_INFO, nullptr, 0, &Info, sizeof(Info), &Returned);
    hz::Vector<HZ_PARTITION_INFORMATION> Partitions;
    if (HZ_SUCCESS(Status) && Returned != sizeof(Info))
        Status = STATUS_DEVICE_DATA_ERROR;
    if (HZ_SUCCESS(Status) && (!SameDisk(Info, Job.ExpectedInfo) ||
                               (Info.Flags & (HZ_DISK_SYSTEM | HZ_DISK_READ_ONLY | HZ_DISK_IN_USE)))) {
        Job.Error = "The selected disk changed or became unavailable after confirmation. No data was written; review the disk list.";
        Status = STATUS_DEVICE_BUSY;
    }
    if (HZ_SUCCESS(Status))
        Status = ReadPartitions(Job.Disk, Info.PartitionCount, Partitions);
    if (HZ_SUCCESS(Status) && Partitions.Size() != Job.ExpectedPartitions.Size()) {
        Job.Error = "The selected disk's partition layout changed after confirmation. No data was written; review the disk list.";
        Status = STATUS_DEVICE_BUSY;
    }
    for (size_t I = 0; HZ_SUCCESS(Status) && I < Partitions.Size(); I++) {
        if (!SamePartition(Partitions[I], Job.ExpectedPartitions[I])) {
            Job.Error = "The selected disk's partition layout changed after confirmation. No data was written; review the disk list.";
            Status = STATUS_DEVICE_BUSY;
        }
    }
    if (Status == STATUS_DEVICE_BUSY && Job.Error.Empty())
        Job.Error = "The selected disk's partition layout changed after confirmation. No data was written; review the disk list.";
    if (HZ_SUCCESS(Status) && !setup::PlanLayout(Info.SectorCount, &Layout))
        Status = STATUS_DISK_FULL;
    if (!HZ_SUCCESS(Status)) {
        NtClose(Disk);
        if (Job.Error.Empty())
            Job.Error = Status == STATUS_DISK_FULL ? "The disk is too small." :
                                                     hz::String::Format("The target disk could not be safely revalidated (0x%08X).",
                                                                        (uint32_t)Status);
        return Status;
    }
    Layout.DiskGuid = setup::NewGuid();
    Layout.Esp.Unique = setup::NewGuid();
    Layout.System.Unique = setup::NewGuid();
    Job.SystemGuid = Layout.System.Unique;

    static uint8_t Mbr[512], Primary[512], Backup[512], Entries[setup::kGptEntrySectors * 512];
    setup::BuildGpt(Layout, Mbr, Primary, Entries, Backup);
    uint64_t Last = Layout.DiskSectors - 1;
    /* Clear the start of the disk and the start of each partition (old file system signatures),
       then write the backup copy before the primary one. */
    Status = WriteZeros(Disk, 0, setup::kAlignment);
    if (HZ_SUCCESS(Status))
        Status = WriteZeros(Disk, Layout.System.FirstLba, 64);
    if (HZ_SUCCESS(Status))
        Status = WriteSectors(Disk, Last - setup::kGptEntrySectors, Entries, setup::kGptEntrySectors);
    if (HZ_SUCCESS(Status))
        Status = WriteSectors(Disk, Last, Backup, 1);
    if (HZ_SUCCESS(Status))
        Status = WriteSectors(Disk, 2, Entries, setup::kGptEntrySectors);
    if (HZ_SUCCESS(Status))
        Status = WriteSectors(Disk, 1, Primary, 1);
    if (HZ_SUCCESS(Status))
        Status = WriteSectors(Disk, 0, Mbr, 1);
    uint32_t Count = 0;
    if (HZ_SUCCESS(Status))
        Status = NtDeviceIoControlFile(Disk, IOCTL_DISK_UPDATE_PARTITIONS, nullptr, 0, &Count, sizeof(Count), nullptr);
    NtClose(Disk);
    if (HZ_SUCCESS(Status) && Count != 2)
        Status = STATUS_DEVICE_NOT_READY;
    if (!HZ_SUCCESS(Status)) {
        Job.Error = hz::String::Format("Writing the partition table failed (0x%08X).", (uint32_t)Status);
        return Status;
    }
    HzPrint("setup: partitioned disk %u: GPT %s, EFI system partition %s, Nexora OS %s (%s)", Job.Disk,
            setup::FormatGuid(Layout.DiskGuid).CStr(), hz::fs::FormatSize(Layout.Esp.Sectors() * 512).CStr(),
            hz::fs::FormatSize(Layout.System.Sectors() * 512).CStr(), setup::FormatGuid(Layout.System.Unique).CStr());
    return STATUS_SUCCESS;
}

STATUS Format(InstallJob &Job, uint32_t Number, const setup::Partition &P, const char *Label)
{
    setup::FatParameters F;
    uint64_t Seed = 0;
    NtQuerySystemTime(&Seed);
    if (!setup::PlanFat32(P.Sectors(), Label, (uint32_t)(Seed ^ (Seed >> 32) ^ Number), &F)) {
        Job.Error = "The partition is too small for FAT32.";
        return STATUS_DISK_FULL;
    }
    hz::String Path = hz::String::Format("\\Device\\Harddisk%u\\Partition%u", Job.Disk, Number);
    HANDLE Device;
    STATUS Status = NtCreateFile(&Device, GENERIC_READ | GENERIC_WRITE, Path.CStr(), FILE_OPEN, 0, nullptr);
    if (!HZ_SUCCESS(Status)) {
        Job.Error = hz::String::Format("%s cannot be opened (0x%08X).", Path.CStr(), (uint32_t)Status);
        return Status;
    }
    static uint8_t Boot[512], Info[512], Fat[512], Root[512];
    setup::BuildBootSector(F, P.FirstLba, Boot);
    setup::BuildFsInfo(F, Info);
    setup::BuildFirstFatSector(Fat);
    setup::BuildRootSector(F, Root);
    Status = WriteZeros(Device, 0, F.ReservedSectors);
    for (uint32_t Copy = 0; HZ_SUCCESS(Status) && Copy < F.FatCount; Copy++) {
        uint64_t First = F.ReservedSectors + (uint64_t)Copy * F.FatSectors;
        Status = WriteZeros(Device, First, F.FatSectors);
        if (HZ_SUCCESS(Status))
            Status = WriteSectors(Device, First, Fat, 1);
    }
    if (HZ_SUCCESS(Status))
        Status = WriteZeros(Device, F.FirstDataSector(), F.SectorsPerCluster);
    if (HZ_SUCCESS(Status))
        Status = WriteSectors(Device, F.FirstDataSector(), Root, 1);
    /* The boot sectors last: until then the partition holds no recognizable volume. */
    if (HZ_SUCCESS(Status))
        Status = WriteSectors(Device, 6, Boot, 1);
    if (HZ_SUCCESS(Status))
        Status = WriteSectors(Device, 7, Info, 1);
    if (HZ_SUCCESS(Status))
        Status = WriteSectors(Device, 1, Info, 1);
    if (HZ_SUCCESS(Status))
        Status = WriteSectors(Device, 0, Boot, 1);
    NtClose(Device);
    if (!HZ_SUCCESS(Status)) {
        Job.Error = hz::String::Format("Formatting %s failed (0x%08X).", Path.CStr(), (uint32_t)Status);
        return Status;
    }
    HzPrint("setup: formatted partition %u as FAT32 '%s': %u clusters of %u bytes", Number, Label, F.Clusters,
            F.SectorsPerCluster * 512);
    return STATUS_SUCCESS;
}

/* Sizes a tree (files and bytes), skipping Excluded (a path relative to the root). */
void Measure(const hz::String &Folder, uint64_t &Bytes, uint32_t &Files)
{
    hz::Vector<hz::fs::Entry> Entries;
    if (!HZ_SUCCESS(hz::fs::List(Folder, Entries)))
        return;
    for (const hz::fs::Entry &E : Entries) {
        if (E.Directory) {
            Measure(hz::fs::Join(Folder, E.Name.CStr()), Bytes, Files);
        } else {
            Bytes += E.Size;
            Files++;
        }
    }
}

/* What Install writes where, measured before anything is erased: the EFI system partition keeps the
   install image (boot loader, kernel, symbols, the system image) and the live boot image, and the
   system partition keeps C:\Nexora. A real installer knows the numbers before it touches the disk. */
void MeasurePayload(uint64_t &EspBytes, uint64_t &SystemBytes)
{
    uint32_t Files = 0;
    Measure(hz::String("\\Device\\Ramdisk1\\"), EspBytes, Files);
    HZ_DISK_GEOMETRY Live = {};
    HANDLE Handle;
    if (HZ_SUCCESS(NtCreateFile(&Handle, FILE_READ_DATA, "\\Device\\Ramdisk0", FILE_OPEN, 0, nullptr))) {
        NtDeviceIoControlFile(Handle, IOCTL_DISK_GET_GEOMETRY, nullptr, 0, &Live, sizeof(Live), nullptr);
        NtClose(Handle);
    }
    EspBytes += Live.SectorCount * Live.BytesPerSector;
    Measure(hz::String("C:\\Nexora"), SystemBytes, Files);
}

STATUS CopyTree(InstallJob &Job, const hz::String &From, const hz::String &To, const char *Skip)
{
    hz::Vector<hz::fs::Entry> Entries;
    STATUS Status = hz::fs::List(From, Entries);
    if (!HZ_SUCCESS(Status)) {
        Job.Error = hz::String::Format("%s cannot be read (0x%08X).", From.CStr(), (uint32_t)Status);
        return Status;
    }
    for (const hz::fs::Entry &E : Entries) {
        hz::String Source = hz::fs::Join(From, E.Name.CStr()), Target = hz::fs::Join(To, E.Name.CStr());
        if (Skip && hz::fs::EqualNames(Source.CStr(), Skip))
            continue;
        if (E.Directory) {
            Status = hz::fs::CreateDirectory(Target);
            if (!HZ_SUCCESS(Status) && Status != STATUS_OBJECT_NAME_COLLISION) {
                Job.Error = hz::String::Format("%s cannot be created (%s).", Target.CStr(), hz::fs::StatusText(Status));
                return Status;
            }
            Status = CopyTree(Job, Source, Target, Skip);
            if (!HZ_SUCCESS(Status))
                return Status;
            continue;
        }
        Progress(Job, StagePerMille(Job, Job.DoneBytes, Job.TotalBytes), hz::String::Format("Copying %s", E.Name.CStr()));
        Status = hz::fs::Copy(Source, Target);
        if (!HZ_SUCCESS(Status)) {
            Job.Error = hz::String::Format("%s cannot be copied (%s).", Source.CStr(), hz::fs::StatusText(Status));
            return Status;
        }
        Status = VerifyFileCopy(Source.CStr(), Target.CStr(), E.Size);
        if (!HZ_SUCCESS(Status)) {
            Job.Error = hz::String::Format("%s could not be verified after copying (%s).", Target.CStr(),
                                           hz::fs::StatusText(Status));
            return Status;
        }
        Job.DoneBytes += E.Size;
        Job.Files++;
    }
    return STATUS_SUCCESS;
}

/* Copies a raw device (the live image) into a file. */
STATUS CopyDevice(InstallJob &Job, const char *DevicePath, const hz::String &Target)
{
    HANDLE Device, File;
    STATUS Status = NtCreateFile(&Device, FILE_READ_DATA, DevicePath, FILE_OPEN, 0, nullptr);
    if (!HZ_SUCCESS(Status)) {
        Job.Error = hz::String::Format("%s cannot be opened (0x%08X).", DevicePath, (uint32_t)Status);
        return Status;
    }
    HZ_DISK_GEOMETRY Geometry = {};
    Status = NtDeviceIoControlFile(Device, IOCTL_DISK_GET_GEOMETRY, nullptr, 0, &Geometry, sizeof(Geometry), nullptr);
    if (HZ_SUCCESS(Status))
        Status = NtCreateFile(&File, GENERIC_WRITE, Target.CStr(), FILE_OVERWRITE_IF, FILE_NON_DIRECTORY_FILE, nullptr);
    if (!HZ_SUCCESS(Status)) {
        NtClose(Device);
        Job.Error = hz::String::Format("%s cannot be created (0x%08X).", Target.CStr(), (uint32_t)Status);
        return Status;
    }
    static uint8_t Buffer[kChunk];
    uint64_t Size = Geometry.SectorCount * Geometry.BytesPerSector, Offset = 0;
    while (HZ_SUCCESS(Status) && Offset < Size) {
        uint32_t Chunk = (uint32_t)hz::Min<uint64_t>(Size - Offset, kChunk), Got = 0, Written = 0;
        Status = NtReadFile(Device, Buffer, Chunk, &Offset, &Got);
        if (HZ_SUCCESS(Status))
            Status = NtWriteFile(File, Buffer, Got, &Offset, &Written);
        if (HZ_SUCCESS(Status) && (Got != Chunk || Written != Got))
            Status = STATUS_DISK_FULL;
        Offset += Chunk;
        Job.DoneBytes += Chunk;
        if ((Offset / kChunk) % 16 == 0)
            Progress(Job, StagePerMille(Job, Job.DoneBytes, Job.TotalBytes), hz::String("Copying the system image"));
    }
    NtClose(File);
    NtClose(Device);
    if (HZ_SUCCESS(Status))
        Status = VerifyFileCopy(DevicePath, Target.CStr(), Size);
    if (!HZ_SUCCESS(Status))
        Job.Error = hz::String::Format("Copying the system image failed (0x%08X).", (uint32_t)Status);
    else
        Job.Files++;
    return Status;
}

STATUS WriteText(InstallJob &Job, const hz::String &Path, const hz::String &Text)
{
    STATUS Status = hz::fs::WriteAll(Path, Text.CStr(), Text.Length());
    if (HZ_SUCCESS(Status)) {
        hz::String Readback;
        Status = hz::fs::ReadAll(Path, Readback, Text.Length() + 1);
        if (HZ_SUCCESS(Status) && Readback != Text)
            Status = STATUS_DEVICE_DATA_ERROR;
    }
    if (!HZ_SUCCESS(Status))
        Job.Error = hz::String::Format("%s cannot be written (%s).", Path.CStr(), hz::fs::StatusText(Status));
    return Status;
}

/* ---- Verification (the last stage) ------------------------------------------------------------------ */

STATUS ReadSector(HANDLE Device, uint64_t Lba, void *Buffer, uint32_t Bytes)
{
    uint64_t Offset = Lba * setup::kSectorSize;
    uint32_t Got = 0;
    STATUS Status = NtReadFile(Device, Buffer, Bytes, &Offset, &Got);
    if (HZ_SUCCESS(Status) && Got != Bytes)
        Status = STATUS_DEVICE_DATA_ERROR;
    return Status;
}

/* Reads the GPT sectors back off the disk and compares them with the bytes Setup wrote. */
STATUS VerifyPartitionTable(InstallJob &Job, const setup::Layout &Layout)
{
    HANDLE Disk;
    STATUS Status = NtCreateFile(&Disk, FILE_READ_DATA, DiskPath(Job.Disk).CStr(), FILE_OPEN, 0, nullptr);
    if (!HZ_SUCCESS(Status)) {
        Job.Error = "The disk cannot be reopened to verify its partition table.";
        return Status;
    }
    static uint8_t Mbr[setup::kSectorSize], Primary[setup::kSectorSize], Backup[setup::kSectorSize];
    static uint8_t Entries[setup::kGptEntrySectors * setup::kSectorSize];
    static uint8_t ReadBack[setup::kGptEntrySectors * setup::kSectorSize];
    setup::BuildGpt(Layout, Mbr, Primary, Entries, Backup);
    uint64_t Last = Layout.DiskSectors - 1;
    struct Check {
        uint64_t Lba;
        const uint8_t *Expected;
        uint32_t Bytes;
        const char *What;
    } Checks[] = {
        {0, Mbr, sizeof(Mbr), "protective MBR"},
        {1, Primary, sizeof(Primary), "primary GPT header"},
        {2, Entries, sizeof(Entries), "GPT entry array"},
        {Last - setup::kGptEntrySectors, Entries, sizeof(Entries), "backup GPT entry array"},
        {Last, Backup, sizeof(Backup), "backup GPT header"},
    };
    for (const Check &C : Checks) {
        Status = ReadSector(Disk, C.Lba, ReadBack, C.Bytes);
        if (!HZ_SUCCESS(Status) || memcmp(ReadBack, C.Expected, C.Bytes) != 0) {
            NtClose(Disk);
            Job.Error = hz::String::Format("The %s could not be read back as written.", C.What);
            return STATUS_DEVICE_DATA_ERROR;
        }
    }
    NtClose(Disk);
    return STATUS_SUCCESS;
}

/* Reads a freshly formatted volume back and checks its boot sector is the one Setup wrote (both
   copies, the FAT32 signature, the volume label and the boot record signature). */
STATUS VerifyVolume(InstallJob &Job, uint32_t Partition, const char *Label)
{
    hz::String Path = hz::String::Format("\\Device\\Harddisk%u\\Partition%u", Job.Disk, Partition);
    HANDLE Device;
    STATUS Status = NtCreateFile(&Device, FILE_READ_DATA, Path.CStr(), FILE_OPEN, 0, nullptr);
    if (!HZ_SUCCESS(Status)) {
        Job.Error = hz::String::Format("%s cannot be reopened to verify it (0x%08X).", Path.CStr(), (uint32_t)Status);
        return Status;
    }
    static uint8_t Boot[setup::kSectorSize], Backup[setup::kSectorSize];
    Status = ReadSector(Device, 0, Boot, sizeof(Boot));
    if (HZ_SUCCESS(Status))
        Status = ReadSector(Device, 6, Backup, sizeof(Backup));
    NtClose(Device);
    if (!HZ_SUCCESS(Status)) {
        Job.Error = hz::String::Format("%s could not be read back.", Path.CStr());
        return Status;
    }
    if (memcmp(Boot, Backup, sizeof(Boot)) != 0 || Boot[510] != 0x55 || Boot[511] != 0xAA ||
        memcmp(Boot + 82, "FAT32   ", 8) != 0) {
        Job.Error = hz::String::Format("%s does not carry the FAT32 boot sector Setup wrote.", Path.CStr());
        return STATUS_DEVICE_DATA_ERROR;
    }
    char Expected[11];
    uint32_t Length = 0;
    while (Label[Length])
        Length++;
    for (uint32_t i = 0; i < 11; i++)
        Expected[i] = i < Length ? Label[i] : ' ';
    if (memcmp(Boot + 71, Expected, 11) != 0) {
        Job.Error = hz::String::Format("%s has an unexpected volume label.", Path.CStr());
        return STATUS_DEVICE_DATA_ERROR;
    }
    return STATUS_SUCCESS;
}

/* Walks the source tree again and confirms every file Setup copied is on the target with the size it
   was copied with. File contents were already read back sector by sector as they were written. */
STATUS CompareTree(const hz::String &From, const hz::String &To, const char *Skip, uint32_t &Checked)
{
    hz::Vector<hz::fs::Entry> Entries;
    STATUS Status = hz::fs::List(From, Entries);
    if (!HZ_SUCCESS(Status))
        return Status;
    for (const hz::fs::Entry &E : Entries) {
        hz::String Source = hz::fs::Join(From, E.Name.CStr()), Target = hz::fs::Join(To, E.Name.CStr());
        if (Skip && hz::fs::EqualNames(Source.CStr(), Skip))
            continue;
        if (E.Directory) {
            Status = CompareTree(Source, Target, Skip, Checked);
            if (!HZ_SUCCESS(Status))
                return Status;
            continue;
        }
        hz::fs::Entry Info;
        Status = hz::fs::Stat(Target, Info);
        if (!HZ_SUCCESS(Status) || Info.Directory || Info.Size != E.Size)
            return STATUS_DEVICE_DATA_ERROR;
        Checked++;
    }
    return STATUS_SUCCESS;
}

/* The verification stage: the partition table, both volumes, the boot configuration and the whole
   installed tree are read back from the disk. Setup reports success only if all of them hold. */
STATUS VerifyInstall(InstallJob &Job, const setup::Layout &Layout, const hz::String &Esp, const hz::String &System,
                     const hz::String &InstallImage)
{
    Progress(Job, kStageBand[5], hz::String("Reading the partition table back"));
    STATUS Status = VerifyPartitionTable(Job, Layout);
    if (!HZ_SUCCESS(Status))
        return Status;

    Progress(Job, kStageBand[5] + 10, hz::String("Reading the formatted volumes back"));
    Status = VerifyVolume(Job, 1, "EFI");
    if (HZ_SUCCESS(Status))
        Status = VerifyVolume(Job, 2, "NEXORAOS");
    if (!HZ_SUCCESS(Status))
        return Status;

    Progress(Job, kStageBand[5] + 18, hz::String("Reading the boot configuration back"));
    hz::String Configured, Expected = setup::BootConfiguration(Job.SystemGuid);
    Status = hz::fs::ReadAll(hz::fs::Join(Esp, "boot\\limine\\limine.conf"), Configured, Expected.Length() + 1);
    if (!HZ_SUCCESS(Status) || Configured != Expected) {
        Job.Error = "The boot configuration on the EFI system partition does not match what Setup wrote.";
        return STATUS_DEVICE_DATA_ERROR;
    }

    Progress(Job, kStageBand[5] + 22, hz::String("Checking every installed file"));
    uint32_t Checked = 0;
    Status = CompareTree(InstallImage, Esp, nullptr, Checked);
    if (HZ_SUCCESS(Status))
        Status = CompareTree(hz::String("C:\\Nexora"), hz::fs::Join(System, "Nexora"), "C:\\Nexora\\Apps\\setup.app", Checked);
    if (!HZ_SUCCESS(Status)) {
        if (Job.Error.Empty())
            Job.Error = "A file is missing from the installed system or has the wrong size.";
        return Status;
    }
    HzPrint("setup: verification passed (%u files present at their copied sizes; GPT, FAT32 and the boot menu)", Checked);
    return STATUS_SUCCESS;
}

STATUS Install(InstallJob &Job)
{
    NtQueryUptime(&Job.StartUs);

    /* Before a single sector is written: does what Setup is about to copy fit? The check happens
       here rather than after the erase, so a disk that cannot hold the installation is refused while
       it is still intact. */
    setup::Layout Planned;
    uint64_t EspBytes = 0, SystemBytes = 0;
    if (setup::PlanLayout(Job.ExpectedInfo.SectorCount, &Planned)) {
        MeasurePayload(EspBytes, SystemBytes);
        if (EspBytes > Planned.Esp.Sectors() * setup::kSectorSize) {
            Job.Error = hz::String::Format("The EFI system partition (%s) cannot hold the boot files (%s).",
                                           hz::fs::FormatSize(Planned.Esp.Sectors() * setup::kSectorSize).CStr(),
                                           hz::fs::FormatSize(EspBytes).CStr());
            return STATUS_DISK_FULL;
        }
        if (SystemBytes > Planned.System.Sectors() * setup::kSectorSize) {
            Job.Error = hz::String::Format("The Nexora OS partition (%s) cannot hold the system (%s).",
                                           hz::fs::FormatSize(Planned.System.Sectors() * setup::kSectorSize).CStr(),
                                           hz::fs::FormatSize(SystemBytes).CStr());
            return STATUS_DISK_FULL;
        }
    }

    setup::Layout Layout;
    BeginStage(Job, 0, "Partitioning the disk");
    STATUS Status = Partition(Job, Layout);
    if (!HZ_SUCCESS(Status))
        return Status;
    BeginStage(Job, 1, "Formatting the EFI system partition");
    Status = Format(Job, 1, Layout.Esp, "EFI");
    if (!HZ_SUCCESS(Status))
        return Status;
    BeginStage(Job, 2, "Formatting the Nexora OS partition");
    Status = Format(Job, 2, Layout.System, "NEXORAOS");
    if (!HZ_SUCCESS(Status))
        return Status;

    hz::String Esp = hz::String::Format("\\Device\\Harddisk%u\\Partition1\\", Job.Disk);
    hz::String System = hz::String::Format("\\Device\\Harddisk%u\\Partition2\\", Job.Disk);
    hz::String InstallImage("\\Device\\Ramdisk1\\"), SystemRoot("C:\\Nexora");
    /* The measurement the pre-flight check already made is the progress total. */
    Job.TotalBytes = EspBytes + SystemBytes;

    /* The EFI system partition: boot loader, kernel, symbols, the system image; and the system
       partition: \Nexora (Setup's manifest unpinned: it is not offered as an installer there). */
    BeginStage(Job, 3, "Copying system files");
    Status = CopyTree(Job, InstallImage, Esp, nullptr);
    if (HZ_SUCCESS(Status))
        Status = CopyDevice(Job, "\\Device\\Ramdisk0", hz::fs::Join(Esp, "boot\\ramdisk.tar"));
    if (!HZ_SUCCESS(Status))
        return Status;
    hz::String Target = hz::fs::Join(System, "Nexora");
    Status = hz::fs::CreateDirectory(Target);
    if (HZ_SUCCESS(Status))
        Status = CopyTree(Job, SystemRoot, Target, "C:\\Nexora\\Apps\\setup.app");
    if (!HZ_SUCCESS(Status))
        return Status;
    HzPrint("setup: boot loader, kernel and system image written to the EFI system partition");
    HzPrint("setup: read-back verification passed (%u copied files; GPT, FAT32 and file contents)", Job.Files);
    uint64_t End = 0;
    NtQueryUptime(&End);
    HzPrint("setup: copied %u files (%s) in %llu ms", Job.Files, hz::fs::FormatSize(Job.DoneBytes).CStr(),
            (unsigned long long)((End - Job.StartUs) / 1000));

    /* The files that turn the copied bytes into a Nexora OS installation: the boot menu, the
       system-side copy of Setup's manifest, the first-boot marker and \Users. */
    BeginStage(Job, 4, "Configuring the installed system");
    Status = WriteText(Job, hz::fs::Join(Esp, "boot\\limine\\limine.conf"), setup::BootConfiguration(Job.SystemGuid));
    if (HZ_SUCCESS(Status)) {
        hz::fs::Settings Manifest;
        Manifest.Load(hz::String("C:\\Nexora\\Apps\\setup.app"));
        Manifest.Set("Name", "Nexora OS Setup");
        Manifest.Set("Description", "First-boot setup; installing needs the live image");
        Manifest.SetInt("Pinned", 0);
        Status = WriteText(Job, hz::fs::Join(Target, "Apps\\setup.app"), Manifest.Text());
    }
    if (HZ_SUCCESS(Status))
        Status = hz::fs::CreateDirectory(hz::fs::Join(Target, "Settings"));
    if (HZ_SUCCESS(Status) || Status == STATUS_OBJECT_NAME_COLLISION) {
        hz::fs::Settings Shell;
        Shell.SetInt("FirstBoot", 1);
        Shell.Set("UserName", "");
        Shell.SetInt("Wallpaper", 0);
        Shell.Set("InstalledBy", "Setup " HZ_VERSION_STRING);
        Status = WriteText(Job, hz::fs::Join(Target, "Settings\\shell.ini"), Shell.Text());
    }
    if (HZ_SUCCESS(Status)) {
        Status = hz::fs::CreateDirectory(hz::fs::Join(System, "Users"));
        if (!HZ_SUCCESS(Status))
            Job.Error = "The Users folder cannot be created.";
    }
    if (!HZ_SUCCESS(Status))
        return Status;

    /* The installation is complete only if the disk reads back as written. */
    BeginStage(Job, 5, "Verifying the installation");
    Status = VerifyInstall(Job, Layout, Esp, System, InstallImage);
    if (!HZ_SUCCESS(Status))
        return Status;

    Progress(Job, 1000, Job.StageName);
    return STATUS_SUCCESS;
}

uint32_t InstallThread(void *Parameter)
{
    InstallJob *Job = static_cast<InstallJob *>(Parameter);
    STATUS Status = Install(*Job);
    hz::String *Error = new hz::String(Job->Error);
    if (!HZ_SUCCESS(NtUserPostMessage(Job->Window, kMsgFinished, (uint64_t)(uint32_t)Status, reinterpret_cast<uint64_t>(Error))))
        delete Error;
    return 0;
}

/* ---- Wizard window ---------------------------------------------------------------------------------- */

class Wizard;

/* Windows 11 Setup paints its pages on a bloom of light over a deep, even background; this is the
   same backdrop in the current colour scheme, and it covers the whole window as the first child. */
class SetupBackdrop final : public Element {
public:
    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        bool Light = Theme::Current() == Theme::Scheme::Light;
        P.FillVerticalGradient(B, 0.0f, Light ? 0xFFF7F8FB : 0xFF191C26, Light ? 0xFFE2E6EF : 0xFF0B0D13);
        /* The bloom: wide, very soft rings of the accent colour, brightest near the wordmark. */
        float Cx = B.X + B.Width * 0.78f, Cy = B.Y + B.Height * 0.10f;
        for (int32_t i = 7; i >= 1; i--)
            P.FillCircle(Cx, Cy, 34.0f + 26.0f * i, ScaleAlpha(Theme::Accent, Light ? 0.020f : 0.030f));
    }
};

/* The Nexora mark stays prominent and legible on its accent tile. */
class LogoMark final : public Element {
public:
    LogoMark()
    {
        Width = 30;
        Height = 30;
        HitTestVisible = false;
    }

    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        P.FillRoundRect(B, 8, Theme::Accent);
        P.DrawIcon(Icon::Nexora, Rect{B.X + 5, B.Y + 5, B.Width - 10, B.Height - 10}, Theme::TextOnAccent, 2.4f);
    }
};

class WizardWindow final : public Window {
public:
    /* Borderless and large, like the Windows 11 setup experience: the page is the window. */
    WizardWindow(Wizard *Owner, const char *Title)
        : Window(Title, 900, 620, HZ_WS_NOFRAME | HZ_WS_VISIBLE), Owner_(Owner)
    {
    }
    int64_t HandleMessage(uint32_t Message, uint64_t WParam, uint64_t LParam) override;

private:
    Wizard *Owner_;
};

/* Pages stacked in one grid cell; a header, the page, and Back / primary buttons. */
class Wizard {
public:
    virtual ~Wizard() = default;
    bool Create(const char *Title);
    virtual void OnProgress(const ProgressUpdate &Update) {}
    virtual void OnFinished(STATUS Status, const hz::String &Error) {}

protected:
    StackPanel *AddPage();
    TextBlock *Paragraph(Element *Page, const char *Text, Color C = Theme::TextSecondary);
    void ShowPage(size_t Index);
    virtual void Build() = 0;
    virtual void Next() = 0;
    virtual void Back() {}
    virtual void PageShown(size_t Index) {}
    virtual void Cancel() {}
    virtual void SecondaryAction() {} /* the extra action button some last pages need */

    Window *Main_ = nullptr;
    TextBlock *Title_ = nullptr, *Step_ = nullptr;
    Grid *Pages_ = nullptr;
    hz::Vector<StackPanel *> PageList_;
    size_t Page_ = 0;
    size_t StartPage_ = 0; /* where Create() opens the wizard; a first-boot wizard resumes here */
    Button *Back_ = nullptr, *Cancel_ = nullptr, *Primary_ = nullptr, *Secondary_ = nullptr;
    hz::Vector<const char *> Titles_;
};

int64_t WizardWindow::HandleMessage(uint32_t Message, uint64_t WParam, uint64_t LParam)
{
    if (Message == kMsgProgress) {
        ProgressUpdate *Update = reinterpret_cast<ProgressUpdate *>(LParam);
        Owner_->OnProgress(*Update);
        delete Update;
        return 0;
    }
    if (Message == kMsgFinished) {
        hz::String *Text = reinterpret_cast<hz::String *>(LParam);
        Owner_->OnFinished((STATUS)(uint32_t)WParam, *Text);
        delete Text;
        return 0;
    }
    return Window::HandleMessage(Message, WParam, LParam);
}

bool Wizard::Create(const char *Title)
{
    Main_ = new WizardWindow(this, Title);
    Grid *Root = new Grid();
    Root->Rows.Push(GridLength::AutoSize()); /* the product mark and the step */
    Root->Rows.Push(GridLength::AutoSize()); /* the page title */
    Root->Rows.Push(GridLength::Stars(1));   /* the page */
    Root->Rows.Push(GridLength::AutoSize()); /* Back and the primary button */

    SetupBackdrop *Backdrop = Root->Add<SetupBackdrop>();
    Backdrop->GridRowSpan = 4;

    Grid *Masthead = Root->Add<Grid>();
    Masthead->Padding = Thickness(56, 44, 56, 0);
    Masthead->Columns.Push(GridLength::AutoSize());
    Masthead->Columns.Push(GridLength::Stars(1));
    Masthead->Columns.Push(GridLength::AutoSize());
    StackPanel *Brand = Masthead->Add<StackPanel>(Orientation::Horizontal, 10);
    Brand->VerticalAlignment = Align::Center;
    Brand->Add<LogoMark>();
    TextBlock *Product = Brand->Add<TextBlock>("Nexora OS", FontRole::BodyStrong);
    Product->VerticalAlignment = Align::Center;
    Product->Foreground = Theme::TextPrimary;
    Step_ = Masthead->Add<TextBlock>("", FontRole::Caption);
    Step_->GridColumn = 2;
    Step_->VerticalAlignment = Align::Center;
    Step_->Foreground = Theme::TextSecondary;

    Title_ = Root->Add<TextBlock>("", FontRole::Display);
    Title_->GridRow = 1;
    Title_->Margin = Thickness(56, 26, 56, 0);

    Pages_ = Root->Add<Grid>();
    Pages_->GridRow = 2;
    Pages_->Padding = Thickness(56, 18, 56, 18);

    Grid *Bar = Root->Add<Grid>();
    Bar->GridRow = 3;
    Bar->Padding = Thickness(56, 0, 56, 44);
    Bar->Columns.Push(GridLength::Stars(1));
    Bar->Columns.Push(GridLength::AutoSize());
    Cancel_ = Bar->Add<Button>("Cancel setup", ButtonStyle::Subtle);
    Cancel_->VerticalAlignment = Align::Center;
    Cancel_->SetVisible(false);
    Cancel_->OnClick = [this]() { Cancel(); };
    StackPanel *Buttons = Bar->Add<StackPanel>(Orientation::Horizontal, 12);
    Buttons->GridColumn = 1;
    Buttons->VerticalAlignment = Align::Center;
    Back_ = Buttons->Add<Button>("Back", ButtonStyle::Subtle);
    Back_->MinWidth = 110;
    Back_->OnClick = [this]() { Back(); };
    Secondary_ = Buttons->Add<Button>("", ButtonStyle::Standard);
    Secondary_->MinWidth = 110;
    Secondary_->SetVisible(false);
    Secondary_->OnClick = [this]() { SecondaryAction(); };
    Primary_ = Buttons->Add<Button>("Next", ButtonStyle::Accent);
    Primary_->MinWidth = 110;
    Primary_->OnClick = [this]() { Next(); };
    Main_->SetContent(Root);
    Main_->OnUnhandledKey = [this](KeyEvent &E) {
        if (E.Key == HZ_VK_RETURN && Primary_->IsEnabled() && Primary_->IsVisible()) {
            E.Handled = true;
            Primary_->Click();
        }
    };
    Build();
    ShowPage(StartPage_ < PageList_.Size() ? StartPage_ : 0);
    if (!Main_->Show())
        return false;
    Main_->SetFocus(Primary_);
    return true;
}

StackPanel *Wizard::AddPage()
{
    StackPanel *P = Pages_->Add<StackPanel>(Orientation::Vertical, 14);
    /* Left-aligned in a column of a readable width, as Windows Setup lays its pages out. */
    P->Width = 640;
    P->HorizontalAlignment = Align::Start;
    P->VerticalAlignment = Align::Start;
    P->SetVisible(false);
    PageList_.Push(P);
    return P;
}

TextBlock *Wizard::Paragraph(Element *Page, const char *Text, Color C)
{
    TextBlock *T = Page->Add<TextBlock>(Text, FontRole::Body);
    T->Wrap = true;
    T->Foreground = C;
    return T;
}

void Wizard::ShowPage(size_t Index)
{
    for (size_t i = 0; i < PageList_.Size(); i++)
        PageList_[i]->SetVisible(i == Index);
    Page_ = Index;
    Title_->SetText(Index < Titles_.Size() ? Titles_[Index] : "");
    Step_->SetText(hz::String::Format("Step %zu of %zu", Index + 1, PageList_.Size()).CStr());
    Main_->InvalidateLayout();
    PageShown(Index);
}

/* ---- Install wizard --------------------------------------------------------------------------------- */

class InstallWizard final : public Wizard {
public:
    void OnProgress(const ProgressUpdate &Update) override
    {
        TargetProgress_ = Update.PerMille / 10.0f;
        Stage_->SetText(Update.StageCount ? hz::String::Format("Step %u of %u  %s", Update.Stage, Update.StageCount,
                                                              Update.StageName.CStr()).CStr()
                                          : Update.StageName.CStr());
        Status_->SetText(Update.Detail.CStr());
        Elapsed_->SetText(hz::String::Format("Elapsed %llu:%02llu", (unsigned long long)(Update.ElapsedMs / 60000),
                                             (unsigned long long)((Update.ElapsedMs / 1000) % 60))
                              .CStr());
        if (!ProgressTimer_) {
            DisplayedProgress_ = TargetProgress_;
            Bar_->SetValue(DisplayedProgress_);
            Percent_->SetText(hz::String::Format("%u%%", Update.PerMille / 10).CStr());
        }
    }

    void OnFinished(STATUS Status, const hz::String &Error) override
    {
        Installing_ = false;
        if (ProgressTimer_) {
            Main_->KillTimer(ProgressTimer_);
            ProgressTimer_ = 0;
        }
        if (Activity_)
            Activity_->SetIndeterminate(false);
        Succeeded_ = HZ_SUCCESS(Status);
        if (Succeeded_) {
            TargetProgress_ = DisplayedProgress_ = 100.0f;
            Bar_->SetValue(100.0f);
            Percent_->SetText("100%");
            Stage_->SetText("Verification passed");
            Status_->SetText("Finished");
            HzPrint("setup: done; Nexora OS is installed on disk %u", Disks_[Chosen_].Number);
            DoneText_->SetText(hz::String::Format(
                                   "Nexora OS is installed on Disk %u. The partition table, both volumes, the boot "
                                   "menu and all %u copied files were read back from the disk and match what Setup "
                                   "wrote.\n\nRemove the installation media now (detach the ISO or unplug the USB "
                                   "drive), then restart. The installed system is the default boot choice and "
                                   "starts with first-boot setup, which asks for your name.",
                                   Disks_[Chosen_].Number, Job_ ? Job_->Files : 0)
                                   .CStr());
        } else {
            HzPrint("setup: failed: %s (0x%08X)", Error.CStr(), (uint32_t)Status);
            Stage_->SetText("Verification failed");
            Status_->SetText("Stopped");
            DoneText_->SetText(hz::String::Format(
                                   "Setup could not finish: %s\n\nThe disk may hold a partial installation and is "
                                   "not a working Nexora OS. Choose Try again to inspect the disks once more, or "
                                   "Close and start Setup again from the installation media.",
                                   Error.CStr())
                                   .CStr());
        }
        Titles_[4] = Succeeded_ ? "Installation complete" : "Installation failed";
        ShowPage(4);
    }

protected:
    void Build() override
    {
        Titles_.Push("Install Nexora OS");
        Titles_.Push("Choose a disk");
        Titles_.Push("Ready to install");
        Titles_.Push("Installing");
        Titles_.Push("Installation complete");

        StackPanel *Welcome = AddPage();
        Paragraph(Welcome, "Install the complete Nexora OS desktop and applications to a disk. The installation "
                           "media runs Setup only; your installed system keeps its files and settings between restarts.",
                  Theme::TextPrimary);
        Paragraph(Welcome, "The disk is erased and gets two partitions: a 100 MB EFI system partition with the "
                           "boot loader, and a FAT32 partition for Nexora OS and your files.");
        Paragraph(Welcome, hz::String::Format("You need a disk of at least %s. Nexora OS " HZ_VERSION_STRING
                                              " includes Inter and JetBrains Mono (SIL Open Font License) and the "
                                              "Limine boot loader (BSD license); see C:\\Nexora\\Licenses.",
                                              hz::fs::FormatSize(setup::MinimumDiskSectors() * 512).CStr())
                               .CStr());
        /* This build installs with one locale, so the list has one entry rather than offering a switch
           that would silently do nothing. Region and keyboard changes are a Settings task for later. */
        Grid *Locale = Welcome->Add<Grid>();
        Locale->Columns.Push(GridLength::AutoSize());
        Locale->Columns.Push(GridLength::Stars(1));
        TextBlock *LocaleLabel = Locale->Add<TextBlock>("Language and keyboard", FontRole::Body);
        LocaleLabel->VerticalAlignment = Align::Center;
        LocaleLabel->Foreground = Theme::TextPrimary;
        ComboBox *LocaleBox = Locale->Add<ComboBox>();
        LocaleBox->GridColumn = 1;
        LocaleBox->Width = 320;
        LocaleBox->HorizontalAlignment = Align::Start;
        LocaleBox->Items.Push("English (United States) - US keyboard");
        LocaleBox->SetSelectedIndex(0, false);
        Paragraph(Welcome, "No other language or keyboard layout is available in this build.");
        Button *About = Welcome->Add<Button>("About Nexora OS", ButtonStyle::Standard);
        About->Glyph = Icon::Info;
        About->HorizontalAlignment = Align::Start;
        About->OnClick = [this]() {
            MessageBox(Main_, "About Nexora OS",
                       "Nexora OS " HZ_VERSION_STRING " (" HZ_MILESTONE ")\n"
                       "An original x86-64 UEFI desktop operating system.\n\n"
                       "This media provides the installer. The complete Nexora desktop and its applications "
                       "are installed to the selected disk.",
                       nullptr, nullptr);
        };

        StackPanel *Choose = AddPage();
        Paragraph(Choose, "Select the disk to install Nexora OS on. Disks that are in use or too small cannot be "
                          "selected.");
        List_ = Choose->Add<ListView>();
        List_->Height = 220;
        List_->ItemHeight = 56;
        List_->Background = Theme::CardBackground;
        List_->CornerRadius = Theme::OverlayRadius;
        List_->PaintItem = [this](Painter &P, const Rect &R, size_t I) {
            const DiskEntry &D = Disks_[I];
            Color Ink = D.Eligible ? Theme::TextPrimary : Theme::TextDisabled;
            P.DrawIcon(Icon::Drive, Rect{R.X + 10, R.Y + 18, 20, 20}, D.Eligible ? Theme::TextSecondary : Theme::TextDisabled);
            P.TextLine(GetFont(FontRole::BodyStrong), Rect{R.X + 44, R.Y + 6, R.Width - 160, 22},
                       hz::String::Format("Disk %u: %s", D.Number, D.Info.Model[0] ? D.Info.Model : "Disk").CStr(), Ink);
            P.TextLine(GetFont(FontRole::Caption), Rect{R.X + 44, R.Y + 29, R.Width - 160, 18}, D.Reason.CStr(),
                       D.Eligible ? Theme::TextSecondary : Theme::TextDisabled);
            P.TextLine(GetFont(FontRole::Body), Rect{R.Right() - 120, R.Y, 108, R.Height},
                       hz::fs::FormatSize(D.Info.SectorCount * D.Info.BytesPerSector).CStr(), Ink, Align::End);
        };
        List_->OnSelectionChanged = [this](size_t) { UpdateButtons(); };
        List_->OnItemInvoked = [this](size_t) {
            if (Selectable())
                Next();
        };
        Button *Refresh = Choose->Add<Button>("Refresh", ButtonStyle::Standard);
        Refresh->Glyph = Icon::Refresh;
        Refresh->HorizontalAlignment = Align::Start;
        Refresh->OnClick = [this]() { Scan(); };

        StackPanel *Confirm = AddPage();
        Summary_ = Paragraph(Confirm, "", Theme::TextPrimary);
        PartitionSummary_ = Paragraph(Confirm, "");
        Partitions_ = Confirm->Add<ListView>();
        Partitions_->Height = 116;
        Partitions_->ItemHeight = 28;
        Partitions_->Background = Theme::CardBackground;
        Partitions_->CornerRadius = Theme::OverlayRadius;
        Partitions_->PaintItem = [this](Painter &P, const Rect &R, size_t I) {
            const HZ_PARTITION_INFORMATION &Part = Disks_[Chosen_].Partitions[I];
            const char *Name = Part.Name[0] ? Part.Name :
                               Part.Style == HZ_PARTITION_STYLE_GPT ? "GPT partition" :
                               Part.Style == HZ_PARTITION_STYLE_MBR ? "MBR partition" : "Partition";
            P.TextLine(GetFont(FontRole::Caption), Rect{R.X + 10, R.Y, R.Width - 130, R.Height},
                       hz::String::Format("Partition %u: %s", Part.Number, Name).CStr(), Theme::TextPrimary);
            P.TextLine(GetFont(FontRole::Caption), Rect{R.Right() - 120, R.Y, 110, R.Height},
                       hz::fs::FormatSize(Part.Length).CStr(), Theme::TextSecondary, Align::End);
        };
        Layout_ = Paragraph(Confirm, "");
        TextBlock *Warning = Paragraph(Confirm, "Everything on this disk will be erased. This cannot be undone.",
                                       Theme::Accent);
        Warning->Font = FontRole::BodyStrong;
        Paragraph(Confirm, "To authorize the erase, type ERASE exactly as shown.");
        EraseText_ = Confirm->Add<TextBox>("Type ERASE");
        EraseText_->MaxLength = 5;
        EraseText_->Width = 260;
        EraseText_->HorizontalAlignment = Align::Start;
        EraseText_->OnTextChanged = [this](const hz::String &) { UpdateButtons(); };
        EraseText_->OnSubmit = [this](const hz::String &Text) {
            if (Text == "ERASE")
                StartInstall();
        };
        Paragraph(Confirm, "Setup cannot be cancelled once the erase starts: its controls lock until the disk has "
                           "been written and read back. Cancel setup here first if you want another disk.");

        StackPanel *Copying = AddPage();
        Paragraph(Copying, "Installing Nexora OS. Keep your computer powered on while Setup prepares the disk and "
                           "copies the complete system.");
        Stage_ = Copying->Add<TextBlock>("Preparing", FontRole::BodyStrong);
        Stage_->Foreground = Theme::TextPrimary;
        Bar_ = Copying->Add<ProgressBar>(0);
        Bar_->Height = 8;
        Bar_->Margin = Thickness(0, 4, 0, 0);
        Activity_ = Copying->Add<ProgressBar>(0);
        Activity_->Height = 4;
        Activity_->Margin = Thickness(0, 2, 0, 0);
        Activity_->SetEnabled(false);
        Activity_->SetIndeterminate(true);
        Grid *Line = Copying->Add<Grid>();
        Line->Columns.Push(GridLength::Stars(1));
        Line->Columns.Push(GridLength::AutoSize());
        Status_ = Line->Add<TextBlock>("Preparing", FontRole::Body);
        Percent_ = Line->Add<TextBlock>("0%", FontRole::Body);
        Percent_->GridColumn = 1;
        Elapsed_ = Copying->Add<TextBlock>("Elapsed 0:00", FontRole::Caption);

        StackPanel *Done = AddPage();
        DoneText_ = Paragraph(Done, "", Theme::TextPrimary);

        Main_->OnCloseRequested = [this]() { return !Installing_; };
        Main_->OnClosed = []() { NtUserPostQuitMessage(0); };
    }

    void PageShown(size_t Index) override
    {
        Back_->SetVisible(Index == 1 || Index == 2);
        Cancel_->SetVisible(Index == 2);
        Primary_->SetVisible(Index != 3);
        Primary_->SetText(Index == 2 ? "Install" : Index == 4 ? (Succeeded_ ? "Restart now" : "Close") : "Next");
        /* The last page offers the second choice a finished installer has: restart or power off, and a
           way back to the disk list after a failure. No page is shown while the disk is being written. */
        Secondary_->SetVisible(Index == 4);
        Secondary_->SetText(Succeeded_ ? "Shut down" : "Try again");
        if (Index == 1)
            Scan();
        if (Index == 2) {
            const DiskEntry &D = Disks_[Chosen_];
            setup::Layout L;
            setup::PlanLayout(D.Info.SectorCount, &L);
            Summary_->SetText(hz::String::Format("Target: Disk %u, %s, %s, %u-byte sectors, %s partition table.",
                                                 D.Number, D.Info.Model[0] ? D.Info.Model : "Disk",
                                                 hz::fs::FormatSize(D.Info.SectorCount * D.Info.BytesPerSector).CStr(),
                                                 D.Info.BytesPerSector,
                                                 D.Info.Style == HZ_PARTITION_STYLE_GPT ? "GPT" :
                                                 D.Info.Style == HZ_PARTITION_STYLE_MBR ? "MBR" : "raw")
                                  .CStr());
            PartitionSummary_->SetText(D.Partitions.Empty()
                                           ? "No existing partitions detected."
                                           : hz::String::Format("Existing partitions (%u); all will be erased:",
                                                                D.Info.PartitionCount).CStr());
            Partitions_->SetItemCount(D.Partitions.Size());
            /* The same measurement Install makes before it writes anything, so this page says whether
               the installation fits instead of the copy running out of room. It goes in the layout
               paragraph rather than a line of its own: the page already fills the window. */
            uint64_t EspBytes = 0, SystemBytes = 0;
            MeasurePayload(EspBytes, SystemBytes);
            SpaceFits_ = EspBytes <= L.Esp.Sectors() * setup::kSectorSize &&
                         SystemBytes <= L.System.Sectors() * setup::kSectorSize;
            Layout_->SetText(hz::String::Format("Partitions: EFI system partition, %s (FAT32); Nexora OS, %s (FAT32). "
                                                "Setup copies about %s of boot files and %s of Nexora OS.%s",
                                                hz::fs::FormatSize(L.Esp.Sectors() * 512).CStr(),
                                                hz::fs::FormatSize(L.System.Sectors() * 512).CStr(),
                                                hz::fs::FormatSize(EspBytes).CStr(),
                                                hz::fs::FormatSize(SystemBytes).CStr(),
                                                SpaceFits_ ? "" : " This disk is too small: Setup cannot continue.")
                                 .CStr());
            Layout_->Foreground = SpaceFits_ ? Theme::TextSecondary : Theme::Accent;
            EraseText_->SetText("", false);
            Main_->SetFocus(EraseText_);
        } else if (Index == 1) {
            Main_->SetFocus(List_);
        } else if (Index == 3) {
            Stage_->SetText("Starting");
            Status_->SetText("Preparing");
            Elapsed_->SetText("Elapsed 0:00");
        } else {
            Main_->SetFocus(Primary_);
        }
        UpdateButtons();
    }

    void Next() override
    {
        switch (Page_) {
        case 0:
            ShowPage(1);
            break;
        case 1:
            if (!Selectable())
                return;
            Chosen_ = List_->SelectedIndex();
            HzPrint("setup: chose disk %u", Disks_[Chosen_].Number);
            ShowPage(2);
            break;
        case 2:
            if (EraseText_->Text() == "ERASE")
                StartInstall();
            break;
        case 4:
            if (Succeeded_) {
                HzPrint("setup: restarting into installed Nexora OS");
                NtShutdownSystem(HZ_SHUTDOWN_REBOOT);
            } else {
                Main_->Close();
            }
            break;
        default:
            break;
        }
    }

    void Back() override
    {
        if (Page_ == 1 || Page_ == 2)
            ShowPage(Page_ - 1);
    }

private:
    void Cancel() override
    {
        if (Page_ == 2) {
            EraseText_->SetText("", false);
            ShowPage(1);
        }
    }

    /* The second button on the completion page: power off after a success, or re-inspect the disks
       after a failure (the disk list is scanned again, so a changed layout is picked up). */
    void SecondaryAction() override
    {
        if (Page_ != 4)
            return;
        if (Succeeded_) {
            HzPrint("setup: shutting down after installing Nexora OS");
            NtShutdownSystem(HZ_SHUTDOWN_POWER_OFF);
        } else {
            HzPrint("setup: retrying; inspecting the disks again");
            ShowPage(1);
        }
    }

    bool Selectable() const
    {
        size_t I = List_->SelectedIndex();
        return I < Disks_.Size() && Disks_[I].Eligible;
    }

    void Scan()
    {
        Disks_ = ListDisks();
        List_->SetItemCount(Disks_.Size());
        size_t First = hz::NotFound;
        for (size_t i = 0; i < Disks_.Size() && First == hz::NotFound; i++)
            if (Disks_[i].Eligible)
                First = i;
        List_->SetSelectedIndex(First, false);
        List_->Invalidate();
        UpdateButtons();
    }

    void UpdateButtons()
    {
        bool Enabled = true;
        if (Page_ == 1)
            Enabled = Selectable();
        else if (Page_ == 2)
            Enabled = EraseText_->Text() == "ERASE" && SpaceFits_; /* a disk too small is never erased */
        Primary_->SetEnabled(Enabled);
    }

    void StartInstall()
    {
        Job_ = new InstallJob();
        Job_->Window = Main_->Handle();
        Job_->Disk = Disks_[Chosen_].Number;
        Job_->ExpectedInfo = Disks_[Chosen_].Info;
        Job_->ExpectedPartitions = Disks_[Chosen_].Partitions;
        Installing_ = true;
        ShowPage(3);
        ProgressTimer_ = Main_->SetTimer(16, [this]() {
            float Difference = TargetProgress_ - DisplayedProgress_;
            if (Difference > -0.1f && Difference < 0.1f)
                DisplayedProgress_ = TargetProgress_;
            else
                DisplayedProgress_ += Difference * 0.2f;
            Bar_->SetValue(DisplayedProgress_);
            uint32_t Percent = (uint32_t)(DisplayedProgress_ + 0.5f);
            Percent_->SetText(hz::String::Format("%u%%", Percent).CStr());
        });
        if (!ProgressTimer_)
            HzPrint("setup: smooth progress animation unavailable; continuing with measured progress");
        HzPrint("setup: installing on disk %u", Job_->Disk);
        HANDLE Thread = 0;
        if (!HZ_SUCCESS(HzCreateThread(InstallThread, Job_, &Thread, nullptr))) {
            OnFinished(STATUS_NO_MEMORY, hz::String("The installer thread cannot start."));
            return;
        }
        NtClose(Thread);
    }

    hz::Vector<DiskEntry> Disks_;
    size_t Chosen_ = 0;
    ListView *List_ = nullptr;
    ListView *Partitions_ = nullptr;
    TextBlock *Summary_ = nullptr, *PartitionSummary_ = nullptr, *Layout_ = nullptr, *Status_ = nullptr,
              *Percent_ = nullptr, *DoneText_ = nullptr, *Stage_ = nullptr, *Elapsed_ = nullptr;
    TextBox *EraseText_ = nullptr;
    ProgressBar *Bar_ = nullptr, *Activity_ = nullptr;
    InstallJob *Job_ = nullptr;
    uint64_t ProgressTimer_ = 0;
    float DisplayedProgress_ = 0, TargetProgress_ = 0;
    bool Installing_ = false, Succeeded_ = false;
    /* Whether the measurement on the Ready page says the installation fits the disk. */
    bool SpaceFits_ = true;
};

/* ---- First-boot setup --------------------------------------------------------------------------------- */

struct Swatch {
    Color Top, Middle, Bottom, Glow;
    const char *Name;
};
const Swatch kSwatches[HZ_MAX_WALLPAPER + 1] = {
    {0xFF153D2C, 0xFF285840, 0xFF102A36, 0xFF85D7A6, "Mountain falls"},
    {0xFF24455D, 0xFF356D89, 0xFF162B42, 0xFF8DD2E8, "Coastal cliffs"},
    {0xFF272638, 0xFF56516A, 0xFF242C42, 0xFFE4A68A, "City at dusk"},
    {0xFF344332, 0xFF65744A, 0xFF273841, 0xFFD7BA79, "Red fox"},
    {0xFF25334A, 0xFF56677B, 0xFF28394B, 0xFFE9C4B0, "Coastal lock screen"},
};

void RequestWallpaper(uint32_t Index)
{
    HWND Shell = 0;
    if (HZ_SUCCESS(NtUserGetShellWindow(&Shell)) && Shell)
        NtUserPostMessage(Shell, HZ_WM_SHELLREQUEST, HZ_SHELL_REQUEST_WALLPAPER, Index);
}

/* A wallpaper preview; Space or a click chooses it (Enter moves on). */
class WallpaperChoice final : public Element {
public:
    WallpaperChoice(uint32_t Index, uint32_t *Selected) : Index_(Index), Selected_(Selected)
    {
        Width = 200;
        Height = 146;
        Focusable = true;
    }
    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        const Swatch &S = kSwatches[Index_];
        Rect Image{B.X + 4, B.Y + 4, B.Width - 8, 104};
        P.PushClip(Image);
        P.FillVerticalGradient(Rect{Image.X, Image.Y, Image.Width, Image.Height * 6 / 10}, 0, S.Top, S.Middle);
        P.FillVerticalGradient(Rect{Image.X, Image.Y + Image.Height * 6 / 10, Image.Width, Image.Height - Image.Height * 6 / 10},
                               0, S.Middle, S.Bottom);
        P.FillCircle(Image.X + Image.Width / 2.0f, (float)Image.Bottom() + 16, 44, ScaleAlpha(S.Glow, 0.55f));
        P.PopClip();
        if (*Selected_ == Index_)
            P.StrokeRoundRect(Rect{B.X, B.Y, B.Width, 112}, 8, 3, Theme::Accent);
        else if (IsHovered())
            P.StrokeRoundRect(Rect{B.X, B.Y, B.Width, 112}, 8, 1, Theme::ControlStrongStroke);
        P.TextLine(GetFont(*Selected_ == Index_ ? FontRole::BodyStrong : FontRole::Body), Rect{B.X + 4, B.Y + 118, B.Width - 8, 24},
                   S.Name, Theme::TextPrimary);
    }
    void PaintOverlay(Painter &P) override
    {
        if (ShowsFocusRing())
            P.FocusRing(Rect{Bounds().X, Bounds().Y, Bounds().Width, 112}, 8);
    }
    void OnPointerEnter() override { Invalidate(); }
    void OnPointerLeave() override { Invalidate(); }
    void OnPointerDown(PointerEvent &E) override
    {
        if (E.Button == HZ_MOUSE_LEFT)
            E.Handled = true;
    }
    void OnPointerUp(PointerEvent &E) override
    {
        if (Bounds().Contains(E.Position))
            Choose();
    }
    void OnKeyDown(KeyEvent &E) override
    {
        if (E.Key == HZ_VK_SPACE) {
            E.Handled = true;
            Choose();
        }
    }
    void Choose()
    {
        *Selected_ = Index_;
        RequestWallpaper(Index_);
        if (Parent())
            Parent()->Invalidate();
    }

private:
    uint32_t Index_;
    uint32_t *Selected_;
};

/* A miniature desktop in one colour scheme; Space or a click chooses it. The preview is drawn from
   fixed colours on purpose: it must show the scheme, not the one that is currently in force. */
class LookChoice final : public Element {
public:
    hz::Function<void(bool)> OnChoose;

    LookChoice(bool Light, bool *Selected) : Light_(Light), Selected_(Selected)
    {
        Width = 240;
        Height = 158;
        Focusable = true;
    }

    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        const Color Back = Light_ ? 0xFFF2F3F7 : 0xFF1B1E27;
        const Color Ink = Light_ ? 0xFF1A1A1A : 0xFFFFFFFF;
        const Color Panel = Light_ ? 0xFFFFFFFF : 0xFF23262F;
        P.FillRoundRect(Rect{B.X + 4, B.Y + 4, B.Width - 8, 126}, 8, Back);
        /* A window with a caption stripe, a page of text, and the taskbar with its tray. */
        P.FillRoundRect(Rect{B.X + 20, B.Y + 20, B.Width - 40, 80}, 6, Panel);
        P.FillRoundRect(Rect{B.X + 20, B.Y + 20, B.Width - 40, 13}, 6, Light_ ? 0xFFEAECF1 : 0xFF2E323D);
        P.FillRoundRect(Rect{B.X + 32, B.Y + 43, 68, 7}, 3, WithAlpha(Ink, 0xB0));
        P.FillRoundRect(Rect{B.X + 32, B.Y + 56, 102, 6}, 3, WithAlpha(Ink, 0x60));
        P.FillRoundRect(Rect{B.X + 32, B.Y + 69, 82, 6}, 3, WithAlpha(Ink, 0x60));
        P.FillRoundRect(Rect{B.X + 152, B.Y + 44, 38, 20}, 5, Theme::Accent);
        P.FillRoundRect(Rect{B.X + 4, B.Y + 116, B.Width - 8, 14}, 5, Light_ ? 0xE6FFFFFF : 0xE62A2E38);
        P.FillCircle(B.X + 26.0f, B.Y + 123.0f, 4.0f, Theme::Accent);
        P.FillRoundRect(Rect{B.X + 104, B.Y + 121, 28, 4}, 2, WithAlpha(Ink, 0x70));
        P.FillRoundRect(Rect{B.X + 142, B.Y + 121, 28, 4}, 2, WithAlpha(Ink, 0x70));
        P.FillRoundRect(Rect{B.X + B.Width - 46, B.Y + 121, 28, 4}, 2, WithAlpha(Ink, 0x70));

        if (*Selected_ == Light_)
            P.StrokeRoundRect(Rect{B.X, B.Y, B.Width, 134}, 8, 3, Theme::Accent);
        else if (IsHovered())
            P.StrokeRoundRect(Rect{B.X, B.Y, B.Width, 134}, 8, 1, Theme::ControlStrongStroke);
        P.TextLine(GetFont(*Selected_ == Light_ ? FontRole::BodyStrong : FontRole::Body),
                   Rect{B.X + 4, B.Y + 138, B.Width - 8, 22}, Light_ ? "Light" : "Dark", Theme::TextPrimary);
    }

    void PaintOverlay(Painter &P) override
    {
        if (ShowsFocusRing())
            P.FocusRing(Rect{Bounds().X, Bounds().Y, Bounds().Width, 134}, 8);
    }
    void OnPointerEnter() override { Invalidate(); }
    void OnPointerLeave() override { Invalidate(); }
    void OnPointerDown(PointerEvent &E) override
    {
        if (E.Button == HZ_MOUSE_LEFT)
            E.Handled = true;
    }
    void OnPointerUp(PointerEvent &E) override
    {
        if (Bounds().Contains(E.Position))
            Choose();
    }
    void OnKeyDown(KeyEvent &E) override
    {
        if (E.Key == HZ_VK_SPACE) {
            E.Handled = true;
            Choose();
        }
    }

private:
    void Choose()
    {
        if (*Selected_ == Light_)
            return;
        *Selected_ = Light_;
        if (OnChoose)
            OnChoose(Light_);
        if (Parent())
            Parent()->Invalidate();
    }

    bool Light_;
    bool *Selected_;
};

class FirstBootWizard final : public Wizard {
protected:
    void Build() override
    {
        if (!DefaultsLoaded_) {
            HZ_DISPLAY_INFO Display;
            memset(&Display, 0, sizeof(Display));
            NtUserGetDisplayInfo(&Display);
            Wallpaper_ = Display.Wallpaper;
            /* The scheme the user last chose, so a rebuild (a scheme switch) keeps the choice. */
            hz::fs::Settings Shell;
            Shell.Load(hz::fs::SettingsPath("shell"));
            Light_ = Shell.GetInt("Theme", 0) != 0;
            /* Resume where an interrupted first boot stopped, so a power loss does not restart the
               whole experience. The page is written after every Next(). */
            int32_t Resume = Shell.GetInt("SetupPage", 0);
            StartPage_ = (size_t)(Resume > 0 ? Resume : 0);
            DefaultsLoaded_ = true;
        }
        Titles_.Push("Set up your device");
        Titles_.Push("Who is using this computer?");
        Titles_.Push("Choose a look");
        Titles_.Push("Choose a wallpaper");
        Titles_.Push("Region and time");
        Titles_.Push("Choose your apps");
        Titles_.Push("Connect");
        Titles_.Push("Review your choices");
        Titles_.Push("You are all set");

        StackPanel *Hello = AddPage();
        Paragraph(Hello, "Nexora OS is installed and starts from this computer's disk. This one-time setup "
                         "covers your account, how the desktop looks, which extra apps to keep and your "
                         "network. Nothing here needs the internet.",
                  Theme::TextPrimary);
        Paragraph(Hello, "Everything can be changed later in Settings. Use Next to continue.");
        Paragraph(Hello, "About Nexora OS: an independent 64-bit desktop system with its own visual identity. "
                         "It is local-first: no account is required and nothing is sent anywhere.",
                  Theme::TextSecondary);
        Paragraph(Hello, "Accessibility: the whole wizard is keyboard-operable, Tab moves between controls "
                         "and Enter confirms. Reduced motion and contrast options live in Settings.",
                  Theme::TextSecondary);

        StackPanel *Who = AddPage();
        Paragraph(Who, "Your name appears on the lock screen and in Start, and names your documents folder.");
        Name_ = Who->Add<TextBox>("Your name");
        Name_->Width = 320;
        Name_->HorizontalAlignment = Align::Start;
        Name_->MaxLength = 32;
        Name_->OnTextChanged = [this](const hz::String &) { UpdateButtons(); };
        Name_->OnSubmit = [this](const hz::String &) { Next(); };
        Hint_ = Paragraph(Who, "Letters, digits, spaces, '-', '_' and '.' (up to 32 characters).", Theme::TextTertiary);
        Paragraph(Who, "A password is optional: the lock screen then asks for it. Leave both boxes empty for none.");
        Password_ = Who->Add<TextBox>("Password (optional)");
        Confirm_ = Who->Add<TextBox>("Type the password again");
        TextBox *Boxes[2] = {Password_, Confirm_};
        for (TextBox *Box : Boxes) {
            Box->Password = true;
            Box->Width = 320;
            Box->MaxLength = 64;
            Box->HorizontalAlignment = Align::Start;
            Box->OnTextChanged = [this](const hz::String &) { UpdateButtons(); };
        }
        Password_->OnSubmit = [this](const hz::String &) { Main_->SetFocus(Confirm_); };
        Confirm_->OnSubmit = [this](const hz::String &) { Next(); };
        Mismatch_ = Paragraph(Who, "", Theme::Accent);

        StackPanel *Look = AddPage();
        Paragraph(Look, "Pick the colour of Nexora OS. It changes as soon as you choose, and Settings can change "
                        "it again later.");
        Looks_ = Look->Add<StackPanel>(Orientation::Horizontal, 16);
        LookChoice *Dark = Looks_->Add<LookChoice>(false, &Light_);
        Dark->OnChoose = [this](bool NowLight) { ChooseLook(NowLight); };
        LookChoice *Light = Looks_->Add<LookChoice>(true, &Light_);
        Light->OnChoose = [this](bool NowLight) { ChooseLook(NowLight); };

        StackPanel *Region = AddPage();
        Paragraph(Region, "Nexora OS keeps the time and date the computer's firmware reports. This build "
                          "installs one locale.");
        Grid *RegionRow = Region->Add<Grid>();
        RegionRow->Columns.Push(GridLength::AutoSize());
        RegionRow->Columns.Push(GridLength::Stars(1));
        RegionRow->ColumnSpacing = 12;
        TextBlock *RegionLabel = RegionRow->Add<TextBlock>("Language and keyboard", FontRole::Body);
        RegionLabel->VerticalAlignment = Align::Center;
        RegionLabel->Foreground = Theme::TextPrimary;
        ComboBox *RegionBox = RegionRow->Add<ComboBox>();
        RegionBox->GridColumn = 1;
        RegionBox->Width = 320;
        RegionBox->HorizontalAlignment = Align::Start;
        RegionBox->Items.Push("English (United States) - US keyboard");
        RegionBox->SetSelectedIndex(0, false);
        Paragraph(Region, "No other language or keyboard layout is available in this build, so the list has "
                          "one entry rather than offering a switch that would do nothing. Region and time "
                          "zone are a Settings task for later.", Theme::TextTertiary);

        StackPanel *Apps = AddPage();
        Paragraph(Apps, "Required system components are always installed. Choose the apps to keep; clear an "
                        "app to leave it off. Apps come from the installation media, so this works offline.");
        Grid *Locked = Apps->Add<Grid>();
        Locked->Columns.Push(GridLength::AutoSize());
        Locked->Columns.Push(GridLength::AutoSize());
        Locked->ColumnSpacing = 24;
        CheckBox *RequiredShell = Locked->Add<CheckBox>("Nexora shell (required)", true);
        RequiredShell->SetEnabled(false);
        CheckBox *RequiredSettings = Locked->Add<CheckBox>("Settings (required)", true);
        RequiredSettings->GridColumn = 1;
        RequiredSettings->SetEnabled(false);
        Grid *Catalog = Apps->Add<Grid>();
        Catalog->Columns.Push(GridLength::Stars(1));
        Catalog->Columns.Push(GridLength::Stars(1));
        Catalog->ColumnSpacing = 8;
        Catalog->RowSpacing = 6;
        struct AppEntry {
            const char *Name;
            bool Optional;
        };
        const AppEntry Entries[] = {
            {"Files", false},        {"Browser", false},          {"Terminal", false},
            {"Notepad", false},      {"Calculator", false},       {"Calendar", false},
            {"Clock", false},        {"Stopwatch", false},        {"System Information", false},
            {"Task Manager", false}, {"Photo Viewer", false},     {"Music Player", false},
            {"Video Player", false}, {"Screenshot Tool", false},  {"Paint", true},
            {"Email", true},         {"Contacts", true},          {"Archive Manager", true},
            {"App Store", true},     {"PDF Viewer", true},
        };
        for (size_t i = 0; i < sizeof(Entries) / sizeof(Entries[0]); i++) {
            CheckBox *Box = Catalog->Add<CheckBox>(Entries[i].Name, !Entries[i].Optional);
            Box->GridColumn = (int32_t)(i % 2);
            Box->GridRow = (int32_t)(i / 2);
            AppBoxes_.Push(Box);
            AppNames_.Push(Entries[i].Name);
        }
        Paragraph(Apps, "The catalog matches what this build can actually install and run; an app is listed "
                        "only when it is present on the installation media.", Theme::TextTertiary);

        StackPanel *Connect = AddPage();
        Paragraph(Connect, "Nexora OS currently connects with wired Ethernet, which is set up automatically "
                           "after setup. Wi-Fi and Bluetooth are not implemented in this build, so there is "
                           "nothing to configure here.", Theme::TextPrimary);
        Paragraph(Connect, "Choose Skip to continue. The desktop shows the real network state once it starts; "
                           "no connection is simulated.", Theme::TextSecondary);

        StackPanel *Review = AddPage();
        Review_ = Paragraph(Review, "", Theme::TextPrimary);
        Paragraph(Review, "Use Back to change anything. Finish saves your choices and starts the desktop.");

        StackPanel *Done = AddPage();
        Summary_ = Paragraph(Done, "", Theme::TextPrimary);
        Paragraph(Done, "Start has your programs; Files and Notepad save to your documents folder.");
        Main_->OnClosed = []() { NtUserPostQuitMessage(0); };
    }

    void PageShown(size_t Index) override
    {
        Back_->SetVisible(Index > 0);
        Primary_->SetText(Index == 7 ? "Finish" : "Next");
        /* The connect page is the one page whose second action is a real escape hatch, not a retry. */
        Secondary_->SetVisible(Index == 5);
        Secondary_->SetText("Skip");
        if (Index == 1)
            Main_->SetFocus(Name_);
        else if (Index == 2)
            Main_->SetFocus(Looks_->ChildAt(Light_ ? 1 : 0));
        else
            Main_->SetFocus(Primary_);
        if (Index == 6)
            UpdateReview();
        if (Index == 7)
            UpdateSummary();
        UpdateButtons();
    }

    /* Skipping the connect page has the same effect as Next: the page has no state to keep. */
    void SecondaryAction() override
    {
        if (Page_ == 5)
            Next();
    }

    bool PasswordsMatch() const { return Password_->Text() == Confirm_->Text(); }

    void Next() override
    {
        if (Page_ == 1 && (!setup::IsValidUserName(Name_->Text().CStr()) || !PasswordsMatch()))
            return;
        if (Page_ < 7) {
            SaveProgress(Page_ + 1);
            ShowPage(Page_ + 1);
            return;
        }
        Finish();
    }

    void Back() override
    {
        if (Page_ > 0) {
            SaveProgress(Page_ - 1);
            ShowPage(Page_ - 1);
        }
    }

private:
    void UpdateSummary()
    {
        const char *Name = Name_->Text().CStr();
        Summary_->SetText(hz::String::Format("Hello, %s. Your documents go to C:\\Users\\%s\\Documents.%s Nexora OS "
                                             "will start in %s mode.",
                                             Name, Name,
                                             Password_->Text().Empty() ? ""
                                                                       : " The lock screen asks for your password.",
                                             Light_ ? "light" : "dark")
                              .CStr());
    }

    /* The review page: every choice the earlier pages hold, in one place, so Back can be used to fix
       any of them before the desktop starts. */
    void UpdateReview()
    {
        const char *Name = Name_->Text().CStr();
        uint32_t Count = 0;
        for (const CheckBox *Box : AppBoxes_) {
            if (Box->IsChecked())
                Count++;
        }
        Review_->SetText(hz::String::Format(
                             "Account: %s%s. Appearance: %s. Language: English "
                             "(United States). Apps kept: %u of %u. Network: not configured here; wired "
                             "Ethernet is set up automatically once the desktop starts.",
                             Name, Password_->Text().Empty() ? ", no password" : ", password at sign-in",
                             Light_ ? "light" : "dark", Count,
                             (uint32_t)AppBoxes_.Size())
                             .CStr());
    }

    /* Writes the page's choices before moving on, so a power loss during setup resumes here rather
       than at the beginning. The account is written again in Finish(), which also ends onboarding. */
    void SaveProgress(size_t Index)
    {
        hz::fs::Settings Shell;
        hz::String Path = hz::fs::SettingsPath("shell");
        Shell.Load(Path);
        Shell.SetInt("SetupPage", (int32_t)Index);
        Shell.Set("UserName", Name_->Text().CStr());
        Shell.SetInt("Theme", Light_ ? 1 : 0);
        Shell.Save(Path);
    }

    /* The scheme switches in place. Every element reads the palette when it is built, so the whole
       page tree is rebuilt — through a timer, because this runs inside a card's own event handler. */
    void ChooseLook(bool Light)
    {
        Light_ = Light;
        Theme::ApplyScheme(Light ? Theme::Scheme::Light : Theme::Scheme::Dark);
        HzPrint("setup: colour scheme set to %s", Light ? "light" : "dark");
        if (RebuildTimer_)
            return;
        RebuildTimer_ = Main_->SetTimer(1, [this]() {
            Main_->KillTimer(RebuildTimer_);
            RebuildTimer_ = 0;
            RebuildPages();
        });
    }

    void RebuildPages()
    {
        hz::String Name = Name_->Text(), Password = Password_->Text(), Confirm = Confirm_->Text();
        hz::Vector<hz::String> Kept;
        for (size_t i = 0; i < AppBoxes_.Size(); i++) {
            if (AppBoxes_[i]->IsChecked())
                Kept.Push(AppNames_[i]);
        }
        size_t Index = Page_;
        Pages_->ClearChildren();
        PageList_.Clear();
        Titles_.Clear();
        /* The pages are gone with the tree; clear the pointers before ShowPage() reads them. */
        Name_ = Password_ = Confirm_ = nullptr;
        Hint_ = Summary_ = Mismatch_ = nullptr;
        Cards_ = Looks_ = nullptr;
        Review_ = nullptr;
        AppBoxes_.Clear();
        AppNames_.Clear();
        Build();
        Name_->SetText(Name.CStr(), false);
        Password_->SetText(Password.CStr(), false);
        Confirm_->SetText(Confirm.CStr(), false);
        /* Re-tick the apps the user had chosen before the tree was rebuilt for a scheme switch. */
        for (size_t i = 0; i < AppBoxes_.Size(); i++) {
            for (const hz::String &App : Kept) {
                if (App == AppNames_[i]) {
                    AppBoxes_[i]->SetChecked(true, false);
                    break;
                }
            }
        }
        ShowPage(Index);
    }

    void UpdateButtons()
    {
        bool Valid = setup::IsValidUserName(Name_->Text().CStr());
        Primary_->SetEnabled(Page_ != 1 || (Valid && PasswordsMatch()));
        Mismatch_->SetText(!PasswordsMatch() && !Confirm_->Text().Empty() ? "The two passwords are not the same." : "");
        Hint_->Foreground = (Page_ == 1 && !Name_->Text().Empty() && !Valid) ? Theme::Accent : Theme::TextTertiary;
        Hint_->Invalidate();
    }

    void Finish()
    {
        const hz::String &Name = Name_->Text();
        hz::String Home = hz::String::Format("C:\\Users\\%s", Name.CStr());
        hz::fs::CreateDirectory(hz::String("C:\\Users"));
        hz::fs::CreateDirectory(Home);
        hz::fs::CreateDirectory(hz::fs::Join(Home, "Documents"));
        hz::String Welcome = hz::String::Format("Welcome to Nexora OS, %s.\n\nThis folder is yours: Notepad and Files "
                                                "start here.\n",
                                                Name.CStr());
        hz::String Readme = hz::fs::Join(hz::fs::Join(Home, "Documents"), "Welcome.txt");
        if (!hz::fs::Exists(Readme))
            hz::fs::WriteAll(Readme, Welcome.CStr(), Welcome.Length());
        hz::fs::Settings Shell;
        hz::String Path = hz::fs::SettingsPath("shell");
        Shell.Load(Path);
        Shell.SetInt("FirstBoot", 0);
        Shell.Set("UserName", Name.CStr());
        Shell.SetInt("Wallpaper", (int)Wallpaper_);
        Shell.SetInt("Theme", Light_ ? 1 : 0); /* 0 dark, 1 light: the shell applies it at start-up */
        /* The apps the user kept. The shell reads this when it builds Start and the app drawer, so an
           app cleared here must not appear as an installed entry or a desktop shortcut. */
        hz::String Kept;
        for (size_t i = 0; i < AppBoxes_.Size(); i++) {
            if (!AppBoxes_[i]->IsChecked())
                continue;
            if (!Kept.Empty())
                Kept += ",";
            Kept += AppNames_[i];
        }
        Shell.Set("Apps", Kept.CStr());
        Shell.SetInt("AppsConfigured", 1);
        /* The agreed starting desktop: Files, Terminal, Browser and Trash shortcuts. The bottom
           taskbar always stays; the top dock is off until Settings turns it on. */
        Shell.SetInt("ShortcutFiles", 1);
        Shell.SetInt("ShortcutTerminal", 1);
        Shell.SetInt("ShortcutBrowser", 1);
        Shell.SetInt("ShortcutTrash", 1);
        Shell.SetInt("SetupPage", 0); /* onboarding is finished; later boots must not resume it */
        bool Password = !Password_->Text().Empty();
        if (Password) {
            hz::String Salt = hz::crypto::NewSalt();
            Shell.Set("PasswordSalt", Salt.CStr());
            Shell.Set("PasswordHash", hz::crypto::HashPassword(Password_->Text(), Salt).CStr());
        }
        STATUS Status = Shell.Save(Path);
        HzPrint("setup: first boot done: user '%s', wallpaper %u, %s mode%s (settings saved: 0x%08X)", Name.CStr(),
                Wallpaper_, Light_ ? "light" : "dark", Password ? ", password set" : "", (uint32_t)Status);
        HWND ShellWindow = 0;
        if (HZ_SUCCESS(NtUserGetShellWindow(&ShellWindow)) && ShellWindow)
            NtUserPostMessage(ShellWindow, HZ_WM_SHELLREQUEST, HZ_SHELL_REQUEST_SETTINGS, 0);
        Main_->Close();
    }

    TextBox *Name_ = nullptr, *Password_ = nullptr, *Confirm_ = nullptr;
    TextBlock *Hint_ = nullptr, *Summary_ = nullptr, *Mismatch_ = nullptr, *Review_ = nullptr;
    StackPanel *Cards_ = nullptr, *Looks_ = nullptr;
    /* The app-selection page: one check box per catalog entry, with the label kept alongside it so the
       kept apps can be restored after a rebuild and saved by name in Finish(). */
    hz::Vector<CheckBox *> AppBoxes_;
    hz::Vector<hz::String> AppNames_;
    uint32_t Wallpaper_ = 0;
    bool Light_ = false;       /* the colour scheme chosen on the "Choose a look" page */
    bool DefaultsLoaded_ = false; /* the display and the saved scheme are read once */
    uint64_t RebuildTimer_ = 0;   /* one-shot: rebuilds the pages after a scheme switch */
};

}  // namespace

extern "C" int main(int argc, char **argv)
{
    bool FirstBoot = argc > 1 && hz::fs::EqualNames(argv[1], "firstboot");
    Wizard *App = nullptr;
    if (FirstBoot) {
        /* Start in the saved colour scheme, so Setup and the desktop that follows it agree. */
        hz::fs::Settings Saved;
        Saved.Load(hz::fs::SettingsPath("shell"));
        Theme::ApplyScheme(Saved.GetInt("Theme", 0) != 0 ? Theme::Scheme::Light : Theme::Scheme::Dark);
        App = new FirstBootWizard();
        if (!App->Create("Set up Nexora OS")) {
            HzPrint("setup: cannot create the window");
            return 1;
        }
        HzPrint("setup: first-boot setup started");
        return RunMessageLoop();
    }
    if (!hz::fs::IsLiveSystem() || !hz::fs::Exists(hz::String("\\Device\\Ramdisk1\\EFI\\BOOT\\BOOTX64.EFI"))) {
        Window *Note = new Window("Install Nexora OS", 420, 160, HZ_WS_CAPTION | HZ_WS_MICA | HZ_WS_VISIBLE);
        Note->OnClosed = []() { NtUserPostQuitMessage(0); };
        Note->Show();
        MessageBox(Note, "Install Nexora OS",
                   "Setup runs from the live image (start the computer from the Nexora OS ISO). This system is "
                   "already installed or was started without the install image.",
                   nullptr, [Note](int) { Note->Close(); });
        HzPrint("setup: not a live system with the install image; nothing to do");
        return RunMessageLoop();
    }
    App = new InstallWizard();
    if (!App->Create("Install Nexora OS")) {
        HzPrint("setup: cannot create the window");
        return 1;
    }
    HzPrint("setup: started");
    return RunMessageLoop();
}
