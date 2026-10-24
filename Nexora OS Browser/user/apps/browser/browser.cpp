/*
 * Nexora Browser — native shell prototype for Nexora OS.
 *
 * This UI intentionally reports when the engine is unavailable. It does not fake successful
 * network navigation or pretend that addresses were fetched. See docs/ENGINE_PORT.md.
 */
#include "hzapps.h"
#include "hzfile.h"
#include "hzui.h"
#include <string.h>

using namespace hzui;

namespace {
constexpr size_t MaxHistory = 300;
constexpr size_t MaxBookmarks = 100;
const char *Profile = "C:\\Nexora\\Profiles\\Default\\Browser";
const char *Downloads = "C:\\Nexora\\Profiles\\Default\\Downloads";
const char *HistoryFile = "C:\\Nexora\\Profiles\\Default\\Browser\\history.txt";
const char *BookmarksFile = "C:\\Nexora\\Profiles\\Default\\Browser\\bookmarks.txt";

bool HasLineBreak(const char *s) {
    for (; s && *s; ++s) if (*s == '\n' || *s == '\r') return true;
    return false;
}
bool Starts(const char *s, const char *p) { return s && p && strncmp(s, p, strlen(p)) == 0; }

hz::String Normalize(const hz::String &input) {
    if (input.Empty() || HasLineBreak(input.CStr())) return hz::String();
    if (Starts(input.CStr(), "https://") || Starts(input.CStr(), "http://")) return input;
    bool domain = false;
    for (const char *p = input.CStr(); *p; ++p) {
        if (*p == '.' && p != input.CStr() && p[1]) domain = true;
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '-' || *p == '.' || *p == '/' ||
               *p == ':' || *p == '?' || *p == '&' || *p == '=' || *p == '%' ||
               *p == '_' || *p == '~' || *p == '#')) return hz::String();
    }
    return domain ? hz::String::Format("https://%s", input.CStr()) : hz::String();
}

void LoadLines(const char *path, hz::Vector<hz::String> &out, size_t limit) {
    out.Clear();
    hz::String text;
    if (!HZ_SUCCESS(hz::fs::ReadAll(hz::String(path), text, 256 * 1024))) return;
    const char *p = text.CStr(), *end = p + text.Length();
    while (p < end && out.Size() < limit) {
        const char *q = p;
        while (q < end && *q != '\n' && *q != '\r') ++q;
        if (q > p) {
            hz::String line(p, (size_t)(q - p));
            if (!HasLineBreak(line.CStr())) out.Push(hz::Move(line));
        }
        while (q < end && (*q == '\n' || *q == '\r')) ++q;
        p = q;
    }
}
bool SaveLines(const char *path, const hz::Vector<hz::String> &items) {
    hz::String text;
    for (const auto &item : items) {
        if (HasLineBreak(item.CStr())) continue;
        text.Append(item);
        text.Append('\n');
    }
    return HZ_SUCCESS(hz::fs::WriteAll(hz::String(path), text.CStr(), text.Length()));
}
bool EnsureDir(const char *path) {
    return hz::fs::Exists(hz::String(path)) ||
           HZ_SUCCESS(hz::fs::CreateDirectory(hz::String(path)));
}
void EnsureProfile() {
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
    void Go(const hz::String &raw, bool record = true);
    void StartPage();
    void ShowList(Page page);
    void Back();
    void Forward();
    void Bookmark();
    void Refresh();
    void UpdateButtons();
    void Status(const char *text);

    Window *window_ = nullptr;
    TextBox *address_ = nullptr;
    Button *back_ = nullptr, *forward_ = nullptr, *reload_ = nullptr, *bookmark_ = nullptr;
    TextBlock *status_ = nullptr;
    TextBlock *heading_ = nullptr, *detail_ = nullptr, *items_ = nullptr;
    hz::Vector<hz::String> history_, bookmarks_;
    size_t index_ = 0;
    bool hasHistory_ = false;
    Page page_ = Page::Start;
};

bool Browser::Create() {
    EnsureProfile();
    LoadLines(HistoryFile, history_, MaxHistory);
    LoadLines(BookmarksFile, bookmarks_, MaxBookmarks);
    hasHistory_ = !history_.Empty();
    index_ = hasHistory_ ? history_.Size() - 1 : 0;

    window_ = new Window("Nexora Browser", 1120, 740);
    window_->SetMinimumSize(760, 500);
    Build();
    window_->OnClosed = [this]() { SaveLines(HistoryFile, history_); SaveLines(BookmarksFile, bookmarks_); NtUserPostQuitMessage(0); };
    window_->OnUnhandledKey = [this](KeyEvent &e) {
        if (e.Ctrl() && e.Key == 'L') { e.Handled = true; window_->SetFocus(address_, true); address_->SelectAll(); }
        else if (e.Ctrl() && e.Key == 'D') { e.Handled = true; Bookmark(); }
        else if (e.Ctrl() && e.Key == 'H') { e.Handled = true; ShowList(Page::History); }
        else if (e.Ctrl() && e.Key == 'B') { e.Handled = true; ShowList(Page::Bookmarks); }
        else if (e.Ctrl() && e.Key == 'J') { e.Handled = true; ShowList(Page::Downloads); }
        else if (e.Ctrl() && e.Key == '1') { e.Handled = true; StartPage(); }
        else if (e.Alt() && e.Key == HZ_VK_LEFT) { e.Handled = true; Back(); }
        else if (e.Alt() && e.Key == HZ_VK_RIGHT) { e.Handled = true; Forward(); }
        else if (e.Key == HZ_VK_F1 + 4) { e.Handled = true; Refresh(); }
    };
    StartPage();
    if (!window_->Show()) return false;
    window_->SetFocus(address_);
    return true;
}

void Browser::Build() {
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
    tab->Glyph = Icon::Globe;
    tab->SetEnabled(false);
    Button *newTab = tabs->Add<Button>("New tab", ButtonStyle::Subtle);
    newTab->GridColumn = 2;
    newTab->Glyph = Icon::Add;
    newTab->OnClick = [this]() { StartPage(); };

    Grid *bar = root->Add<Grid>();
    bar->Padding = Thickness(12, 4, 12, 10);
    bar->ColumnSpacing = 4;
    for (int i = 0; i < 7; ++i) bar->Columns.Push(GridLength::AutoSize());
    bar->Columns.Push(GridLength::Stars(1));
    bar->Columns.Push(GridLength::AutoSize());

    back_ = bar->Add<Button>("", ButtonStyle::Subtle);
    back_->Glyph = Icon::ChevronLeft; back_->ToolTip = "Back · Alt+Left";
    back_->OnClick = [this]() { Back(); };
    forward_ = bar->Add<Button>("", ButtonStyle::Subtle);
    forward_->GridColumn = 1; forward_->Glyph = Icon::ChevronRight; forward_->ToolTip = "Forward · Alt+Right";
    forward_->OnClick = [this]() { Forward(); };
    reload_ = bar->Add<Button>("", ButtonStyle::Subtle);
    reload_->GridColumn = 2; reload_->Glyph = Icon::Refresh; reload_->ToolTip = "Reload · F5";
    reload_->OnClick = [this]() { Refresh(); };
    Button *home = bar->Add<Button>("", ButtonStyle::Subtle);
    home->GridColumn = 3; home->Glyph = Icon::Home; home->ToolTip = "Home · Ctrl+1";
    home->OnClick = [this]() { StartPage(); };
    Button *hist = bar->Add<Button>("", ButtonStyle::Subtle);
    hist->GridColumn = 4; hist->Glyph = Icon::Clock; hist->ToolTip = "History · Ctrl+H";
    hist->OnClick = [this]() { ShowList(Page::History); };
    Button *downloads = bar->Add<Button>("", ButtonStyle::Subtle);
    downloads->GridColumn = 5; downloads->Glyph = Icon::Download; downloads->ToolTip = "Downloads · Ctrl+J";
    downloads->OnClick = [this]() { ShowList(Page::Downloads); };
    Button *bookmarks = bar->Add<Button>("", ButtonStyle::Subtle);
    bookmarks->GridColumn = 6; bookmarks->Glyph = Icon::Star; bookmarks->ToolTip = "Bookmarks · Ctrl+B";
    bookmarks->OnClick = [this]() { ShowList(Page::Bookmarks); };
    address_ = bar->Add<TextBox>("Search or enter address");
    address_->GridColumn = 7; address_->MaxLength = 2048;
    address_->OnSubmit = [this](const hz::String &text) { Go(text); };
    bookmark_ = bar->Add<Button>("", ButtonStyle::Subtle);
    bookmark_->GridColumn = 8; bookmark_->Glyph = Icon::Star; bookmark_->ToolTip = "Bookmark · Ctrl+D";
    bookmark_->OnClick = [this]() { Bookmark(); };

    Grid *body = root->Add<Grid>();
    body->GridRow = 2;
    body->Padding = Thickness(28, 28, 28, 24);
    body->Rows.Push(GridLength::AutoSize());
    body->Rows.Push(GridLength::AutoSize());
    body->Rows.Push(GridLength::Stars(1));
    heading_ = body->Add<TextBlock>("Nexora Browser", FontRole::Display);
    heading_->Margin = Thickness(0, 0, 0, 10);
    detail_ = body->Add<TextBlock>("A calm, native starting point for the web.", FontRole::Body);
    detail_->GridRow = 1;
    items_ = body->Add<TextBlock>("", FontRole::Body);
    items_->GridRow = 2;
    items_->Margin = Thickness(0, 24, 0, 0);

    Grid *footer = root->Add<Grid>();
    footer->GridRow = 3;
    footer->Padding = Thickness(16, 5, 16, 7);
    footer->Columns.Push(GridLength::Stars(1));
    status_ = footer->Add<TextBlock>("Ready", FontRole::Caption);
    window_->SetContent(root);
}

void Browser::Status(const char *text) { status_->SetText(text); }

void Browser::Go(const hz::String &raw, bool record) {
    hz::String url = Normalize(raw);
    if (url.Empty()) { Status("Enter a valid web address, for example https://example.com"); return; }
    if (record) {
        if (hasHistory_ && index_ + 1 < history_.Size())
            while (history_.Size() > index_ + 1) history_.Pop();
        if (history_.Empty() || history_.Back() != url) history_.Push(url);
        if (history_.Size() > MaxHistory) history_.Erase(0);
        index_ = history_.Size() - 1;
        hasHistory_ = !history_.Empty();
        SaveLines(HistoryFile, history_);
    }
    address_->SetText(url.CStr(), false);
    page_ = Page::Start;
    heading_->SetText("Web engine not connected");
    detail_->SetText("This address is saved in history, but Nexora OS does not yet provide the DNS, TCP, TLS, HTTP, and HTML renderer services required to load it.");
    items_->SetText("");
    window_->SetTitle(hz::String::Format("%s - Nexora Browser", url.CStr()).CStr());
    Status("Address saved · no network request was made");
    UpdateButtons();
}
void Browser::StartPage() {
    page_ = Page::Start;
    address_->SetText("", false);
    heading_->SetText("Nexora Browser");
    detail_->SetText("A quiet, native starting point for the web. History and bookmarks are stored in the default local profile.");
    items_->SetText("Ctrl+L  Address     Ctrl+D  Bookmark     Ctrl+H  History     Ctrl+B  Bookmarks     Ctrl+J  Downloads");
    window_->SetTitle("Nexora Browser");
    Status("Ready · web engine not connected");
    UpdateButtons();
}
void Browser::ShowList(Page page) {
    page_ = page;
    hz::String lines;
    if (page == Page::History) {
        LoadLines(HistoryFile, history_, MaxHistory);
        heading_->SetText("History");
        detail_->SetText("Recent addresses in this local profile.");
        for (const auto &s : history_) { lines.Append(s); lines.Append("\n"); }
        Status("History · Ctrl+H");
    } else if (page == Page::Bookmarks) {
        LoadLines(BookmarksFile, bookmarks_, MaxBookmarks);
        heading_->SetText("Bookmarks");
        detail_->SetText("Saved locally in the default profile. Use Ctrl+D to add or remove the current address.");
        for (const auto &s : bookmarks_) { lines.Append(s); lines.Append("\n"); }
        Status("Bookmarks · Ctrl+B");
    } else {
        heading_->SetText("Downloads");
        detail_->SetText("Files in the profile Downloads folder. Internet downloads are unavailable until the network and engine layers exist.");
        hz::Vector<hz::fs::Entry> files;
        if (HZ_SUCCESS(hz::fs::List(hz::String(Downloads), files)))
            for (const auto &f : files) { lines.Append(f.Name); lines.Append("\n"); }
        Status("Downloads · network download service not connected");
    }
    if (lines.Empty()) lines = page == Page::History ? "No history yet." : page == Page::Bookmarks ? "No bookmarks yet." : "No files in Downloads.";
    items_->SetText(lines.CStr());
}
void Browser::Back() {
    if (!hasHistory_ || index_ == 0) return;
    --index_; Go(history_[index_], false); UpdateButtons();
}
void Browser::Forward() {
    if (!hasHistory_ || index_ + 1 >= history_.Size()) return;
    ++index_; Go(history_[index_], false); UpdateButtons();
}
void Browser::Bookmark() {
    if (!hasHistory_ || history_.Empty()) { Status("Open an address before bookmarking it"); return; }
    const hz::String current = history_[index_];
    for (size_t i = 0; i < bookmarks_.Size(); ++i) {
        if (bookmarks_[i] == current) {
            bookmarks_.Erase(i); SaveLines(BookmarksFile, bookmarks_);
            Status("Bookmark removed");
            if (page_ == Page::Bookmarks) ShowList(page_);
            return;
        }
    }
    if (bookmarks_.Size() >= MaxBookmarks) { Status("Bookmark limit reached"); return; }
    bookmarks_.Push(current); SaveLines(BookmarksFile, bookmarks_);
    Status("Bookmark saved to the default local profile");
    if (page_ == Page::Bookmarks) ShowList(page_);
}
void Browser::Refresh() {
    if (page_ == Page::Start) {
        if (hasHistory_ && !history_.Empty()) Go(history_[index_], false);
        else StartPage();
    } else ShowList(page_);
}
void Browser::UpdateButtons() {
    back_->SetEnabled(hasHistory_ && index_ > 0);
    forward_->SetEnabled(hasHistory_ && index_ + 1 < history_.Size());
    reload_->SetEnabled(hasHistory_ && !history_.Empty());
    bookmark_->SetEnabled(hasHistory_ && !history_.Empty());
}
} // namespace

extern "C" int main(int argc, char **argv) {
    (void)argc; (void)argv;
    Browser *app = new Browser();
    if (!app->Create()) { HzPrint("browser: failed to create window"); return 1; }
    return RunMessageLoop();
}
