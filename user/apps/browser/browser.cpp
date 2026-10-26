/*
 * Nexora Browser — native shell and bounded HTML text renderer for Nexora OS.
 *
 * This UI does not fake successful network navigation or pretend that addresses were fetched.
 * See docs/ENGINE_PORT.md.
 */
#include "hzapps.h"
#include "hzfile.h"
#include "hzsys.h"
#include "hzui.h"
#include "html.h"
#include "url.h"

using namespace hzui;
namespace BrowserUrl = nexora::browser;

namespace {
constexpr size_t MaxHistory = 300;
constexpr size_t MaxBookmarks = 100;
const char *Profile = "C:\\Nexora\\Profiles\\Default\\Browser";
const char *Downloads = "C:\\Nexora\\Profiles\\Default\\Downloads";
const char *HistoryFile = "C:\\Nexora\\Profiles\\Default\\Browser\\history.txt";
const char *BookmarksFile = "C:\\Nexora\\Profiles\\Default\\Browser\\bookmarks.txt";
const char HomePage[] =
    "<!doctype html><html><head><title>Nexora Browser</title></head><body>"
    "<h1>Nexora Browser</h1>"
    "<p>Open a local file with an absolute path or a file:/// URL. Remote pages stay unavailable "
    "until TCP, HTTP and verified HTTPS exist. DNS lookup already reports whether a host resolves.</p>"
    "</body></html>";

bool HasLineBreak(const char *s)
{
    for (; s && *s; ++s)
        if (*s == '\n' || *s == '\r')
            return true;
    return false;
}

bool StartsWith(const char *Text, const char *Prefix)
{
    while (*Prefix && *Text == *Prefix) {
        ++Text;
        ++Prefix;
    }
    return *Prefix == 0;
}

class AnimatedPageText : public TextBlock {
public:
    explicit AnimatedPageText(FontRole Role) : TextBlock("", Role), target_(Foreground) {}

    void Reveal()
    {
        target_ = Foreground;
        fade_.Jump(0);
        fade_.To(1, Motion::ControlStateMs);
        Foreground = WithAlpha(target_, 0);
        StartAnimation();
    }

    bool Animate() override
    {
        Foreground = WithAlpha(target_, (uint32_t)(fade_.Value() * 255.0f));
        Invalidate();
        return fade_.Running();
    }

private:
    Transition fade_;
    Color target_;
};

hz::String Normalize(const hz::String &Input)
{
    if (Input.Empty() || HasLineBreak(Input.CStr()))
        return hz::String();
    if (StartsWith(Input.CStr(), "https://") || StartsWith(Input.CStr(), "http://") ||
        StartsWith(Input.CStr(), "file:///"))
        return Input;

    if (Input.Length() >= 3 &&
        ((Input[0] >= 'A' && Input[0] <= 'Z') || (Input[0] >= 'a' && Input[0] <= 'z')) &&
        Input[1] == ':' && (Input[2] == '\\' || Input[2] == '/')) {
        hz::String Address("file:///");
        for (const char *P = Input.CStr(); *P; ++P) {
            if (*P == '\\')
                Address.Append('/');
            else if (*P == ' ')
                Address.Append("%20");
            else if (*P == '%' || *P == '#' || *P == '?') {
                const char *Hex = "0123456789ABCDEF";
                Address.Append('%');
                Address.Append(Hex[((uint8_t)*P) >> 4]);
                Address.Append(Hex[((uint8_t)*P) & 0x0F]);
            } else
                Address.Append(*P);
        }
        return Address;
    }

    bool Domain = false;
    for (const char *p = Input.CStr(); *p; ++p) {
        if (*p == '.' && p != Input.CStr() && p[1])
            Domain = true;
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '-' || *p == '.' || *p == '/' || *p == ':' || *p == '?' || *p == '&' || *p == '=' ||
              *p == '%' || *p == '_' || *p == '~' || *p == '#'))
            return hz::String();
    }
    return Domain ? hz::String::Format("https://%s", Input.CStr()) : hz::String();
}

void LoadLines(const char *Path, hz::Vector<hz::String> &Out, size_t Limit)
{
    Out.Clear();
    hz::String Text;
    if (!HZ_SUCCESS(hz::fs::ReadAll(hz::String(Path), Text, 256 * 1024)))
        return;
    const char *p = Text.CStr(), *End = p + Text.Length();
    while (p < End && Out.Size() < Limit) {
        const char *q = p;
        while (q < End && *q != '\n' && *q != '\r')
            ++q;
        if (q > p) {
            hz::String Line(p, (size_t)(q - p));
            if (!HasLineBreak(Line.CStr()))
                Out.Push(hz::Move(Line));
        }
        while (q < End && (*q == '\n' || *q == '\r'))
            ++q;
        p = q;
    }
}

bool SaveLines(const char *Path, const hz::Vector<hz::String> &Items)
{
    hz::String Text;
    for (const auto &Item : Items) {
        if (HasLineBreak(Item.CStr()))
            continue;
        Text.Append(Item);
        Text.Append('\n');
    }
    return HZ_SUCCESS(hz::fs::WriteAll(hz::String(Path), Text.CStr(), Text.Length()));
}

void EnsureDir(const char *Path)
{
    if (!hz::fs::Exists(hz::String(Path)))
        hz::fs::CreateDirectory(hz::String(Path));
}

void EnsureProfile()
{
    EnsureDir("C:\\Nexora\\Profiles");
    EnsureDir("C:\\Nexora\\Profiles\\Default");
    EnsureDir(Profile);
    EnsureDir(Downloads);
}

int HexDigit(char C)
{
    if (C >= '0' && C <= '9')
        return C - '0';
    if (C >= 'a' && C <= 'f')
        return C - 'a' + 10;
    if (C >= 'A' && C <= 'F')
        return C - 'A' + 10;
    return -1;
}

bool DecodeLocalPath(const char *UrlPath, hz::String &Path)
{
    Path.Clear();
    for (size_t I = 0; UrlPath[I]; ++I) {
        char C = UrlPath[I];
        if (C == '%') {
            int Hi = HexDigit(UrlPath[I + 1]), Lo = UrlPath[I + 1] ? HexDigit(UrlPath[I + 2]) : -1;
            if (Hi < 0 || Lo < 0)
                return false;
            C = (char)((Hi << 4) | Lo);
            I += 2;
            if (!C || (uint8_t)C < 0x20 || (uint8_t)C == 0x7F)
                return false;
        }
        Path.Append(C == '/' ? '\\' : C);
    }
    return Path.Length() >= 3 && Path[1] == ':' && Path[2] == '\\';
}

hz::String FileUrl(const hz::String &Path)
{
    hz::String Address("file:///");
    const char *Hex = "0123456789ABCDEF";
    for (const char *P = Path.CStr(); *P; ++P) {
        uint8_t C = (uint8_t)*P;
        if (C == '\\')
            Address.Append('/');
        else if (C == ' ' || C == '%' || C == '#' || C == '?') {
            Address.Append('%');
            Address.Append(Hex[C >> 4]);
            Address.Append(Hex[C & 0x0F]);
        } else
            Address.Append(*P);
    }
    return Address;
}

enum class Page { Start, History, Bookmarks, Downloads };

class Browser {
public:
    bool Create();

private:
    void Build();
    void Go(const hz::String &Raw, bool Record = true);
    void StartPage();
    void ShowList(Page View);
    void Back();
    void Forward();
    void Bookmark();
    void Refresh();
    void UpdateButtons();
    void SetTabTitle(const char *Title);
    void Status(const char *Text);
    void ShowLocalFile(const hz::String &Address);
    void SetLinks(const BrowserUrl::HtmlDocument &Document);
    void OpenLink(size_t Index);

    Window *window_ = nullptr;
    Button *tab_ = nullptr;
    TextBox *address_ = nullptr;
    Button *back_ = nullptr, *forward_ = nullptr, *reload_ = nullptr, *bookmark_ = nullptr;
    TextBlock *status_ = nullptr;
    TextBlock *heading_ = nullptr, *detail_ = nullptr;
    AnimatedPageText *items_ = nullptr;
    ListView *links_ = nullptr;
    BrowserUrl::HtmlDocument document_{};
    hz::Vector<hz::String> linkTargets_, linkLabels_;
    hz::Vector<hz::String> history_, bookmarks_;
    hz::String currentAddress_;
    size_t index_ = 0;
    bool hasHistory_ = false;
    Page page_ = Page::Start;
};

bool Browser::Create()
{
    EnsureProfile();
    LoadLines(HistoryFile, history_, MaxHistory);
    LoadLines(BookmarksFile, bookmarks_, MaxBookmarks);
    hasHistory_ = !history_.Empty();
    index_ = hasHistory_ ? history_.Size() - 1 : 0;

    window_ = new Window("Nexora Browser", 1120, 740);
    window_->SetMinimumSize(760, 500);
    Build();
    window_->OnClosed = [this]() {
        SaveLines(HistoryFile, history_);
        SaveLines(BookmarksFile, bookmarks_);
        NtUserPostQuitMessage(0);
    };
    window_->OnUnhandledKey = [this](KeyEvent &e) {
        if (e.Ctrl() && e.Key == 'L') {
            e.Handled = true;
            window_->SetFocus(address_, true);
            address_->SelectAll();
        } else if (e.Ctrl() && e.Key == 'D') {
            e.Handled = true;
            Bookmark();
        } else if (e.Ctrl() && e.Key == 'H') {
            e.Handled = true;
            ShowList(Page::History);
        } else if (e.Ctrl() && e.Key == 'B') {
            e.Handled = true;
            ShowList(Page::Bookmarks);
        } else if (e.Ctrl() && e.Key == 'J') {
            e.Handled = true;
            ShowList(Page::Downloads);
        } else if (e.Ctrl() && e.Key == '1') {
            e.Handled = true;
            StartPage();
        } else if (e.Alt() && e.Key == HZ_VK_LEFT) {
            e.Handled = true;
            Back();
        } else if (e.Alt() && e.Key == HZ_VK_RIGHT) {
            e.Handled = true;
            Forward();
        } else if (e.Key == HZ_VK_F1 + 4) {
            e.Handled = true;
            Refresh();
        }
    };
    StartPage();
    if (!window_->Show())
        return false;
    window_->SetFocus(address_);
    return true;
}

void Browser::Build()
{
    Grid *root = new Grid();
    root->Rows.Push(GridLength::AutoSize());
    root->Rows.Push(GridLength::AutoSize());
    root->Rows.Push(GridLength::Stars(1));
    root->Rows.Push(GridLength::AutoSize());

    Grid *tabs = root->Add<Grid>();
    tabs->Padding = Thickness(20, 10, 20, 4);
    tabs->ColumnSpacing = 8;
    tabs->Columns.Push(GridLength::AutoSize());
    tabs->Columns.Push(GridLength::Stars(1));
    tabs->Columns.Push(GridLength::AutoSize());
    tab_ = tabs->Add<Button>("Start page", ButtonStyle::Subtle);
    tab_->Glyph = Icon::Nexora;
    tab_->SetEnabled(false);
    tab_->MaxWidth = 240;
    Button *startPage = tabs->Add<Button>("Start page", ButtonStyle::Subtle);
    startPage->GridColumn = 2;
    startPage->Glyph = Icon::Home;
    startPage->ToolTip = "Return to the start page in the current tab; multiple tabs are not supported";
    startPage->OnClick = [this]() { StartPage(); };

    Grid *bar = root->Add<Grid>();
    bar->GridRow = 1;
    bar->Padding = Thickness(20, 6, 20, 12);
    bar->ColumnSpacing = 8;
    bar->Columns.Push(GridLength::AutoSize());
    bar->Columns.Push(GridLength::AutoSize());
    bar->Columns.Push(GridLength::Stars(1));

    Grid *navigation = bar->Add<Grid>();
    navigation->Background = Theme::CardBackground;
    navigation->BorderColor = Theme::CardStroke;
    navigation->CornerRadius = Theme::OverlayRadius;
    navigation->Padding = Thickness(4, 0, 4, 0);
    navigation->ColumnSpacing = 2;
    navigation->Columns.Push(GridLength::AutoSize());
    navigation->Columns.Push(GridLength::AutoSize());
    navigation->Columns.Push(GridLength::AutoSize());
    navigation->Columns.Push(GridLength::AutoSize());
    back_ = navigation->Add<Button>("", ButtonStyle::Subtle);
    back_->Glyph = Icon::ChevronLeft;
    back_->ToolTip = "Back · Alt+Left";
    back_->OnClick = [this]() { Back(); };
    forward_ = navigation->Add<Button>("", ButtonStyle::Subtle);
    forward_->GridColumn = 1;
    forward_->Glyph = Icon::ChevronRight;
    forward_->ToolTip = "Forward · Alt+Right";
    forward_->OnClick = [this]() { Forward(); };
    reload_ = navigation->Add<Button>("", ButtonStyle::Subtle);
    reload_->GridColumn = 2;
    reload_->Glyph = Icon::Refresh;
    reload_->ToolTip = "Reload · F5";
    reload_->OnClick = [this]() { Refresh(); };
    Button *home = navigation->Add<Button>("", ButtonStyle::Subtle);
    home->GridColumn = 3;
    home->Glyph = Icon::Home;
    home->ToolTip = "Home · Ctrl+1";
    home->OnClick = [this]() { StartPage(); };

    Grid *library = bar->Add<Grid>();
    library->GridColumn = 1;
    library->Background = Theme::CardBackground;
    library->BorderColor = Theme::CardStroke;
    library->CornerRadius = Theme::OverlayRadius;
    library->Padding = Thickness(4, 0, 4, 0);
    library->ColumnSpacing = 2;
    library->Columns.Push(GridLength::AutoSize());
    library->Columns.Push(GridLength::AutoSize());
    library->Columns.Push(GridLength::AutoSize());
    Button *hist = library->Add<Button>("", ButtonStyle::Subtle);
    hist->Glyph = Icon::Clock;
    hist->ToolTip = "History · Ctrl+H";
    hist->OnClick = [this]() { ShowList(Page::History); };
    Button *downloads = library->Add<Button>("", ButtonStyle::Subtle);
    downloads->GridColumn = 1;
    downloads->Glyph = Icon::Folder;
    downloads->ToolTip = "Downloads · Ctrl+J";
    downloads->OnClick = [this]() { ShowList(Page::Downloads); };
    Button *bookmarks = library->Add<Button>("", ButtonStyle::Subtle);
    bookmarks->GridColumn = 2;
    bookmarks->Glyph = Icon::Star;
    bookmarks->ToolTip = "Bookmarks · Ctrl+B";
    bookmarks->OnClick = [this]() { ShowList(Page::Bookmarks); };

    Grid *addressPill = bar->Add<Grid>();
    addressPill->GridColumn = 2;
    addressPill->Background = Theme::CardBackground;
    addressPill->BorderColor = Theme::CardStroke;
    addressPill->CornerRadius = Theme::OverlayRadius;
    addressPill->Padding = Thickness(5, 4, 5, 4);
    addressPill->ColumnSpacing = 6;
    addressPill->Columns.Push(GridLength::AutoSize());
    addressPill->Columns.Push(GridLength::Stars(1));
    addressPill->Columns.Push(GridLength::AutoSize());
    Button *addressAction = addressPill->Add<Button>("", ButtonStyle::Subtle);
    addressAction->Glyph = Icon::Globe;
    addressAction->ToolTip = "Focus address · Ctrl+L";
    addressAction->OnClick = [this]() {
        window_->SetFocus(address_, true);
        address_->SelectAll();
    };
    address_ = addressPill->Add<TextBox>("Search or enter address");
    address_->GridColumn = 1;
    address_->MaxLength = 2048;
    address_->MinWidth = 120;
    address_->OnSubmit = [this](const hz::String &Text) { Go(Text); };
    bookmark_ = addressPill->Add<Button>("", ButtonStyle::Subtle);
    bookmark_->GridColumn = 2;
    bookmark_->Glyph = Icon::Star;
    bookmark_->ToolTip = "Bookmark · Ctrl+D";
    bookmark_->OnClick = [this]() { Bookmark(); };

    Grid *body = root->Add<Grid>();
    body->GridRow = 2;
    body->MaxWidth = 1040;
    body->HorizontalAlignment = Align::Center;
    body->Padding = Thickness(40, 32, 40, 28);
    body->Background = Theme::CardBackground;
    body->BorderColor = Theme::CardStroke;
    body->CornerRadius = Theme::OverlayRadius;
    body->Margin = Thickness(16, 0, 16, 8);
    body->Rows.Push(GridLength::AutoSize());
    body->Rows.Push(GridLength::AutoSize());
    body->Rows.Push(GridLength::Stars(1));
    body->Rows.Push(GridLength::Pixels(126));
    heading_ = body->Add<TextBlock>("Nexora Browser", FontRole::Display);
    heading_->Margin = Thickness(0, 0, 0, 8);
    detail_ = body->Add<TextBlock>("A calm, native starting point for the web.", FontRole::Body);
    detail_->GridRow = 1;
    detail_->Foreground = Theme::TextSecondary;
    detail_->Wrap = true;

    ScrollViewer *page = body->Add<ScrollViewer>();
    page->GridRow = 2;
    page->Margin = Thickness(0, 22, 0, 0);
    items_ = new AnimatedPageText(FontRole::BodyLarge);
    items_->Wrap = true;
    items_->Ellipsis = false;
    items_->Padding = Thickness(24);
    items_->Background = Theme::CardBackground;
    items_->BorderColor = Theme::CardStroke;
    items_->CornerRadius = Theme::OverlayRadius;
    page->SetContent(items_);

    links_ = body->Add<ListView>();
    links_->GridRow = 3;
    links_->ItemHeight = 30;
    links_->SetItemCount(0);
    links_->PaintItem = [this](Painter &P, const Rect &R, size_t Index) {
        if (Index >= linkLabels_.Size() || Index >= linkTargets_.Size())
            return;
        P.DrawIcon(Icon::ChevronRight, Rect{R.X + 6, R.Y + (R.Height - 14) / 2, 14, 14}, Theme::Accent);
        hz::String Text = hz::String::Format("%s  ·  %s", linkLabels_[Index].CStr(), linkTargets_[Index].CStr());
        P.TextLine(GetFont(FontRole::Caption), Rect{R.X + 26, R.Y, R.Width - 32, R.Height}, Text.CStr(),
                   Theme::TextPrimary);
    };
    links_->OnItemInvoked = [this](size_t Index) { OpenLink(Index); };

    Grid *footer = root->Add<Grid>();
    footer->GridRow = 3;
    footer->Padding = Thickness(20, 8, 20, 10);
    footer->Columns.Push(GridLength::Stars(1));
    status_ = footer->Add<TextBlock>("Ready", FontRole::Caption);
    window_->SetContent(root);
}

void Browser::Status(const char *Text)
{
    status_->SetText(Text);
}

void Browser::SetTabTitle(const char *Title)
{
    if (tab_)
        tab_->SetText(Title);
}

void Browser::Go(const hz::String &Raw, bool Record)
{
    hz::String Address = Normalize(Raw);
    BrowserUrl::Url Parsed;
    if (Address.Empty() || BrowserUrl::ParseUrl(Address.CStr(), &Parsed) != BrowserUrl::UrlError::None) {
        Status("Enter an HTTP address, file:///C:/... URL, or an absolute local path");
        HzPrint("browser: rejected invalid address");
        return;
    }
    bool Stored = true;
    if (Record) {
        if (hasHistory_ && index_ + 1 < history_.Size())
            while (history_.Size() > index_ + 1)
                history_.Pop();
        if (history_.Empty() || history_.Back() != Address)
            history_.Push(Address);
        if (history_.Size() > MaxHistory)
            history_.Erase(0);
        index_ = history_.Size() - 1;
        hasHistory_ = !history_.Empty();
        Stored = SaveLines(HistoryFile, history_);
    }
    address_->SetText(Address.CStr(), false);
    currentAddress_ = Address;
    if (BrowserUrl::IsLocalFile(Parsed)) {
        ShowLocalFile(Address);
        UpdateButtons();
        return;
    }
    page_ = Page::Start;
    SetLinks(BrowserUrl::HtmlDocument{});
    uint8_t AddressBytes[4] = {0};
    STATUS ResolveStatus = HzResolveHostA(Parsed.host, AddressBytes);
    heading_->SetText("Internet page loading unavailable");
    SetTabTitle("Remote page");
    if (HZ_SUCCESS(ResolveStatus)) {
        detail_->SetText(hz::String::Format("DNS resolved %s to %u.%u.%u.%u.",
                                             Parsed.host, AddressBytes[0], AddressBytes[1], AddressBytes[2],
                                             AddressBytes[3]).CStr());
        items_->SetText("DNS is working, but Nexora does not yet have an application TCP connection, HTTP client, or verified TLS implementation. No page request was sent.");
        HzPrint("browser: DNS resolved %s to %u.%u.%u.%u; no page fetched", Parsed.host,
                AddressBytes[0], AddressBytes[1], AddressBytes[2], AddressBytes[3]);
    } else {
        detail_->SetText(hz::String::Format("DNS lookup for %s failed (0x%08X).", Parsed.host,
                                             (uint32_t)ResolveStatus).CStr());
        items_->SetText("The operating system could not resolve this host. Check the network interface and DNS configuration. Remote page fetching also requires TCP, HTTP, and verified TLS, which are not available yet.");
        HzPrint("browser: DNS lookup failed for %s (0x%08X)", Parsed.host, (uint32_t)ResolveStatus);
    }
    items_->Reveal();
    window_->SetTitle(hz::String::Format("%s - Nexora Browser", Address.CStr()).CStr());
    Status(Stored ? (HZ_SUCCESS(ResolveStatus) ? "DNS resolved · page not loaded (TCP/TLS unavailable)"
                                                : "DNS lookup failed · page not loaded")
                  : "Not loaded · browser profile is read-only");
    UpdateButtons();
}

void Browser::SetLinks(const BrowserUrl::HtmlDocument &Document)
{
    linkTargets_.Clear();
    linkLabels_.Clear();
    for (size_t Index = 0; Index < Document.LinkCount; ++Index) {
        if (!Document.Links[Index].Href[0])
            continue;
        linkLabels_.Push(hz::String(Document.Links[Index].Text[0] ? Document.Links[Index].Text
                                                                  : Document.Links[Index].Href));
        linkTargets_.Push(hz::String(Document.Links[Index].Href));
    }
    links_->SetItemCount(linkTargets_.Size());
    links_->SetVisible(!linkTargets_.Empty());
}

void Browser::ShowLocalFile(const hz::String &Address)
{
    page_ = Page::Start;
    SetTabTitle("Local file");
    BrowserUrl::Url Parsed;
    hz::String Path, Source;
    if (BrowserUrl::ParseUrl(Address.CStr(), &Parsed) != BrowserUrl::UrlError::None ||
        !BrowserUrl::IsLocalFile(Parsed) || !DecodeLocalPath(Parsed.path, Path)) {
        heading_->SetText("Invalid local file address");
        detail_->SetText("Use an absolute local path such as file:///C:/Users/Name/Documents/page.html.");
        items_->SetText("");
        SetLinks(BrowserUrl::HtmlDocument{});
        Status("Invalid local file URL");
        return;
    }
    STATUS ReadStatus = hz::fs::ReadAll(Path, Source, BrowserUrl::HtmlMaximumSource);
    if (!HZ_SUCCESS(ReadStatus)) {
        heading_->SetText("Local file could not be opened");
        detail_->SetText(Path.CStr());
        items_->SetText(hz::String::Format("%s (0x%08X).", hz::fs::StatusText(ReadStatus),
                                           (uint32_t)ReadStatus).CStr());
        SetLinks(BrowserUrl::HtmlDocument{});
        Status("Local file open failed");
        HzPrint("browser: local read failed for %s (0x%08X)", Path.CStr(), (uint32_t)ReadStatus);
        return;
    }
    currentAddress_ = Address;
    address_->SetText(Address.CStr(), false);
    bool Html = hz::fs::Extension(Path) == "html" || hz::fs::Extension(Path) == "htm";
    if (!Html) {
        items_->SetText(Source.CStr());
        heading_->SetText(hz::fs::FileName(Path).CStr());
        detail_->SetText("Local text file · no active content executed.");
        SetLinks(BrowserUrl::HtmlDocument{});
    } else {
        if (!BrowserUrl::ParseHtml(Source.CStr(), Source.Length(), &document_)) {
            heading_->SetText("HTML document is too large or invalid");
            detail_->SetText(Path.CStr());
            items_->SetText("Local HTML source is limited to 64 KiB.");
            SetLinks(BrowserUrl::HtmlDocument{});
            Status("HTML source exceeds the renderer limit");
            return;
        }
        heading_->SetText(document_.Title[0] ? document_.Title : hz::fs::FileName(Path).CStr());
        detail_->SetText(document_.UnsupportedContent
                             ? "Local HTML · text-only rendering; styles, scripts, images, and embedded content are not executed."
                             : "Local HTML · text-only rendering.");
        items_->SetText(document_.Text);
        SetLinks(document_);
    }
    SetTabTitle(heading_->Text().CStr());
    items_->Foreground = Theme::TextPrimary;
    items_->Reveal();
    window_->SetTitle(hz::String::Format("%s - Nexora Browser", heading_->Text().CStr()).CStr());
    Status("Local file loaded · active content disabled");
    HzPrint("browser: local file loaded %s (%u bytes)", Path.CStr(), (uint32_t)Source.Length());
    if (Html)
        HzPrint("browser: local HTML parsed title=%s links=%u", heading_->Text().CStr(),
                (uint32_t)linkTargets_.Size());
}

void Browser::OpenLink(size_t Index)
{
    if (Index >= linkTargets_.Size())
        return;
    const hz::String &Href = linkTargets_[Index];
    if (StartsWith(Href.CStr(), "http://") || StartsWith(Href.CStr(), "https://") ||
        StartsWith(Href.CStr(), "file:///")) {
        Go(Href);
        return;
    }
    BrowserUrl::Url Current;
    if (BrowserUrl::ParseUrl(currentAddress_.CStr(), &Current) != BrowserUrl::UrlError::None ||
        !BrowserUrl::IsLocalFile(Current)) {
        Status("This link type is not supported");
        return;
    }
    hz::String Base;
    if (Href[0] == '/') {
        Base = hz::String::Format("%c:\\", Current.path[0]);
        Base.Append(Href.CStr() + 1);
    } else {
        Base = hz::fs::Parent(hz::String(Current.path));
        Base = hz::fs::Resolve(Base, Href.CStr());
    }
    Go(FileUrl(Base));
}

void Browser::StartPage()
{
    page_ = Page::Start;
    currentAddress_.Clear();
    SetTabTitle("Start page");
    address_->SetText("", false);
    nexora::browser::HtmlDocument Document;
    if (!nexora::browser::ParseHtml(HomePage, sizeof(HomePage) - 1, &Document)) {
        heading_->SetText("HTML rendering failed");
        detail_->SetText("The built-in page exceeded the native renderer's input limit.");
        items_->SetText("");
        Status("HTML renderer error");
        return;
    }
    heading_->SetText(Document.Title);
    detail_->SetText("Local files and DNS lookup work. Web page loading does not: there is no TCP, HTTP or TLS yet.");
    items_->SetText(Document.Text);
    items_->Foreground = Theme::TextPrimary;
    SetLinks(Document);
    items_->Reveal();
    window_->SetTitle("Nexora Browser");
    Status("Ready · local files and DNS lookup available · web page networking unavailable");
    UpdateButtons();
}

void Browser::ShowList(Page View)
{
    page_ = View;
    SetLinks(BrowserUrl::HtmlDocument{});
    hz::String Lines;
    if (View == Page::History) {
        LoadLines(HistoryFile, history_, MaxHistory);
        heading_->SetText("History");
        SetTabTitle("History");
        detail_->SetText("Recent addresses in this local profile.");
        for (const auto &Item : history_) {
            Lines.Append(Item);
            Lines.Append("\n");
        }
        Status("History · Ctrl+H");
    } else if (View == Page::Bookmarks) {
        LoadLines(BookmarksFile, bookmarks_, MaxBookmarks);
        heading_->SetText("Bookmarks");
        SetTabTitle("Bookmarks");
        detail_->SetText("Saved locally in the default profile. Use Ctrl+D to add or remove the current address.");
        for (const auto &Item : bookmarks_) {
            Lines.Append(Item);
            Lines.Append("\n");
        }
        Status("Bookmarks · Ctrl+B");
    } else {
        heading_->SetText("Downloads");
        SetTabTitle("Downloads");
        detail_->SetText("Files in the profile Downloads folder. Internet downloads are unavailable until the network and engine layers exist.");
        hz::Vector<hz::fs::Entry> Files;
        if (HZ_SUCCESS(hz::fs::List(hz::String(Downloads), Files)))
            for (const auto &File : Files) {
                Lines.Append(File.Name);
                Lines.Append("\n");
            }
        Status("Downloads · network download service not connected");
    }
    if (Lines.Empty())
        Lines = View == Page::History ? "No history yet." : View == Page::Bookmarks ? "No bookmarks yet." : "No files in Downloads.";
    items_->SetText(Lines.CStr());
    items_->Reveal();
}

void Browser::Back()
{
    if (!hasHistory_ || index_ == 0)
        return;
    --index_;
    Go(history_[index_], false);
    UpdateButtons();
}

void Browser::Forward()
{
    if (!hasHistory_ || index_ + 1 >= history_.Size())
        return;
    ++index_;
    Go(history_[index_], false);
    UpdateButtons();
}

void Browser::Bookmark()
{
    if (page_ != Page::Start || currentAddress_.Empty() || !hasHistory_ || history_.Empty()) {
        Status("Open an address before bookmarking it");
        return;
    }
    const hz::String Current = history_[index_];
    for (size_t i = 0; i < bookmarks_.Size(); ++i) {
        if (bookmarks_[i] == Current) {
            bookmarks_.Erase(i);
            SaveLines(BookmarksFile, bookmarks_);
            Status("Bookmark removed");
            if (page_ == Page::Bookmarks)
                ShowList(page_);
            return;
        }
    }
    if (bookmarks_.Size() >= MaxBookmarks) {
        Status("Bookmark limit reached");
        return;
    }
    bookmarks_.Push(Current);
    SaveLines(BookmarksFile, bookmarks_);
    Status("Bookmark saved to the default local profile");
    if (page_ == Page::Bookmarks)
        ShowList(page_);
}

void Browser::Refresh()
{
    if (page_ == Page::Start) {
        if (!currentAddress_.Empty())
            Go(currentAddress_, false);
        else
            StartPage();
    } else {
        ShowList(page_);
    }
}

void Browser::UpdateButtons()
{
    back_->SetEnabled(hasHistory_ && index_ > 0);
    forward_->SetEnabled(hasHistory_ && index_ + 1 < history_.Size());
    reload_->SetEnabled(page_ != Page::Start || !currentAddress_.Empty());
    bookmark_->SetEnabled(page_ == Page::Start && !currentAddress_.Empty());
}
} // namespace

extern "C" int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    Browser *App = new Browser();
    if (!App->Create()) {
        HzPrint("browser: failed to create window");
        return 1;
    }
    HzPrint("browser: started; local renderer and DNS lookup ready; page networking unavailable");
    return RunMessageLoop();
}
