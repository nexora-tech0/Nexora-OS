/*
 * Nexora OS - user/apps/setup/setup.cpp
 * Setup, in two modes:
 *   - "setup.exe" (the live system, Start's "Install Nexora OS"): a wizard that installs Nexora OS
 *     on a disk. It lists the disks (\Device\HarddiskN\DR0, IOCTL_DISK_GET_INFO), and after a
 *     confirmation a worker thread writes a GPT (installer.cpp), has the kernel read it again
 *     (IOCTL_DISK_UPDATE_PARTITIONS), formats both partitions as FAT32, copies the install image
 *     (\Device\Ramdisk1: boot loader, kernel, symbols) and the live image itself (\Device\Ramdisk0
 *     as boot\ramdisk.tar) to the EFI system partition with a limine.conf naming the system
 *     partition, and copies C:\Nexora to the system partition (without Setup's own manifest),
 *     marking it for first-boot setup. Progress arrives in the window as messages.
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

constexpr uint32_t kMsgProgress = HZ_WM_USER + 1; /* WParam = per mille, LParam = hz::String * */
constexpr uint32_t kMsgFinished = HZ_WM_USER + 2; /* WParam = STATUS, LParam = hz::String * */
constexpr uint32_t kMaxDisks = 16;
constexpr uint32_t kChunk = 64 * 1024;

/* ---- Disks --------------------------------------------------------------------------------------- */

struct DiskEntry {
    uint32_t Number = 0;
    HZ_DISK_INFORMATION Info = {};
    bool Eligible = false;
    hz::String Reason; /* why it cannot be used, or what will happen */
};

hz::String DiskPath(uint32_t Number)
{
    return hz::String::Format("\\Device\\Harddisk%u\\DR0", Number);
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
        if (!HZ_SUCCESS(Status))
            continue;
        uint64_t Bytes = E.Info.SectorCount * E.Info.BytesPerSector;
        if (E.Info.Flags & HZ_DISK_SYSTEM)
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
    uint64_t TotalBytes = 0, DoneBytes = 0;
    uint32_t Files = 0;
    hz::String Error;
    setup::Guid SystemGuid;
};

void Progress(InstallJob &Job, uint32_t PerMille, const hz::String &Text)
{
    hz::String *Message = new hz::String(Text);
    if (!HZ_SUCCESS(NtUserPostMessage(Job.Window, kMsgProgress, PerMille, reinterpret_cast<uint64_t>(Message))))
        delete Message;
}

uint32_t CopyPerMille(const InstallJob &Job)
{
    /* Partitioning and formatting take the first 5 %, copying the rest. */
    uint64_t Total = Job.TotalBytes ? Job.TotalBytes : 1;
    return 50 + (uint32_t)hz::Min<uint64_t>(Job.DoneBytes * 950 / Total, 950);
}

STATUS WriteSectors(HANDLE Device, uint64_t Lba, const void *Data, uint32_t Count)
{
    uint64_t Offset = Lba * setup::kSectorSize;
    uint32_t Written = 0;
    STATUS Status = NtWriteFile(Device, Data, Count * setup::kSectorSize, &Offset, &Written);
    if (HZ_SUCCESS(Status) && Written != Count * setup::kSectorSize)
        Status = STATUS_DISK_FULL;
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
    Status = NtDeviceIoControlFile(Disk, IOCTL_DISK_GET_INFO, nullptr, 0, &Info, sizeof(Info), nullptr);
    if (HZ_SUCCESS(Status) && !setup::PlanLayout(Info.SectorCount, &Layout))
        Status = STATUS_DISK_FULL;
    if (!HZ_SUCCESS(Status)) {
        NtClose(Disk);
        Job.Error = "The disk is too small.";
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
        Progress(Job, CopyPerMille(Job), hz::String::Format("Copying %s", E.Name.CStr()));
        Status = hz::fs::Copy(Source, Target);
        if (!HZ_SUCCESS(Status)) {
            Job.Error = hz::String::Format("%s cannot be copied (%s).", Source.CStr(), hz::fs::StatusText(Status));
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
            Progress(Job, CopyPerMille(Job), hz::String("Copying the system image"));
    }
    NtClose(File);
    NtClose(Device);
    if (!HZ_SUCCESS(Status))
        Job.Error = hz::String::Format("Copying the system image failed (0x%08X).", (uint32_t)Status);
    else
        Job.Files++;
    return Status;
}

STATUS WriteText(InstallJob &Job, const hz::String &Path, const hz::String &Text)
{
    STATUS Status = hz::fs::WriteAll(Path, Text.CStr(), Text.Length());
    if (!HZ_SUCCESS(Status))
        Job.Error = hz::String::Format("%s cannot be written (%s).", Path.CStr(), hz::fs::StatusText(Status));
    return Status;
}

STATUS Install(InstallJob &Job)
{
    uint64_t Start = 0;
    NtQueryUptime(&Start);
    Progress(Job, 0, hz::String("Writing the partition table"));
    setup::Layout Layout;
    STATUS Status = Partition(Job, Layout);
    if (!HZ_SUCCESS(Status))
        return Status;
    Progress(Job, 20, hz::String("Formatting the EFI system partition"));
    Status = Format(Job, 1, Layout.Esp, "EFI");
    if (!HZ_SUCCESS(Status))
        return Status;
    Progress(Job, 35, hz::String("Formatting the Nexora OS partition"));
    Status = Format(Job, 2, Layout.System, "NEXORAOS");
    if (!HZ_SUCCESS(Status))
        return Status;

    hz::String Esp = hz::String::Format("\\Device\\Harddisk%u\\Partition1\\", Job.Disk);
    hz::String System = hz::String::Format("\\Device\\Harddisk%u\\Partition2\\", Job.Disk);
    hz::String InstallImage("\\Device\\Ramdisk1\\"), SystemRoot("C:\\Nexora");
    HZ_DISK_GEOMETRY LiveGeometry = {};
    HANDLE Live;
    if (HZ_SUCCESS(NtCreateFile(&Live, FILE_READ_DATA, "\\Device\\Ramdisk0", FILE_OPEN, 0, nullptr))) {
        NtDeviceIoControlFile(Live, IOCTL_DISK_GET_GEOMETRY, nullptr, 0, &LiveGeometry, sizeof(LiveGeometry), nullptr);
        NtClose(Live);
    }
    uint32_t Files = 0;
    Measure(InstallImage, Job.TotalBytes, Files);
    Measure(SystemRoot, Job.TotalBytes, Files);
    Job.TotalBytes += LiveGeometry.SectorCount * LiveGeometry.BytesPerSector;

    /* The EFI system partition: boot loader, kernel, symbols, the system image, the menu. */
    Status = CopyTree(Job, InstallImage, Esp, nullptr);
    if (HZ_SUCCESS(Status))
        Status = CopyDevice(Job, "\\Device\\Ramdisk0", hz::fs::Join(Esp, "boot\\ramdisk.tar"));
    if (HZ_SUCCESS(Status))
        Status = WriteText(Job, hz::fs::Join(Esp, "boot\\limine\\limine.conf"), setup::BootConfiguration(Job.SystemGuid));
    if (!HZ_SUCCESS(Status))
        return Status;
    HzPrint("setup: boot loader, kernel and system image written to the EFI system partition");

    /* The system partition: \Nexora (Setup's manifest unpinned: it is not offered as an
       installer there), \Users, the first-boot marker. */
    hz::String Target = hz::fs::Join(System, "Nexora");
    Status = hz::fs::CreateDirectory(Target);
    if (HZ_SUCCESS(Status))
        Status = CopyTree(Job, SystemRoot, Target, "C:\\Nexora\\Apps\\setup.app");
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
    uint64_t End = 0;
    NtQueryUptime(&End);
    HzPrint("setup: copied %u files (%s) in %llu ms", Job.Files, hz::fs::FormatSize(Job.DoneBytes).CStr(),
            (unsigned long long)((End - Start) / 1000));
    Progress(Job, 1000, hz::String("Finished"));
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

class WizardWindow final : public Window {
public:
    WizardWindow(Wizard *Owner, const char *Title) : Window(Title, 760, 540, HZ_WS_CAPTION | HZ_WS_MICA | HZ_WS_MINIMIZEBOX | HZ_WS_VISIBLE), Owner_(Owner) {}
    int64_t HandleMessage(uint32_t Message, uint64_t WParam, uint64_t LParam) override;

private:
    Wizard *Owner_;
};

/* Pages stacked in one grid cell; a header, the page, and Back / primary buttons. */
class Wizard {
public:
    virtual ~Wizard() = default;
    bool Create(const char *Title);
    virtual void OnProgress(uint32_t PerMille, const hz::String &Text) {}
    virtual void OnFinished(STATUS Status, const hz::String &Error) {}

protected:
    StackPanel *AddPage();
    TextBlock *Paragraph(Element *Page, const char *Text, Color C = Theme::TextSecondary);
    void ShowPage(size_t Index);
    virtual void Build() = 0;
    virtual void Next() = 0;
    virtual void Back() {}
    virtual void PageShown(size_t Index) {}

    Window *Main_ = nullptr;
    TextBlock *Title_ = nullptr, *Step_ = nullptr;
    Grid *Pages_ = nullptr;
    hz::Vector<StackPanel *> PageList_;
    size_t Page_ = 0;
    Button *Back_ = nullptr, *Primary_ = nullptr;
    hz::Vector<const char *> Titles_;
};

int64_t WizardWindow::HandleMessage(uint32_t Message, uint64_t WParam, uint64_t LParam)
{
    if (Message == kMsgProgress) {
        hz::String *Text = reinterpret_cast<hz::String *>(LParam);
        Owner_->OnProgress((uint32_t)WParam, *Text);
        delete Text;
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
    Root->Rows.Push(GridLength::AutoSize());
    Root->Rows.Push(GridLength::Stars(1));
    Root->Rows.Push(GridLength::Pixels(72));
    StackPanel *Header = Root->Add<StackPanel>(Orientation::Vertical, 2);
    Header->Padding = Thickness(32, 16, 32, 8);
    Step_ = Header->Add<TextBlock>("", FontRole::Caption);
    Step_->Foreground = Theme::Accent;
    Title_ = Header->Add<TextBlock>("", FontRole::Title);
    Pages_ = Root->Add<Grid>();
    Pages_->GridRow = 1;
    Pages_->Padding = Thickness(32, 8, 32, 8);
    Grid *Bar = Root->Add<Grid>();
    Bar->GridRow = 2;
    Bar->Background = 0x26000000;
    Bar->Padding = Thickness(24, 0);
    Bar->Columns.Push(GridLength::Stars(1));
    Bar->Columns.Push(GridLength::AutoSize());
    StackPanel *Buttons = Bar->Add<StackPanel>(Orientation::Horizontal, 8);
    Buttons->GridColumn = 1;
    Buttons->VerticalAlignment = Align::Center;
    Back_ = Buttons->Add<Button>("Back");
    Back_->MinWidth = 110;
    Back_->OnClick = [this]() { Back(); };
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
    ShowPage(0);
    if (!Main_->Show())
        return false;
    Main_->SetFocus(Primary_);
    return true;
}

StackPanel *Wizard::AddPage()
{
    StackPanel *P = Pages_->Add<StackPanel>(Orientation::Vertical, 12);
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
    Step_->SetText(hz::String::Format("STEP %zu OF %zu", Index + 1, PageList_.Size()).CStr());
    Main_->InvalidateLayout();
    PageShown(Index);
}

/* ---- Install wizard --------------------------------------------------------------------------------- */

class InstallWizard final : public Wizard {
public:
    void OnProgress(uint32_t PerMille, const hz::String &Text) override
    {
        Bar_->SetValue(PerMille / 10.0f);
        Status_->SetText(Text.CStr());
        Percent_->SetText(hz::String::Format("%u%%", PerMille / 10).CStr());
    }

    void OnFinished(STATUS Status, const hz::String &Error) override
    {
        Installing_ = false;
        Succeeded_ = HZ_SUCCESS(Status);
        if (Succeeded_) {
            HzPrint("setup: done; Nexora OS is installed on disk %u", Disks_[Chosen_].Number);
            DoneText_->SetText("Nexora OS is installed. Remove the installation media (detach the ISO) and restart; "
                               "the computer then starts from the disk, and first-boot setup asks for your name.");
        } else {
            HzPrint("setup: failed: %s (0x%08X)", Error.CStr(), (uint32_t)Status);
            DoneText_->SetText(hz::String::Format("Setup could not finish: %s The disk may hold a partial installation; "
                                                  "you can run Setup again.", Error.CStr())
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
        Paragraph(Welcome, "Setup copies Nexora OS from this live session to a disk, so the computer can start "
                           "from it. Settings and documents are then kept between restarts.",
                  Theme::TextPrimary);
        Paragraph(Welcome, "The disk is erased and gets two partitions: a 100 MB EFI system partition with the "
                           "boot loader, and a FAT32 partition for Nexora OS and your files.");
        Paragraph(Welcome, hz::String::Format("You need a disk of at least %s. Nexora OS " HZ_VERSION_STRING
                                              " includes Inter and JetBrains Mono (SIL Open Font License) and the "
                                              "Limine boot loader (BSD license); see C:\\Nexora\\Licenses.",
                                              hz::fs::FormatSize(setup::MinimumDiskSectors() * 512).CStr())
                               .CStr());

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
        Layout_ = Paragraph(Confirm, "");
        TextBlock *Warning = Paragraph(Confirm, "Everything on this disk will be erased. This cannot be undone.",
                                       Theme::Accent);
        Warning->Font = FontRole::BodyStrong;
        Agree_ = Confirm->Add<CheckBox>("I understand that the disk will be erased");
        Agree_->OnChanged = [this](bool) { UpdateButtons(); };

        StackPanel *Copying = AddPage();
        Paragraph(Copying, "This takes a minute or two. Do not turn off the computer.");
        Bar_ = Copying->Add<ProgressBar>(0);
        Bar_->Margin = Thickness(0, 12, 0, 0);
        Grid *Line = Copying->Add<Grid>();
        Line->Columns.Push(GridLength::Stars(1));
        Line->Columns.Push(GridLength::AutoSize());
        Status_ = Line->Add<TextBlock>("Preparing", FontRole::Body);
        Percent_ = Line->Add<TextBlock>("0%", FontRole::Body);
        Percent_->GridColumn = 1;

        StackPanel *Done = AddPage();
        DoneText_ = Paragraph(Done, "", Theme::TextPrimary);

        Main_->OnCloseRequested = [this]() { return !Installing_; };
        Main_->OnClosed = []() { NtUserPostQuitMessage(0); };
    }

    void PageShown(size_t Index) override
    {
        Back_->SetVisible(Index == 1 || Index == 2);
        Primary_->SetVisible(Index != 3);
        Primary_->SetText(Index == 2 ? "Install" : Index == 4 ? (Succeeded_ ? "Restart now" : "Close") : "Next");
        if (Index == 1)
            Scan();
        if (Index == 2) {
            const DiskEntry &D = Disks_[Chosen_];
            setup::Layout L;
            setup::PlanLayout(D.Info.SectorCount, &L);
            Summary_->SetText(hz::String::Format("Nexora OS will be installed on disk %u, %s (%s).", D.Number,
                                                 D.Info.Model, hz::fs::FormatSize(D.Info.SectorCount * 512).CStr())
                                  .CStr());
            Layout_->SetText(hz::String::Format("Partitions: EFI system partition, %s (FAT32); Nexora OS, %s (FAT32). "
                                                "The live image's files are copied.",
                                                hz::fs::FormatSize(L.Esp.Sectors() * 512).CStr(),
                                                hz::fs::FormatSize(L.System.Sectors() * 512).CStr())
                                 .CStr());
            Agree_->SetChecked(false, false);
            Main_->SetFocus(Agree_);
        } else if (Index == 1) {
            Main_->SetFocus(List_);
        } else if (Index != 3) {
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
            if (Agree_->IsChecked())
                StartInstall();
            break;
        case 4:
            if (Succeeded_) {
                HzPrint("setup: restarting");
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
            Enabled = Agree_->IsChecked();
        Primary_->SetEnabled(Enabled);
    }

    void StartInstall()
    {
        Job_ = new InstallJob();
        Job_->Window = Main_->Handle();
        Job_->Disk = Disks_[Chosen_].Number;
        Installing_ = true;
        ShowPage(3);
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
    TextBlock *Summary_ = nullptr, *Layout_ = nullptr, *Status_ = nullptr, *Percent_ = nullptr, *DoneText_ = nullptr;
    CheckBox *Agree_ = nullptr;
    ProgressBar *Bar_ = nullptr;
    InstallJob *Job_ = nullptr;
    bool Installing_ = false, Succeeded_ = false;
};

/* ---- First-boot setup --------------------------------------------------------------------------------- */

struct Swatch {
    Color Top, Middle, Bottom, Glow;
    const char *Name;
};
const Swatch kSwatches[HZ_MAX_WALLPAPER + 1] = {
    {0xFF0B0E26, 0xFF261A4A, 0xFF4A2448, 0xFFFF8A4C, "Nexora dusk"},
    {0xFF050D22, 0xFF0F2A52, 0xFF1C4E74, 0xFF4CC8FF, "Blue hour"},
    {0xFF161028, 0xFF4E2E58, 0xFF9A5466, 0xFFFFC27A, "Dawn"},
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

class FirstBootWizard final : public Wizard {
protected:
    void Build() override
    {
        HZ_DISPLAY_INFO Display;
        memset(&Display, 0, sizeof(Display));
        NtUserGetDisplayInfo(&Display);
        Wallpaper_ = Display.Wallpaper;
        Titles_.Push("Welcome to Nexora OS");
        Titles_.Push("Who is using this computer?");
        Titles_.Push("Choose a wallpaper");
        Titles_.Push("You are all set");

        StackPanel *Hello = AddPage();
        Paragraph(Hello, "Nexora OS now starts from this computer's disk. A couple of questions, and it is ready.",
                  Theme::TextPrimary);
        Paragraph(Hello, "Everything here can be changed later in Settings.");

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
        Paragraph(Look, "Pick the desktop you like. It changes right away.");
        Cards_ = Look->Add<StackPanel>(Orientation::Horizontal, 16);
        for (uint32_t i = 0; i <= HZ_MAX_WALLPAPER; i++)
            Cards_->Add<WallpaperChoice>(i, &Wallpaper_);

        StackPanel *Done = AddPage();
        Summary_ = Paragraph(Done, "", Theme::TextPrimary);
        Paragraph(Done, "Start has your programs; Files and Notepad save to your documents folder.");
        Main_->OnClosed = []() { NtUserPostQuitMessage(0); };
    }

    void PageShown(size_t Index) override
    {
        Back_->SetVisible(Index > 0);
        Primary_->SetText(Index == 3 ? "Finish" : "Next");
        if (Index == 1)
            Main_->SetFocus(Name_);
        else if (Index == 2)
            Main_->SetFocus(Cards_->ChildAt(Wallpaper_ <= HZ_MAX_WALLPAPER ? Wallpaper_ : 0));
        else
            Main_->SetFocus(Primary_);
        if (Index == 3)
            Summary_->SetText(hz::String::Format("Hello, %s. Your documents go to C:\\Users\\%s\\Documents.%s",
                                                 Name_->Text().CStr(), Name_->Text().CStr(),
                                                 Password_->Text().Empty() ? ""
                                                                           : " The lock screen asks for your password.")
                                  .CStr());
        UpdateButtons();
    }

    bool PasswordsMatch() const { return Password_->Text() == Confirm_->Text(); }

    void Next() override
    {
        if (Page_ == 1 && (!setup::IsValidUserName(Name_->Text().CStr()) || !PasswordsMatch()))
            return;
        if (Page_ < 3) {
            ShowPage(Page_ + 1);
            return;
        }
        Finish();
    }

    void Back() override
    {
        if (Page_ > 0)
            ShowPage(Page_ - 1);
    }

private:
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
        bool Password = !Password_->Text().Empty();
        if (Password) {
            hz::String Salt = hz::crypto::NewSalt();
            Shell.Set("PasswordSalt", Salt.CStr());
            Shell.Set("PasswordHash", hz::crypto::HashPassword(Password_->Text(), Salt).CStr());
        }
        STATUS Status = Shell.Save(Path);
        HzPrint("setup: first boot done: user '%s', wallpaper %u%s (settings saved: 0x%08X)", Name.CStr(), Wallpaper_,
                Password ? ", password set" : "", (uint32_t)Status);
        HWND ShellWindow = 0;
        if (HZ_SUCCESS(NtUserGetShellWindow(&ShellWindow)) && ShellWindow)
            NtUserPostMessage(ShellWindow, HZ_WM_SHELLREQUEST, HZ_SHELL_REQUEST_SETTINGS, 0);
        Main_->Close();
    }

    TextBox *Name_ = nullptr, *Password_ = nullptr, *Confirm_ = nullptr;
    TextBlock *Hint_ = nullptr, *Summary_ = nullptr, *Mismatch_ = nullptr;
    StackPanel *Cards_ = nullptr;
    uint32_t Wallpaper_ = 0;
};

}  // namespace

extern "C" int main(int argc, char **argv)
{
    bool FirstBoot = argc > 1 && hz::fs::EqualNames(argv[1], "firstboot");
    Wizard *App = nullptr;
    if (FirstBoot) {
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
