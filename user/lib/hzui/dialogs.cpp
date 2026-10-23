/*
 * Nexora OS - user/lib/hzui/dialogs.cpp
 * The menu bar, dialogs (a window that disables its owner's content while it is open), message
 * boxes and the file open/save dialog.
 */
#include "hzfile.h"
#include "hzuip.h"

namespace hzui {

/* ---- MenuBar ------------------------------------------------------------------------------------ */

namespace {

constexpr int32_t kMenuBarPadding = 10;

int32_t TitleWidth(const hz::String &Title)
{
    HZ_FONT *F = GetFont(FontRole::Body);
    return (F ? HzMeasureTextN(F, Title.CStr(), Title.Length()) : 40) + 2 * kMenuBarPadding;
}

}  // namespace

MenuBar::MenuBar()
{
    Height = 32;
}

void MenuBar::AddMenu(const char *Title, hz::Function<hz::Vector<MenuItem>()> Build)
{
    Entry E;
    E.Title = Title;
    E.Build = hz::Move(Build);
    Entries_.Push(hz::Move(E));
    InvalidateLayout();
}

Size MenuBar::MeasureOverride(Size)
{
    int32_t Width = 0;
    for (const Entry &E : Entries_)
        Width += TitleWidth(E.Title);
    return Size{Width, 32};
}

size_t MenuBar::IndexAt(Point P) const
{
    int32_t X = Bounds().X;
    for (size_t i = 0; i < Entries_.Size(); i++) {
        int32_t W = TitleWidth(Entries_[i].Title);
        if (P.X >= X && P.X < X + W && P.Y >= Bounds().Y && P.Y < Bounds().Bottom())
            return i;
        X += W;
    }
    return hz::NotFound;
}

void MenuBar::Paint(Painter &P)
{
    Element::Paint(P);
    int32_t X = Bounds().X;
    for (size_t i = 0; i < Entries_.Size(); i++) {
        int32_t W = TitleWidth(Entries_[i].Title);
        Rect R{X, Bounds().Y + 2, W, Bounds().Height - 4};
        if (i == Hot_)
            P.FillRoundRect(R, Theme::ControlRadius, Theme::SubtleFillHover);
        P.TextLine(GetFont(FontRole::Body), R, Entries_[i].Title.CStr(), IsEnabled() ? Theme::TextPrimary : Theme::TextDisabled,
                   Align::Center);
        X += W;
    }
}

void MenuBar::OnPointerMove(PointerEvent &E)
{
    size_t Hot = IndexAt(E.Position);
    if (Hot != Hot_) {
        Hot_ = Hot;
        Invalidate();
    }
}

void MenuBar::OnPointerLeave()
{
    Hot_ = hz::NotFound;
    Invalidate();
}

void MenuBar::OnPointerDown(PointerEvent &E)
{
    if (E.Button != HZ_MOUSE_LEFT)
        return;
    size_t Index = IndexAt(E.Position);
    if (Index != hz::NotFound) {
        E.Handled = true;
        Open(Index);
    }
}

void MenuBar::Open(size_t Index)
{
    Window *W = GetWindow();
    if (!W || Index >= Entries_.Size() || !Entries_[Index].Build)
        return;
    int32_t X = Bounds().X;
    for (size_t i = 0; i < Index; i++)
        X += TitleWidth(Entries_[i].Title);
    Point O = W->ScreenOrigin();
    ShowMenu(W, Point{O.X + X, O.Y + Bounds().Bottom()}, Entries_[Index].Build(), 200);
}

/* ---- Dialog ----------------------------------------------------------------------------------------- */

Dialog::Dialog(Window *Owner, const char *Title, int32_t Width, int32_t Height)
    : Window(Title, Width, Height, HZ_WS_CAPTION | HZ_WS_MICA | HZ_WS_VISIBLE | (Owner ? HZ_WS_OWNED : 0u)), Owner_(Owner),
      Caption_(Title)
{
    Window::Owner = Owner;
}

bool Dialog::Open()
{
    Rect Area = WorkArea();
    Size S = WindowSize();
    int32_t X = Area.X + (Area.Width - S.Width) / 2, Y = Area.Y + (Area.Height - S.Height) / 2;
    if (Owner_ && Owner_->IsOpen()) {
        Point O = Owner_->ScreenOrigin();
        Size OwnerSize = Owner_->WindowSize();
        X = hz::Clamp(O.X + (OwnerSize.Width - S.Width) / 2, Area.X, hz::Max(Area.Right() - S.Width, Area.X));
        Y = hz::Clamp(O.Y + (OwnerSize.Height - S.Height) / 2, Area.Y, hz::Max(Area.Bottom() - S.Height, Area.Y));
        if (Element *Content = Owner_->Content()) {
            Element *Focus = Owner_->FocusedElement();
            Content->SetEnabled(false);
            hz::Function<void()> Previous = OnClosed;
            Window *OwnerWindow = Owner_;
            OnClosed = [OwnerWindow, Content, Focus, Previous]() {
                Content->SetEnabled(true);
                if (Focus && Focus->IsVisible())
                    OwnerWindow->SetFocus(Focus);
                if (OwnerWindow->IsOpen())
                    NtUserSetForegroundWindow(OwnerWindow->Handle());
                if (Previous)
                    Previous();
            };
        }
    }
    if (!ShowAt(X, Y))
        return false;
    HzLog("hzui: dialog \"%s\" opened", Caption_.CStr()); /* a marker for the smoke tests */
    return true;
}

void Dialog::Finish()
{
    if (IsOpen())
        Close();
    DeferDelete(this);
}

/* ---- MessageBox ------------------------------------------------------------------------------------- */

namespace {

class MessageDialog final : public Dialog {
public:
    MessageDialog(Window *Owner, const char *Title, int32_t Width, int32_t Height, hz::Function<void(int)> Result)
        : Dialog(Owner, Title, Width, Height), Result_(hz::Move(Result))
    {
    }

    void Answer(int Choice)
    {
        if (Answered_)
            return;
        Answered_ = true;
        hz::Function<void(int)> Result = Result_;
        Finish();
        if (Result)
            Result(Choice);
    }

    int64_t HandleMessage(uint32_t Message, uint64_t WParam, uint64_t LParam) override
    {
        if (Message == HZ_WM_CLOSE) {
            Answer(-1);
            return 0;
        }
        return Dialog::HandleMessage(Message, WParam, LParam);
    }

private:
    hz::Function<void(int)> Result_;
    bool Answered_ = false;
};

}  // namespace

void MessageBox(Window *Owner, const char *Title, const char *Text, const char *ButtonList,
                hz::Function<void(int)> Result)
{
    hz::Vector<hz::String> Buttons;
    for (const char *P = ButtonList ? ButtonList : "OK"; *P;) {
        const char *End = P;
        while (*End && *End != '|')
            End++;
        Buttons.Push(hz::String(P, (size_t)(End - P)));
        P = *End ? End + 1 : End;
    }
    if (Buttons.Empty())
        Buttons.Push(hz::String("OK"));
    const int32_t Width = 440, Padding = 24, BarHeight = 72;
    HZ_FONT *F = GetFont(FontRole::Body);
    TextLayout Layout;
    Layout.Layout(F, Text, strlen(Text), Width - 2 * Padding);
    int32_t Height = HZ_CAPTION_HEIGHT + Padding + Layout.Height + Padding + BarHeight;

    MessageDialog *D = new MessageDialog(Owner, Title, Width, Height, hz::Move(Result));
    Grid *Root = new Grid();
    Root->Rows.Push(GridLength::Stars(1));
    Root->Rows.Push(GridLength::Pixels(BarHeight));
    TextBlock *Body = Root->Add<TextBlock>(Text, FontRole::Body);
    Body->Wrap = true;
    Body->Margin = Thickness(Padding, Padding / 2, Padding, 0);
    Grid *Bar = Root->Add<Grid>();
    Bar->GridRow = 1;
    Bar->Background = 0x26000000;
    Bar->Padding = Thickness(Padding, 0);
    Bar->Columns.Push(GridLength::Stars(1));
    Bar->Columns.Push(GridLength::AutoSize());
    StackPanel *Row = Bar->Add<StackPanel>(Orientation::Horizontal, 8);
    Row->GridColumn = 1;
    Row->VerticalAlignment = Align::Center;
    Button *First = nullptr;
    for (size_t i = 0; i < Buttons.Size(); i++) {
        Button *B = Row->Add<Button>(Buttons[i].CStr(), i == 0 ? ButtonStyle::Accent : ButtonStyle::Standard);
        B->MinWidth = 96;
        int Choice = (int)i;
        B->OnClick = [D, Choice]() { D->Answer(Choice); };
        if (!First)
            First = B;
    }
    int Cancel = Buttons.Size() > 1 ? (int)Buttons.Size() - 1 : 0;
    D->OnUnhandledKey = [D, Cancel](KeyEvent &E) {
        if (E.Key == HZ_VK_ESCAPE) {
            E.Handled = true;
            D->Answer(Cancel);
        } else if (E.Key == HZ_VK_RETURN) {
            E.Handled = true;
            D->Answer(0);
        }
    };
    D->SetContent(Root);
    D->SetFocus(First);
    if (!D->Open()) {
        delete D;
    }
}

/* ---- InputBox ------------------------------------------------------------------------------------ */

namespace {

class InputDialog final : public Dialog {
public:
    InputDialog(Window *Owner, const char *Title, hz::Function<void(const hz::String &)> Result)
        : Dialog(Owner, Title, 400, HZ_CAPTION_HEIGHT + 168), Result_(hz::Move(Result))
    {
    }

    void Answer(bool Accept, const hz::String &Text)
    {
        if (Answered_)
            return;
        Answered_ = true;
        hz::Function<void(const hz::String &)> Result = Result_;
        hz::String Value = Text;
        Finish();
        if (Accept && Result)
            Result(Value);
    }

    int64_t HandleMessage(uint32_t Message, uint64_t WParam, uint64_t LParam) override
    {
        if (Message == HZ_WM_CLOSE) {
            Answer(false, hz::String());
            return 0;
        }
        return Dialog::HandleMessage(Message, WParam, LParam);
    }

private:
    hz::Function<void(const hz::String &)> Result_;
    bool Answered_ = false;
};

}  // namespace

void InputBox(Window *Owner, const char *Title, const char *Prompt, const char *Initial, const char *Action,
              hz::Function<void(const hz::String &)> Result)
{
    InputDialog *D = new InputDialog(Owner, Title, hz::Move(Result));
    Grid *Root = new Grid();
    Root->Rows.Push(GridLength::Stars(1));
    Root->Rows.Push(GridLength::Pixels(72));
    StackPanel *Body = Root->Add<StackPanel>(Orientation::Vertical, 8);
    Body->Padding = Thickness(24, 12, 24, 0);
    TextBlock *Label = Body->Add<TextBlock>(Prompt, FontRole::Body);
    Label->Foreground = Theme::TextSecondary;
    TextBox *Box = Body->Add<TextBox>("");
    Box->MaxLength = 200;
    Box->SetText(Initial ? Initial : "", false);
    Grid *Bar = Root->Add<Grid>();
    Bar->GridRow = 1;
    Bar->Background = 0x26000000;
    Bar->Padding = Thickness(24, 0);
    Bar->Columns.Push(GridLength::Stars(1));
    Bar->Columns.Push(GridLength::AutoSize());
    StackPanel *Row = Bar->Add<StackPanel>(Orientation::Horizontal, 8);
    Row->GridColumn = 1;
    Row->VerticalAlignment = Align::Center;
    Button *Ok = Row->Add<Button>(Action ? Action : "OK", ButtonStyle::Accent);
    Ok->MinWidth = 96;
    Ok->OnClick = [D, Box]() { D->Answer(true, Box->Text()); };
    Button *Cancel = Row->Add<Button>("Cancel");
    Cancel->MinWidth = 96;
    Cancel->OnClick = [D]() { D->Answer(false, hz::String()); };
    Box->OnSubmit = [D](const hz::String &Text) { D->Answer(true, Text); };
    D->OnUnhandledKey = [D](KeyEvent &E) {
        if (E.Key == HZ_VK_ESCAPE) {
            E.Handled = true;
            D->Answer(false, hz::String());
        }
    };
    D->SetContent(Root);
    D->SetFocus(Box);
    /* Select the name without its extension. */
    const hz::String &Text = Box->Text();
    size_t Dot = hz::NotFound;
    for (size_t i = 0; i < Text.Length(); i++)
        if (Text[i] == '.')
            Dot = i;
    Box->Select(0, Dot != hz::NotFound && Dot > 0 ? Dot : Text.Length());
    if (!D->Open())
        delete D;
}

/* ---- File dialog ------------------------------------------------------------------------------------ */

namespace {

class FileDialogWindow final : public Dialog {
public:
    FileDialogWindow(Window *Owner, FileDialogMode Mode, hz::Function<void(const hz::String &)> Done)
        : Dialog(Owner, Mode == FileDialogMode::Open ? "Open" : "Save as", 720, 480), Mode_(Mode), Done_(hz::Move(Done))
    {
        Volumes_ = hz::fs::Volumes();
        Grid *Root = new Grid();
        Root->Padding = Thickness(12);
        Root->RowSpacing = 10;
        Root->Rows.Push(GridLength::AutoSize());
        Root->Rows.Push(GridLength::Stars(1));
        Root->Rows.Push(GridLength::AutoSize());

        Grid *Top = Root->Add<Grid>();
        Top->Columns.Push(GridLength::AutoSize());
        Top->Columns.Push(GridLength::Stars(1));
        Top->ColumnSpacing = 8;
        Button *Up = Top->Add<Button>("", ButtonStyle::Subtle);
        Up->Glyph = Icon::ArrowUp;
        Up->ToolTip = "Up one folder";
        Up->OnClick = [this]() { Navigate(hz::fs::Parent(Folder_)); };
        Address_ = Top->Add<TextBox>("Folder");
        Address_->GridColumn = 1;
        Address_->MaxLength = 250;
        Address_->OnSubmit = [this](const hz::String &Text) { Navigate(hz::fs::Resolve(Folder_, Text.CStr())); };

        Grid *Middle = Root->Add<Grid>();
        Middle->GridRow = 1;
        Middle->Columns.Push(GridLength::Pixels(170));
        Middle->Columns.Push(GridLength::Stars(1));
        Middle->ColumnSpacing = 10;
        Places_ = Middle->Add<ListView>();
        Places_->ItemHeight = 34;
        Places_->SetItemCount(Volumes_.Size());
        Places_->PaintItem = [this](Painter &P, const Rect &R, size_t I) {
            P.DrawIcon(Icon::Drive, Rect{R.X, R.Y + (R.Height - 16) / 2, 16, 16}, Theme::TextSecondary);
            const hz::fs::Volume &V = Volumes_[I];
            hz::String Name = hz::String::Format("%s (%c:)", V.Label.Empty() ? "Local disk" : V.Label.CStr(), V.Root[0]);
            P.TextLine(GetFont(FontRole::Body), Rect{R.X + 26, R.Y, R.Width - 26, R.Height}, Name.CStr(), Theme::TextPrimary);
        };
        Places_->OnSelectionChanged = [this](size_t I) {
            if (I < Volumes_.Size())
                Navigate(Volumes_[I].Root);
        };
        Places_->Background = Theme::CardBackground;
        Places_->CornerRadius = Theme::OverlayRadius;

        Files_ = Middle->Add<ListView>();
        Files_->GridColumn = 1;
        Files_->ItemHeight = 32;
        Files_->Background = Theme::CardBackground;
        Files_->CornerRadius = Theme::OverlayRadius;
        Files_->PaintItem = [this](Painter &P, const Rect &R, size_t I) {
            const hz::fs::Entry &E = Entries_[I];
            P.DrawIcon(E.Directory ? Icon::Folder : Icon::Document, Rect{R.X, R.Y + (R.Height - 16) / 2, 16, 16},
                       E.Directory ? 0xFFFFC66B : Theme::TextSecondary);
            HZ_FONT *F = GetFont(FontRole::Body);
            int32_t Right = R.Right();
            if (!E.Directory)
                P.TextLine(F, Rect{Right - 80, R.Y, 80, R.Height}, hz::fs::FormatSize(E.Size).CStr(), Theme::TextTertiary,
                           Align::End);
            P.TextLine(F, Rect{Right - 230, R.Y, 140, R.Height}, hz::fs::FormatTime(E.Modified).CStr(), Theme::TextTertiary);
            P.TextLine(F, Rect{R.X + 26, R.Y, R.Width - 26 - 240, R.Height}, E.Name.CStr(), Theme::TextPrimary);
        };
        Files_->OnSelectionChanged = [this](size_t I) {
            if (I < Entries_.Size() && !Entries_[I].Directory)
                Name_->SetText(Entries_[I].Name.CStr());
        };
        Files_->OnItemInvoked = [this](size_t I) {
            if (I >= Entries_.Size())
                return;
            if (Entries_[I].Directory)
                Navigate(hz::fs::Join(Folder_, Entries_[I].Name.CStr()));
            else
                Choose(Entries_[I].Name);
        };

        Grid *Bottom = Root->Add<Grid>();
        Bottom->GridRow = 2;
        Bottom->Columns.Push(GridLength::AutoSize());
        Bottom->Columns.Push(GridLength::Stars(1));
        Bottom->Columns.Push(GridLength::AutoSize());
        Bottom->Columns.Push(GridLength::AutoSize());
        Bottom->ColumnSpacing = 8;
        TextBlock *Label = Bottom->Add<TextBlock>("File name", FontRole::Body);
        Label->VerticalAlignment = Align::Center;
        Label->Foreground = Theme::TextSecondary;
        Name_ = Bottom->Add<TextBox>("Name");
        Name_->GridColumn = 1;
        Name_->MaxLength = 200;
        Name_->OnSubmit = [this](const hz::String &Text) { Choose(Text); };
        Button *Action = Bottom->Add<Button>(Mode == FileDialogMode::Open ? "Open" : "Save", ButtonStyle::Accent);
        Action->GridColumn = 2;
        Action->MinWidth = 96;
        Action->OnClick = [this]() { Choose(Name_->Text()); };
        Button *Cancel = Bottom->Add<Button>("Cancel");
        Cancel->GridColumn = 3;
        Cancel->MinWidth = 96;
        Cancel->OnClick = [this]() { Finish(hz::String()); };
        OnUnhandledKey = [this](KeyEvent &E) {
            if (E.Key == HZ_VK_ESCAPE) {
                E.Handled = true;
                Finish(hz::String());
            }
        };
        SetContent(Root);
        SetFocus(Name_);
    }

    void Start(const char *Folder, const char *Name)
    {
        /* Default: the user's documents (or the first writable volume). */
        hz::String Initial = Folder && *Folder ? hz::String(Folder) : hz::fs::DocumentsFolder();
        Navigate(Initial);
        if (Name)
            Name_->SetText(Name, false);
    }

    void Navigate(const hz::String &Path)
    {
        hz::Vector<hz::fs::Entry> Entries;
        STATUS Status = hz::fs::List(Path, Entries);
        if (!HZ_SUCCESS(Status)) {
            Address_->SetText(Folder_.CStr(), false);
            MessageBox(this, "Cannot open the folder", hz::String::Format("%s.", hz::fs::StatusText(Status)).CStr(), nullptr,
                       nullptr);
            return;
        }
        Folder_ = Path;
        Entries_ = hz::Move(Entries);
        Address_->SetText(Folder_.CStr(), false);
        Files_->SetSelectedIndex(hz::NotFound, false);
        Files_->SetItemCount(Entries_.Size());
        Files_->Invalidate();
    }

    void Choose(const hz::String &Name)
    {
        if (Name.Empty())
            return;
        hz::String Path = hz::fs::Resolve(Folder_, Name.CStr());
        bool Directory = false;
        bool Exists = hz::fs::Exists(Path, &Directory);
        if (Exists && Directory) {
            Navigate(Path);
            Name_->SetText("", false);
            return;
        }
        if (Mode_ == FileDialogMode::Open) {
            if (!Exists) {
                MessageBox(this, "Open", hz::String::Format("%s was not found.", Path.CStr()).CStr(), nullptr, nullptr);
                return;
            }
            Finish(Path);
            return;
        }
        if (Exists) {
            MessageBox(this, "Save as", hz::String::Format("%s already exists. Replace it?", Path.CStr()).CStr(),
                       "Replace|Cancel", [this, Path](int Choice) {
                           if (Choice == 0)
                               Finish(Path);
                       });
            return;
        }
        Finish(Path);
    }

    void Finish(const hz::String &Path)
    {
        if (Answered_)
            return;
        Answered_ = true;
        hz::Function<void(const hz::String &)> Done = Done_;
        hz::String Result = Path;
        Dialog::Finish();
        if (Done)
            Done(Result);
    }

    int64_t HandleMessage(uint32_t Message, uint64_t WParam, uint64_t LParam) override
    {
        if (Message == HZ_WM_CLOSE) {
            Finish(hz::String());
            return 0;
        }
        return Dialog::HandleMessage(Message, WParam, LParam);
    }

private:
    FileDialogMode Mode_;
    hz::Function<void(const hz::String &)> Done_;
    bool Answered_ = false;
    hz::String Folder_;
    hz::Vector<hz::fs::Volume> Volumes_;
    hz::Vector<hz::fs::Entry> Entries_;
    TextBox *Address_, *Name_;
    ListView *Places_, *Files_;
};

}  // namespace

void ShowFileDialog(Window *Owner, FileDialogMode Mode, const char *Folder, const char *Name,
                    hz::Function<void(const hz::String &)> Done)
{
    FileDialogWindow *D = new FileDialogWindow(Owner, Mode, hz::Move(Done));
    if (!D->Open()) {
        delete D;
        return;
    }
    D->Start(Folder, Name);
}

}  // namespace hzui
