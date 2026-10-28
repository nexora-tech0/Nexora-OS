/*
 * Nexora OS - user/apps/store/store.cpp
 * Nexora Store: a native app library and local .zip bundle installer. The package input is a flat
 * ZIP with exactly one .app manifest and its matching .exe. There is no remote catalog or signing
 * service in this build, so the UI says so instead of showing invented online inventory.
 */
#include "hzapps.h"
#include "hzarchive.h"
#include "hzfile.h"
#include "hzui.h"
#include "../browser/http.h"
#include "../browser/tls.h"
#include "../browser/url.h"

using namespace hzui;

namespace {

constexpr const char *kPackageNote =
    "Downloads accept direct HTTPS links to ZIP bundles and save them to Downloads. Downloads are not automatically "
    "installed; use Import app bundle after reviewing the source. Nexora does not yet have a curated catalog or "
    "package-signing service.";

Icon IconFor(const hz::String &Name)
{
    if (hz::fs::EqualNames(Name.CStr(), "Globe")) return Icon::Globe;
    if (hz::fs::EqualNames(Name.CStr(), "Calculator")) return Icon::Calculator;
    if (hz::fs::EqualNames(Name.CStr(), "Calendar")) return Icon::Calendar;
    if (hz::fs::EqualNames(Name.CStr(), "Clock")) return Icon::Clock;
    if (hz::fs::EqualNames(Name.CStr(), "Prompt")) return Icon::Prompt;
    if (hz::fs::EqualNames(Name.CStr(), "Folder")) return Icon::Folder;
    if (hz::fs::EqualNames(Name.CStr(), "Nexora")) return Icon::Nexora;
    if (hz::fs::EqualNames(Name.CStr(), "AppWindow")) return Icon::AppWindow;
    if (hz::fs::EqualNames(Name.CStr(), "Chart")) return Icon::Chart;
    if (hz::fs::EqualNames(Name.CStr(), "Image")) return Icon::Image;
    if (hz::fs::EqualNames(Name.CStr(), "Palette")) return Icon::Palette;
    return Icon::Document;
}

class AppTile final : public Element {
public:
    explicit AppTile(const hz::apps::App &Program) : Program_(Program)
    {
        Width = 54;
        Height = 54;
    }

    void Paint(Painter &P) override
    {
        Rect R = Bounds();
        P.FillVerticalGradient(R, 17, Program_.Color | 0xFF000000u,
                               (Program_.Color & 0x00FFFFFFu) | 0xFF000000u);
        P.DrawIcon(IconFor(Program_.Icon), R.Deflate(Thickness(15)), 0xFFFFFFFF, 1.7f);
    }

private:
    hz::apps::App Program_;
};

class StoreMark final : public Element {
public:
    StoreMark() { Width = 72; Height = 72; VerticalAlignment = Align::Center; }
    void Paint(Painter &P) override
    {
        Rect R = Bounds();
        P.FillVerticalGradient(R, 24, 0xFF7368F5, 0xFF5146CC);
        P.DrawIcon(Icon::Nexora, R.Deflate(Thickness(19)), 0xFFFFFFFF, 1.8f);
    }
};

class StoreApp {
public:
    bool Create()
    {
        HZ_DISPLAY_INFO Display = {};
        STATUS DisplayStatus = NtUserGetDisplayInfo(&Display);
        if (!HZ_SUCCESS(DisplayStatus)) {
            HzPrint("store: cannot query the display work area (0x%08X)", (uint32_t)DisplayStatus);
            return false;
        }
        Rect Area = Rect::FromHz(Display.WorkArea);
        if (Area.Width <= 0 || Area.Height <= 0) {
            HzPrint("store: display work area is empty");
            return false;
        }
        int32_t Width = hz::Min(1080, hz::Max(Area.Width - 32, 1));
        int32_t Height = hz::Min(720, hz::Max(Area.Height - 32, 1));
        Main_ = new Window("Nexora Store", Width, Height);
        Main_->SetMinimumSize(hz::Min(760, Width), hz::Min(520, Height));
        Root_ = new Grid();
        Root_->Rows.Push(GridLength::AutoSize());
        Root_->Rows.Push(GridLength::AutoSize());
        Root_->Rows.Push(GridLength::AutoSize());
        Root_->Rows.Push(GridLength::AutoSize());
        Root_->Rows.Push(GridLength::Stars(1));
        Root_->Rows.Push(GridLength::AutoSize());
        Root_->RowSpacing = 14;
        Root_->Padding = Thickness(28, 26, 28, 24);

        StackPanel *Hero = Root_->Add<StackPanel>(Orientation::Horizontal, 18);
        Hero->GridRow = 0;
        Hero->Padding = Thickness(22, 20, 22, 20);
        Hero->Background = Theme::CardBackground;
        Hero->BorderColor = Theme::CardStroke;
        Hero->CornerRadius = 24;
        StoreMark *Mark = Hero->Add<StoreMark>();
        (void)Mark;
        StackPanel *Copy = Hero->Add<StackPanel>(Orientation::Vertical, 4);
        Copy->VerticalAlignment = Align::Center;
        TextBlock *Eyebrow = Copy->Add<TextBlock>("NEXORA  /  DISCOVER", FontRole::Caption);
        Eyebrow->Foreground = Theme::Accent;
        TextBlock *Title = Copy->Add<TextBlock>("Find your next favorite", FontRole::Display);
        Title->Foreground = Theme::TextPrimary;
        TextBlock *Subtitle = Copy->Add<TextBlock>("A thoughtful home for the tools that make Nexora yours.",
                                                   FontRole::Body);
        Subtitle->Foreground = Theme::TextSecondary;
        Subtitle->Wrap = true;

        StackPanel *Controls = Root_->Add<StackPanel>(Orientation::Horizontal, 10);
        Controls->GridRow = 1;
        Search_ = Controls->Add<TextBox>("Search apps");
        Search_->Width = 250;
        Search_->OnTextChanged = [this](const hz::String &) { BuildCards(); };
        DownloadUrl_ = Controls->Add<TextBox>("HTTPS app bundle URL");
        DownloadUrl_->Width = 350;
        DownloadUrl_->OnSubmit = [this](const hz::String &) { DownloadPackage(); };
        Button *Download = Controls->Add<Button>("Download", ButtonStyle::Standard);
        Download->OnClick = [this]() { DownloadPackage(); };
        Button *Import = Controls->Add<Button>("Import app bundle", ButtonStyle::Accent);
        Import->OnClick = [this]() { ChoosePackage(); };

        TextBlock *SectionTitle = Root_->Add<TextBlock>("In your Nexora library", FontRole::Subtitle);
        SectionTitle->GridRow = 2;
        SectionTitle->Foreground = Theme::TextPrimary;
        Status_ = Root_->Add<TextBlock>("", FontRole::Caption);
        Status_->GridRow = 3;
        Status_->Foreground = Theme::TextSecondary;
        Status_->Wrap = true;

        ScrollViewer *Scroll = Root_->Add<ScrollViewer>();
        Scroll->GridRow = 4;
        Cards_ = new Grid();
        Scroll->SetContent(Cards_);
        Cards_->Columns.Push(GridLength::Stars(1));
        Cards_->Columns.Push(GridLength::Stars(1));
        Cards_->ColumnSpacing = 14;
        Cards_->RowSpacing = 14;

        TextBlock *Note = Root_->Add<TextBlock>(kPackageNote, FontRole::Caption);
        Note->GridRow = 5;
        Note->Foreground = Theme::TextTertiary;
        Note->Wrap = true;

        Main_->SetContent(Root_);
        Main_->OnClosed = []() { NtUserPostQuitMessage(0); };
        if (!Main_->Show())
            return false;
        RefreshCatalog();
        Main_->UpdateLayout();
        if (Scroll->Extent() > Scroll->Bounds().Height)
            HzPrint("store: catalog scroll ready (%d px content, %d px viewport)", Scroll->Extent(),
                    Scroll->Bounds().Height);
        else
            HzPrint("store: catalog has no scroll range (%d px content, %d px viewport)", Scroll->Extent(),
                    Scroll->Bounds().Height);
        HzPrint("store: opened (%u apps)", (uint32_t)Apps_.Size());
        return true;
    }

private:
    void DownloadPackage()
    {
        using namespace nexora::browser;
        Url Address{};
        if (ParseUrl(DownloadUrl_->Text().CStr(), &Address) != UrlError::None || Address.scheme != Scheme::Https) {
            SetStatus("Enter a valid HTTPS URL for a .zip Nexora app bundle.", false);
            return;
        }

        uint8_t Ip[4] = {};
        if (ParseIpv4Literal(Address.host, Ip) || IsIpv4LiteralCandidate(Address.host)) {
            SetStatus("Downloads require an HTTPS hostname so TLS can verify the server identity.", false);
            return;
        }
        STATUS Result = HzResolveHostA(Address.host, Ip);
        if (!HZ_SUCCESS(Result)) {
            SetStatus(hz::String::Format("DNS lookup failed (0x%08X).", (uint32_t)Result).CStr(), false);
            return;
        }
        uint32_t Ipv4 = ((uint32_t)Ip[0] << 24) | ((uint32_t)Ip[1] << 16) |
                        ((uint32_t)Ip[2] << 8) | (uint32_t)Ip[3];
        HZ_TCP_HANDLE Socket = HzTcpConnect(Ipv4, Address.port);
        if (Socket < 0) {
            SetStatus("Could not connect to the download server.", false);
            return;
        }

        char Target[sizeof(Address.path)] = {};
        if (!FormatHttpRequestTarget(Address, Target, sizeof(Target))) {
            HzTcpClose(Socket);
            SetStatus("The download URL path is invalid.", false);
            return;
        }
        hz::String Host(Address.host);
        if (Address.explicitPort)
            Host.Append(hz::String::Format(":%u", (uint32_t)Address.port));
        hz::String Request("GET ");
        Request.Append(Target);
        Request.Append(" HTTP/1.1\r\nHost: ");
        Request.Append(Host);
        Request.Append("\r\nUser-Agent: NexoraStore/1.0\r\nAccept: application/zip, application/octet-stream\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n");

        hz::Vector<char> Wire, Body;
        Wire.Resize(HttpMaximumWire);
        Body.Resize(HtmlMaximumSource);
        size_t WireLength = 0;
        bool CleanClose = false;
        char Error[192] = {};
        SetStatus("Connecting securely and downloading the bundle…", true);
        bool Fetched = FetchTlsResponse((int32_t)Socket, Address.host, Request.CStr(), Request.Length(),
                                        &Wire[0], Wire.Size(), &WireLength, &CleanClose, Error, sizeof(Error));
        HzTcpClose(Socket);
        if (!Fetched) {
            SetStatus(Error[0] ? Error : "The HTTPS download failed.", false);
            return;
        }

        HttpResponse Response{};
        size_t BodyLength = 0;
        HttpParseError Parse = ParseHttpResponse(&Wire[0], WireLength, &Body[0], Body.Size(), &BodyLength, &Response);
        if (Parse != HttpParseError::None || Response.StatusCode != 200 || Response.UnsupportedContentEncoding ||
            BodyLength < 4 || (uint8_t)Body[0] != 'P' || (uint8_t)Body[1] != 'K' ||
            (uint8_t)Body[2] != 3 || (uint8_t)Body[3] != 4) {
            SetStatus("The server did not return a complete ZIP bundle (HTTP 200 required).", false);
            return;
        }

        hz::String Folder = hz::fs::CurrentKnownFolderPath(hz::fs::KnownFolder::Downloads);
        if (Folder.Empty())
            Folder = "C:\\Nexora\\Downloads";
        if (!hz::fs::Exists(Folder))
            hz::fs::CreateDirectory(Folder);
        uint64_t Uptime = 0;
        NtQueryUptime(&Uptime);
        hz::String Destination = hz::fs::Join(
            Folder, hz::String::Format("NexoraApp-%llu.zip", (unsigned long long)Uptime).CStr());
        Result = hz::fs::WriteAll(Destination, &Body[0], BodyLength);
        if (!HZ_SUCCESS(Result)) {
            SetStatus(hz::String::Format("Could not save the bundle (0x%08X).", (uint32_t)Result).CStr(), false);
            return;
        }
        SetStatus(hz::String::Format("Downloaded %s. Review it, then choose Import app bundle.",
                                     Destination.CStr()).CStr(), true);
        HzPrint("store: downloaded HTTPS app bundle to %s (%zu bytes)", Destination.CStr(), BodyLength);
    }

    void RefreshCatalog()
    {
        Apps_ = hz::apps::Load();
        BuildCards();
    }

    void BuildCards()
    {
        Cards_->ClearChildren();
        Cards_->Rows.Clear();
        size_t Shown = 0;
        const hz::String Query = Search_ ? Search_->Text() : hz::String();
        for (const hz::apps::App &Program : Apps_) {
            if (!Query.Empty() && !Program.Name.ContainsIgnoreCase(Query.CStr()) &&
                !Program.Description.ContainsIgnoreCase(Query.CStr()))
                continue;
            StackPanel *Card = Cards_->Add<StackPanel>(Orientation::Horizontal, 14);
            Card->Padding = Thickness(18, 17, 18, 17);
            Card->Background = Theme::CardBackground;
            Card->BorderColor = Theme::CardStroke;
            Card->CornerRadius = 20;
            if (Shown % 2 == 0)
                Cards_->Rows.Push(GridLength::AutoSize());
            Card->GridColumn = (uint8_t)(Shown % 2);
            Card->GridRow = (uint8_t)(Shown / 2);

            AppTile *Tile = Card->Add<AppTile>(Program);
            StackPanel *Details = Card->Add<StackPanel>(Orientation::Vertical, 5);
            Details->VerticalAlignment = Align::Center;
            TextBlock *Name = Details->Add<TextBlock>(Program.Name.CStr(), FontRole::BodyStrong);
            Name->Foreground = Theme::TextPrimary;
            TextBlock *Description = Details->Add<TextBlock>(Program.Description.Empty() ? "A Nexora app" :
                                                                                           Program.Description.CStr(),
                                                             FontRole::Caption);
            Description->Foreground = Theme::TextSecondary;
            Description->Wrap = true;
            StackPanel *Actions = Details->Add<StackPanel>(Orientation::Horizontal, 10);
            TextBlock *State = Actions->Add<TextBlock>("Included", FontRole::Caption);
            State->Foreground = Theme::TextTertiary;
            Button *Open = Actions->Add<Button>("Open", ButtonStyle::Standard);
            Open->OnClick = [this, Program]() {
                STATUS Result = hz::apps::Launch(Program, nullptr);
                if (HZ_SUCCESS(Result)) {
                    Status_->Foreground = Theme::TextSecondary;
                    Status_->SetText(hz::String::Format("Opening %s.", Program.Name.CStr()).CStr());
                } else {
                    Status_->Foreground = Theme::Accent;
                    Status_->SetText(hz::String::Format("Could not open %s (0x%08X).", Program.Name.CStr(),
                                                        (uint32_t)Result).CStr());
                }
            };
            (void)Tile;
            Shown++;
        }
        if (Shown == 0) {
            TextBlock *Empty = Cards_->Add<TextBlock>("No apps match this search.", FontRole::Body);
            Empty->GridColumnSpan = 2;
            Empty->Foreground = Theme::TextSecondary;
        }
        Status_->SetText(hz::String::Format("%u apps included with Nexora.", (uint32_t)Apps_.Size()).CStr());
        Cards_->InvalidateLayout();
    }

    void ChoosePackage()
    {
        ShowFileDialog(Main_, FileDialogMode::Open, "C:\\", nullptr, [this](const hz::String &Path) {
            if (Path.Empty())
                return;
            InstallPackage(Path);
        });
    }

    void InstallPackage(const hz::String &Path)
    {
        if (hz::fs::Extension(Path) != "zip") {
            SetStatus("Choose a .zip Nexora app bundle.", false);
            return;
        }
        hz::Vector<hz::archive::Entry> Entries;
        STATUS Result = hz::archive::List(Path, Entries);
        if (!HZ_SUCCESS(Result) || Entries.Size() != 2 || Entries[0].Directory || Entries[1].Directory ||
            hz::fs::FileName(Entries[0].Name) != Entries[0].Name ||
            hz::fs::FileName(Entries[1].Name) != Entries[1].Name) {
            SetStatus("A Nexora bundle must be a flat .zip with exactly one manifest and one executable.", false);
            return;
        }
        const hz::archive::Entry *Manifest = nullptr, *Executable = nullptr;
        for (const hz::archive::Entry &Entry : Entries) {
            if (hz::fs::Extension(Entry.Name) == "app")
                Manifest = &Entry;
            else if (hz::fs::Extension(Entry.Name) == "exe")
                Executable = &Entry;
        }
        if (!Manifest || !Executable) {
            SetStatus("The bundle needs one .app manifest and one .exe program.", false);
            return;
        }

        uint64_t Uptime = 0;
        NtQueryUptime(&Uptime);
        hz::fs::CreateDirectory("C:\\Nexora\\Store");
        hz::fs::CreateDirectory("C:\\Nexora\\Store\\Staging");
        hz::String Stage = hz::String::Format("C:\\Nexora\\Store\\Staging\\%llu",
                                              (unsigned long long)Uptime);
        Result = hz::fs::CreateDirectory(Stage);
        if (!HZ_SUCCESS(Result)) {
            SetStatus("The app install location is unavailable or read-only.", false);
            return;
        }
        Result = hz::archive::Extract(Path, Stage);
        hz::String ManifestPath = hz::fs::Join(Stage, Manifest->Name.CStr());
        hz::String ExecutablePath = hz::fs::Join(Stage, Executable->Name.CStr());
        hz::String ManifestText;
        hz::apps::App Program;
        hz::String ParseError;
        if (HZ_SUCCESS(Result) &&
            (!HZ_SUCCESS(hz::fs::ReadAll(ManifestPath, ManifestText, 64 * 1024)) ||
             !hz::apps::ParseManifest(ManifestText.CStr(), ManifestText.Length(), Program, &ParseError)))
            Result = STATUS_INVALID_PARAMETER;
        hz::String SystemFolder = hz::fs::Parent(Program.Path);
        hz::String BinaryName = hz::fs::FileName(Program.Path);
        if (HZ_SUCCESS(Result) &&
            (!hz::fs::EqualNames(SystemFolder.CStr(), "C:\\Nexora\\System") ||
             !hz::fs::EqualNames(BinaryName.CStr(), Executable->Name.CStr()) ||
             hz::fs::Extension(BinaryName) != "exe"))
            Result = STATUS_INVALID_PARAMETER;

        hz::String ManifestTarget = hz::fs::Join(hz::String(hz::apps::kManifestFolder), Manifest->Name.CStr());
        if (HZ_SUCCESS(Result) && (hz::fs::Exists(Program.Path) || hz::fs::Exists(ManifestTarget)))
            Result = STATUS_OBJECT_NAME_COLLISION;
        if (HZ_SUCCESS(Result))
            Result = hz::fs::Copy(ExecutablePath, Program.Path);
        if (HZ_SUCCESS(Result)) {
            Result = hz::fs::Copy(ManifestPath, ManifestTarget);
            if (!HZ_SUCCESS(Result))
                hz::fs::Delete(Program.Path);
        }
        hz::fs::DeleteTree(Stage);
        if (!HZ_SUCCESS(Result)) {
            SetStatus(Result == STATUS_OBJECT_NAME_COLLISION ? "That app is already installed."
                                                             : "The bundle could not be installed on this volume.", false);
            return;
        }
        RefreshCatalog();
        SetStatus(hz::String::Format("%s is ready to use.", Program.Name.CStr()).CStr(), true);
        HWND ShellWindow = 0;
        if (HZ_SUCCESS(NtUserGetShellWindow(&ShellWindow)))
            NtUserPostMessage(ShellWindow, HZ_WM_SHELLREQUEST, HZ_SHELL_REQUEST_NOTIFICATION,
                              HZ_NOTIFICATION_APP_INSTALLED);
    }

    void SetStatus(const char *Text, bool Good)
    {
        Status_->Foreground = Good ? Theme::Accent : Theme::TextSecondary;
        Status_->SetText(Text);
    }

    Window *Main_ = nullptr;
    Grid *Root_ = nullptr;
    TextBox *Search_ = nullptr;
    TextBox *DownloadUrl_ = nullptr;
    Grid *Cards_ = nullptr;
    TextBlock *Status_ = nullptr;
    hz::Vector<hz::apps::App> Apps_;
};

} // namespace

extern "C" int main(int argc, char **argv)
{
    StoreApp App;
    if (!App.Create()) {
        HzPrint("store: could not create the window");
        return 1;
    }
    return RunMessageLoop();
}
