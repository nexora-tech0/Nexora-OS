/*
 * Nexora Browser — native shell and bounded HTML text renderer for Nexora OS.
 *
 * This UI does not fake successful network navigation or pretend that addresses were fetched.
 * See docs/ENGINE_PORT.md.
 */
#include "hzapps.h"
#include "hzfile.h"
#include "hzui.h"
#include "html.h"
#include "url.h"

using namespace hzui;

namespace {
constexpr size_t MaxHistory = 300;
constexpr size_t MaxBookmarks = 100;
const char *Profile = "C:\\Nexora\\Profiles\\Default\\Browser";
const char *Downloads = "C:\\Nexora\\Profiles\\Default\\Downloads";
const char *HistoryFile = "C:\\Nexora\\Profiles\\Default\\Browser\\history.txt";
const char *BookmarksFile = "C:\\Nexora\\Profiles\\Default\\Browser\\bookmarks.txt";
const char HomePage[] =
    "<!doctype html><html><head><title>Nexora Browser</title></head><body>"
    "<h1>Welcome</h1>"
    "<p>A native browser shell for Nexora OS.</p>"
    "<p>The current page pipeline extracts headings, paragraphs, links and basic text entities.</p>"
    "<p>Remote pages are not available yet. DNS, TCP, an HTTP client and a full layout engine remain to be built.</p>"
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
    if (StartsWith(Input.CStr(), "https://") || StartsWith(Input.CStr(), "http://"))
        return Input;

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
    void Status(const char *Text);

    Window *window_ = nullptr;
    TextBox *address_ = nullptr;
    Button *back_ = nullptr, *forward_ = nullptr, *reload_ = nullptr, *bookmark_ = nullptr;
    TextBlock *status_ = nullptr;
    TextBlock *heading_ = nullptr, *detail_ = nullptr;
    AnimatedPageText *items_ = nullptr;
    hz::Vector<hz::String> history_, bookmarks_;
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
    tabs->Padding = Thickness(14, 8, 14, 2);
    tabs->Columns.Push(GridLength::Pixels(220));
    tabs->Columns.Push(GridLength::Stars(1));
    tabs->Columns.Push(GridLength::AutoSize());
    Button *tab = tabs->Add<Button>("Nexora Start", ButtonStyle::Subtle);
    tab->Glyph = Icon::Nexora;
    tab->SetEnabled(false);
    Button *newTab = tabs->Add<Button>("New tab", ButtonStyle::Subtle);
    newTab->GridColumn = 2;
    newTab->Glyph = Icon::Add;
    newTab->OnClick = [this]() { StartPage(); };

    Grid *bar = root->Add<Grid>();
    bar->GridRow = 1;
    bar->Padding = Thickness(12, 4, 12, 10);
    bar->ColumnSpacing = 4;
    for (int i = 0; i < 7; ++i)
        bar->Columns.Push(GridLength::AutoSize());
    bar->Columns.Push(GridLength::Stars(1));
    bar->Columns.Push(GridLength::AutoSize());

    back_ = bar->Add<Button>("", ButtonStyle::Subtle);
    back_->Glyph = Icon::ChevronLeft;
    back_->ToolTip = "Back · Alt+Left";
    back_->OnClick = [this]() { Back(); };
    forward_ = bar->Add<Button>("", ButtonStyle::Subtle);
    forward_->GridColumn = 1;
    forward_->Glyph = Icon::ChevronRight;
    forward_->ToolTip = "Forward · Alt+Right";
    forward_->OnClick = [this]() { Forward(); };
    reload_ = bar->Add<Button>("", ButtonStyle::Subtle);
    reload_->GridColumn = 2;
    reload_->Glyph = Icon::Refresh;
    reload_->ToolTip = "Reload · F5";
    reload_->OnClick = [this]() { Refresh(); };
    Button *home = bar->Add<Button>("", ButtonStyle::Subtle);
    home->GridColumn = 3;
    home->Glyph = Icon::Home;
    home->ToolTip = "Home · Ctrl+1";
    home->OnClick = [this]() { StartPage(); };
    Button *hist = bar->Add<Button>("", ButtonStyle::Subtle);
    hist->GridColumn = 4;
    hist->Glyph = Icon::Clock;
    hist->ToolTip = "History · Ctrl+H";
    hist->OnClick = [this]() { ShowList(Page::History); };
    Button *downloads = bar->Add<Button>("", ButtonStyle::Subtle);
    downloads->GridColumn = 5;
    downloads->Glyph = Icon::Folder;
    downloads->ToolTip = "Downloads · Ctrl+J";
    downloads->OnClick = [this]() { ShowList(Page::Downloads); };
    Button *bookmarks = bar->Add<Button>("", ButtonStyle::Subtle);
    bookmarks->GridColumn = 6;
    bookmarks->Glyph = Icon::Star;
    bookmarks->ToolTip = "Bookmarks · Ctrl+B";
    bookmarks->OnClick = [this]() { ShowList(Page::Bookmarks); };
    address_ = bar->Add<TextBox>("Search or enter address");
    address_->GridColumn = 7;
    address_->MaxLength = 2048;
    address_->OnSubmit = [this](const hz::String &Text) { Go(Text); };
    bookmark_ = bar->Add<Button>("", ButtonStyle::Subtle);
    bookmark_->GridColumn = 8;
    bookmark_->Glyph = Icon::Star;
    bookmark_->ToolTip = "Bookmark · Ctrl+D";
    bookmark_->OnClick = [this]() { Bookmark(); };

    Grid *body = root->Add<Grid>();
    body->GridRow = 2;
    body->Padding = Thickness(32, 26, 32, 24);
    body->Rows.Push(GridLength::AutoSize());
    body->Rows.Push(GridLength::AutoSize());
    body->Rows.Push(GridLength::Stars(1));
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
    items_->Padding = Thickness(20);
    items_->Background = Theme::CardBackground;
    items_->BorderColor = Theme::CardStroke;
    items_->CornerRadius = Theme::OverlayRadius;
    page->SetContent(items_);

    Grid *footer = root->Add<Grid>();
    footer->GridRow = 3;
    footer->Padding = Thickness(16, 5, 16, 7);
    footer->Columns.Push(GridLength::Stars(1));
    status_ = footer->Add<TextBlock>("Ready", FontRole::Caption);
    window_->SetContent(root);
}

void Browser::Status(const char *Text)
{
    status_->SetText(Text);
}

void Browser::Go(const hz::String &Raw, bool Record)
{
    hz::String Address = Normalize(Raw);
    nexora::browser::Url Parsed;
    if (Address.Empty() || nexora::browser::ParseUrl(Address.CStr(), &Parsed) != nexora::browser::UrlError::None) {
        Status("Enter a valid HTTP or HTTPS address, for example https://example.com");
        HzPrint("browser: rejected invalid web address");
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
    page_ = Page::Start;
    heading_->SetText("Network access not available");
    detail_->SetText("The address was validated, but the network stack is not connected to browser navigation.");
    items_->SetText("No remote page was loaded.\n\nNexora does not yet provide DNS resolution, TCP connections, or an HTTP client to this app. HTTPS also requires verified TLS support.");
    items_->Reveal();
    window_->SetTitle(hz::String::Format("%s - Nexora Browser", Address.CStr()).CStr());
    Status(Stored ? "Address saved · no network request was made"
                  : "Address validated · profile is read-only; page not loaded");
    HzPrint("browser: address validated; no page request");
    UpdateButtons();
}

void Browser::StartPage()
{
    page_ = Page::Start;
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
    detail_->SetText("A minimal native browser shell. Enter an address to check its format.");
    items_->SetText(Document.Text);
    items_->Foreground = Theme::TextPrimary;
    items_->Reveal();
    window_->SetTitle("Nexora Browser");
    Status("Ready · HTML text renderer active · network unavailable");
    UpdateButtons();
}

void Browser::ShowList(Page View)
{
    page_ = View;
    hz::String Lines;
    if (View == Page::History) {
        LoadLines(HistoryFile, history_, MaxHistory);
        heading_->SetText("History");
        detail_->SetText("Recent addresses in this local profile.");
        for (const auto &Item : history_) {
            Lines.Append(Item);
            Lines.Append("\n");
        }
        Status("History · Ctrl+H");
    } else if (View == Page::Bookmarks) {
        LoadLines(BookmarksFile, bookmarks_, MaxBookmarks);
        heading_->SetText("Bookmarks");
        detail_->SetText("Saved locally in the default profile. Use Ctrl+D to add or remove the current address.");
        for (const auto &Item : bookmarks_) {
            Lines.Append(Item);
            Lines.Append("\n");
        }
        Status("Bookmarks · Ctrl+B");
    } else {
        heading_->SetText("Downloads");
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
    if (!hasHistory_ || history_.Empty()) {
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
        if (hasHistory_ && !history_.Empty())
            Go(history_[index_], false);
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
    reload_->SetEnabled(hasHistory_ && !history_.Empty());
    bookmark_->SetEnabled(hasHistory_ && !history_.Empty());
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
    HzPrint("browser: started; HTML text renderer ready; network unavailable");
    return RunMessageLoop();
}
