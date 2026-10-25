/*
 * Nexora OS - user/shell/desktop.cpp
 * The desktop icon layer: a full-screen window that carries HZ_WS_DESKTOP, so win32k keeps it
 * under every other window while still letting a click give it the keyboard. It draws the
 * shortcut icons (every pinned application, then Trash) followed by the items of the user's
 * Desktop folder over the wallpaper (HZ_WS_TRANSPARENT: the compositor blends what it paints).
 *
 * Icons are laid out in columns that fill downwards, are revealed with a short staggered slide,
 * and answer to a single click (select), a double click or Enter (open) and the background or
 * item context menus - which offer the same file operations as Files (New, Rename, Delete to
 * Trash, Copy/Cut/Paste, Properties). The keys F2, Delete, F5, Ctrl+C/X/V, Shift+F10 and the
 * arrows act on the selection. Actions are logged ("shell: desktop ...") for the smoke tests.
 */
#include "desktop.h"
#include "hzapps.h"
#include "hzfile.h"
#include "hzui.h"

namespace shell {

/* ---- Items ---------------------------------------------------------------------------------- */

enum class ItemKind : uint8_t { App, File, Folder, Trash };

struct DesktopItem {
    hz::String Name;   /* the label under the icon */
    hz::String Path;   /* File and Folder: the full path */
    const hz::apps::App *App = nullptr;
    ItemKind Kind = ItemKind::File;
    Icon Glyph = Icon::Document;
    Color Tile = 0xFF5E6A7A;
};

/* ---- Layout and motion constants ------------------------------------------------------------ */

namespace {

constexpr int32_t kCellWidth = 96;
constexpr int32_t kCellPad = 52;    /* below the icon: two lines of the label */
constexpr int32_t kGridLeft = 16;
constexpr int32_t kGridTop = kWorkAreaTop + 10;
constexpr int32_t kGridBottom = 10;
constexpr int32_t kSelectionRadius = 6;

/* The reveal: every icon slides up and fades in, one a little after the one before it. */
constexpr uint64_t kRevealUs = 220000;
constexpr uint64_t kRevealStaggerUs = 42000;

constexpr Color kFolderTile = 0xFFD79A3C;
constexpr Color kFileTile = 0xFF5E6A7A;
constexpr Color kTrashTile = 0xFF4C5666;
constexpr Color kSelectionFill = 0x4C7099E8;
constexpr Color kHoverFill = 0x14FFFFFF;

/* Views (the icon size in pixels) and sorts, as Windows 11 names them. */
constexpr int32_t kViewSizes[3] = {36, 48, 56};
const char *const kViewNames[3] = {"Small icons", "Medium icons", "Large icons"};

enum SortKey : uint8_t { SortByName = 0, SortByType, SortBySize, SortByDate, SortKeyCount };

const char *SortName(int Key)
{
    switch (Key) {
    case SortByName: return "Name";
    case SortByType: return "Item type";
    case SortBySize: return "Size";
    default: return "Date modified";
    }
}

/* ---- Small helpers -------------------------------------------------------------------------- */

const char *const kInvalidName = "\\/:*?\"<>|";

bool ValidName(const char *Name)
{
    size_t Length = strlen(Name);
    if (Length == 0 || Length > 200)
        return false;
    if (strcmp(Name, ".") == 0 || strcmp(Name, "..") == 0)
        return false;
    for (const char *C = Name; *C; C++) {
        if ((uint8_t)*C < 0x20)
            return false;
        for (const char *Bad = kInvalidName; *Bad; Bad++)
            if (*Bad == *C)
                return false;
    }
    char Last = Name[Length - 1];
    return Last != '.' && Last != ' ';
}

Icon GlyphForEntry(const hz::fs::Entry &E)
{
    if (E.Directory)
        return Icon::Folder;
    hz::String Ext = hz::fs::Extension(E.Name);
    if (Ext == "exe")
        return Icon::Prompt;
    if (Ext == "app")
        return Icon::AppWindow;
    return Icon::Document;
}

Color TileForEntry(const hz::fs::Entry &E)
{
    return E.Directory ? kFolderTile : kFileTile;
}

/* The user's Desktop folder: C:\Users\<name>\Desktop (created when the system is installed),
   else the first writable volume, so the desktop always has somewhere to put its icons. */
hz::String DesktopFolderPath()
{
    hz::fs::Settings Shell;
    if (HZ_SUCCESS(Shell.Load(hz::fs::SettingsPath("shell")))) {
        hz::String Name = Shell.Get("UserName");
        if (!Name.Empty() && hz::fs::IsValidUserName(Name.CStr())) {
            hz::String Path = hz::String::Format("C:\\Users\\%s\\Desktop", Name.CStr());
            bool Directory = false;
            if (hz::fs::Exists(Path, &Directory) && Directory)
                return Path;
            if (hz::fs::Exists(hz::String::Format("C:\\Users\\%s", Name.CStr()), &Directory) && Directory &&
                HZ_SUCCESS(hz::fs::CreateDirectory(Path)))
                return Path;
        }
    }
    for (const hz::fs::Volume &V : hz::fs::Volumes())
        if (!V.ReadOnly && !V.Root.Empty())
            return V.Root;
    return hz::String("C:\\");
}

int CompareEntries(const hz::fs::Entry &A, const hz::fs::Entry &B, int Key)
{
    if (A.Directory != B.Directory)
        return A.Directory ? -1 : 1; /* folders first */
    int Result = 0;
    switch (Key) {
    case SortByType: {
        hz::String X = hz::fs::Extension(A.Name), Y = hz::fs::Extension(B.Name);
        Result = strcmp(X.CStr(), Y.CStr());
        break;
    }
    case SortBySize:
        Result = A.Size < B.Size ? -1 : A.Size > B.Size ? 1 : 0;
        break;
    case SortByDate:
        Result = A.Modified < B.Modified ? -1 : A.Modified > B.Modified ? 1 : 0;
        break;
    default:
        break;
    }
    if (Result == 0) {
        const char *X = A.Name.CStr(), *Y = B.Name.CStr();
        while (*X && *Y) {
            char Cx = *X, Cy = *Y;
            if (Cx >= 'a' && Cx <= 'z') Cx = (char)(Cx - 32);
            if (Cy >= 'a' && Cy <= 'z') Cy = (char)(Cy - 32);
            if (Cx != Cy) return Cx < Cy ? -1 : 1;
            X++, Y++;
        }
        Result = *X ? 1 : *Y ? -1 : 0;
    }
    return Result;
}

/* The manifest glyph on its coloured tile, faded by Alpha; a letter when the glyph is unknown. */
void PaintTile(Painter &P, const Rect &Box, Color Tile, Icon Glyph, const char *Title, uint32_t Alpha)
{
    float Fade = (float)Alpha / 255.0f;
    if (Fade <= 0)
        return;
    float Radius = Box.Width / 4.0f;
    P.FillVerticalGradient(Box, Radius, ScaleAlpha(MixColors(Tile, 0xFFFFFFFF, 0.14f), Fade), ScaleAlpha(Tile, Fade));
    Color Ink = ScaleAlpha(0xFFFFFFFF, Fade);
    if (Glyph != Icon::None) {
        P.DrawIcon(Glyph, Box.Deflate(Thickness(Box.Width / 5)), Ink, 1.5f);
        return;
    }
    char Letter[2] = {Title[0] ? Title[0] : '?', 0};
    if (Letter[0] >= 'a' && Letter[0] <= 'z')
        Letter[0] = (char)(Letter[0] - 32);
    P.TextLine(GetFont(FontRole::Subtitle), Box, Letter, Ink, Align::Center);
}

/* Starts a program by its manifest id with one optional argument. */
void LaunchApp(Window *Owner, const char *Id, const char *Argument)
{
    const hz::apps::App *A = g_Shell.FindApp(hz::String(Id));
    if (!A) {
        MessageBox(Owner, "Desktop", hz::String::Format("\"%s\" is not installed.", Id).CStr(), nullptr, nullptr);
        return;
    }
    uint32_t Pid = 0;
    STATUS Status = hz::apps::Launch(*A, Argument, &Pid);
    if (HZ_SUCCESS(Status))
        HzPrint("shell: desktop opened %s (process %u)", A->Name.CStr(), Pid);
    else
        HzPrint("shell: desktop cannot start %s (0x%08X)", A->Name.CStr(), (uint32_t)Status);
}

}  // namespace

/* ---- The icon layer ------------------------------------------------------------------------- */

class DesktopView final : public Element {
public:
    hz::Vector<DesktopItem> Shortcuts; /* the fixed icons, in order, before the folder's items */
    hz::Vector<DesktopItem> Items;     /* shortcuts then the desktop folder, as shown */
    hz::Vector<hz::fs::Entry> Entries; /* the desktop folder's content, sorted */
    size_t Selected = hz::NotFound, Hover = hz::NotFound;
    int32_t IconSize = 48;
    int SortKey = SortByName;
    bool SortDescending = false;
    hz::Function<void(size_t)> OnOpen;
    hz::Function<void(size_t, Point)> OnItemMenu;
    hz::Function<void(Point)> OnBackgroundMenu;

    /* Starts the staggered reveal (after the desktop appears or the folder is re-read). */
    void Reveal()
    {
        RevealStartUs_ = NowUs();
        if (Window *W = GetWindow())
            W->InvalidateRect(GridBounds());
        StartAnimation();
    }

    Rect CellRect(size_t Index) const
    {
        size_t Rows = Rows_ > 0 ? (size_t)Rows_ : 1;
        int32_t Col = (int32_t)(Index / Rows), Row = (int32_t)(Index % Rows);
        return Rect{GridX_ + Col * kCellWidth, GridY_ + Row * CellHeight(), kCellWidth, CellHeight()};
    }

    void InvalidateCell(size_t Index)
    {
        if (Index == hz::NotFound)
            return;
        if (Window *W = GetWindow())
            W->InvalidateRect(CellRect(Index));
    }

    void SetSelected(size_t Index)
    {
        if (Selected == Index)
            return;
        size_t Old = Selected;
        Selected = Index;
        InvalidateCell(Old);
        InvalidateCell(Index);
    }

    /* The arrow keys: move one cell, staying inside the grid. */
    void MoveSelection(int32_t Dx, int32_t Dy)
    {
        UpdateGrid();
        if (Items.Empty())
            return;
        if (Selected == hz::NotFound) {
            SetSelected(0);
            return;
        }
        int32_t Rows = Rows_ > 0 ? Rows_ : 1;
        int32_t Row = (int32_t)(Selected % (size_t)Rows) + Dy;
        int32_t Col = (int32_t)(Selected / (size_t)Rows) + Dx;
        Row = hz::Clamp(Row, 0, Rows - 1);
        Col = hz::Max(Col, 0);
        int32_t LastCol = (int32_t)((Items.Size() - 1) / (size_t)Rows);
        Col = hz::Min(Col, LastCol);
        size_t Index = (size_t)Col * (size_t)Rows + (size_t)Row;
        SetSelected(Index < Items.Size() ? Index : Items.Size() - 1);
    }

    size_t IndexAt(Point P) const
    {
        for (size_t i = 0; i < Items.Size(); i++)
            if (CellRect(i).Contains(P))
                return i;
        return hz::NotFound;
    }

    void UpdateGrid()
    {
        const Rect &B = Bounds();
        GridX_ = kGridLeft;
        GridY_ = kGridTop;
        int32_t Room = hz::Max(B.Height - kGridTop - kTaskbarHeight - kGridBottom, CellHeight());
        Rows_ = hz::Max(Room / CellHeight(), 1);
        Cols_ = hz::Max(hz::Max(B.Width - 2 * kGridLeft, kCellWidth) / kCellWidth, 1);
    }

    void InvalidateGrid()
    {
        UpdateGrid();
        if (Window *W = GetWindow())
            W->InvalidateRect(GridBounds());
    }

    /* ---- Element ------------------------------------------------------------------------ */

    void Paint(Painter &P) override
    {
        UpdateGrid();
        HZ_FONT *Label = GetFont(FontRole::Caption);
        uint64_t Now = NowUs();
        for (size_t i = 0; i < Items.Size(); i++) {
            Rect Cell = CellRect(i);
            if (!P.IsVisible(Cell))
                continue;
            float Progress = RevealProgress(Now, i);
            if (Progress <= 0)
                continue;
            uint32_t Alpha = (uint32_t)(Progress * 255.0f + 0.5f);
            Cell = Cell.Offset(0, (int32_t)((1.0f - Progress) * 16.0f));
            PaintItem(P, Cell, Label, Items[i], i == Selected, i == Hover, Alpha);
        }
    }

    bool Animate() override
    {
        if (!RevealStartUs_)
            return false;
        uint64_t Now = NowUs();
        uint64_t Total = kRevealUs + (uint64_t)Items.Size() * kRevealStaggerUs;
        if (Now - RevealStartUs_ >= Total) {
            RevealStartUs_ = 0; /* every icon is fully drawn: one last paint, no animation */
            if (Window *W = GetWindow())
                W->InvalidateRect(GridBounds());
            return false;
        }
        if (Window *W = GetWindow())
            W->InvalidateRect(GridBounds());
        return true;
    }

    void OnPointerDown(PointerEvent &E) override
    {
        if (E.Button != HZ_MOUSE_LEFT)
            return;
        SetSelected(IndexAt(E.Position));
        E.Handled = true; /* keep the release (and its click count) for this element */
    }

    void OnPointerUp(PointerEvent &E) override
    {
        size_t Index = IndexAt(E.Position);
        if (E.Button == HZ_MOUSE_RIGHT) {
            if (Index != hz::NotFound) {
                SetSelected(Index);
                if (OnItemMenu)
                    OnItemMenu(Index, E.ScreenPosition);
            } else if (OnBackgroundMenu) {
                SetSelected(hz::NotFound);
                OnBackgroundMenu(E.ScreenPosition);
            }
            E.Handled = true;
            return;
        }
        if (E.Button == HZ_MOUSE_LEFT && Index != hz::NotFound && E.ClickCount >= 2) {
            SetSelected(Index);
            if (OnOpen)
                OnOpen(Index);
            E.Handled = true;
        }
    }

    void OnPointerMove(PointerEvent &E) override
    {
        size_t Index = IndexAt(E.Position);
        if (Index == Hover)
            return;
        size_t Old = Hover;
        Hover = Index;
        InvalidateCell(Old);
        InvalidateCell(Index);
    }

    void OnPointerLeave() override
    {
        if (Hover == hz::NotFound)
            return;
        size_t Old = Hover;
        Hover = hz::NotFound;
        InvalidateCell(Old);
    }

private:
    int32_t CellHeight() const { return hz::Clamp(IconSize, 36, 56) + kCellPad; }

    Rect GridBounds() const
    {
        int32_t Width = Cols_ > 0 ? Cols_ * kCellWidth : Bounds().Width;
        return Rect{GridX_, GridY_, Width, Bounds().Height - GridY_};
    }

    float RevealProgress(uint64_t Now, size_t Index) const
    {
        if (!RevealStartUs_)
            return 1.0f;
        int64_t Elapsed = (int64_t)(Now - RevealStartUs_) - (int64_t)Index * (int64_t)kRevealStaggerUs;
        if (Elapsed <= 0)
            return 0.0f;
        if ((uint64_t)Elapsed >= kRevealUs)
            return 1.0f;
        float T = (float)Elapsed / (float)kRevealUs, Inv = 1.0f - T;
        return 1.0f - Inv * Inv * Inv; /* ease out */
    }

    void PaintItem(Painter &P, const Rect &Cell, HZ_FONT *Label, const DesktopItem &Item, bool Selected,
                   bool Hovered, uint32_t Alpha)
    {
        float Fade = (float)Alpha / 255.0f;
        Rect Box{Cell.X + 4, Cell.Y + 2, Cell.Width - 8, Cell.Height - 6};
        if (Selected)
            P.FillRoundRect(Box, kSelectionRadius, ScaleAlpha(kSelectionFill, Fade));
        else if (Hovered)
            P.FillRoundRect(Box, kSelectionRadius, ScaleAlpha(kHoverFill, Fade));

        int32_t Size = hz::Clamp(IconSize, 36, 56);
        Rect Icon_Box{Cell.X + (Cell.Width - Size) / 2, Cell.Y + 6, Size, Size};
        Color Tile = Item.Kind == ItemKind::App ? Item.App->Color : Item.Tile;
        Icon Glyph = Item.Kind == ItemKind::App ? IconByName(Item.App->Icon) : Item.Glyph;
        PaintTile(P, Icon_Box, Tile, Glyph, Item.Name.CStr(), Alpha);

        /* The label: up to two lines, centred, with a soft shadow so it reads on any wallpaper. */
        TextLayout Layout;
        Layout.Layout(Label, Item.Name.CStr(), Item.Name.Length(), kCellWidth - 16);
        int32_t LineHeight = Layout.LineHeight > 0 ? Layout.LineHeight : 14;
        int32_t Top = Cell.Y + 6 + Size + 6;
        size_t Lines = hz::Min(Layout.Lines.Size(), (size_t)2);
        for (size_t i = 0; i < Lines; i++) {
            const TextLine &Line = Layout.Lines[i];
            hz::String Text = Item.Name.Substring(Line.Start, Line.Length);
            Rect Row{Cell.X + 4, Top + (int32_t)i * LineHeight, Cell.Width - 8, LineHeight};
            P.TextLine(Label, Row.Offset(0, 1), Text.CStr(), ScaleAlpha(0x70000000, Fade), Align::Center);
            P.TextLine(Label, Row, Text.CStr(), ScaleAlpha(Theme::TextPrimary, Fade), Align::Center);
        }
    }

    uint64_t RevealStartUs_ = 0;
    int32_t GridX_ = kGridLeft, GridY_ = kGridTop, Rows_ = 1, Cols_ = 1;
};

/* ---- The desktop window --------------------------------------------------------------------- */

Desktop::Desktop(int32_t Width, int32_t Height)
    : Window("Desktop", Width, Height, HZ_WS_DESKTOP | HZ_WS_TRANSPARENT | HZ_WS_NOFRAME | HZ_WS_VISIBLE)
{
    Background = 0x00000000; /* the wallpaper shows through wherever nothing is drawn */
    Folder_ = DesktopFolderPath();
    View_ = new DesktopView();
    View_->OnOpen = [this](size_t Index) { Open(Index); };
    View_->OnItemMenu = [this](size_t Index, Point Screen) { ShowItemMenu(Index, Screen); };
    View_->OnBackgroundMenu = [this](Point Screen) { ShowBackgroundMenu(Screen); };
    SetContent(View_);
    LoadPrefs();
    Reload();
}

void Desktop::LoadPrefs()
{
    hz::fs::Settings Prefs;
    if (!HZ_SUCCESS(Prefs.Load(hz::fs::SettingsPath("desktop"))))
        return;
    int32_t Size = Prefs.GetInt("IconSize", 48);
    View_->IconSize = Size <= 36 ? 36 : Size >= 56 ? 56 : 48;
    View_->SortKey = hz::Clamp(Prefs.GetInt("Sort", SortByName), (int)SortByName, (int)SortKeyCount - 1);
    View_->SortDescending = Prefs.GetInt("Descending", 0) != 0;
}

void Desktop::SavePrefs() const
{
    hz::fs::Settings Prefs;
    Prefs.SetInt("IconSize", View_->IconSize);
    Prefs.SetInt("Sort", View_->SortKey);
    Prefs.SetInt("Descending", View_->SortDescending ? 1 : 0);
    Prefs.Save(hz::fs::SettingsPath("desktop")); /* fails silently on the read-only live system */
}

void Desktop::RefreshPrefs()
{
    LoadPrefs();
    RebuildView();
}

void Desktop::RebuildView()
{
    View_->Items.Clear();
    for (const DesktopItem &S : View_->Shortcuts)
        View_->Items.Push(S);
    /* Insertion sort: folders first, then by the chosen key, the name breaking ties. */
    for (size_t i = 1; i < View_->Entries.Size(); i++) {
        hz::fs::Entry Value = hz::Move(View_->Entries[i]);
        size_t j = i;
        while (j > 0) {
            int Order = CompareEntries(View_->Entries[j - 1], Value, View_->SortKey);
            /* Descending reverses the key but never moves folders below the files. */
            if (View_->SortDescending && View_->Entries[j - 1].Directory == Value.Directory)
                Order = -Order;
            if (Order <= 0)
                break;
            View_->Entries[j] = hz::Move(View_->Entries[j - 1]);
            j--;
        }
        View_->Entries[j] = hz::Move(Value);
    }
    for (const hz::fs::Entry &E : View_->Entries) {
        DesktopItem Item;
        Item.Name = E.Name;
        Item.Path = hz::fs::Join(Folder_, E.Name.CStr());
        Item.Kind = E.Directory ? ItemKind::Folder : ItemKind::File;
        Item.Glyph = GlyphForEntry(E);
        Item.Tile = TileForEntry(E);
        View_->Items.Push(Item);
    }
    View_->SetSelected(hz::NotFound);
    View_->InvalidateGrid();
}

void Desktop::Reload()
{
    View_->Shortcuts.Clear();
    /* Every pinned application (Start's pinned grid, from the manifests), in Order, then Trash:
       the desktop keeps step with the installed apps, and an app added later gets an icon here.
       The installer is not a desktop icon: installing is a system action, not a shortcut. */
    for (const hz::apps::App &Program : g_Shell.Apps) {
        if (!Program.Pinned || Program.Id == "setup")
            continue;
        DesktopItem Item;
        Item.Name = Program.Name;
        Item.App = &Program;
        Item.Kind = ItemKind::App;
        View_->Shortcuts.Push(Item);
    }
    DesktopItem Bin;
    Bin.Name = "Trash";
    Bin.Kind = ItemKind::Trash;
    Bin.Glyph = Icon::Trash;
    Bin.Tile = kTrashTile;
    View_->Shortcuts.Push(Bin);

    View_->Entries.Clear();
    hz::Vector<hz::fs::Entry> Found;
    if (HZ_SUCCESS(hz::fs::List(Folder_, Found)))
        for (const hz::fs::Entry &E : Found)
            View_->Entries.Push(E);

    RebuildView();
    View_->Reveal();
    HzPrint("shell: desktop %zu icons in %s", View_->Items.Size(), Folder_.CStr());
}

/* ---- Opening -------------------------------------------------------------------------------- */

void Desktop::Open(size_t Index)
{
    if (Index >= View_->Items.Size())
        return;
    const DesktopItem &Item = View_->Items[Index];
    switch (Item.Kind) {
    case ItemKind::App: {
        uint32_t Pid = 0;
        STATUS Status = hz::apps::Launch(*Item.App, nullptr, &Pid);
        if (HZ_SUCCESS(Status))
            HzPrint("shell: desktop opened %s (process %u)", Item.App->Name.CStr(), Pid);
        else {
            HzPrint("shell: desktop cannot start %s (0x%08X)", Item.App->Name.CStr(), (uint32_t)Status);
            MessageBox(this, "Desktop",
                       hz::String::Format("%s cannot be started (0x%08X).", Item.App->Name.CStr(), (uint32_t)Status).CStr(),
                       nullptr, nullptr);
        }
        return;
    }
    case ItemKind::Trash:
        OpenInFiles(hz::fs::TrashFolder());
        return;
    case ItemKind::Folder:
        OpenInFiles(Item.Path);
        return;
    default:
        break;
    }
    hz::String Program;
    uint32_t Pid = 0;
    STATUS Status = hz::apps::OpenPath(Item.Path, &Pid, &Program);
    if (HZ_SUCCESS(Status)) {
        HzPrint("shell: desktop opened %s with %s (process %u)", Item.Path.CStr(), Program.CStr(), Pid);
        return;
    }
    HzPrint("shell: desktop cannot open %s (0x%08X)", Item.Path.CStr(), (uint32_t)Status);
    if (Status == STATUS_NOT_FOUND) {
        hz::String Ext = hz::fs::Extension(Item.Path);
        MessageBox(this, "Desktop",
                   Ext.Empty() ? hz::String::Format("No program opens \"%s\" yet.", Item.Name.CStr()).CStr()
                               : hz::String::Format("No program opens %s files yet.", Ext.CStr()).CStr(),
                   nullptr, nullptr);
    } else {
        MessageBox(this, "Desktop",
                   hz::String::Format("%s cannot be opened (0x%08X).", Item.Name.CStr(), (uint32_t)Status).CStr(),
                   nullptr, nullptr);
    }
}

void Desktop::OpenInFiles(const hz::String &Path)
{
    const hz::apps::App *Files = g_Shell.FindApp(hz::String("files"));
    if (!Files) {
        MessageBox(this, "Desktop", "The Files program is not installed.", nullptr, nullptr);
        return;
    }
    uint32_t Pid = 0;
    STATUS Status = hz::apps::Launch(*Files, Path.CStr(), &Pid);
    if (HZ_SUCCESS(Status))
        HzPrint("shell: desktop opened %s in Files (process %u)", Path.CStr(), Pid);
    else
        HzPrint("shell: desktop cannot open Files (0x%08X)", (uint32_t)Status);
}

/* ---- Menus ------------------------------------------------------------------------------------ */

void Desktop::ShowItemMenu(size_t Index, Point Screen)
{
    if (Index >= View_->Items.Size())
        return;
    const DesktopItem &Item = View_->Items[Index];
    hz::Vector<MenuItem> Items;
    Items.Push(MenuItem::Make("Open", [this, Index]() { Open(Index); }, "Enter"));
    if (Item.Kind != ItemKind::Trash)
        Items.Push(MenuItem::Make("Open in Files", [this, Item]() { OpenInFiles(Item.Path); }));
    if (Item.Kind == ItemKind::Trash) {
        Items.Push(MenuItem::MakeSeparator());
        MenuItem Empty = MenuItem::Make("Empty Trash", [this]() { EmptyTrash(); });
        Empty.Enabled = hz::fs::TrashHasItems();
        Items.Push(hz::Move(Empty));
        Items.Push(MenuItem::MakeSeparator());
        Items.Push(MenuItem::Make("Properties", [this, Index]() { ShowProperties(Index); }));
        ShowMenu(this, Screen, hz::Move(Items));
        return;
    }
    if (Item.Kind == ItemKind::App) {
        Items.Push(MenuItem::MakeSeparator());
        Items.Push(MenuItem::Make("Properties", [this, Index]() { ShowProperties(Index); }));
        ShowMenu(this, Screen, hz::Move(Items));
        return;
    }

    /* Files and folders: the same operations as Files offers. */
    bool Writable = this->Writable();
    Items.Push(MenuItem::MakeSeparator());
    MenuItem Cut = MenuItem::Make("Cut", [this, Index]() { CopyItem(Index, true); }, "Ctrl+X");
    Cut.Enabled = Writable;
    Items.Push(hz::Move(Cut));
    Items.Push(MenuItem::Make("Copy", [this, Index]() { CopyItem(Index, false); }, "Ctrl+C"));
    MenuItem PasteItem = MenuItem::Make("Paste", [this]() { Paste(); }, "Ctrl+V");
    PasteItem.Enabled = Writable && !ClipPath_.Empty();
    Items.Push(hz::Move(PasteItem));
    Items.Push(MenuItem::MakeSeparator());
    MenuItem Rename = MenuItem::Make("Rename", [this, Index]() { RenameItem(Index); }, "F2");
    Rename.Enabled = Writable;
    Items.Push(hz::Move(Rename));
    MenuItem Delete = MenuItem::Make("Delete", [this, Index]() { DeleteItem(Index); }, "Del");
    Delete.Enabled = Writable;
    Items.Push(hz::Move(Delete));
    Items.Push(MenuItem::MakeSeparator());
    Items.Push(MenuItem::Make("Properties", [this, Index]() { ShowProperties(Index); }));
    ShowMenu(this, Screen, hz::Move(Items));
}

void Desktop::ShowBackgroundMenu(Point Screen)
{
    hz::Vector<MenuItem> Items;
    Items.Push(MenuItem::Make("View", [this, Screen]() { ShowViewMenu(Screen); }));
    Items.Push(MenuItem::Make("Sort by", [this, Screen]() { ShowSortMenu(Screen); }));
    Items.Push(MenuItem::Make("Refresh", [this]() {
        HzPrint("shell: desktop refresh");
        Reload();
    }, "F5"));
    Items.Push(MenuItem::MakeSeparator());
    MenuItem PasteItem = MenuItem::Make("Paste", [this]() { Paste(); }, "Ctrl+V");
    PasteItem.Enabled = Writable() && !ClipPath_.Empty();
    Items.Push(hz::Move(PasteItem));
    Items.Push(MenuItem::MakeSeparator());
    MenuItem NewFolder = MenuItem::Make("New folder", [this]() { NewItem(true); });
    NewFolder.Enabled = Writable();
    Items.Push(hz::Move(NewFolder));
    MenuItem NewFile = MenuItem::Make("New text document", [this]() { NewItem(false); });
    NewFile.Enabled = Writable();
    Items.Push(hz::Move(NewFile));
    Items.Push(MenuItem::MakeSeparator());
    Items.Push(MenuItem::Make("Open Terminal", [this]() { LaunchApp(this, "terminal", nullptr); }));
    Items.Push(MenuItem::MakeSeparator());
    Items.Push(MenuItem::Make("Personalize", [this]() { LaunchApp(this, "settings", "Personalization"); }));
    Items.Push(MenuItem::Make("Display settings", [this]() { LaunchApp(this, "settings", "System"); }));
    ShowMenu(this, Screen, hz::Move(Items), 220);
}

void Desktop::ShowViewMenu(Point Screen)
{
    hz::Vector<MenuItem> Items;
    for (int i = 0; i < 3; i++) {
        MenuItem Entry = MenuItem::Make(kViewNames[i], [this, i]() {
            View_->IconSize = kViewSizes[i];
            SavePrefs();
            RebuildView();
            View_->Reveal();
        });
        Entry.Checked = View_->IconSize == kViewSizes[i];
        Items.Push(hz::Move(Entry));
    }
    ShowMenu(this, Screen, hz::Move(Items));
}

void Desktop::ShowSortMenu(Point Screen)
{
    hz::Vector<MenuItem> Items;
    for (int i = 0; i < SortKeyCount; i++) {
        MenuItem Entry = MenuItem::Make(SortName(i), [this, i]() {
            View_->SortKey = i;
            SavePrefs();
            RebuildView();
            View_->Reveal();
        });
        Entry.Checked = View_->SortKey == i;
        Items.Push(hz::Move(Entry));
    }
    Items.Push(MenuItem::MakeSeparator());
    MenuItem Descending = MenuItem::Make("Descending", [this]() {
        View_->SortDescending = !View_->SortDescending;
        SavePrefs();
        RebuildView();
        View_->Reveal();
    });
    Descending.Checked = View_->SortDescending;
    Items.Push(hz::Move(Descending));
    ShowMenu(this, Screen, hz::Move(Items));
}

void Desktop::ShowNewMenu(Point Screen)
{
    hz::Vector<MenuItem> Items;
    Items.Push(MenuItem::Make("Folder", [this]() { NewItem(true); }));
    Items.Push(MenuItem::Make("Text document", [this]() { NewItem(false); }));
    ShowMenu(this, Screen, hz::Move(Items));
}

void Desktop::OnContextMenu(Point Screen)
{
    ShowBackgroundMenu(Screen);
}

/* ---- File operations -------------------------------------------------------------------------- */

bool Desktop::Writable() const
{
    hz::fs::Volume V;
    if (HZ_SUCCESS(hz::fs::QueryVolume(Folder_, V)))
        return !V.ReadOnly;
    return !hz::fs::IsLiveSystem();
}

void Desktop::Select(const hz::String &Name)
{
    for (size_t i = 0; i < View_->Items.Size(); i++)
        if (hz::fs::EqualNames(View_->Items[i].Name.CStr(), Name.CStr())) {
            View_->SetSelected(i);
            return;
        }
}

void Desktop::NewItem(bool Directory)
{
    if (!Writable())
        return;
    hz::String Name = Directory ? hz::String("New folder") : hz::String("New text document.txt");
    for (int i = 2; i < 100 && hz::fs::Exists(hz::fs::Join(Folder_, Name.CStr())); i++)
        Name = Directory ? hz::String::Format("New folder (%d)", i)
                         : hz::String::Format("New text document (%d).txt", i);
    InputBox(this, Directory ? "New folder" : "New text document",
             Directory ? "Name of the new folder" : "Name of the new file", Name.CStr(), "Create",
             [this, Directory](const hz::String &Typed) {
                 if (Typed.Empty())
                     return;
                 if (!ValidName(Typed.CStr())) {
                     MessageBox(this, "Desktop",
                                "A name cannot contain \\ / : * ? \" < > | or end with a dot or a space.", nullptr,
                                nullptr);
                     return;
                 }
                 hz::String Path = hz::fs::Join(Folder_, Typed.CStr());
                 if (hz::fs::Exists(Path)) {
                     MessageBox(this, "Desktop",
                                hz::String::Format("This folder already has an item named \"%s\".", Typed.CStr()).CStr(),
                                nullptr, nullptr);
                     return;
                 }
                 STATUS Status = Directory ? hz::fs::CreateDirectory(Path) : hz::fs::WriteAll(Path, "", 0);
                 if (!HZ_SUCCESS(Status)) {
                     HzPrint("shell: desktop cannot create %s (0x%08X)", Path.CStr(), (uint32_t)Status);
                     MessageBox(this, "Desktop",
                                hz::String::Format("%s cannot be created: %s.", Typed.CStr(), hz::fs::StatusText(Status)).CStr(),
                                nullptr, nullptr);
                     return;
                 }
                 HzPrint("shell: desktop created %s", Path.CStr());
                 Reload();
                 Select(Typed);
             });
}

void Desktop::RenameItem(size_t Index)
{
    if (Index >= View_->Items.Size())
        return;
    const DesktopItem &Item = View_->Items[Index];
    if (Item.Kind == ItemKind::App || Item.Kind == ItemKind::Trash || !Writable())
        return;
    hz::String Old = Item.Name, Path = Item.Path;
    InputBox(this, "Rename", "New name", Old.CStr(), "Rename", [this, Old, Path](const hz::String &Typed) {
        if (Typed.Empty() || Typed == Old)
            return;
        if (!ValidName(Typed.CStr())) {
            MessageBox(this, "Desktop", "A name cannot contain \\ / : * ? \" < > | or end with a dot or a space.",
                       nullptr, nullptr);
            return;
        }
        if (hz::fs::Exists(hz::fs::Join(Folder_, Typed.CStr()))) {
            MessageBox(this, "Desktop",
                       hz::String::Format("This folder already has an item named \"%s\".", Typed.CStr()).CStr(), nullptr,
                       nullptr);
            return;
        }
        STATUS Status = hz::fs::Rename(Path, Typed.CStr());
        if (!HZ_SUCCESS(Status)) {
            MessageBox(this, "Desktop",
                       hz::String::Format("%s cannot be renamed: %s.", Old.CStr(), hz::fs::StatusText(Status)).CStr(),
                       nullptr, nullptr);
            return;
        }
        HzPrint("shell: desktop renamed %s to %s", Path.CStr(), Typed.CStr());
        Reload();
        Select(Typed);
    });
}

void Desktop::DeleteItem(size_t Index)
{
    if (Index >= View_->Items.Size())
        return;
    const DesktopItem &Item = View_->Items[Index];
    if (Item.Kind == ItemKind::App || Item.Kind == ItemKind::Trash || !Writable())
        return;
    hz::String Name = Item.Name, Path = Item.Path;
    hz::String Question =
        Item.Kind == ItemKind::Folder
            ? hz::String::Format("Move the folder \"%s\" and everything in it to the Trash?", Name.CStr())
            : hz::String::Format("Move \"%s\" to the Trash?", Name.CStr());
    MessageBox(this, "Delete", Question.CStr(), "Move to Trash|Cancel", [this, Name, Path](int Choice) {
        if (Choice != 0)
            return;
        STATUS Status = hz::fs::Trash(Path);
        if (hz::fs::EqualNames(ClipPath_.CStr(), Path.CStr()) || hz::fs::IsInside(ClipPath_, Path))
            ClipPath_ = hz::String();
        if (!HZ_SUCCESS(Status)) {
            HzPrint("shell: desktop cannot trash %s (0x%08X)", Path.CStr(), (uint32_t)Status);
            MessageBox(this, "Desktop",
                       hz::String::Format("%s cannot be moved to the Trash: %s.", Name.CStr(), hz::fs::StatusText(Status)).CStr(),
                       nullptr, nullptr);
            return;
        }
        HzPrint("shell: desktop deleted %s to Trash", Path.CStr());
        Reload();
    });
}

void Desktop::ShowProperties(size_t Index)
{
    if (Index >= View_->Items.Size())
        return;
    const DesktopItem &Item = View_->Items[Index];
    hz::String Text;
    if (Item.Kind == ItemKind::App) {
        const hz::apps::App &A = *Item.App;
        Text = hz::String::Format("%s\n\nType:        Program\nDescription: %s\nProgram:     %s", A.Name.CStr(),
                                  A.Description.Empty() ? "(none)" : A.Description.CStr(), A.Path.CStr());
    } else if (Item.Kind == ItemKind::Trash) {
        hz::Vector<hz::fs::TrashItem> Bin;
        hz::fs::TrashList(Bin);
        Text = hz::String::Format("Trash\n\nType:        System folder\nLocation:    %s\nContains:    %zu item%s",
                                  hz::fs::TrashFolder().CStr(), Bin.Size(), Bin.Size() == 1 ? "" : "s");
    } else {
        hz::fs::Entry E;
        if (!HZ_SUCCESS(hz::fs::Stat(Item.Path, E))) {
            MessageBox(this, "Properties",
                       hz::String::Format("%s is no longer there.", Item.Name.CStr()).CStr(), nullptr, nullptr);
            return;
        }
        hz::String Time = E.Modified ? hz::fs::FormatTime(E.Modified, g_Shell.UtcOffset) : hz::String("(unknown)");
        if (E.Directory) {
            Text = hz::String::Format("%s\n\nType:        Folder\nModified:    %s\nLocation:    %s", E.Name.CStr(),
                                      Time.CStr(), Folder_.CStr());
        } else {
            Text = hz::String::Format("%s\n\nType:        %s\nSize:        %s\nModified:    %s\nLocation:    %s",
                                      E.Name.CStr(), hz::fs::Extension(E.Name).Empty() ? "File" : "Document",
                                      hz::fs::FormatSize(E.Size).CStr(), Time.CStr(), Folder_.CStr());
        }
        if (g_Shell.UtcOffset == 0)
            Text.Append("\n\n(The clock is set to UTC.)");
    }
    MessageBox(this, "Properties", Text.CStr(), nullptr, nullptr);
}

void Desktop::CopyItem(size_t Index, bool Cut)
{
    if (Index >= View_->Items.Size())
        return;
    const DesktopItem &Item = View_->Items[Index];
    if (Item.Kind == ItemKind::App || Item.Kind == ItemKind::Trash)
        return;
    if (Cut && !Writable())
        return;
    ClipPath_ = Item.Path;
    ClipCut_ = Cut;
    HzPrint("shell: desktop %s %s", Cut ? "cut" : "copied", ClipPath_.CStr());
}

void Desktop::Paste()
{
    if (ClipPath_.Empty())
        return;
    hz::String Target = hz::fs::Join(Folder_, hz::fs::FileName(ClipPath_).CStr());
    if (!Writable()) {
        MessageBox(this, "Desktop", "This volume is read-only.", nullptr, nullptr);
        return;
    }
    hz::String Source = ClipPath_;
    if (ClipCut_ && hz::fs::EqualNames(hz::fs::Parent(Source).CStr(), Folder_.CStr())) {
        HzPrint("shell: desktop nothing to do: %s is already here", Source.CStr());
        ClipPath_ = hz::String();
        return;
    }
    if (hz::fs::IsInside(Source, Folder_)) {
        MessageBox(this, "Desktop", "A folder cannot be pasted into itself.", nullptr, nullptr);
        return;
    }
    if (!hz::fs::Exists(Source)) {
        MessageBox(this, "Desktop", hz::String::Format("%s is no longer there.", Source.CStr()).CStr(), nullptr,
                   nullptr);
        ClipPath_ = hz::String();
        return;
    }
    hz::String Name = hz::fs::FileName(Source);
    if (hz::fs::Exists(Target)) { /* never overwrite: find a free name */
        hz::String Base = Name, Ext;
        size_t Dot = Name.Length();
        while (Dot > 0 && Name[Dot - 1] != '.' && Name[Dot - 1] != '\\')
            Dot--;
        if (Dot > 0 && Name[Dot - 1] == '.') {
            Base = Name.Substring(0, Dot - 1);
            Ext = Name.Substring(Dot - 1);
        }
        if (hz::fs::EqualNames(Source.CStr(), Target.CStr()))
            Target = hz::fs::Join(Folder_, hz::String::Format("%s - Copy%s", Base.CStr(), Ext.CStr()).CStr());
        else {
            int Copy = 2;
            do {
                Target = hz::fs::Join(
                    Folder_, hz::String::Format("%s - Copy (%d)%s", Base.CStr(), Copy, Ext.CStr()).CStr());
                Copy++;
            } while (hz::fs::Exists(Target) && Copy < 100);
        }
    }
    uint64_t Start = NowUs();
    STATUS Status = ClipCut_ ? hz::fs::MoveTree(Source, Target) : hz::fs::CopyTree(Source, Target);
    if (!HZ_SUCCESS(Status)) {
        HzPrint("shell: desktop cannot paste %s (0x%08X)", Source.CStr(), (uint32_t)Status);
        MessageBox(this, "Desktop",
                   hz::String::Format("%s cannot be pasted: %s.", Name.CStr(), hz::fs::StatusText(Status)).CStr(),
                   nullptr, nullptr);
        return;
    }
    HzPrint("shell: desktop %s %s to %s (%u ms)", ClipCut_ ? "moved" : "pasted", Source.CStr(), Target.CStr(),
            (uint32_t)((NowUs() - Start) / 1000));
    if (ClipCut_)
        ClipPath_ = hz::String();
    Reload();
    Select(hz::fs::FileName(Target));
}

void Desktop::EmptyTrash()
{
    hz::Vector<hz::fs::TrashItem> Bin;
    hz::fs::TrashList(Bin);
    if (Bin.Empty()) {
        MessageBox(this, "Trash", "The Trash is empty.", nullptr, nullptr);
        return;
    }
    size_t Count = Bin.Size();
    hz::String Question = hz::String::Format(
        "Permanently delete %zu item%s in the Trash? This cannot be undone.", Count, Count == 1 ? "" : "s");
    MessageBox(this, "Empty Trash", Question.CStr(), "Empty Trash|Cancel", [this](int Choice) {
        if (Choice != 0)
            return;
        STATUS Status = hz::fs::TrashEmpty();
        if (!HZ_SUCCESS(Status)) {
            HzPrint("shell: desktop cannot empty the trash (0x%08X)", (uint32_t)Status);
            MessageBox(this, "Trash",
                       hz::String::Format("The Trash cannot be emptied: %s.", hz::fs::StatusText(Status)).CStr(),
                       nullptr, nullptr);
            return;
        }
        HzPrint("shell: desktop emptied the trash");
        Reload();
    });
}

/* ---- Keyboard ---------------------------------------------------------------------------------- */

bool Desktop::OnKey(KeyEvent &E)
{
    size_t Selected = View_->Selected;
    bool Has = Selected != hz::NotFound;
    if (E.Key == HZ_VK_RETURN) {
        if (!Has)
            return false;
        Open(Selected);
        return true;
    }
    if (E.Key == HZ_VK_F1 + 1) { /* F2 */
        if (!Has)
            return false;
        RenameItem(Selected);
        return true;
    }
    if (E.Key == HZ_VK_DELETE) {
        if (!Has)
            return false;
        DeleteItem(Selected);
        return true;
    }
    if (E.Key == HZ_VK_F1 + 4) { /* F5 */
        HzPrint("shell: desktop refresh");
        Reload();
        return true;
    }
    if (E.Ctrl() && (E.Key == 'C')) {
        if (!Has)
            return false;
        CopyItem(Selected, false);
        return true;
    }
    if (E.Ctrl() && (E.Key == 'X')) {
        if (!Has)
            return false;
        CopyItem(Selected, true);
        return true;
    }
    if (E.Ctrl() && (E.Key == 'V')) {
        Paste();
        return true;
    }
    if (E.Shift() && E.Key == HZ_VK_F1 + 9) { /* Shift+F10: the context menu */
        Point Origin = ScreenOrigin();
        if (Has) {
            Rect Cell = View_->CellRect(Selected);
            ShowItemMenu(Selected, Point{Origin.X + Cell.X + 8, Origin.Y + Cell.Y + 8});
        } else {
            ShowBackgroundMenu(Point{Origin.X + 24, Origin.Y + kWorkAreaTop + 24});
        }
        return true;
    }
    if (E.Key == HZ_VK_ESCAPE) {
        View_->SetSelected(hz::NotFound);
        return true;
    }
    if (E.Key == HZ_VK_LEFT) {
        View_->MoveSelection(-1, 0);
        return true;
    }
    if (E.Key == HZ_VK_RIGHT) {
        View_->MoveSelection(1, 0);
        return true;
    }
    if (E.Key == HZ_VK_UP) {
        View_->MoveSelection(0, -1);
        return true;
    }
    if (E.Key == HZ_VK_DOWN) {
        View_->MoveSelection(0, 1);
        return true;
    }
    return false;
}

int64_t Desktop::HandleMessage(uint32_t Message, uint64_t WParam, uint64_t LParam)
{
    if (Message == HZ_WM_KEYDOWN) {
        KeyEvent E;
        E.Key = (uint32_t)WParam;
        E.Modifiers = HZ_KEY_DATA_MODIFIERS(LParam);
        if (OnKey(E))
            return 0;
    }
    return Window::HandleMessage(Message, WParam, LParam);
}

/* ---- Factory ---------------------------------------------------------------------------------- */

Desktop *CreateDesktop()
{
    if (!g_Shell.Screen.Width || !g_Shell.Screen.Height)
        return nullptr;
    Desktop *Desk = new Desktop(g_Shell.Screen.Width, g_Shell.Screen.Height);
    if (!Desk->ShowAt(0, 0)) {
        delete Desk;
        return nullptr;
    }
    HzPrint("shell: desktop ready");
    return Desk;
}

}  // namespace shell
