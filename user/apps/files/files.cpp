/*
 * Nexora OS - user/apps/files/files.cpp
 * Files: browses volumes and folders.
 *
 * Navigation: Back, Forward and Up, a breadcrumb address bar (Ctrl+L types a path instead) and an
 * in-folder search box (Ctrl+F) that filters by name. Three views (Details, List, Tiles) and a
 * sort by name, date modified, type or size - ascending or descending, folders always first -
 * chosen from the command bar, the column headers or the context menus, and kept in
 * C:\Nexora\Settings\files.ini.
 *
 * The sidebar lists Quick access (Home, Desktop, Documents - only the ones that exist) above the
 * volumes, and the row for the folder being shown is highlighted.
 * A command bar (New folder, New file, cut, copy, paste, rename, delete, sort, view) and
 * target-sensitive context menus on items and on the background. Enter or a double-click opens a
 * folder, runs a program or opens a document with the program whose manifest lists its extension.
 * An archive (.zip, .tar, .gz, .tgz) offers "Extract all..." in its context menu, which unpacks it
 * into a folder named after it (hz::archive, which refuses names that would escape the folder and
 * verifies each stream's CRC-32). Typing a name selects the first match (type-ahead). Ctrl+X/C/V
 * cut, copy and paste a file or a folder with everything in it (the clipboard of files belongs to
 * this window); a pasted copy next to its original is named "X - Copy". Delete removes a folder
 * with its content after a confirmation that counts it. Changes are possible on writable volumes
 * only. Navigation and actions are logged ("files: ...") for the smoke tests.
 */
#include "hzapps.h"
#include "hzarchive.h"
#include "hzfile.h"
#include "hzui.h"

using namespace hzui;

namespace {

/* ---- Views, sorting and the Details columns -------------------------------------------------- */

enum class FileView : uint8_t { Details, List, Tiles };

enum SortKey : uint8_t { SortName = 0, SortDate, SortType, SortSize, SortKeyCount };

const char *SortKeyName(int Key)
{
    switch (Key) {
    case SortName: return "Name";
    case SortDate: return "Date modified";
    case SortType: return "Type";
    default: return "Size";
    }
}

const char *ViewName(FileView V)
{
    switch (V) {
    case FileView::Details: return "Details";
    case FileView::List: return "List";
    default: return "Tiles";
    }
}

/* The Details columns are measured from the row's right edge, so "Name" takes what is left. */
namespace Col {
constexpr int32_t IconX = 10, IconSize = 18, NameX = 38;
constexpr int32_t DateFromRight = 430, DateWidth = 140;
constexpr int32_t TypeFromRight = 280, TypeWidth = 160;
constexpr int32_t SizeFromRight = 100, SizeWidth = 100;
constexpr int32_t NameGap = 44; /* between the name column and the date column */
}  // namespace Col

struct Columns {
    int32_t NameX, NameW, DateX, TypeX, SizeX;
};

/* Heading rows are 8 px wider on the right than item rows (the list is inset, so the columns
   stay aligned with what PaintItem draws). */
Columns ColumnsOf(const Rect &R, bool Heading)
{
    int32_t Right = R.Right() - (Heading ? 22 : 14);
    Columns C;
    C.NameX = R.X + Col::NameX;
    C.DateX = Right - Col::DateFromRight;
    C.TypeX = Right - Col::TypeFromRight;
    C.SizeX = Right - Col::SizeFromRight;
    C.NameW = hz::Max(C.DateX - Col::NameGap - C.NameX, 60);
    return C;
}

/* One row of the sidebar: the Quick access folders first (Home, Documents, Desktop), then the
   volumes. Both kinds live in one list so the current folder can be traced back to its row. */
struct Place {
    Icon Kind = Icon::Folder;
    hz::String Name;
    hz::String Detail;
    hz::String Path;
};

const char *TypeName(const hz::fs::Entry &E)
{
    if (E.Directory)
        return "Folder";
    hz::String Ext = hz::fs::Extension(E.Name);
    if (Ext == "exe")
        return "Program";
    if (Ext == "zip")
        return "Compressed folder";
    if (Ext == "tar" || Ext == "gz" || Ext == "tgz")
        return "Archive";
    if (Ext == "png" || Ext == "jpg" || Ext == "jpeg" || Ext == "bmp" || Ext == "gif")
        return "Image";
    if (Ext == "txt" || Ext == "md" || Ext == "log")
        return "Text document";
    if (Ext == "app")
        return "Program manifest";
    if (Ext == "ttf")
        return "Font";
    if (Ext == "sym")
        return "Symbols";
    if (Ext.Empty())
        return "File";
    return "Document";
}

Icon IconFor(const hz::fs::Entry &E)
{
    if (E.Directory)
        return Icon::Folder;
    hz::String Ext = hz::fs::Extension(E.Name);
    if (Ext == "exe")
        return Icon::Prompt;
    if (hz::archive::IsArchive(E.Name))
        return Icon::Archive;
    if (Ext == "png" || Ext == "jpg" || Ext == "jpeg" || Ext == "bmp" || Ext == "gif")
        return Icon::Image;
    return Icon::Document;
}

Color IconColorFor(const hz::fs::Entry &E)
{
    if (E.Directory)
        return 0xFFFFC66B;
    hz::String Ext = hz::fs::Extension(E.Name);
    if (Ext == "exe")
        return 0xFF8FB8FF;
    if (hz::archive::IsArchive(E.Name))
        return 0xFFD9B8FF;
    if (Ext == "png" || Ext == "jpg" || Ext == "jpeg" || Ext == "bmp" || Ext == "gif")
        return 0xFF8FE0C0;
    return Theme::TextSecondary;
}

/* ASCII lower case of one character, as the file systems compare names. */
unsigned char Lower(unsigned char C)
{
    return C >= 'A' && C <= 'Z' ? (unsigned char)(C + 32) : C;
}

int CompareText(const char *A, const char *B)
{
    for (;;) {
        unsigned char Ca = Lower((unsigned char)*A++), Cb = Lower((unsigned char)*B++);
        if (Ca != Cb)
            return Ca < Cb ? -1 : 1;
        if (!Ca)
            return 0;
    }
}

/* The column comparison alone (0 when equal). */
int CompareColumn(const hz::fs::Entry &A, const hz::fs::Entry &B, int Key)
{
    switch (Key) {
    case SortDate:
        return A.Modified < B.Modified ? -1 : A.Modified > B.Modified ? 1 : 0;
    case SortSize:
        return A.Size < B.Size ? -1 : A.Size > B.Size ? 1 : 0;
    case SortType:
        return CompareText(TypeName(A), TypeName(B));
    default:
        return CompareText(A.Name.CStr(), B.Name.CStr());
    }
}

/* Folders first (never reversed), then the column, then the name. */
bool EntryLess(const hz::fs::Entry &A, const hz::fs::Entry &B, int Key, bool Descending)
{
    if (A.Directory != B.Directory)
        return A.Directory;
    int R = CompareColumn(A, B, Key);
    if (R == 0)
        R = CompareText(A.Name.CStr(), B.Name.CStr());
    if (R == 0)
        return false;
    return Descending ? R > 0 : R < 0;
}

void SortEntries(hz::Vector<hz::fs::Entry> &Entries, int Key, bool Descending)
{
    for (size_t i = 1; i < Entries.Size(); i++) {
        hz::fs::Entry Value = hz::Move(Entries[i]);
        size_t j = i;
        while (j > 0 && EntryLess(Value, Entries[j - 1], Key, Descending)) {
            Entries[j] = hz::Move(Entries[j - 1]);
            j--;
        }
        Entries[j] = hz::Move(Value);
    }
}

/* A file name that can be created on the target: no path separators, control characters or the
   characters the file systems reserve, no trailing dot or space, not "." or "..". */
bool ValidFileName(const char *Name)
{
    size_t Length = strlen(Name);
    if (Length == 0 || Length > 200)
        return false;
    if (strcmp(Name, ".") == 0 || strcmp(Name, "..") == 0)
        return false;
    if (Name[Length - 1] == '.' || Name[Length - 1] == ' ')
        return false;
    /* Windows forbids these characters in a name (plus control characters). */
    static const char *Invalid = "\\/:*?\"<>|";
    for (size_t i = 0; i < Length; i++) {
        unsigned char C = (unsigned char)Name[i];
        if (C < 0x20 || C == 0x7F)
            return false;
        for (const char *P = Invalid; *P; P++)
            if ((unsigned char)*P == C)
                return false;
    }
    return true;
}

/* ---- The file list --------------------------------------------------------------------------- */

/* The list with type-ahead: characters typed within a second form a prefix to match. */
class FileList final : public ListView {
public:
    hz::Function<size_t(const hz::String &)> FindPrefix;
    hz::Function<void(KeyEvent &)> OnKey;
    hz::Function<void(size_t, Point)> OnRightClick;
    hz::Function<void(Point)> OnBackgroundRightClick;

    void OnPointerUp(PointerEvent &E) override
    {
        if (E.Button == HZ_MOUSE_RIGHT) {
            size_t Index = IndexAt(E.Position);
            if (Index != hz::NotFound) {
                if (OnRightClick) {
                    OnRightClick(Index, E.ScreenPosition);
                    E.Handled = true;
                }
            } else if (OnBackgroundRightClick) {
                OnBackgroundRightClick(E.ScreenPosition);
                E.Handled = true;
            }
        }
        ListView::OnPointerUp(E);
    }

    void OnChar(KeyEvent &E) override
    {
        if (E.Ctrl() || E.Alt() || E.Character < 0x20 || E.Character == 0x7F || !FindPrefix)
            return;
        uint64_t Now = NowUs();
        if (Now - LastTypeUs_ > 1000000)
            Typed_ = hz::String();
        LastTypeUs_ = Now;
        Typed_.AppendCodePoint(E.Character);
        size_t Index = FindPrefix(Typed_);
        if (Index != hz::NotFound) {
            SetSelectedIndex(Index);
            ScrollIntoView(Index);
        }
        E.Handled = true;
    }

    void OnKeyDown(KeyEvent &E) override
    {
        if (OnKey)
            OnKey(E);
        if (!E.Handled)
            ListView::OnKeyDown(E);
    }

private:
    hz::String Typed_;
    uint64_t LastTypeUs_ = 0;
};

/* The Details column headers: clicking one sorts by it, and the current column shows the
   direction. Only the header area hit-tests, so the rows below are unaffected. */
class ColumnBar final : public Element {
public:
    int Key = SortName;
    bool Descending = false;
    hz::Function<void(int)> OnSort;

    void Paint(Painter &P) override
    {
        const Rect &R = Bounds();
        Columns C = ColumnsOf(R, true);
        HZ_FONT *F = GetFont(FontRole::Caption);
        const int Keys[4] = {SortName, SortDate, SortType, SortSize};
        const int32_t Xs[4] = {C.NameX, C.DateX, C.TypeX, C.SizeX};
        const int32_t Ws[4] = {C.NameW, Col::DateWidth, Col::TypeWidth, Col::SizeWidth};
        const Align As[4] = {Align::Start, Align::Start, Align::Start, Align::End};
        for (int i = 0; i < 4; i++) {
            Rect Box{Xs[i], R.Y, Ws[i], R.Height};
            if (Hot_ == Keys[i] && Box.Width > 0) {
                P.FillRoundRect(Box, Theme::ControlRadius, Theme::SubtleFillHover);
                Box = Box.Deflate(Thickness(6, 0));
            }
            DrawLabel(P, F, Box, SortKeyName(Keys[i]), Keys[i], As[i]);
        }
        P.FillRect(Rect{R.X + 8, R.Bottom() - 1, R.Width - 16, 1}, Theme::Divider);
    }

    void OnPointerDown(PointerEvent &E) override
    {
        if (E.Button != HZ_MOUSE_LEFT)
            return;
        int Hit = ColumnAt(E.Position);
        if (Hit >= 0) {
            E.Handled = true; /* keeps the pointer until the release */
        }
    }

    void OnPointerUp(PointerEvent &E) override
    {
        if (E.Button != HZ_MOUSE_LEFT)
            return;
        int Hit = ColumnAt(E.Position);
        if (Hit >= 0 && OnSort) {
            E.Handled = true;
            OnSort(Hit);
        }
    }

    void OnPointerMove(PointerEvent &E) override
    {
        int Hit = ColumnAt(E.Position);
        if (Hit != Hot_) {
            Hot_ = Hit;
            Invalidate();
        }
    }

    void OnPointerLeave() override
    {
        if (Hot_ >= 0) {
            Hot_ = -1;
            Invalidate();
        }
    }

private:
    int ColumnAt(Point P) const
    {
        const Rect &R = Bounds();
        if (P.Y < R.Y || P.Y >= R.Bottom())
            return -1;
        Columns C = ColumnsOf(R, true);
        if (P.X >= C.SizeX)
            return P.X < C.SizeX + Col::SizeWidth ? SortSize : -1;
        if (P.X >= C.TypeX)
            return SortType;
        if (P.X >= C.DateX)
            return SortDate;
        if (P.X >= C.NameX)
            return SortName;
        return -1;
    }

    void DrawLabel(Painter &P, HZ_FONT *F, const Rect &R, const char *Text, int Column, Align A)
    {
        Color Foreground = Column == Key ? Theme::TextSecondary : Theme::TextTertiary;
        P.TextLine(F, R, Text, Foreground, A);
        if (Column != Key)
            return;
        TextLayout Layout;
        Layout.Layout(F, Text, strlen(Text), 0);
        int32_t ArrowX = A == Align::End ? R.Right() - Layout.Width - 16 : R.X + Layout.Width + 6;
        P.DrawIcon(Descending ? Icon::ChevronDown : Icon::ChevronUp, Rect{ArrowX, R.Y + R.Height / 2 - 7, 14, 14},
                   Foreground, 1.1f);
    }

    int Hot_ = -1;
};

/* ---- The application ------------------------------------------------------------------------- */

class FilesApp {
public:
    bool Create(const char *Start);

private:
    void Build();
    void LoadSettings();
    void SaveSettings() const;
    void Navigate(const hz::String &Path, bool Record = true, const hz::String &SelectName = hz::String());
    void Relist();
    void ApplyView();
    void Refresh();
    void UpdateCrumbs();
    void SetAddressEditing(bool Editing);
    void UpdateCommands();
    void UpdateStatus();
    void UpdateViewControls();
    void RebuildPlaces();
    void Open(size_t Index);
    void Back();
    void Forward();
    void Up();
    void NewFolder();
    void NewFile();
    void RenameSelected();
    void DeleteSelected();
    void ClipSelected(bool Cut);
    void Paste();
    void ExtractSelected();
    void ShowProperties();
    void ShowItemContextMenu(size_t Index, Point Screen);
    void ShowBackgroundContextMenu(Point Screen);
    void ShowNewMenu();
    void ShowSortMenu();
    void ShowViewMenu();
    void SetSort(int Key, bool Descending);
    void SetView(FileView View);
    void SortByColumn(int Key); /* a header click: the same column reverses the direction */
    bool Writable() const;
    const hz::fs::Entry *Selected() const;
    void Select(const hz::String &Name);
    Point Below(Element *E) const; /* screen point just below E (a menu anchor) */

    Window *Main_ = nullptr;
    TextBox *Address_ = nullptr;
    TextBox *Search_ = nullptr;
    StackPanel *Crumbs_ = nullptr;
    ListView *Places_ = nullptr;
    FileList *List_ = nullptr;
    ColumnBar *Columns_ = nullptr;
    TextBlock *Status_ = nullptr, *Free_ = nullptr;
    Button *BackButton_ = nullptr, *ForwardButton_ = nullptr, *UpButton_ = nullptr, *NewButton_ = nullptr,
           *RenameButton_ = nullptr, *DeleteButton_ = nullptr, *CutButton_ = nullptr, *CopyButton_ = nullptr,
           *PasteButton_ = nullptr, *SortButton_ = nullptr, *ViewButton_ = nullptr;
    hz::String ClipPath_; /* cut or copied; "" if none */
    bool ClipCut_ = false;
    hz::Vector<Place> PlaceItems_; /* the sidebar rows: Quick access, then the volumes */
    hz::Vector<hz::fs::Entry> All_;     /* the folder as listed, sorted */
    hz::Vector<hz::fs::Entry> Entries_; /* what the list shows (the filter applied to All_) */
    hz::Vector<hz::String> History_;
    size_t HistoryIndex_ = 0;
    hz::String Folder_;
    hz::String Filter_;
    hz::fs::Volume Volume_;
    int SortKey_ = SortName;
    bool SortDescending_ = false;
    FileView View_ = FileView::Details;
    bool AddressEditing_ = false;
};

/* ---- Window ---------------------------------------------------------------------------------- */

bool FilesApp::Create(const char *Start)
{
    LoadSettings();
    Main_ = new Window("Files", 960, 600);
    Main_->SetMinimumSize(560, 320);
    Build();
    Main_->OnClosed = []() { NtUserPostQuitMessage(0); };
    Main_->OnUnhandledKey = [this](KeyEvent &E) {
        if (E.Ctrl() && E.Key == 'L') {
            E.Handled = true;
            SetAddressEditing(true);
        } else if (E.Ctrl() && !E.Shift() && (E.Key == 'F' || E.Key == 'E')) {
            E.Handled = true;
            Main_->SetFocus(Search_, true);
            Search_->SelectAll();
        } else if (E.Key == HZ_VK_ESCAPE && Main_->FocusedElement() == Search_ && !Filter_.Empty()) {
            E.Handled = true;
            Search_->SetText("");
            Main_->SetFocus(List_);
        } else if (E.Key == HZ_VK_F1 + 4) { /* F5 */
            E.Handled = true;
            Refresh();
        } else if (E.Alt() && E.Key == HZ_VK_LEFT) {
            E.Handled = true;
            Back();
        } else if (E.Alt() && E.Key == HZ_VK_RIGHT) {
            E.Handled = true;
            Forward();
        } else if (E.Alt() && E.Key == HZ_VK_UP) {
            E.Handled = true;
            Up();
        } else if (E.Ctrl() && E.Shift() && E.Key == 'N') {
            E.Handled = true;
            NewFolder();
        }
    };
    hz::String Initial;
    if (Start && *Start) {
        Initial = hz::fs::Resolve(hz::String("C:\\"), Start);
    } else {
        Initial = hz::fs::DocumentsFolder();
    }
    Navigate(Initial);
    if (!Main_->Show())
        return false;
    Main_->SetFocus(List_);
    return true;
}

void FilesApp::Build()
{
    Grid *Root = new Grid();
    Root->Rows.Push(GridLength::AutoSize()); /* 0: navigation bar */
    Root->Rows.Push(GridLength::AutoSize()); /* 1: command bar */
    Root->Rows.Push(GridLength::Stars(1));   /* 2: content (tree + list) */
    Root->Rows.Push(GridLength::AutoSize()); /* 3: status bar */

    /* ---- Navigation bar: back / forward / up, breadcrumbs (or a typed path), search ---- */
    Grid *Nav = Root->Add<Grid>();
    Nav->Padding = Thickness(10, 8, 10, 6);
    Nav->ColumnSpacing = 4;
    for (int i = 0; i < 3; i++)
        Nav->Columns.Push(GridLength::AutoSize());
    Nav->Columns.Push(GridLength::Stars(1));
    Nav->Columns.Push(GridLength::Pixels(190));

    BackButton_ = Nav->Add<Button>("", ButtonStyle::Subtle);
    BackButton_->Glyph = Icon::ChevronLeft;
    BackButton_->ToolTip = "Back (Alt+Left)";
    BackButton_->Focusable = false;
    BackButton_->OnClick = [this]() { Back(); };
    ForwardButton_ = Nav->Add<Button>("", ButtonStyle::Subtle);
    ForwardButton_->GridColumn = 1;
    ForwardButton_->Glyph = Icon::ChevronRight;
    ForwardButton_->ToolTip = "Forward (Alt+Right)";
    ForwardButton_->Focusable = false;
    ForwardButton_->OnClick = [this]() { Forward(); };
    UpButton_ = Nav->Add<Button>("", ButtonStyle::Subtle);
    UpButton_->GridColumn = 2;
    UpButton_->Glyph = Icon::ArrowUp;
    UpButton_->ToolTip = "Up (Alt+Up)";
    UpButton_->Focusable = false;
    UpButton_->OnClick = [this]() { Up(); };

    /* The address cell holds the breadcrumbs and the path box; one of them is visible. */
    Grid *AddressCell = Nav->Add<Grid>();
    AddressCell->GridColumn = 3;
    AddressCell->Margin = Thickness(6, 0, 8, 0);
    AddressCell->ClipChildren = true;
    StackPanel *Crumbs = AddressCell->Add<StackPanel>(Orientation::Horizontal, 2);
    Crumbs->VerticalAlignment = Align::Center;
    Crumbs_ = Crumbs;
    Address_ = AddressCell->Add<TextBox>("Path");
    Address_->Height = Theme::ControlHeight;
    Address_->VerticalAlignment = Align::Center;
    Address_->MaxLength = 250;
    Address_->SetVisible(false);
    Address_->OnSubmit = [this](const hz::String &Text) {
        hz::String Path = hz::fs::Resolve(Folder_, Text.CStr());
        bool Directory = false;
        if (hz::fs::Exists(Path, &Directory) && !Directory) {
            hz::String Program;
            STATUS Status = hz::apps::OpenPath(Path, nullptr, &Program);
            if (HZ_SUCCESS(Status))
                HzPrint("files: opened %s with %s", Path.CStr(), Program.CStr());
            SetAddressEditing(false);
            return;
        }
        SetAddressEditing(false);
        Navigate(Path);
        Main_->SetFocus(List_);
    };

    Search_ = Nav->Add<TextBox>("Search this folder");
    Search_->GridColumn = 4;
    Search_->Height = Theme::ControlHeight;
    Search_->VerticalAlignment = Align::Center;
    Search_->MaxLength = 120;
    Search_->OnTextChanged = [this](const hz::String &Text) {
        if (Filter_ == Text)
            return;
        Filter_ = Text;
        ApplyView();
        UpdateStatus();
    };

    /* ---- Command bar ---- */
    Grid *Bar = Root->Add<Grid>();
    Bar->GridRow = 1;
    Bar->Padding = Thickness(10, 0, 10, 8);
    Bar->ColumnSpacing = 4;
    Bar->Columns.Push(GridLength::AutoSize());
    for (int i = 0; i < 5; i++)
        Bar->Columns.Push(GridLength::AutoSize());
    Bar->Columns.Push(GridLength::Stars(1));
    for (int i = 0; i < 2; i++)
        Bar->Columns.Push(GridLength::AutoSize());

    auto Command = [Bar](uint8_t Column, Icon Glyph, const char *Text, const char *Tip) {
        Button *B = Bar->Add<Button>(Text, ButtonStyle::Subtle);
        B->GridColumn = Column;
        B->Glyph = Glyph;
        B->ToolTip = Tip;
        B->Focusable = false;
        return B;
    };
    NewButton_ = Command(0, Icon::Add, "New", "New folder or file");
    NewButton_->OnClick = [this]() { ShowNewMenu(); };
    CutButton_ = Command(1, Icon::Cut, "", "Cut (Ctrl+X)");
    CutButton_->OnClick = [this]() { ClipSelected(true); };
    CopyButton_ = Command(2, Icon::Copy, "", "Copy (Ctrl+C)");
    CopyButton_->OnClick = [this]() { ClipSelected(false); };
    PasteButton_ = Command(3, Icon::Paste, "", "Paste (Ctrl+V)");
    PasteButton_->OnClick = [this]() { Paste(); };
    RenameButton_ = Command(4, Icon::Pencil, "", "Rename (F2)");
    RenameButton_->OnClick = [this]() { RenameSelected(); };
    DeleteButton_ = Command(5, Icon::Trash, "", "Delete (Del)");
    DeleteButton_->OnClick = [this]() { DeleteSelected(); };
    SortButton_ = Command(7, Icon::Chart, "", "Sort");
    SortButton_->OnClick = [this]() { ShowSortMenu(); };
    ViewButton_ = Command(8, Icon::AppWindow, "", "View");
    ViewButton_->OnClick = [this]() { ShowViewMenu(); };

    /* ---- Volumes and the folder ---- */
    Grid *Middle = Root->Add<Grid>();
    Middle->GridRow = 2;
    Middle->Padding = Thickness(10, 0, 10, 0);
    Middle->ColumnSpacing = 10;
    Middle->Columns.Push(GridLength::Pixels(210));
    Middle->Columns.Push(GridLength::Stars(1));
    Places_ = Middle->Add<ListView>();
    Places_->ItemHeight = 48;
    Places_->Background = Theme::CardBackground;
    Places_->CornerRadius = Theme::OverlayRadius;
    Places_->PaintItem = [this](Painter &P, const Rect &R, size_t I) {
        if (I >= PlaceItems_.Size())
            return;
        const Place &Item = PlaceItems_[I];
        P.DrawIcon(Item.Kind, Rect{R.X + 2, R.Y + 8, 18, 18}, Theme::TextSecondary);
        P.TextLine(GetFont(FontRole::Body), Rect{R.X + 30, R.Y + 4, R.Width - 30, 22}, Item.Name.CStr(),
                   Theme::TextPrimary);
        P.TextLine(GetFont(FontRole::Caption), Rect{R.X + 30, R.Y + 25, R.Width - 30, 18}, Item.Detail.CStr(),
                   Theme::TextTertiary);
    };
    Places_->OnSelectionChanged = [this](size_t I) {
        if (I < PlaceItems_.Size() && !PlaceItems_[I].Path.Empty() &&
            !hz::fs::EqualNames(Folder_.CStr(), PlaceItems_[I].Path.CStr()))
            Navigate(PlaceItems_[I].Path);
    };
    RebuildPlaces();

    Grid *Right = Middle->Add<Grid>();
    Right->GridColumn = 1;
    Right->Background = Theme::CardBackground;
    Right->CornerRadius = Theme::OverlayRadius;
    Right->Rows.Push(GridLength::AutoSize());
    Right->Rows.Push(GridLength::Stars(1));
    Columns_ = Right->Add<ColumnBar>();
    Columns_->Height = 30;
    Columns_->OnSort = [this](int Key) { SortByColumn(Key); };
    List_ = Right->Add<FileList>();
    List_->GridRow = 1;
    List_->ItemHeight = 32;
    List_->Margin = Thickness(4);
    List_->PaintItem = [this](Painter &P, const Rect &R, size_t I) {
        if (I >= Entries_.Size())
            return;
        const hz::fs::Entry &E = Entries_[I];
        HZ_FONT *F = GetFont(FontRole::Body);
        if (View_ == FileView::List) {
            P.DrawIcon(IconFor(E), Rect{R.X + 10, R.Y + (R.Height - 16) / 2, 16, 16}, IconColorFor(E));
            P.TextLine(F, Rect{R.X + 32, R.Y, R.Width - 40, R.Height}, E.Name.CStr(), Theme::TextPrimary);
            return;
        }
        if (View_ == FileView::Tiles) {
            int32_t Icon = 32;
            P.DrawIcon(IconFor(E), Rect{R.X + 12, R.Y + (R.Height - Icon) / 2, Icon, Icon}, IconColorFor(E), 1.75f);
            int32_t TextX = R.X + 12 + Icon + 12;
            hz::String Detail(E.Directory ? "Folder" : TypeName(E));
            if (!E.Directory)
                Detail = hz::String::Format("%s  -  %s", TypeName(E), hz::fs::FormatSize(E.Size).CStr());
            P.TextLine(F, Rect{TextX, R.Y + 9, R.Width - TextX + R.X - 14, 22}, E.Name.CStr(), Theme::TextPrimary);
            P.TextLine(GetFont(FontRole::Caption), Rect{TextX, R.Y + 31, R.Width - TextX + R.X - 14, 18}, Detail.CStr(),
                       Theme::TextTertiary);
            return;
        }
        Columns C = ColumnsOf(R, false);
        P.DrawIcon(IconFor(E), Rect{R.X + Col::IconX, R.Y + (R.Height - Col::IconSize) / 2, Col::IconSize, Col::IconSize},
                   IconColorFor(E));
        P.TextLine(F, Rect{C.NameX, R.Y, C.NameW, R.Height}, E.Name.CStr(), Theme::TextPrimary);
        P.TextLine(F, Rect{C.DateX, R.Y, Col::DateWidth, R.Height}, hz::fs::FormatTime(E.Modified).CStr(),
                   Theme::TextSecondary);
        P.TextLine(F, Rect{C.TypeX, R.Y, Col::TypeWidth, R.Height}, TypeName(E), Theme::TextSecondary);
        if (!E.Directory)
            P.TextLine(F, Rect{C.SizeX, R.Y, Col::SizeWidth, R.Height}, hz::fs::FormatSize(E.Size).CStr(),
                       Theme::TextSecondary, Align::End);
    };
    List_->OnItemInvoked = [this](size_t I) { Open(I); };
    List_->OnRightClick = [this](size_t I, Point Screen) { ShowItemContextMenu(I, Screen); };
    List_->OnBackgroundRightClick = [this](Point Screen) { ShowBackgroundContextMenu(Screen); };
    List_->OnSelectionChanged = [this](size_t) {
        UpdateCommands();
        UpdateStatus();
    };
    List_->FindPrefix = [this](const hz::String &Prefix) {
        for (size_t i = 0; i < Entries_.Size(); i++) {
            const hz::String &Name = Entries_[i].Name;
            if (Name.Length() >= Prefix.Length() &&
                hz::fs::EqualNames(Name.Substring(0, Prefix.Length()).CStr(), Prefix.CStr()))
                return i;
        }
        return hz::NotFound;
    };
    List_->OnKey = [this](KeyEvent &E) {
        if (E.Key == HZ_VK_DELETE) {
            E.Handled = true;
            DeleteSelected();
        } else if (E.Key == HZ_VK_F1 + 1) { /* F2 */
            E.Handled = true;
            RenameSelected();
        } else if (E.Key == HZ_VK_BACK || (E.Alt() && E.Key == HZ_VK_UP)) {
            E.Handled = true; /* before the list takes Up for itself */
            Up();
        } else if (E.Alt() && E.Key == HZ_VK_LEFT) {
            E.Handled = true;
            Back();
        } else if (E.Alt() && E.Key == HZ_VK_RIGHT) {
            E.Handled = true;
            Forward();
        } else if (E.Ctrl() && !E.Alt() && (E.Key == 'X' || E.Key == 'C')) {
            E.Handled = true;
            ClipSelected(E.Key == 'X');
        } else if (E.Ctrl() && !E.Alt() && E.Key == 'V') {
            E.Handled = true;
            Paste();
        }
    };

    /* ---- Status bar ---- */
    Grid *StatusBar = Root->Add<Grid>();
    StatusBar->GridRow = 3;
    StatusBar->Height = 30;
    StatusBar->Padding = Thickness(16, 0);
    StatusBar->Columns.Push(GridLength::Stars(1));
    StatusBar->Columns.Push(GridLength::AutoSize());
    Status_ = StatusBar->Add<TextBlock>("", FontRole::Caption);
    Status_->VerticalAlignment = Align::Center;
    Status_->Foreground = Theme::TextSecondary;
    Free_ = StatusBar->Add<TextBlock>("", FontRole::Caption);
    Free_->GridColumn = 1;
    Free_->VerticalAlignment = Align::Center;
    Free_->Foreground = Theme::TextTertiary;
    Main_->SetContent(Root);
}

/* ---- Settings (the view and the sort survive a restart) --------------------------------------- */

void FilesApp::LoadSettings()
{
    hz::fs::Settings Settings;
    if (!HZ_SUCCESS(Settings.Load(hz::fs::SettingsPath("files"))))
        return;
    hz::String View = Settings.Get("View", "Details");
    if (View == "List")
        View_ = FileView::List;
    else if (View == "Tiles")
        View_ = FileView::Tiles;
    else
        View_ = FileView::Details;
    SortKey_ = hz::Clamp(Settings.GetInt("Sort", SortName), (int)SortName, (int)SortSize);
    SortDescending_ = Settings.GetInt("SortDescending", 0) != 0;
}

void FilesApp::SaveSettings() const
{
    hz::fs::Settings Settings;
    Settings.Set("View", ViewName(View_));
    Settings.SetInt("Sort", SortKey_);
    Settings.SetInt("SortDescending", SortDescending_ ? 1 : 0);
    Settings.Save(hz::fs::SettingsPath("files")); /* a read-only live medium just keeps the defaults */
}

/* ---- Navigation ----------------------------------------------------------------------------------- */

void FilesApp::Navigate(const hz::String &Path, bool Record, const hz::String &SelectName)
{
    hz::Vector<hz::fs::Entry> Entries;
    STATUS Status = hz::fs::List(Path, Entries);
    if (!HZ_SUCCESS(Status)) {
        HzPrint("files: cannot open %s (0x%08X)", Path.CStr(), (uint32_t)Status);
        if (!AddressEditing_)
            SetAddressEditing(true);
        Address_->SetText(Folder_.CStr(), false);
        MessageBox(Main_, "Files", hz::String::Format("%s cannot be opened: %s.", Path.CStr(), hz::fs::StatusText(Status)).CStr(),
                   nullptr, nullptr);
        return;
    }
    if (Record && !Folder_.Empty() && Folder_ != Path) {
        if (HistoryIndex_ + 1 < History_.Size())
            History_.Resize(HistoryIndex_ + 1); /* a new branch drops what was ahead */
        History_.Push(Folder_);
        if (History_.Size() > 50) {
            History_.Erase(0);
        }
        HistoryIndex_ = History_.Size() - 1;
    }
    Folder_ = Path;
    All_ = hz::Move(Entries);
    SortEntries(All_, SortKey_, SortDescending_);
    ApplyView();
    hz::fs::QueryVolume(Folder_, Volume_);
    SetAddressEditing(false);
    UpdateCrumbs();
    List_->SetSelectedIndex(hz::NotFound, false);
    List_->Invalidate();
    /* Highlight the sidebar row for this folder: an exact Quick access match wins, otherwise the
       volume the folder sits on, otherwise nothing. */
    size_t Place = hz::NotFound;
    for (size_t i = 0; i < PlaceItems_.Size(); i++) {
        if (hz::fs::EqualNames(PlaceItems_[i].Path.CStr(), Folder_.CStr())) {
            Place = i;
            break;
        }
    }
    if (Place == hz::NotFound && Folder_.Length() >= 3) {
        hz::String Root = Folder_.Substring(0, 3);
        for (size_t i = 0; i < PlaceItems_.Size(); i++) {
            if (PlaceItems_[i].Path.Length() == 3 && hz::fs::EqualNames(PlaceItems_[i].Path.CStr(), Root.CStr())) {
                Place = i;
                break;
            }
        }
    }
    Places_->SetSelectedIndex(Place, false);
    hz::String Title = hz::fs::IsRoot(Folder_) ? Folder_ : hz::fs::FileName(Folder_);
    Main_->SetTitle(hz::String::Format("%s - Files", Title.CStr()).CStr());
    if (!SelectName.Empty())
        Select(SelectName);
    UpdateCommands();
    UpdateStatus();
    HzPrint("files: %s (%zu items)", Folder_.CStr(), Entries_.Size());
}

/* The filter turns the folder listing into what the list shows. */
void FilesApp::ApplyView()
{
    Entries_.Clear();
    Entries_.Reserve(All_.Size());
    for (size_t i = 0; i < All_.Size(); i++) {
        if (Filter_.Empty() || All_[i].Name.ContainsIgnoreCase(Filter_.CStr()))
            Entries_.Push(All_[i]);
    }
    List_->SetItemCount(Entries_.Size());
    List_->Invalidate();
}

void FilesApp::Relist()
{
    hz::Vector<hz::fs::Entry> Entries;
    if (!HZ_SUCCESS(hz::fs::List(Folder_, Entries)))
        return;
    All_ = hz::Move(Entries);
    SortEntries(All_, SortKey_, SortDescending_);
    ApplyView();
}

/* The sidebar, rebuilt when the volumes or the user name change: Quick access first - Home and
   Desktop only when the profile folder exists, so nothing points at a folder that is not there -
   then one row per volume, as the Windows 11 sidebar does. */
void FilesApp::RebuildPlaces()
{
    hz::Vector<hz::fs::Volume> Volumes = hz::fs::Volumes();
    PlaceItems_.Clear();
    hz::String Home;
    hz::fs::Settings Shell;
    if (HZ_SUCCESS(Shell.Load(hz::fs::SettingsPath("shell")))) {
        hz::String Name = Shell.Get("UserName");
        if (!Name.Empty()) {
            Home = hz::String::Format("C:\\Users\\%s", Name.CStr());
            bool Directory = false;
            if (!(hz::fs::Exists(Home, &Directory) && Directory))
                Home = hz::String();
        }
    }
    if (!Home.Empty()) {
        PlaceItems_.Push(Place{Icon::Home, "Home", "Your files", Home});
        hz::String Desktop = hz::fs::Join(Home, "Desktop");
        bool Directory = false;
        if (hz::fs::Exists(Desktop, &Directory) && Directory)
            PlaceItems_.Push(Place{Icon::Folder, "Desktop", "Your desktop", Desktop});
    }
    hz::String Documents = hz::fs::DocumentsFolder();
    if (!hz::fs::IsRoot(Documents))
        PlaceItems_.Push(Place{Icon::Document, "Documents", "Your documents", Documents});
    for (const hz::fs::Volume &V : Volumes) {
        hz::String Name = hz::String::Format("%s (%c:)", V.Label.Empty() ? "Local disk" : V.Label.CStr(), V.Root[0]);
        hz::String Detail = V.ReadOnly ? hz::String::Format("%s, read-only", V.FileSystem.CStr())
                                       : hz::String::Format("%s free", hz::fs::FormatSize(V.FreeBytes).CStr());
        PlaceItems_.Push(Place{Icon::Drive, Name, Detail, V.Root});
    }
    Places_->SetItemCount(PlaceItems_.Size());
    Places_->Invalidate();
}

void FilesApp::Refresh()
{
    hz::String Keep = Selected() ? Selected()->Name : hz::String();
    RebuildPlaces();
    Navigate(Folder_, false, Keep);
}

void FilesApp::Back()
{
    if (HistoryIndex_ == 0)
        return;
    HistoryIndex_--;
    Navigate(History_[HistoryIndex_], false);
}

void FilesApp::Forward()
{
    if (HistoryIndex_ + 1 >= History_.Size())
        return;
    HistoryIndex_++;
    Navigate(History_[HistoryIndex_], false);
}

void FilesApp::Up()
{
    if (hz::fs::IsRoot(Folder_))
        return;
    hz::String Child = hz::fs::FileName(Folder_);
    Navigate(hz::fs::Parent(Folder_), true, Child);
}

/* ---- The breadcrumbs ----------------------------------------------------------------------------- */

void FilesApp::UpdateCrumbs()
{
    Crumbs_->ClearChildren();
    hz::Vector<hz::String> Parts, Paths;
    hz::String Accumulated;
    size_t At = 0;
    for (;;) {
        size_t Sep = Folder_.Find("\\", At);
        hz::String Part = Sep == hz::NotFound ? Folder_.Substring(At) : Folder_.Substring(At, Sep - At);
        if (!Part.Empty()) {
            if (Accumulated.Empty())
                Accumulated = Part;
            else
                Accumulated = hz::String::Format("%s\\%s", Accumulated.CStr(), Part.CStr());
            if (Accumulated.Find("\\") == hz::NotFound)
                Accumulated.Append("\\"); /* a drive root, as the volume list names it */
            Parts.Push(Part);
            Paths.Push(Accumulated);
        }
        if (Sep == hz::NotFound)
            break;
        At = Sep + 1;
    }
    if (Parts.Empty())
        return;
    /* With a deep path show the drive, an ellipsis and the last three parts. */
    size_t First = 0, Last = Parts.Size();
    bool Ellipsis = false;
    if (Parts.Size() > 5) {
        First = 1;
        Last = Parts.Size() - 3;
        Ellipsis = true;
    }
    auto AddCrumb = [this, &Parts, &Paths](size_t Index) {
        Button *Crumb = Crumbs_->Add<Button>(Parts[Index].CStr(), ButtonStyle::Subtle);
        Crumb->Focusable = false;
        Crumb->ToolTip = Paths[Index].CStr();
        Crumb->VerticalAlignment = Align::Center;
        Crumb->Height = 26;
        hz::String Path = Paths[Index];
        Crumb->OnClick = [this, Path]() { Navigate(Path); };
        if (Index != Parts.Size() - 1) {
            TextBlock *Sep = Crumbs_->Add<TextBlock>(">", FontRole::Caption);
            Sep->Foreground = Theme::TextTertiary;
            Sep->VerticalAlignment = Align::Center;
        }
    };
    AddCrumb(0);
    if (Ellipsis) {
        TextBlock *More = Crumbs_->Add<TextBlock>("> ... >", FontRole::Caption);
        More->Foreground = Theme::TextTertiary;
        More->VerticalAlignment = Align::Center;
        for (size_t i = Last; i < Parts.Size(); i++)
            AddCrumb(i);
    } else {
        for (size_t i = First + 1; i < Parts.Size(); i++)
            AddCrumb(i);
    }
}

void FilesApp::SetAddressEditing(bool Editing)
{
    AddressEditing_ = Editing;
    Address_->SetVisible(Editing);
    Crumbs_->SetVisible(!Editing);
    if (Editing) {
        Address_->SetText(Folder_.CStr(), false);
        Main_->SetFocus(Address_, true);
        Address_->SelectAll();
    }
}

/* ---- State --------------------------------------------------------------------------------------- */

void FilesApp::Select(const hz::String &Name)
{
    for (size_t i = 0; i < Entries_.Size(); i++) {
        if (hz::fs::EqualNames(Entries_[i].Name.CStr(), Name.CStr())) {
            List_->SetSelectedIndex(i);
            List_->ScrollIntoView(i);
            return;
        }
    }
}

bool FilesApp::Writable() const
{
    return !Volume_.ReadOnly;
}

const hz::fs::Entry *FilesApp::Selected() const
{
    size_t I = List_->SelectedIndex();
    return I < Entries_.Size() ? &Entries_[I] : nullptr;
}

Point FilesApp::Below(Element *E) const
{
    const Rect &R = E->Bounds();
    Point Origin = Main_->ScreenOrigin();
    return Point{Origin.X + R.X, Origin.Y + R.Bottom() + 2};
}

void FilesApp::UpdateCommands()
{
    bool HasSelection = Selected() != nullptr;
    BackButton_->SetEnabled(HistoryIndex_ > 0);
    ForwardButton_->SetEnabled(HistoryIndex_ + 1 < History_.Size());
    UpButton_->SetEnabled(!hz::fs::IsRoot(Folder_));
    NewButton_->SetEnabled(Writable());
    RenameButton_->SetEnabled(Writable() && HasSelection);
    DeleteButton_->SetEnabled(Writable() && HasSelection);
    CutButton_->SetEnabled(Writable() && HasSelection);
    CopyButton_->SetEnabled(HasSelection);
    PasteButton_->SetEnabled(Writable() && !ClipPath_.Empty());
}

void FilesApp::UpdateViewControls()
{
    Columns_->SetVisible(View_ == FileView::Details);
    Columns_->Key = SortKey_;
    Columns_->Descending = SortDescending_;
    Columns_->Invalidate();
    List_->ItemHeight = View_ == FileView::Details ? 32 : View_ == FileView::List ? 24 : 60;
    List_->InvalidateLayout();
    ViewButton_->ToolTip = hz::String::Format("View: %s", ViewName(View_)).CStr();
    SortButton_->ToolTip = hz::String::Format("Sort by %s, %s", SortKeyName(SortKey_), SortDescending_ ? "descending" : "ascending")
                              .CStr();
}

void FilesApp::UpdateStatus()
{
    size_t Folders = 0, Files = 0;
    for (const hz::fs::Entry &E : Entries_)
        (E.Directory ? Folders : Files)++;
    hz::String Text;
    if (Filter_.Empty()) {
        Text = hz::String::Format("%zu item%s", Entries_.Size(), Entries_.Size() == 1 ? "" : "s");
    } else {
        Text = hz::String::Format("%zu of %zu items", Entries_.Size(), All_.Size());
    }
    if (Folders && Files)
        Text.Append(hz::String::Format("  |  %zu folder%s, %zu file%s", Folders, Folders == 1 ? "" : "s", Files,
                                       Files == 1 ? "" : "s"));
    if (const hz::fs::Entry *E = Selected())
        Text.Append(hz::String::Format("  |  %s selected%s%s", E->Name.CStr(), E->Directory ? "" : ", ",
                                       E->Directory ? "" : hz::fs::FormatSize(E->Size).CStr()));
    Status_->SetText(Text.CStr());
    Free_->SetText(Volume_.ReadOnly ? hz::String::Format("%s  |  read-only", Volume_.FileSystem.CStr()).CStr()
                                    : hz::String::Format("%s free of %s  |  %s", hz::fs::FormatSize(Volume_.FreeBytes).CStr(),
                                                         hz::fs::FormatSize(Volume_.TotalBytes).CStr(),
                                                         Volume_.FileSystem.CStr())
                                          .CStr());
}

void FilesApp::SetSort(int Key, bool Descending)
{
    SortKey_ = hz::Clamp(Key, (int)SortName, (int)SortSize);
    SortDescending_ = Descending;
    SortEntries(All_, SortKey_, SortDescending_);
    ApplyView();
    UpdateViewControls();
    UpdateStatus();
    SaveSettings();
    HzPrint("files: sorted by %s%s", SortKeyName(SortKey_), SortDescending_ ? ", descending" : "");
}

void FilesApp::SortByColumn(int Key)
{
    if (Key == SortKey_)
        SetSort(Key, !SortDescending_);
    else
        SetSort(Key, false);
}

void FilesApp::SetView(FileView View)
{
    View_ = View;
    UpdateViewControls();
    List_->ScrollIntoView(List_->SelectedIndex() == hz::NotFound ? 0 : List_->SelectedIndex());
    SaveSettings();
    HzPrint("files: view %s", ViewName(View_));
}

/* ---- Actions -------------------------------------------------------------------------------------- */

void FilesApp::Open(size_t Index)
{
    if (Index >= Entries_.Size())
        return;
    const hz::fs::Entry E = Entries_[Index];
    hz::String Path = hz::fs::Join(Folder_, E.Name.CStr());
    if (E.Directory) {
        Navigate(Path);
        return;
    }
    hz::String Program;
    uint32_t Pid = 0;
    STATUS Status = hz::apps::OpenPath(Path, &Pid, &Program);
    if (HZ_SUCCESS(Status)) {
        HzPrint("files: opened %s with %s (process %u)", Path.CStr(), Program.CStr(), Pid);
        return;
    }
    HzPrint("files: cannot open %s (0x%08X)", Path.CStr(), (uint32_t)Status);
    MessageBox(Main_, "Files",
               Status == STATUS_NOT_FOUND
                   ? hz::String::Format("No program opens %s files yet.", TypeName(E)).CStr()
                   : Status == STATUS_DLL_NOT_FOUND
                         ? "This EXE imports DLLs. Nexora currently supports statically linked Nexora programs only."
                         : hz::String::Format("%s cannot be opened (0x%08X).", E.Name.CStr(), (uint32_t)Status).CStr(),
               nullptr, nullptr);
}

void FilesApp::NewFolder()
{
    if (!Writable())
        return;
    hz::String Name("New folder");
    for (int i = 2; hz::fs::Exists(hz::fs::Join(Folder_, Name.CStr())) && i < 100; i++)
        Name = hz::String::Format("New folder (%d)", i);
    InputBox(Main_, "New folder", "Name of the new folder", Name.CStr(), "Create", [this](const hz::String &Typed) {
        if (Typed.Empty())
            return;
        if (!ValidFileName(Typed.CStr())) {
            MessageBox(Main_, "New folder", "A folder name cannot contain \\ / : * ? \" < > | or end with a dot.", nullptr,
                       nullptr);
            return;
        }
        hz::String Path = hz::fs::Join(Folder_, Typed.CStr());
        if (hz::fs::Exists(Path)) {
            MessageBox(Main_, "New folder", hz::String::Format("This folder already has an item named \"%s\".", Typed.CStr()).CStr(),
                       nullptr, nullptr);
            return;
        }
        STATUS Status = hz::fs::CreateDirectory(Path);
        if (!HZ_SUCCESS(Status)) {
            MessageBox(Main_, "Files", hz::String::Format("The folder cannot be created: %s.", hz::fs::StatusText(Status)).CStr(),
                       nullptr, nullptr);
            return;
        }
        HzPrint("files: created %s", Path.CStr());
        Refresh();
        Select(Typed);
    });
}

/* Create file: any extension, an empty file, no overwrite. */
void FilesApp::NewFile()
{
    if (!Writable())
        return;
    hz::String Name("New file.txt");
    for (int i = 2; hz::fs::Exists(hz::fs::Join(Folder_, Name.CStr())) && i < 100; i++)
        Name = hz::String::Format("New file (%d).txt", i);
    InputBox(Main_, "New file", "Name of the new file (any extension)", Name.CStr(), "Create", [this](const hz::String &Typed) {
        if (Typed.Empty())
            return;
        if (!ValidFileName(Typed.CStr())) {
            MessageBox(Main_, "New file", "A file name cannot contain \\ / : * ? \" < > | or end with a dot.", nullptr,
                       nullptr);
            return;
        }
        hz::String Path = hz::fs::Join(Folder_, Typed.CStr());
        if (hz::fs::Exists(Path)) {
            MessageBox(Main_, "New file", hz::String::Format("A file named \"%s\" already exists here.", Typed.CStr()).CStr(),
                       nullptr, nullptr);
            return;
        }
        STATUS Status = hz::fs::WriteAll(Path, "", 0);
        if (!HZ_SUCCESS(Status)) {
            MessageBox(Main_, "Files", hz::String::Format("The file cannot be created: %s.", hz::fs::StatusText(Status)).CStr(),
                       nullptr, nullptr);
            return;
        }
        HzPrint("files: created file %s", Path.CStr());
        Refresh();
        Select(Typed);
    });
}

void FilesApp::RenameSelected()
{
    const hz::fs::Entry *E = Selected();
    if (!E || !Writable())
        return;
    hz::String Old = E->Name;
    InputBox(Main_, "Rename", "New name", Old.CStr(), "Rename", [this, Old](const hz::String &Typed) {
        if (Typed.Empty() || Typed == Old)
            return;
        if (!ValidFileName(Typed.CStr())) {
            MessageBox(Main_, "Rename", "A name cannot contain \\ / : * ? \" < > | or end with a dot.", nullptr, nullptr);
            return;
        }
        hz::String Path = hz::fs::Join(Folder_, Old.CStr());
        if (hz::fs::Exists(hz::fs::Join(Folder_, Typed.CStr()))) {
            MessageBox(Main_, "Rename", hz::String::Format("This folder already has an item named \"%s\".", Typed.CStr()).CStr(),
                       nullptr, nullptr);
            return;
        }
        STATUS Status = hz::fs::Rename(Path, Typed.CStr());
        if (!HZ_SUCCESS(Status)) {
            MessageBox(Main_, "Files", hz::String::Format("%s cannot be renamed: %s.", Old.CStr(), hz::fs::StatusText(Status)).CStr(),
                       nullptr, nullptr);
            return;
        }
        HzPrint("files: renamed %s to %s", Path.CStr(), Typed.CStr());
        Refresh();
        Select(Typed);
    });
}

/* Counts the files and folders below Folder (for the delete confirmation). */
void CountTree(const hz::String &Folder, size_t &Files, size_t &Folders)
{
    hz::Vector<hz::fs::Entry> Children;
    if (!HZ_SUCCESS(hz::fs::List(Folder, Children)))
        return;
    for (const hz::fs::Entry &C : Children) {
        if (C.Directory) {
            Folders++;
            CountTree(hz::fs::Join(Folder, C.Name.CStr()), Files, Folders);
        } else {
            Files++;
        }
    }
}

void FilesApp::DeleteSelected()
{
    const hz::fs::Entry *E = Selected();
    if (!E || !Writable())
        return;
    hz::String Name = E->Name;
    bool Directory = E->Directory;
    hz::String Question;
    if (Directory) {
        size_t Files = 0, Folders = 0;
        CountTree(hz::fs::Join(Folder_, Name.CStr()), Files, Folders);
        Question = Files + Folders == 0
                       ? hz::String::Format("Delete the empty folder \"%s\"?", Name.CStr())
                       : hz::String::Format("Delete the folder \"%s\" and everything in it (%zu file%s, %zu folder%s)? "
                                            "This cannot be undone.",
                                            Name.CStr(), Files, Files == 1 ? "" : "s", Folders, Folders == 1 ? "" : "s");
    } else {
        Question = hz::String::Format("Delete \"%s\"? This cannot be undone.", Name.CStr());
    }
    MessageBox(Main_, "Delete", Question.CStr(), "Delete|Cancel", [this, Name](int Choice) {
        if (Choice != 0)
            return;
        hz::String Path = hz::fs::Join(Folder_, Name.CStr());
        STATUS Status = hz::fs::DeleteTree(Path);
        if (hz::fs::EqualNames(ClipPath_.CStr(), Path.CStr()) || hz::fs::IsInside(ClipPath_, Path))
            ClipPath_ = hz::String();
        if (!HZ_SUCCESS(Status)) {
            MessageBox(Main_, "Files", hz::String::Format("%s cannot be deleted: %s.", Name.CStr(), hz::fs::StatusText(Status)).CStr(),
                       nullptr, nullptr);
            return;
        }
        HzPrint("files: deleted %s", Path.CStr());
        size_t Index = List_->SelectedIndex();
        Refresh();
        if (!Entries_.Empty())
            List_->SetSelectedIndex(hz::Min(Index, Entries_.Size() - 1));
        Main_->SetFocus(List_);
    });
}

void FilesApp::ShowProperties()
{
    const hz::fs::Entry *E = Selected();
    if (!E)
        return;
    hz::String Path = hz::fs::Join(Folder_, E->Name.CStr());
    hz::String Detail;
    if (E->Directory) {
        size_t Files = 0, Folders = 0;
        CountTree(Path, Files, Folders);
        Detail = hz::String::Format("%zu file%s, %zu folder%s", Files, Files == 1 ? "" : "s", Folders,
                                    Folders == 1 ? "" : "s");
    } else {
        Detail = hz::String::Format("%s (%llu bytes)", hz::fs::FormatSize(E->Size).CStr(), (unsigned long long)E->Size);
    }
    hz::String Text = hz::String::Format("%s\n\nType:        %s\nContains:    %s\nModified:    %s\nLocation:    %s\n\n%s",
                                         E->Name.CStr(), TypeName(*E), Detail.CStr(), hz::fs::FormatTime(E->Modified).CStr(),
                                         Folder_.CStr(),
                                         Volume_.ReadOnly ? "This volume is read-only." : "This volume can be changed.");
    MessageBox(Main_, "Properties", Text.CStr(), nullptr, nullptr);
}

void FilesApp::ClipSelected(bool Cut)
{
    const hz::fs::Entry *E = Selected();
    if (!E || (Cut && !Writable()))
        return;
    ClipPath_ = hz::fs::Join(Folder_, E->Name.CStr());
    ClipCut_ = Cut;
    HzPrint("files: %s %s", Cut ? "cut" : "copied", ClipPath_.CStr());
    Status_->SetText(hz::String::Format("%s \"%s\". Open a folder and paste (Ctrl+V).", Cut ? "Cut" : "Copied",
                                        E->Name.CStr()).CStr());
    UpdateCommands();
}

/* "notes.txt" -> "notes - Copy.txt", then "notes - Copy (2).txt", ...; folders have no extension. */
hz::String FreeName(const hz::String &Folder, const hz::String &Name, bool Directory)
{
    size_t Dot = hz::NotFound;
    for (size_t i = 0; !Directory && i < Name.Length(); i++)
        if (Name[i] == '.')
            Dot = i;
    if (Dot == 0)
        Dot = hz::NotFound; /* ".profile" */
    hz::String Stem = Dot == hz::NotFound ? Name : Name.Substring(0, Dot);
    hz::String Ext = Dot == hz::NotFound ? hz::String() : Name.Substring(Dot);
    hz::String Candidate = hz::String::Format("%s - Copy%s", Stem.CStr(), Ext.CStr());
    for (int i = 2; hz::fs::Exists(hz::fs::Join(Folder, Candidate.CStr())) && i < 1000; i++)
        Candidate = hz::String::Format("%s - Copy (%d)%s", Stem.CStr(), i, Ext.CStr());
    return Candidate;
}

void FilesApp::Paste()
{
    if (ClipPath_.Empty() || !Writable())
        return;
    hz::String Source = ClipPath_;
    bool Directory = false;
    if (!hz::fs::Exists(Source, &Directory)) {
        ClipPath_ = hz::String();
        UpdateCommands();
        MessageBox(Main_, "Files", hz::String::Format("%s no longer exists.", Source.CStr()).CStr(), nullptr, nullptr);
        return;
    }
    hz::String Name = hz::fs::FileName(Source);
    bool SameFolder = hz::fs::EqualNames(hz::fs::Parent(Source).CStr(), Folder_.CStr());
    if (ClipCut_ && SameFolder)
        return; /* already here */
    if (Directory && (hz::fs::EqualNames(Source.CStr(), Folder_.CStr()) || hz::fs::IsInside(Folder_, Source))) {
        MessageBox(Main_, "Files", hz::String::Format("\"%s\" cannot be put inside itself.", Name.CStr()).CStr(), nullptr,
                   nullptr);
        return;
    }
    hz::String Target = hz::fs::Join(Folder_, Name.CStr());
    if (hz::fs::Exists(Target)) {
        if (ClipCut_) {
            MessageBox(Main_, "Files", hz::String::Format("This folder already has an item named \"%s\".", Name.CStr()).CStr(),
                       nullptr, nullptr);
            return;
        }
        Target = hz::fs::Join(Folder_, FreeName(Folder_, Name, Directory).CStr());
    }
    uint64_t Start = NowUs();
    STATUS Status = ClipCut_ ? hz::fs::MoveTree(Source, Target) : hz::fs::CopyTree(Source, Target);
    if (!HZ_SUCCESS(Status)) {
        HzPrint("files: cannot paste %s to %s (0x%08X)", Source.CStr(), Target.CStr(), (uint32_t)Status);
        if (!ClipCut_ && hz::fs::Exists(Target))
            hz::fs::DeleteTree(Target); /* no half copies */
        MessageBox(Main_, "Files", hz::String::Format("\"%s\" cannot be %s here: %s.", Name.CStr(),
                                                      ClipCut_ ? "moved" : "copied", hz::fs::StatusText(Status)).CStr(),
                   nullptr, nullptr);
        Refresh();
        return;
    }
    HzPrint("files: %s %s to %s (%u ms)", ClipCut_ ? "moved" : "pasted", Source.CStr(), Target.CStr(),
            (uint32_t)((NowUs() - Start) / 1000));
    if (ClipCut_)
        ClipPath_ = hz::String();
    Refresh();
    Select(hz::fs::FileName(Target));
    Main_->SetFocus(List_);
}

/* Unpacks an archive next to itself, into a folder named after it without the archive suffix
   ("notes.zip" -> "notes\"), which is what the Windows shell does for "Extract all". The tree is
   recreated by hz::archive, which refuses entries that would escape the folder and verifies the
   CRC-32 of every stream, so a damaged archive reports an error instead of half-unpacking. The
   count and total size are shown in the status bar and the outcome is logged. */
void FilesApp::ExtractSelected()
{
    const hz::fs::Entry *E = Selected();
    if (!E || E->Directory || !Writable())
        return;
    hz::String Name = E->Name;
    hz::String Source = hz::fs::Join(Folder_, Name.CStr());
    if (!hz::archive::IsArchive(Name)) {
        MessageBox(Main_, "Extract", hz::String::Format("\"%s\" is not an archive this OS can open.", Name.CStr()).CStr(),
                   nullptr, nullptr);
        return;
    }
    hz::String Base = hz::archive::StripSuffix(Name);
    if (Base.Empty())
        Base = Name;
    hz::String Dest = hz::fs::Join(Folder_, Base.CStr());
    if (hz::fs::Exists(Dest)) {
        MessageBox(Main_, "Extract", hz::String::Format("A folder named \"%s\" already exists here.", Base.CStr()).CStr(),
                   nullptr, nullptr);
        return;
    }
    uint64_t Start = NowUs();
    hz::archive::Result Count;
    STATUS Status = hz::archive::Extract(Source, Dest, &Count);
    if (!HZ_SUCCESS(Status)) {
        HzPrint("files: cannot extract %s (0x%08X)", Source.CStr(), (uint32_t)Status);
        if (Status == STATUS_NOT_SUPPORTED)
            MessageBox(Main_, "Extract",
                       hz::String::Format("\"%s\" uses a form of compression this OS does not read "
                                          "(Zip64, an encrypted entry, or bzip2).",
                                          Name.CStr())
                           .CStr(),
                       nullptr, nullptr);
        else
            MessageBox(Main_, "Extract",
                       hz::String::Format("\"%s\" cannot be extracted: %s.", Name.CStr(), hz::fs::StatusText(Status)).CStr(),
                       nullptr, nullptr);
        Refresh();
        return;
    }
    HzPrint("files: extracted %s to %s (%d files, %d folders, %s) (%u ms)", Source.CStr(), Dest.CStr(), Count.Files,
            Count.Folders, hz::fs::FormatSize(Count.Bytes).CStr(), (uint32_t)((NowUs() - Start) / 1000));
    Refresh();
    Select(Base);
    Main_->SetFocus(List_);
}

/* ---- Context menus ------------------------------------------------------------------------------- */

void FilesApp::ShowItemContextMenu(size_t Index, Point Screen)
{
    if (Index >= Entries_.Size())
        return;
    List_->SetSelectedIndex(Index);
    const hz::fs::Entry &E = Entries_[Index];
    hz::Vector<MenuItem> Items;
    Items.Push(MenuItem::Make("Open", [this, Index]() { Open(Index); }));
    if (!E.Directory && hz::archive::IsArchive(E.Name)) {
        MenuItem ExtractItem = MenuItem::Make("Extract all...", [this]() { ExtractSelected(); });
        ExtractItem.Enabled = Writable();
        Items.Push(hz::Move(ExtractItem));
    }
    Items.Push(MenuItem::MakeSeparator());
    MenuItem CutItem = MenuItem::Make("Cut", [this]() { ClipSelected(true); }, "Ctrl+X");
    CutItem.Enabled = Writable();
    Items.Push(hz::Move(CutItem));
    Items.Push(MenuItem::Make("Copy", [this]() { ClipSelected(false); }, "Ctrl+C"));
    MenuItem PasteItem = MenuItem::Make("Paste", [this]() { Paste(); }, "Ctrl+V");
    PasteItem.Enabled = !ClipPath_.Empty() && Writable();
    Items.Push(hz::Move(PasteItem));
    Items.Push(MenuItem::MakeSeparator());
    MenuItem RenameItem = MenuItem::Make("Rename", [this]() { RenameSelected(); }, "F2");
    RenameItem.Enabled = Writable();
    Items.Push(hz::Move(RenameItem));
    MenuItem DeleteItem = MenuItem::Make("Delete", [this]() { DeleteSelected(); }, "Del");
    DeleteItem.Enabled = Writable();
    Items.Push(hz::Move(DeleteItem));
    Items.Push(MenuItem::MakeSeparator());
    Items.Push(MenuItem::Make("Properties", [this]() { ShowProperties(); }));
    if (!Writable())
        Items.Push(MenuItem::MakeSeparator());
    if (!Writable()) {
        MenuItem Note = MenuItem::Make("This volume is read-only", []() {});
        Note.Enabled = false;
        Items.Push(hz::Move(Note));
    }
    (void)E;
    ShowMenu(Main_, Screen, hz::Move(Items));
}

void FilesApp::ShowBackgroundContextMenu(Point Screen)
{
    List_->SetSelectedIndex(hz::NotFound);
    hz::Vector<MenuItem> Items;
    MenuItem NewFolderItem = MenuItem::Make("New folder", [this]() { NewFolder(); }, "Ctrl+Shift+N");
    NewFolderItem.Enabled = Writable();
    Items.Push(hz::Move(NewFolderItem));
    MenuItem NewFileItem = MenuItem::Make("New file", [this]() { NewFile(); });
    NewFileItem.Enabled = Writable();
    Items.Push(hz::Move(NewFileItem));
    Items.Push(MenuItem::MakeSeparator());
    MenuItem PasteItem = MenuItem::Make("Paste", [this]() { Paste(); }, "Ctrl+V");
    PasteItem.Enabled = !ClipPath_.Empty() && Writable();
    Items.Push(hz::Move(PasteItem));
    Items.Push(MenuItem::MakeSeparator());
    Items.Push(MenuItem::Make("Sort by name", [this]() { SetSort(SortName, false); }));
    Items.Push(MenuItem::Make("Refresh", [this]() { Refresh(); }, "F5"));
    Items.Push(MenuItem::MakeSeparator());
    Items.Push(MenuItem::Make("Open in Notepad", [this]() {
        const hz::fs::Entry *E = Selected();
        if (!E)
            MessageBox(Main_, "Files", "Select an item first.", nullptr, nullptr);
    }, ""));
    if (!Writable()) {
        Items.Push(MenuItem::MakeSeparator());
        MenuItem Note = MenuItem::Make("This volume is read-only", []() {});
        Note.Enabled = false;
        Items.Push(hz::Move(Note));
    }
    ShowMenu(Main_, Screen, hz::Move(Items));
    (void)&Items;
}

void FilesApp::ShowNewMenu()
{
    hz::Vector<MenuItem> Items;
    Items.Push(MenuItem::Make("Folder", [this]() { NewFolder(); }, "Ctrl+Shift+N"));
    Items.Push(MenuItem::Make("Text document", [this]() { NewFile(); }));
    Items.Push(MenuItem::Make("Empty file (any extension)", [this]() { NewFile(); }));
    ShowMenu(Main_, Below(NewButton_), hz::Move(Items), 220);
}

void FilesApp::ShowSortMenu()
{
    hz::Vector<MenuItem> Items;
    for (int Key = SortName; Key < SortKeyCount; Key++) {
        MenuItem Item = MenuItem::Make(SortKeyName(Key), [this, Key]() { SetSort(Key, false); });
        Item.Checked = Key == SortKey_ && !SortDescending_;
        Items.Push(hz::Move(Item));
    }
    Items.Push(MenuItem::MakeSeparator());
    MenuItem Ascending = MenuItem::Make("Ascending", [this]() { SetSort(SortKey_, false); });
    Ascending.Checked = !SortDescending_;
    Items.Push(hz::Move(Ascending));
    MenuItem Descending = MenuItem::Make("Descending", [this]() { SetSort(SortKey_, true); });
    Descending.Checked = SortDescending_;
    Items.Push(hz::Move(Descending));
    ShowMenu(Main_, Below(SortButton_), hz::Move(Items), 200);
}

void FilesApp::ShowViewMenu()
{
    hz::Vector<MenuItem> Items;
    const FileView Views[3] = {FileView::Details, FileView::List, FileView::Tiles};
    for (int i = 0; i < 3; i++) {
        MenuItem Item = MenuItem::Make(ViewName(Views[i]), [this, i, Views]() { SetView(Views[i]); });
        Item.Checked = View_ == Views[i];
        Items.Push(hz::Move(Item));
    }
    ShowMenu(Main_, Below(ViewButton_), hz::Move(Items), 180);
}

}  // namespace

extern "C" int main(int argc, char **argv)
{
    FilesApp *App = new FilesApp();
    if (!App->Create(argc > 1 ? argv[1] : nullptr)) {
        HzPrint("files: cannot create the window");
        return 1;
    }
    HzPrint("files: started");
    return RunMessageLoop();
}
