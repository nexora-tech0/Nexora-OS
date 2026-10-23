/*
 * Nexora OS - user/apps/files/files.cpp
 * Files: browses volumes and folders. A command bar (back, up, refresh, new folder, cut, copy,
 * paste, rename, delete), an address box (Ctrl+L), the volumes on the left with their free space,
 * and the folder's content on the right (icon, name, date, type, size). Enter or a double-click
 * opens a folder, runs a program or opens a document with the program whose manifest lists its
 * extension. Typing a name selects the first match (type-ahead). Ctrl+X/C/V cut, copy and paste
 * a file or a folder with everything in it (M11; the clipboard of files belongs to this window);
 * a pasted copy next to its original is named "X - Copy". Delete removes a folder with its
 * content after a confirmation that counts it. Changes are possible on writable volumes only.
 * Navigation and actions are logged ("files: ...") for the smoke tests.
 */
#include "hzapps.h"
#include "hzfile.h"
#include "hzui.h"

using namespace hzui;

namespace {

/* The file list with type-ahead: characters typed within a second form a prefix to match. */
class FileList final : public ListView {
public:
    hz::Function<size_t(const hz::String &)> FindPrefix;
    hz::Function<void(KeyEvent &)> OnKey;
    hz::Function<void(size_t, Point)> OnRightClick;

    void OnPointerDown(PointerEvent &E) override
    {
        ListView::OnPointerDown(E);
    }

    void OnPointerUp(PointerEvent &E) override
    {
        if (E.Button == HZ_MOUSE_RIGHT) {
            size_t Index = IndexAt(E.Position);
            if (Index != hz::NotFound && OnRightClick) {
                OnRightClick(Index, E.ScreenPosition);
                E.Handled = true;
            }
        }
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

const char *TypeName(const hz::fs::Entry &E)
{
    if (E.Directory)
        return "Folder";
    hz::String Ext = hz::fs::Extension(E.Name);
    if (Ext == "exe")
        return "Program";
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
    if (hz::fs::Extension(E.Name) == "exe")
        return Icon::Prompt;
    return Icon::Document;
}

class FilesApp {
public:
    bool Create(const char *Start);

private:
    void Build();
    void Navigate(const hz::String &Path, bool Record = true);
    void Refresh();
    void UpdateCommands();
    void UpdateStatus();
    void Open(size_t Index);
    void Back();
    void Up();
    void NewFolder();
    void RenameSelected();
    void DeleteSelected();
    void ClipSelected(bool Cut);
    void ShowItemContextMenu(size_t Index, Point Screen);
    void Paste();
    bool Writable() const;
    const hz::fs::Entry *Selected() const;
    void Select(const hz::String &Name);

    Window *Main_ = nullptr;
    TextBox *Address_ = nullptr;
    ListView *Places_ = nullptr;
    FileList *List_ = nullptr;
    TextBlock *Status_ = nullptr, *Free_ = nullptr;
    Button *BackButton_ = nullptr, *UpButton_ = nullptr, *NewButton_ = nullptr, *RenameButton_ = nullptr,
           *DeleteButton_ = nullptr, *CutButton_ = nullptr, *CopyButton_ = nullptr, *PasteButton_ = nullptr;
    hz::String ClipPath_; /* cut or copied; "" if none */
    bool ClipCut_ = false;
    hz::Vector<hz::fs::Volume> Volumes_;
    hz::Vector<hz::fs::Entry> Entries_;
    hz::Vector<hz::String> History_;
    hz::String Folder_;
    hz::fs::Volume Volume_;
};

/* ---- Window ---------------------------------------------------------------------------------- */

bool FilesApp::Create(const char *Start)
{
    Main_ = new Window("Files", 960, 600);
    Main_->SetMinimumSize(560, 320);
    Build();
    Main_->OnClosed = []() { NtUserPostQuitMessage(0); };
    Main_->OnUnhandledKey = [this](KeyEvent &E) {
        if (E.Ctrl() && E.Key == 'L') {
            E.Handled = true;
            Main_->SetFocus(Address_, true);
            Address_->SelectAll();
        } else if (E.Key == HZ_VK_F1 + 4) { /* F5 */
            E.Handled = true;
            Refresh();
        } else if (E.Alt() && E.Key == HZ_VK_LEFT) {
            E.Handled = true;
            Back();
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
    Volumes_ = hz::fs::Volumes();
    Grid *Root = new Grid();
    Root->Rows.Push(GridLength::AutoSize());
    Root->Rows.Push(GridLength::Stars(1));
    Root->Rows.Push(GridLength::AutoSize());

    /* Command bar and address */
    Grid *Bar = Root->Add<Grid>();
    Bar->Padding = Thickness(10, 6, 10, 8);
    Bar->ColumnSpacing = 4;
    for (int i = 0; i < 3; i++)
        Bar->Columns.Push(GridLength::AutoSize());
    Bar->Columns.Push(GridLength::Stars(1));
    for (int i = 0; i < 6; i++)
        Bar->Columns.Push(GridLength::AutoSize());
    auto Command = [Bar](uint8_t Column, Icon Glyph, const char *Text, const char *Tip) {
        Button *B = Bar->Add<Button>(Text, ButtonStyle::Subtle);
        B->GridColumn = Column;
        B->Glyph = Glyph;
        B->ToolTip = Tip;
        return B;
    };
    BackButton_ = Command(0, Icon::ChevronLeft, "", "Back (Alt+Left)");
    BackButton_->OnClick = [this]() { Back(); };
    UpButton_ = Command(1, Icon::ArrowUp, "", "Up (Alt+Up)");
    UpButton_->OnClick = [this]() { Up(); };
    Button *RefreshButton = Command(2, Icon::Refresh, "", "Refresh (F5)");
    RefreshButton->OnClick = [this]() { Refresh(); };
    Address_ = Bar->Add<TextBox>("Path");
    Address_->GridColumn = 3;
    Address_->Margin = Thickness(6, 0, 10, 0);
    Address_->MaxLength = 250;
    Address_->OnSubmit = [this](const hz::String &Text) {
        hz::String Path = hz::fs::Resolve(Folder_, Text.CStr());
        bool Directory = false;
        if (hz::fs::Exists(Path, &Directory) && !Directory) {
            hz::String Program;
            STATUS Status = hz::apps::OpenPath(Path, nullptr, &Program);
            if (HZ_SUCCESS(Status))
                HzPrint("files: opened %s with %s", Path.CStr(), Program.CStr());
            Address_->SetText(Folder_.CStr(), false);
            return;
        }
        Navigate(Path);
        Main_->SetFocus(List_);
    };
    NewButton_ = Command(4, Icon::Add, "New folder", "New folder (Ctrl+Shift+N)");
    NewButton_->OnClick = [this]() { NewFolder(); };
    CutButton_ = Command(5, Icon::Cut, "", "Cut (Ctrl+X)");
    CutButton_->OnClick = [this]() { ClipSelected(true); };
    CopyButton_ = Command(6, Icon::Copy, "", "Copy (Ctrl+C)");
    CopyButton_->OnClick = [this]() { ClipSelected(false); };
    PasteButton_ = Command(7, Icon::Paste, "", "Paste (Ctrl+V)");
    PasteButton_->OnClick = [this]() { Paste(); };
    RenameButton_ = Command(8, Icon::Pencil, "", "Rename (F2)");
    RenameButton_->OnClick = [this]() { RenameSelected(); };
    DeleteButton_ = Command(9, Icon::Trash, "", "Delete (Del)");
    DeleteButton_->OnClick = [this]() { DeleteSelected(); };

    /* Volumes and the folder */
    Grid *Middle = Root->Add<Grid>();
    Middle->GridRow = 1;
    Middle->Padding = Thickness(10, 0, 10, 0);
    Middle->ColumnSpacing = 10;
    Middle->Columns.Push(GridLength::Pixels(210));
    Middle->Columns.Push(GridLength::Stars(1));
    Places_ = Middle->Add<ListView>();
    Places_->ItemHeight = 48;
    Places_->Background = Theme::CardBackground;
    Places_->CornerRadius = Theme::OverlayRadius;
    Places_->SetItemCount(Volumes_.Size());
    Places_->PaintItem = [this](Painter &P, const Rect &R, size_t I) {
        const hz::fs::Volume &V = Volumes_[I];
        P.DrawIcon(Icon::Drive, Rect{R.X + 2, R.Y + 8, 18, 18}, Theme::TextSecondary);
        hz::String Name = hz::String::Format("%s (%c:)", V.Label.Empty() ? "Local disk" : V.Label.CStr(), V.Root[0]);
        P.TextLine(GetFont(FontRole::Body), Rect{R.X + 30, R.Y + 4, R.Width - 30, 22}, Name.CStr(), Theme::TextPrimary);
        hz::String Detail = V.ReadOnly ? hz::String::Format("%s, read-only", V.FileSystem.CStr())
                                       : hz::String::Format("%s free", hz::fs::FormatSize(V.FreeBytes).CStr());
        P.TextLine(GetFont(FontRole::Caption), Rect{R.X + 30, R.Y + 25, R.Width - 30, 18}, Detail.CStr(), Theme::TextTertiary);
    };
    Places_->OnSelectionChanged = [this](size_t I) {
        if (I < Volumes_.Size() && !hz::fs::EqualNames(Folder_.Substring(0, 3).CStr(), Volumes_[I].Root.CStr()))
            Navigate(Volumes_[I].Root);
    };

    Grid *Right = Middle->Add<Grid>();
    Right->GridColumn = 1;
    Right->Background = Theme::CardBackground;
    Right->CornerRadius = Theme::OverlayRadius;
    Right->Rows.Push(GridLength::Pixels(30));
    Right->Rows.Push(GridLength::Stars(1));
    /* Column headings (drawn to match PaintItem below) */
    class Headings final : public Element {
    public:
        Headings() { HitTestVisible = false; }
        void Paint(Painter &P) override
        {
            const Rect &R = Bounds();
            HZ_FONT *F = GetFont(FontRole::Caption);
            int32_t Right = R.Right() - 22;
            P.TextLine(F, Rect{R.X + 46, R.Y, 200, R.Height}, "Name", Theme::TextTertiary);
            P.TextLine(F, Rect{Right - 430, R.Y, 140, R.Height}, "Date modified", Theme::TextTertiary);
            P.TextLine(F, Rect{Right - 280, R.Y, 140, R.Height}, "Type", Theme::TextTertiary);
            P.TextLine(F, Rect{Right - 100, R.Y, 100, R.Height}, "Size", Theme::TextTertiary, Align::End);
            P.FillRect(Rect{R.X + 8, R.Bottom() - 1, R.Width - 16, 1}, Theme::Divider);
        }
    };
    Right->Add<Headings>();
    List_ = Right->Add<FileList>();
    List_->GridRow = 1;
    List_->ItemHeight = 32;
    List_->Margin = Thickness(4);
    List_->PaintItem = [this](Painter &P, const Rect &R, size_t I) {
        if (I >= Entries_.Size())
            return;
        const hz::fs::Entry &E = Entries_[I];
        HZ_FONT *F = GetFont(FontRole::Body);
        int32_t Right = R.Right() - 14;
        P.DrawIcon(IconFor(E), Rect{R.X + 10, R.Y + (R.Height - 18) / 2, 18, 18},
                   E.Directory ? 0xFFFFC66B : (IconFor(E) == Icon::Prompt ? 0xFF8FB8FF : Theme::TextSecondary));
        P.TextLine(F, Rect{R.X + 38, R.Y, Right - 440 - R.X - 38, R.Height}, E.Name.CStr(), Theme::TextPrimary);
        P.TextLine(F, Rect{Right - 430, R.Y, 140, R.Height}, hz::fs::FormatTime(E.Modified).CStr(), Theme::TextSecondary);
        P.TextLine(F, Rect{Right - 280, R.Y, 160, R.Height}, TypeName(E), Theme::TextSecondary);
        if (!E.Directory)
            P.TextLine(F, Rect{Right - 100, R.Y, 100, R.Height}, hz::fs::FormatSize(E.Size).CStr(), Theme::TextSecondary,
                       Align::End);
    };
    List_->OnItemInvoked = [this](size_t I) { Open(I); };
    List_->OnRightClick = [this](size_t I, Point Screen) { ShowItemContextMenu(I, Screen); };
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
        } else if (E.Ctrl() && !E.Alt() && (E.Key == 'X' || E.Key == 'C')) {
            E.Handled = true;
            ClipSelected(E.Key == 'X');
        } else if (E.Ctrl() && !E.Alt() && E.Key == 'V') {
            E.Handled = true;
            Paste();
        }
    };

    /* Status bar */
    Grid *StatusBar = Root->Add<Grid>();
    StatusBar->GridRow = 2;
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

/* ---- Navigation ----------------------------------------------------------------------------------- */

void FilesApp::Navigate(const hz::String &Path, bool Record)
{
    hz::Vector<hz::fs::Entry> Entries;
    STATUS Status = hz::fs::List(Path, Entries);
    if (!HZ_SUCCESS(Status)) {
        HzPrint("files: cannot open %s (0x%08X)", Path.CStr(), (uint32_t)Status);
        Address_->SetText(Folder_.CStr(), false);
        MessageBox(Main_, "Files", hz::String::Format("%s cannot be opened: %s.", Path.CStr(), hz::fs::StatusText(Status)).CStr(),
                   nullptr, nullptr);
        return;
    }
    if (Record && !Folder_.Empty() && Folder_ != Path) {
        History_.Push(Folder_);
        if (History_.Size() > 50)
            History_.Erase(0);
    }
    Folder_ = Path;
    Entries_ = hz::Move(Entries);
    hz::fs::QueryVolume(Folder_, Volume_);
    Address_->SetText(Folder_.CStr(), false);
    List_->SetSelectedIndex(hz::NotFound, false);
    List_->SetItemCount(Entries_.Size());
    List_->Invalidate();
    for (size_t i = 0; i < Volumes_.Size(); i++)
        if (hz::fs::EqualNames(Volumes_[i].Root.CStr(), Folder_.Substring(0, 3).CStr()))
            Places_->SetSelectedIndex(i, false);
    hz::String Title = hz::fs::IsRoot(Folder_) ? Folder_ : hz::fs::FileName(Folder_);
    Main_->SetTitle(hz::String::Format("%s - Files", Title.CStr()).CStr());
    UpdateCommands();
    UpdateStatus();
    HzPrint("files: %s (%zu items)", Folder_.CStr(), Entries_.Size());
}

void FilesApp::Refresh()
{
    hz::String Keep = Selected() ? Selected()->Name : hz::String();
    Volumes_ = hz::fs::Volumes();
    Places_->SetItemCount(Volumes_.Size());
    Places_->Invalidate();
    Navigate(Folder_, false);
    if (!Keep.Empty())
        Select(Keep);
}

void FilesApp::Back()
{
    if (History_.Empty())
        return;
    hz::String Previous = History_.Back();
    History_.Pop();
    Navigate(Previous, false);
}

void FilesApp::Up()
{
    if (hz::fs::IsRoot(Folder_))
        return;
    hz::String Child = hz::fs::FileName(Folder_);
    Navigate(hz::fs::Parent(Folder_));
    Select(Child);
}

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

void FilesApp::UpdateCommands()
{
    bool HasSelection = Selected() != nullptr;
    BackButton_->SetEnabled(!History_.Empty());
    UpButton_->SetEnabled(!hz::fs::IsRoot(Folder_));
    NewButton_->SetEnabled(Writable());
    RenameButton_->SetEnabled(Writable() && HasSelection);
    DeleteButton_->SetEnabled(Writable() && HasSelection);
    CutButton_->SetEnabled(Writable() && HasSelection);
    CopyButton_->SetEnabled(HasSelection);
    PasteButton_->SetEnabled(Writable() && !ClipPath_.Empty());
}

void FilesApp::UpdateStatus()
{
    size_t Folders = 0, Files = 0;
    for (const hz::fs::Entry &E : Entries_)
        (E.Directory ? Folders : Files)++;
    hz::String Text = hz::String::Format("%zu item%s", Entries_.Size(), Entries_.Size() == 1 ? "" : "s");
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
        hz::String Path = hz::fs::Join(Folder_, Typed.CStr());
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

void FilesApp::RenameSelected()
{
    const hz::fs::Entry *E = Selected();
    if (!E || !Writable())
        return;
    hz::String Old = E->Name;
    InputBox(Main_, "Rename", "New name", Old.CStr(), "Rename", [this, Old](const hz::String &Typed) {
        if (Typed.Empty() || Typed == Old)
            return;
        hz::String Path = hz::fs::Join(Folder_, Old.CStr());
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

void FilesApp::ShowItemContextMenu(size_t Index, Point Screen)
{
    if (Index >= Entries_.Size())
        return;
    List_->SetSelectedIndex(Index);
    hz::Vector<MenuItem> Items;
    Items.Push(MenuItem::Make("Open", [this, Index]() { Open(Index); }));
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
    ShowMenu(Main_, Screen, hz::Move(Items));
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
