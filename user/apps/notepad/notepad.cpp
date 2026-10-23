/*
 * Nexora OS - user/apps/notepad/notepad.cpp
 * Notepad: edits one plain-text file. A menu bar (File, Edit, View), the multi-line editor, a find
 * bar (Ctrl+F) and a status bar (line, column, characters). The title shows the file name with a
 * '*' while there are unsaved changes; New, Open and closing the window ask first. A path on the
 * command line opens that file ("notepad.exe D:\notes.txt"). Opening and saving are logged
 * ("notepad: ...") for the smoke tests.
 */
#include "hzfile.h"
#include "hzui.h"

using namespace hzui;

namespace {

class Notepad {
public:
    bool Create(const char *InitialPath);

private:
    void Build();
    void UpdateTitle();
    void UpdateStatus();
    hz::String DisplayName() const;
    /* Runs Then once the current text is saved or the user chose to drop it (not if cancelled). */
    void ConfirmDiscard(hz::Function<void()> Then);
    void New();
    void OpenWithDialog();
    bool Load(const hz::String &Path, bool Report);
    void Save(hz::Function<void()> Then = nullptr);
    void SaveAs(hz::Function<void()> Then = nullptr);
    bool WriteTo(const hz::String &Path);
    void ShowFind(bool Show);
    void FindNext(bool Forward);
    void RequestClose();
    hz::Vector<MenuItem> FileMenu();
    hz::Vector<MenuItem> EditMenu();
    hz::Vector<MenuItem> ViewMenu();
    void ToggleWordWrap();
    void OnKey(KeyEvent &E);

    Window *Main_ = nullptr;
    TextEditor *Editor_ = nullptr;
    Grid *FindBar_ = nullptr;
    TextBox *FindBox_ = nullptr;
    Grid *StatusBar_ = nullptr;
    TextBlock *Position_ = nullptr, *Count_ = nullptr;
    hz::String Path_;
    hz::String LastTitle_;
    bool Closing_ = false;
};

hz::String FolderOf(const hz::String &Path)
{
    return Path.Empty() ? hz::String() : hz::fs::Parent(Path);
}

size_t CodePoints(const char *Text, size_t Length)
{
    size_t Count = 0;
    for (size_t i = 0; i < Length; i++)
        if (((uint8_t)Text[i] & 0xC0) != 0x80)
            Count++;
    return Count;
}

/* ---- Window and layout ------------------------------------------------------------------------- */

bool Notepad::Create(const char *InitialPath)
{
    Main_ = new Window("Notepad", 820, 560);
    Main_->SetMinimumSize(420, 260);
    Build();
    Main_->OnCloseRequested = [this]() {
        if (Closing_ || !Editor_->Modified)
            return true;
        RequestClose();
        return false;
    };
    Main_->OnClosed = []() { NtUserPostQuitMessage(0); };
    Main_->OnUnhandledKey = [this](KeyEvent &E) { OnKey(E); };
    if (InitialPath && *InitialPath) {
        hz::String Path = hz::fs::Resolve(hz::String("C:\\"), InitialPath);
        if (hz::fs::Exists(Path))
            Load(Path, true);
        else
            Path_ = Path; /* a new file: saved there on Ctrl+S */
    }
    UpdateTitle();
    UpdateStatus();
    if (!Main_->Show())
        return false;
    Main_->SetFocus(Editor_);
    return true;
}

void Notepad::Build()
{
    Grid *Root = new Grid();
    Root->Rows.Push(GridLength::AutoSize()); /* menu bar */
    Root->Rows.Push(GridLength::AutoSize()); /* find bar */
    Root->Rows.Push(GridLength::Stars(1));   /* editor */
    Root->Rows.Push(GridLength::AutoSize()); /* status bar */

    MenuBar *Menu = Root->Add<MenuBar>();
    Menu->Margin = Thickness(6, 0, 6, 2);
    Menu->HorizontalAlignment = Align::Start;
    Menu->AddMenu("File", [this]() { return FileMenu(); });
    Menu->AddMenu("Edit", [this]() { return EditMenu(); });
    Menu->AddMenu("View", [this]() { return ViewMenu(); });

    FindBar_ = Root->Add<Grid>();
    FindBar_->GridRow = 1;
    FindBar_->Padding = Thickness(10, 6);
    FindBar_->ColumnSpacing = 6;
    FindBar_->Columns.Push(GridLength::Pixels(280));
    FindBar_->Columns.Push(GridLength::AutoSize());
    FindBar_->Columns.Push(GridLength::AutoSize());
    FindBar_->Columns.Push(GridLength::Stars(1));
    FindBar_->Columns.Push(GridLength::AutoSize());
    FindBox_ = FindBar_->Add<TextBox>("Find");
    FindBox_->OnSubmit = [this](const hz::String &) { FindNext(true); };
    Button *Previous = FindBar_->Add<Button>("", ButtonStyle::Subtle);
    Previous->GridColumn = 1;
    Previous->Glyph = Icon::ChevronUp;
    Previous->ToolTip = "Previous match (Shift+F3)";
    Previous->OnClick = [this]() { FindNext(false); };
    Button *Next = FindBar_->Add<Button>("", ButtonStyle::Subtle);
    Next->GridColumn = 2;
    Next->Glyph = Icon::ChevronDown;
    Next->ToolTip = "Next match (F3)";
    Next->OnClick = [this]() { FindNext(true); };
    Button *CloseFind = FindBar_->Add<Button>("", ButtonStyle::Subtle);
    CloseFind->GridColumn = 4;
    CloseFind->Glyph = Icon::Close;
    CloseFind->ToolTip = "Close (Escape)";
    CloseFind->OnClick = [this]() { ShowFind(false); };
    FindBar_->SetVisible(false);

    Editor_ = Root->Add<TextEditor>();
    Editor_->GridRow = 2;
    Editor_->Background = 0xFF1C1C1C;
    Editor_->Padding = Thickness(10, 6, 0, 0);
    Editor_->OnChanged = [this]() {
        UpdateTitle();
        UpdateStatus();
    };
    Editor_->OnCaretMoved = [this]() { UpdateStatus(); };

    StatusBar_ = Root->Add<Grid>();
    StatusBar_->GridRow = 3;
    StatusBar_->Height = 28;
    StatusBar_->Padding = Thickness(14, 0);
    StatusBar_->ColumnSpacing = 24;
    StatusBar_->Columns.Push(GridLength::AutoSize());
    StatusBar_->Columns.Push(GridLength::AutoSize());
    StatusBar_->Columns.Push(GridLength::Stars(1));
    StatusBar_->Columns.Push(GridLength::AutoSize());
    Position_ = StatusBar_->Add<TextBlock>("", FontRole::Caption);
    Position_->VerticalAlignment = Align::Center;
    Position_->Foreground = Theme::TextSecondary;
    Count_ = StatusBar_->Add<TextBlock>("", FontRole::Caption);
    Count_->GridColumn = 1;
    Count_->VerticalAlignment = Align::Center;
    Count_->Foreground = Theme::TextSecondary;
    TextBlock *Encoding = StatusBar_->Add<TextBlock>("UTF-8  |  LF", FontRole::Caption);
    Encoding->GridColumn = 3;
    Encoding->VerticalAlignment = Align::Center;
    Encoding->Foreground = Theme::TextTertiary;

    Main_->SetContent(Root);
}

hz::String Notepad::DisplayName() const
{
    return Path_.Empty() ? hz::String("Untitled") : hz::fs::FileName(Path_);
}

void Notepad::UpdateTitle()
{
    hz::String Title = hz::String::Format("%s%s - Notepad", Editor_->Modified ? "*" : "", DisplayName().CStr());
    if (Title != LastTitle_) {
        LastTitle_ = Title;
        Main_->SetTitle(Title.CStr());
    }
}

void Notepad::UpdateStatus()
{
    TextPosition Caret = Editor_->Caret();
    const hz::String &Line = Editor_->Line(Caret.Line);
    size_t Column = CodePoints(Line.CStr(), hz::Min(Caret.Column, Line.Length())) + 1;
    Position_->SetText(hz::String::Format("Ln %zu, Col %zu", Caret.Line + 1, Column).CStr());
    size_t Characters = Editor_->CharacterCount();
    Count_->SetText(hz::String::Format("%zu character%s", Characters, Characters == 1 ? "" : "s").CStr());
}

/* ---- Files ------------------------------------------------------------------------------------- */

void Notepad::ConfirmDiscard(hz::Function<void()> Then)
{
    if (!Editor_->Modified) {
        Then();
        return;
    }
    hz::String Question = hz::String::Format("Do you want to save the changes to %s?", DisplayName().CStr());
    MessageBox(Main_, "Notepad", Question.CStr(), "Save|Don't save|Cancel", [this, Then](int Choice) {
        if (Choice == 0)
            Save(Then);
        else if (Choice == 1)
            Then();
    });
}

void Notepad::New()
{
    ConfirmDiscard([this]() {
        Path_ = hz::String();
        Editor_->SetText("", 0);
        UpdateTitle();
        UpdateStatus();
        Main_->SetFocus(Editor_);
        HzPrint("notepad: new document");
    });
}

void Notepad::OpenWithDialog()
{
    ConfirmDiscard([this]() {
        ShowFileDialog(Main_, FileDialogMode::Open, FolderOf(Path_).CStr(), nullptr, [this](const hz::String &Path) {
            if (!Path.Empty())
                Load(Path, true);
        });
    });
}

bool Notepad::Load(const hz::String &Path, bool Report)
{
    hz::String Text;
    STATUS Status = hz::fs::ReadAll(Path, Text, 4 * 1024 * 1024);
    if (!HZ_SUCCESS(Status)) {
        HzPrint("notepad: cannot open %s (0x%08X)", Path.CStr(), (uint32_t)Status);
        if (Report)
            MessageBox(Main_, "Notepad",
                       hz::String::Format("%s could not be opened: %s.", Path.CStr(), hz::fs::StatusText(Status)).CStr(),
                       nullptr, nullptr);
        return false;
    }
    Path_ = Path;
    Editor_->SetText(Text.CStr(), Text.Length());
    UpdateTitle();
    UpdateStatus();
    Main_->SetFocus(Editor_);
    HzPrint("notepad: opened %s (%zu bytes, %zu lines)", Path.CStr(), Text.Length(), Editor_->LineCount());
    return true;
}

bool Notepad::WriteTo(const hz::String &Path)
{
    hz::String Text = Editor_->Text();
    STATUS Status = hz::fs::WriteAll(Path, Text.CStr(), Text.Length());
    if (!HZ_SUCCESS(Status)) {
        HzPrint("notepad: cannot save %s (0x%08X)", Path.CStr(), (uint32_t)Status);
        MessageBox(Main_, "Notepad",
                   hz::String::Format("%s could not be saved: %s.", Path.CStr(), hz::fs::StatusText(Status)).CStr(), nullptr,
                   nullptr);
        return false;
    }
    Path_ = Path;
    Editor_->Modified = false;
    UpdateTitle();
    HzPrint("notepad: saved %s (%zu bytes)", Path.CStr(), Text.Length());
    return true;
}

void Notepad::Save(hz::Function<void()> Then)
{
    if (Path_.Empty()) {
        SaveAs(Then);
        return;
    }
    if (WriteTo(Path_) && Then)
        Then();
}

void Notepad::SaveAs(hz::Function<void()> Then)
{
    hz::String Name = Path_.Empty() ? hz::String("Untitled.txt") : hz::fs::FileName(Path_);
    ShowFileDialog(Main_, FileDialogMode::Save, FolderOf(Path_).CStr(), Name.CStr(), [this, Then](const hz::String &Path) {
        if (Path.Empty())
            return;
        if (WriteTo(Path) && Then)
            Then();
    });
}

void Notepad::RequestClose()
{
    ConfirmDiscard([this]() {
        Closing_ = true;
        Main_->Close();
    });
}

/* ---- Find ------------------------------------------------------------------------------------- */

void Notepad::ShowFind(bool Show)
{
    FindBar_->SetVisible(Show);
    Main_->InvalidateLayout();
    if (Show) {
        if (Editor_->HasSelection() && Editor_->SelectionStart().Line == Editor_->SelectionEnd().Line)
            FindBox_->SetText(Editor_->SelectedText().CStr(), false);
        Main_->SetFocus(FindBox_);
        FindBox_->SelectAll();
    } else {
        Main_->SetFocus(Editor_);
    }
}

void Notepad::FindNext(bool Forward)
{
    if (FindBox_->Text().Empty()) {
        ShowFind(true);
        return;
    }
    if (!Editor_->Find(FindBox_->Text().CStr(), Forward)) {
        MessageBox(Main_, "Notepad", hz::String::Format("Cannot find \"%s\".", FindBox_->Text().CStr()).CStr(), nullptr,
                   nullptr);
        return;
    }
    TextPosition At = Editor_->Caret();
    HzPrint("notepad: found \"%s\" at line %zu", FindBox_->Text().CStr(), At.Line + 1);
}

/* ---- Menus and keys ---------------------------------------------------------------------------- */

hz::Vector<MenuItem> Notepad::FileMenu()
{
    hz::Vector<MenuItem> Items;
    MenuItem Item = MenuItem::Make("New", [this]() { New(); }, "Ctrl+N");
    Item.Glyph = Icon::Document;
    Items.Push(hz::Move(Item));
    Item = MenuItem::Make("Open...", [this]() { OpenWithDialog(); }, "Ctrl+O");
    Item.Glyph = Icon::Open;
    Items.Push(hz::Move(Item));
    Item = MenuItem::Make("Save", [this]() { Save(); }, "Ctrl+S");
    Item.Glyph = Icon::Save;
    Items.Push(hz::Move(Item));
    Items.Push(MenuItem::Make("Save as...", [this]() { SaveAs(); }, "Ctrl+Shift+S"));
    Items.Push(MenuItem::MakeSeparator());
    Items.Push(MenuItem::Make("Exit", [this]() { Main_->Close(); }, "Alt+F4"));
    return Items;
}

hz::Vector<MenuItem> Notepad::EditMenu()
{
    hz::Vector<MenuItem> Items;
    MenuItem Item = MenuItem::Make("Undo", [this]() { Editor_->Undo(); }, "Ctrl+Z");
    Item.Enabled = Editor_->CanUndo();
    Items.Push(hz::Move(Item));
    Item = MenuItem::Make("Redo", [this]() { Editor_->Redo(); }, "Ctrl+Y");
    Item.Enabled = Editor_->CanRedo();
    Items.Push(hz::Move(Item));
    Items.Push(MenuItem::MakeSeparator());
    bool Selection = Editor_->HasSelection();
    Item = MenuItem::Make("Cut", [this]() { Editor_->Cut(); }, "Ctrl+X");
    Item.Enabled = Selection;
    Items.Push(hz::Move(Item));
    Item = MenuItem::Make("Copy", [this]() { Editor_->Copy(); }, "Ctrl+C");
    Item.Enabled = Selection;
    Items.Push(hz::Move(Item));
    Items.Push(MenuItem::Make("Paste", [this]() { Editor_->Paste(); }, "Ctrl+V"));
    Item = MenuItem::Make("Delete", [this]() { Editor_->DeleteSelection(); }, "Del");
    Item.Enabled = Selection;
    Items.Push(hz::Move(Item));
    Items.Push(MenuItem::MakeSeparator());
    Item = MenuItem::Make("Find...", [this]() { ShowFind(true); }, "Ctrl+F");
    Item.Glyph = Icon::Search;
    Items.Push(hz::Move(Item));
    Items.Push(MenuItem::Make("Find next", [this]() { FindNext(true); }, "F3"));
    Items.Push(MenuItem::MakeSeparator());
    Items.Push(MenuItem::Make("Select all", [this]() { Editor_->SelectAll(); }, "Ctrl+A"));
    return Items;
}

hz::Vector<MenuItem> Notepad::ViewMenu()
{
    hz::Vector<MenuItem> Items;
    MenuItem Item = MenuItem::Make("Monospaced font", [this]() {
        Editor_->Font = Editor_->Font == FontRole::Mono ? FontRole::Body : FontRole::Mono;
        Editor_->InvalidateLayout();
        Editor_->Invalidate();
    });
    Item.Checked = Editor_->Font == FontRole::Mono;
    Items.Push(hz::Move(Item));
    Item = MenuItem::Make("Word wrap", [this]() { ToggleWordWrap(); }, "Alt+Z");
    Item.Checked = Editor_->WordWrap;
    Items.Push(hz::Move(Item));
    Item = MenuItem::Make("Status bar", [this]() {
        StatusBar_->SetVisible(!StatusBar_->IsVisible());
        Main_->InvalidateLayout();
    });
    Item.Checked = StatusBar_->IsVisible();
    Items.Push(hz::Move(Item));
    return Items;
}

void Notepad::ToggleWordWrap()
{
    Editor_->WordWrap = !Editor_->WordWrap;
    HzPrint("notepad: word wrap %s", Editor_->WordWrap ? "on" : "off");
    Editor_->InvalidateLayout();
    Editor_->Invalidate();
}

void Notepad::OnKey(KeyEvent &E)
{
    if (E.Key == HZ_VK_ESCAPE && FindBar_->IsVisible()) {
        E.Handled = true;
        ShowFind(false);
        return;
    }
    if (E.Key == HZ_VK_F1 + 2) { /* F3 */
        E.Handled = true;
        FindNext(!E.Shift());
        return;
    }
    if (E.Alt() && !E.Ctrl() && E.Key == 'Z') {
        E.Handled = true;
        ToggleWordWrap();
        return;
    }
    if (!E.Ctrl() || E.Alt())
        return;
    switch (E.Key) {
    case 'N':
        New();
        break;
    case 'O':
        OpenWithDialog();
        break;
    case 'S':
        if (E.Shift())
            SaveAs();
        else
            Save();
        break;
    case 'F':
        ShowFind(true);
        break;
    default:
        return;
    }
    E.Handled = true;
}

}  // namespace

extern "C" int main(int argc, char **argv)
{
    Notepad *App = new Notepad();
    if (!App->Create(argc > 1 ? argv[1] : nullptr)) {
        HzPrint("notepad: cannot create the window");
        return 1;
    }
    HzPrint("notepad: started");
    return RunMessageLoop();
}
