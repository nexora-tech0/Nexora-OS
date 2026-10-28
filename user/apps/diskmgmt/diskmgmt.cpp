/* Nexora OS - Disks & Volumes: inspect disks, initialize a Nexora GPT layout, and format FAT32. */
#include "hzfile.h"
#include "hzui.h"
#include "../setup/installer.h"

using namespace hzui;

namespace {

constexpr uint32_t kDiskLimit = 16;
constexpr uint32_t kFormatChunkSectors = 8;

struct DiskEntry {
    HZ_DISK_INFORMATION Info = {};
    hz::Vector<HZ_PARTITION_INFORMATION> Partitions;
    hz::String BlockReason;
};

hz::String DiskPath(uint32_t Disk) { return hz::String::Format("\\Device\\Harddisk%u\\DR0", Disk); }
hz::String PartPath(uint32_t Disk, uint32_t Part)
{
    return hz::String::Format("\\Device\\Harddisk%u\\Partition%u", Disk, Part);
}

STATUS QueryDisk(uint32_t Number, DiskEntry *Entry)
{
    HANDLE Disk = 0;
    hz::String Path = DiskPath(Number);
    STATUS Status = NtCreateFile(&Disk, FILE_READ_DATA, Path.CStr(), FILE_OPEN, 0, nullptr);
    if (!HZ_SUCCESS(Status)) return Status;
    uint32_t Returned = 0;
    Status = NtDeviceIoControlFile(Disk, IOCTL_DISK_GET_INFO, nullptr, 0, &Entry->Info,
                                   sizeof(Entry->Info), &Returned);
    NtClose(Disk);
    if (!HZ_SUCCESS(Status) || Returned != sizeof(Entry->Info)) return STATUS_DEVICE_DATA_ERROR;
    for (uint32_t I = 1; I <= Entry->Info.PartitionCount && I <= 128; I++) {
        HANDLE Part = 0;
        Path = PartPath(Number, I);
        Status = NtCreateFile(&Part, FILE_READ_DATA, Path.CStr(), FILE_OPEN, 0, nullptr);
        if (!HZ_SUCCESS(Status)) return Status;
        HZ_PARTITION_INFORMATION Info = {};
        Returned = 0;
        Status = NtDeviceIoControlFile(Part, IOCTL_DISK_GET_PARTITION_INFO, nullptr, 0, &Info,
                                       sizeof(Info), &Returned);
        NtClose(Part);
        if (!HZ_SUCCESS(Status) || Returned != sizeof(Info) || Info.Number != I)
            return STATUS_DEVICE_DATA_ERROR;
        Entry->Partitions.Push(Info);
    }
    if (Entry->Info.Flags & HZ_DISK_SYSTEM) Entry->BlockReason = "System disk; changes are blocked";
    else if (Entry->Info.Flags & HZ_DISK_READ_ONLY) Entry->BlockReason = "Read-only media";
    else if (Entry->Info.Flags & HZ_DISK_IN_USE) Entry->BlockReason = "A volume is mounted or open";
    else if (Entry->Info.BytesPerSector != setup::kSectorSize) Entry->BlockReason = "Only 512-byte sectors supported";
    return STATUS_SUCCESS;
}

STATUS WriteSectors(HANDLE Disk, uint64_t Lba, const void *Data, uint32_t Count)
{
    uint64_t Offset = Lba * setup::kSectorSize;
    uint32_t Bytes = Count * setup::kSectorSize, Written = 0;
    STATUS Status = NtWriteFile(Disk, Data, Bytes, &Offset, &Written);
    return HZ_SUCCESS(Status) && Written != Bytes ? STATUS_DISK_FULL : Status;
}

STATUS ZeroSectors(HANDLE Disk, uint64_t Lba, uint64_t Count)
{
    static uint8_t Zeros[kFormatChunkSectors * setup::kSectorSize] = {};
    while (Count) {
        uint32_t Chunk = (uint32_t)hz::Min<uint64_t>(Count, kFormatChunkSectors);
        STATUS Status = WriteSectors(Disk, Lba, Zeros, Chunk);
        if (!HZ_SUCCESS(Status)) return Status;
        Lba += Chunk; Count -= Chunk;
    }
    return STATUS_SUCCESS;
}

class DiskManagementApp {
public:
    DiskManagementApp() : Main_("Disks & Volumes", 880, 660)
    {
        Grid *Root = new Grid();
        Root->Rows.Push(GridLength::AutoSize()); Root->Rows.Push(GridLength::AutoSize());
        Root->Rows.Push(GridLength::AutoSize()); Root->Rows.Push(GridLength::AutoSize());
        Root->Rows.Push(GridLength::AutoSize()); Root->Rows.Push(GridLength::Stars(1));
        Root->Rows.Push(GridLength::AutoSize());
        Root->RowSpacing = 12; Root->Padding = Thickness(24, 20);
        Root->Add<TextBlock>("Disks & Volumes", FontRole::Title)->GridRow = 0;
        TextBlock *Intro = Root->Add<TextBlock>("Review storage devices, create Nexora's standard GPT layout, or format an unused partition as FAT32.", FontRole::Body);
        Intro->GridRow = 1; Intro->Foreground = Theme::TextSecondary; Intro->Wrap = true;
        StackPanel *Select = Root->Add<StackPanel>(Orientation::Horizontal, 10); Select->GridRow = 2;
        DiskPicker_ = Select->Add<ComboBox>(); DiskPicker_->Width = 410;
        DiskPicker_->OnSelectionChanged = [this](size_t I) { SelectedDisk_ = I; RefreshDetails(); };
        Button *Refresh = Select->Add<Button>("Rescan", ButtonStyle::Standard);
        Refresh->OnClick = [this]() { Reload(); };
        StackPanel *SelectPart = Root->Add<StackPanel>(Orientation::Horizontal, 10); SelectPart->GridRow = 3;
        PartPicker_ = SelectPart->Add<ComboBox>(); PartPicker_->Width = 410;
        PartPicker_->OnSelectionChanged = [this](size_t I) { SelectedPartition_ = I; };
        StackPanel *Actions = Root->Add<StackPanel>(Orientation::Horizontal, 8); Actions->GridRow = 4;
        Initialize_ = Actions->Add<Button>("Create Nexora layout…", ButtonStyle::Accent);
        Initialize_->OnClick = [this]() { ConfirmInitialize(); };
        Format_ = Actions->Add<Button>("Format selected FAT32…", ButtonStyle::Standard);
        Format_->OnClick = [this]() { ConfirmFormat(); };
        Status_ = Root->Add<TextBlock>("Select a disk to inspect its partitions.", FontRole::Caption);
        Status_->GridRow = 6; Status_->Foreground = Theme::TextSecondary;
        ScrollViewer *Scroll = Root->Add<ScrollViewer>(); Scroll->GridRow = 5;
        Rows_ = new StackPanel(Orientation::Vertical, 8); Scroll->SetContent(Rows_);
        Main_.SetContent(Root);
        Main_.OnClosed = []() { NtUserPostQuitMessage(0); };
    }

    bool Show()
    {
        if (!Main_.Show()) return false;
        Reload();
        return true;
    }

private:
    void Reload()
    {
        Disks_.Clear(); DiskPicker_->Items.Clear();
        for (uint32_t N = 0; N < kDiskLimit; N++) {
            DiskEntry Entry;
            if (!HZ_SUCCESS(QueryDisk(N, &Entry))) continue;
            DiskPicker_->Items.Push(hz::String::Format("Disk %u  ·  %s  ·  %s", N, Entry.Info.Model,
                                                       hz::fs::FormatSize(Entry.Info.SectorCount * Entry.Info.BytesPerSector).CStr()));
            Disks_.Push(hz::Move(Entry));
        }
        if (Disks_.Empty()) {
            SelectedDisk_ = hz::NotFound; DiskPicker_->SetSelectedIndex(hz::NotFound, false);
            PartPicker_->Items.Clear(); PartPicker_->SetSelectedIndex(hz::NotFound, false);
            Rows_->ClearChildren();
            Rows_->Add<TextBlock>("No disks were found. Connect a supported SATA, NVMe, or USB storage device and rescan.");
            Status_->SetText("No disks detected."); Initialize_->SetEnabled(false);
            return;
        }
        SelectedDisk_ = hz::Min(SelectedDisk_, Disks_.Size() - 1);
        DiskPicker_->SetSelectedIndex(SelectedDisk_, false);
        RefreshDetails();
    }

    void RefreshDetails()
    {
        Rows_->ClearChildren();
        if (SelectedDisk_ >= Disks_.Size()) return;
        const DiskEntry &D = Disks_[SelectedDisk_];
        bool Writable = D.BlockReason.Empty();
        Initialize_->SetEnabled(Writable && D.Info.SectorCount >= setup::MinimumDiskSectors());
        PartPicker_->Items.Clear();
        for (const HZ_PARTITION_INFORMATION &P : D.Partitions)
            PartPicker_->Items.Push(hz::String::Format("Partition %u  ·  %s", P.Number,
                                                       hz::fs::FormatSize(P.Length).CStr()));
        SelectedPartition_ = D.Partitions.Empty() ? hz::NotFound : hz::Min(SelectedPartition_, D.Partitions.Size() - 1);
        PartPicker_->SetSelectedIndex(SelectedPartition_, false);
        Format_->SetEnabled(Writable && SelectedPartition_ < D.Partitions.Size());
        Status_->Foreground = Writable ? Theme::TextSecondary : Theme::Accent;
        Status_->SetText(Writable ? hz::String::Format("%u-byte sectors  ·  %u partitions  ·  %s",
                            D.Info.BytesPerSector, D.Info.PartitionCount,
                            D.Info.Style == HZ_PARTITION_STYLE_GPT ? "GPT" : D.Info.Style == HZ_PARTITION_STYLE_MBR ? "MBR" : "Uninitialized").CStr()
                                   : hz::String::Format("%s. Destructive actions are disabled.", D.BlockReason.CStr()).CStr());
        for (const HZ_PARTITION_INFORMATION &P : D.Partitions) {
            StackPanel *Card = Rows_->Add<StackPanel>(Orientation::Vertical, 5);
            Card->Padding = Thickness(14, 10); Card->Background = Theme::CardBackground;
            Card->BorderColor = Theme::CardStroke; Card->CornerRadius = 12;
            const char *Name = P.Name[0] ? P.Name : "Partition";
            Card->Add<TextBlock>(hz::String::Format("Partition %u  ·  %s", P.Number, Name).CStr(), FontRole::BodyStrong);
            TextBlock *Detail = Card->Add<TextBlock>(hz::String::Format("%s  ·  starts at %s  ·  %s",
                hz::fs::FormatSize(P.Length).CStr(), hz::fs::FormatSize(P.StartingOffset).CStr(),
                P.Style == HZ_PARTITION_STYLE_GPT ? "GPT" : "MBR").CStr(), FontRole::Caption);
            Detail->Foreground = Theme::TextSecondary;
        }
        if (D.Partitions.Empty()) {
            TextBlock *Empty = Rows_->Add<TextBlock>("No partitions are registered on this disk.");
            Empty->Foreground = Theme::TextSecondary;
        }
    }

    void ConfirmInitialize()
    {
        if (SelectedDisk_ >= Disks_.Size() || !Disks_[SelectedDisk_].BlockReason.Empty()) return;
        uint32_t Number = Disks_[SelectedDisk_].Info.DiskNumber;
        InputBox(&Main_, "Create Nexora disk layout", "This erases every partition and file on the selected disk. Type ERASE to continue.",
                 "", "Continue", [this, Number](const hz::String &Text) {
                     if (Text != "ERASE") { SetStatus("No changes made. Confirmation text did not match.", false); return; }
                     InitializeDisk(Number);
                 });
    }

    void ConfirmFormat()
    {
        if (SelectedDisk_ >= Disks_.Size() || SelectedPartition_ >= Disks_[SelectedDisk_].Partitions.Size() ||
            !Disks_[SelectedDisk_].BlockReason.Empty()) return;
        uint32_t Number = Disks_[SelectedDisk_].Info.DiskNumber;
        HZ_PARTITION_INFORMATION Part = Disks_[SelectedDisk_].Partitions[SelectedPartition_];
        InputBox(&Main_, "Format partition as FAT32", "Formatting erases all files on this partition. Type FORMAT to continue.",
                 "", "Format", [this, Number, Part](const hz::String &Text) {
                     if (Text != "FORMAT") { SetStatus("No changes made. Confirmation text did not match.", false); return; }
                     setup::Partition Target;
                     Target.FirstLba = Part.StartingOffset / setup::kSectorSize;
                     Target.LastLba = Target.FirstLba + Part.Length / setup::kSectorSize - 1;
                     Target.Name = Part.Name;
                     STATUS Status = FormatPartition(Number, Part.Number, Target, "NEXORA");
                     Reload();
                     SetStatus(HZ_SUCCESS(Status) ? "Partition formatted as FAT32. Reboot to mount or refresh its volume."
                                                  : hz::String::Format("Format failed (0x%08X).", (uint32_t)Status).CStr(),
                               HZ_SUCCESS(Status));
                 });
    }

    void InitializeDisk(uint32_t Number)
    {
        HANDLE Disk = 0; hz::String Path = DiskPath(Number);
        STATUS Status = NtCreateFile(&Disk, GENERIC_READ | GENERIC_WRITE, Path.CStr(), FILE_OPEN, 0, nullptr);
        HZ_DISK_INFORMATION Info = {};
        uint32_t Returned = 0;
        if (HZ_SUCCESS(Status)) Status = NtDeviceIoControlFile(Disk, IOCTL_DISK_GET_INFO, nullptr, 0, &Info, sizeof(Info), &Returned);
        if (HZ_SUCCESS(Status) && (Info.Flags || Info.BytesPerSector != 512)) Status = STATUS_DEVICE_BUSY;
        setup::Layout Layout;
        if (HZ_SUCCESS(Status) && !setup::PlanLayout(Info.SectorCount, &Layout)) Status = STATUS_DISK_FULL;
        if (HZ_SUCCESS(Status)) {
            Layout.DiskGuid = setup::NewGuid(); Layout.Esp.Unique = setup::NewGuid(); Layout.System.Unique = setup::NewGuid();
            static uint8_t Mbr[512], Primary[512], Backup[512], Entries[setup::kGptEntrySectors * 512];
            setup::BuildGpt(Layout, Mbr, Primary, Entries, Backup);
            uint64_t Last = Layout.DiskSectors - 1;
            Status = ZeroSectors(Disk, 0, setup::kAlignment);
            if (HZ_SUCCESS(Status)) Status = ZeroSectors(Disk, Last - setup::kGptEntrySectors, setup::kGptEntrySectors);
            if (HZ_SUCCESS(Status)) Status = WriteSectors(Disk, Last - setup::kGptEntrySectors, Entries, setup::kGptEntrySectors);
            if (HZ_SUCCESS(Status)) Status = WriteSectors(Disk, Last, Backup, 1);
            if (HZ_SUCCESS(Status)) Status = WriteSectors(Disk, 2, Entries, setup::kGptEntrySectors);
            if (HZ_SUCCESS(Status)) Status = WriteSectors(Disk, 1, Primary, 1);
            if (HZ_SUCCESS(Status)) Status = WriteSectors(Disk, 0, Mbr, 1);
            uint32_t Count = 0;
            if (HZ_SUCCESS(Status)) Status = NtDeviceIoControlFile(Disk, IOCTL_DISK_UPDATE_PARTITIONS, nullptr, 0, &Count, sizeof(Count), nullptr);
            if (HZ_SUCCESS(Status) && Count != 2) Status = STATUS_DEVICE_NOT_READY;
            NtClose(Disk);
            if (HZ_SUCCESS(Status)) {
                Status = FormatPartition(Number, 1, Layout.Esp, "NEXORA EFI");
                if (HZ_SUCCESS(Status)) Status = FormatPartition(Number, 2, Layout.System, "NEXORA");
            }
        } else if (Disk) NtClose(Disk);
        Reload();
        SetStatus(HZ_SUCCESS(Status) ? "GPT created and both FAT32 volumes formatted. Rescan to view them; reboot to mount drive letters."
                                     : hz::String::Format("Disk operation failed (0x%08X).", (uint32_t)Status).CStr(), HZ_SUCCESS(Status));
    }

    STATUS FormatPartition(uint32_t DiskNumber, uint32_t PartNumber, const setup::Partition &Partition, const char *Label)
    {
        setup::FatParameters Fat;
        uint64_t Seed = 0; NtQuerySystemTime(&Seed);
        if (!setup::PlanFat32(Partition.Sectors(), Label, (uint32_t)(Seed ^ (Seed >> 32) ^ PartNumber), &Fat))
            return STATUS_DISK_FULL;
        hz::String Path = PartPath(DiskNumber, PartNumber);
        HANDLE Device = 0;
        STATUS Status = NtCreateFile(&Device, GENERIC_READ | GENERIC_WRITE, Path.CStr(), FILE_OPEN, 0, nullptr);
        if (!HZ_SUCCESS(Status)) return Status;
        Status = ZeroSectors(Device, 0, Fat.ReservedSectors);
        for (uint32_t Copy = 0; HZ_SUCCESS(Status) && Copy < Fat.FatCount; Copy++) {
            uint64_t First = Fat.ReservedSectors + (uint64_t)Copy * Fat.FatSectors;
            Status = ZeroSectors(Device, First, Fat.FatSectors);
            if (HZ_SUCCESS(Status)) {
                static uint8_t FatSector[512]; setup::BuildFirstFatSector(FatSector);
                Status = WriteSectors(Device, First, FatSector, 1);
            }
        }
        if (HZ_SUCCESS(Status)) {
            static uint8_t Boot[512], FsInfo[512], Root[512];
            setup::BuildBootSector(Fat, Partition.FirstLba, Boot); setup::BuildFsInfo(Fat, FsInfo);
            setup::BuildRootSector(Fat, Root);
            Status = ZeroSectors(Device, Fat.FirstDataSector(), Fat.SectorsPerCluster);
            if (HZ_SUCCESS(Status)) Status = WriteSectors(Device, Fat.FirstDataSector(), Root, 1);
            if (HZ_SUCCESS(Status)) Status = WriteSectors(Device, 6, Boot, 1);
            if (HZ_SUCCESS(Status)) Status = WriteSectors(Device, 7, FsInfo, 1);
            if (HZ_SUCCESS(Status)) Status = WriteSectors(Device, 1, FsInfo, 1);
            if (HZ_SUCCESS(Status)) Status = WriteSectors(Device, 0, Boot, 1);
        }
        NtClose(Device);
        return Status;
    }

    void SetStatus(const char *Text, bool Good)
    {
        Status_->Foreground = Good ? Theme::Accent : Theme::TextPrimary; Status_->SetText(Text);
    }
    Window Main_;
    ComboBox *DiskPicker_ = nullptr, *PartPicker_ = nullptr;
    Button *Initialize_ = nullptr, *Format_ = nullptr;
    StackPanel *Rows_ = nullptr;
    TextBlock *Status_ = nullptr;
    hz::Vector<DiskEntry> Disks_;
    size_t SelectedDisk_ = 0, SelectedPartition_ = hz::NotFound;
};

}  // namespace

extern "C" int main(int, char **)
{
    DiskManagementApp App;
    if (!App.Show()) { HzPrint("diskmgmt: cannot create the window"); return 1; }
    HzPrint("diskmgmt: started");
    return RunMessageLoop();
}
