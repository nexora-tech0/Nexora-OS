/*
 * Nexora OS - user/apps/photos/photos.cpp
 * Photos: views the picture files the system can decode (user/include/hzimage.h — JPEG, PNG, BMP,
 * GIF, TGA and PNM through the vendored stb_image). A path on the command line opens that picture
 * ("photos.exe D:\pics\a.png"); otherwise File > Open asks for one.
 *
 * The window is a toolbar (open, previous, next, zoom, rotate) over the stage, a status bar (name,
 * pixels, format, position in the folder), and a zoom badge in the corner of the stage. The folder
 * the picture lives in is walked once, so Left and Right step through its other pictures, and a
 * file that cannot be decoded is named in the stage instead of being drawn blank — that is where
 * the recognised-but-not-compiled formats (Photoshop, Radiance HDR) are reported.
 *
 * Video is not here and is not claimed: a container demuxer plus an open codec would be needed, and
 * for sound an audio device the kernel does not have yet.
 *
 * hzui has no alpha blending for bitmaps (Painter::DrawBitmap forces every pixel opaque), so a
 * picture's transparent areas are drawn as the colour they carry, not blended with the stage.
 */
#include "hzfile.h"
#include "hzimage.h"
#include "hzui.h"

using namespace hzui;

namespace {

constexpr float kMinScale = 0.05f;
constexpr float kMaxScale = 8.0f;
constexpr float kZoomStep = 1.25f;
constexpr int32_t kStageInset = 10;

/* The extensions Photos treats as pictures while it walks a folder for Previous/Next. A file that
   is listed but does not decode is still steppable: the stage then names what it found. */
bool IsPictureName(const char *Name)
{
    static const char *const kNames[] = {"png",  "jpg", "jpeg", "jpe", "gif", "bmp", "tga",
                                        "pnm",  "ppm", "pgm",  "pbm", "psd", "hdr"};
    hz::String Extension = hz::fs::Extension(hz::String(Name));
    if (Extension.Empty())
        return false;
    for (const char *Known : kNames)
        if (Extension == Known)
            return true;
    return false;
}

/* ---- The stage ------------------------------------------------------------------------------- */

/* Draws the picture centred on a plain surface, scaled by the fit factor or by the zoom the toolbar
   set, and pannable by dragging while it is larger than the stage. A picture that could not be
   decoded leaves a message in its place. */
class ImageStage final : public Element {
public:
    ImageStage()
    {
        Name = "PhotoStage";
        Reveal_.Jump(1.0f);
    }

    /* Handed the decoded (and already turned) bitmap, or null for nothing. */
    void SetPicture(const hz::image::Bitmap *Picture)
    {
        Picture_ = Picture;
        Message_.Clear();
        PanX_ = PanY_ = 0;
        Reveal_.Jump(0.94f);
        Reveal_.To(1.0f, 180);
        StartAnimation();
        Invalidate();
    }

    void SetMessage(const char *Text)
    {
        Picture_ = nullptr;
        Message_ = Text;
        Reveal_.Jump(1.0f);
        Invalidate();
    }

    void FitToWindow()
    {
        Fit_ = true;
        PanX_ = PanY_ = 0;
        Invalidate();
    }

    void SetZoom(float Scale)
    {
        Fit_ = false;
        Zoom_ = hz::Clamp(Scale, kMinScale, kMaxScale);
        PanX_ = PanY_ = 0;
        Invalidate();
    }

    void ZoomBy(float Factor) { SetZoom((Fit_ ? FitScale() : Zoom_) * Factor); }

    /* The settled scale (1.0 = one picture pixel per screen pixel). The reveal animation multiplies
       this while it runs but is not part of it, so the badge reads the factor the user chose. */
    float Scale() const { return Fit_ ? FitScale() : Zoom_; }

    Size MeasureOverride(Size) override { return Size{320, 240}; }

    void Paint(Painter &P) override
    {
        const Rect B = Bounds();
        if (B.Empty())
            return;
        P.Clear(B, Theme::SolidBackground);
        const Rect Stage = B.Deflate(Thickness(kStageInset));
        if (Stage.Empty())
            return;

        if (!Picture_ || !Picture_->Pixels || Picture_->Width <= 0 || Picture_->Height <= 0) {
            HZ_FONT *Font = GetFont(FontRole::Body);
            Rect Line{Stage.X, Stage.Y + (Stage.Height - 24) / 2, Stage.Width, 24};
            P.TextLine(Font, Line, Message_.Empty() ? "No picture open" : Message_.CStr(), Theme::TextSecondary,
                       Align::Center);
            return;
        }

        float Reveal = Reveal_.Value();
        float RenderScale = (Fit_ ? FitScale() : Zoom_) * Reveal;
        int32_t Width = hz::Max(1, (int32_t)((float)Picture_->Width * RenderScale + 0.5f));
        int32_t Height = hz::Max(1, (int32_t)((float)Picture_->Height * RenderScale + 0.5f));
        ClampPan(Stage, Width, Height);
        Rect Destination{Stage.X + (Stage.Width - Width) / 2 + PanX_, Stage.Y + (Stage.Height - Height) / 2 + PanY_,
                         Width, Height};

        P.PushClip(Stage);
        P.DrawBitmap(Destination, Picture_->Pixels, Picture_->Width, Picture_->Height);
        P.PopClip();
        P.StrokeRoundRect(Destination, 0.0f, 1, Theme::CardStroke);

        /* The zoom badge, drawn from the scale this frame really used. */
        HZ_FONT *Font = GetFont(FontRole::Caption);
        hz::String Label = hz::String::Format("%d%%", (int)(Scale() * 100.0f + 0.5f));
        int32_t TextWidth = HzMeasureText(Font, Label.CStr());
        Rect Badge{B.Right() - TextWidth - 28, B.Bottom() - 32, TextWidth + 16, 22};
        P.FillRoundRect(Badge, Badge.Height / 2.0f, 0xB3000000);
        P.TextLine(Font, Badge, Label.CStr(), 0xFFFFFFFF, Align::Center);
    }

    bool Animate() override
    {
        if (Reveal_.Running()) {
            Invalidate();
            return true;
        }
        return false;
    }

    uint32_t CursorShape() const override { return CanPan() ? HZ_CURSOR_HAND : HZ_CURSOR_ARROW; }

    void OnPointerDown(PointerEvent &E) override
    {
        if (E.Button != HZ_MOUSE_LEFT || !CanPan())
            return;
        if (!StageRect().Contains(E.Position))
            return;
        Panning_ = true;
        Last_ = E.Position;
        E.Handled = true;
    }

    void OnPointerMove(PointerEvent &E) override
    {
        if (!Panning_)
            return;
        if (!(E.Buttons & HZ_MOUSE_LEFT)) {
            Panning_ = false;
            return;
        }
        PanX_ += E.Position.X - Last_.X;
        PanY_ += E.Position.Y - Last_.Y;
        Last_ = E.Position;
        Invalidate();
        E.Handled = true;
    }

    void OnPointerUp(PointerEvent &E) override
    {
        if (E.Button != HZ_MOUSE_LEFT || !Panning_)
            return;
        Panning_ = false;
        E.Handled = true;
    }

    void OnPointerWheel(PointerEvent &E) override
    {
        if (!Picture_ || E.WheelDelta == 0)
            return;
        ZoomBy(E.WheelDelta > 0 ? kZoomStep : 1.0f / kZoomStep);
        E.Handled = true;
    }

private:
    Rect StageRect() const { return Bounds().Deflate(Thickness(kStageInset)); }

    /* Fit shrinks a picture larger than the stage and never enlarges a smaller one. */
    float FitScale() const
    {
        if (!Picture_ || Picture_->Width <= 0 || Picture_->Height <= 0)
            return 1.0f;
        Rect Stage = StageRect();
        if (Stage.Empty())
            return 1.0f;
        float Across = (float)Stage.Width / (float)Picture_->Width;
        float Down = (float)Stage.Height / (float)Picture_->Height;
        return hz::Min(hz::Min(Across, Down), 1.0f);
    }

    bool CanPan() const
    {
        if (!Picture_ || Picture_->Width <= 0 || Picture_->Height <= 0)
            return false;
        Rect Stage = StageRect();
        if (Stage.Empty())
            return false;
        float Scale = Fit_ ? FitScale() : Zoom_;
        return (float)Picture_->Width * Scale > (float)Stage.Width + 1.0f ||
               (float)Picture_->Height * Scale > (float)Stage.Height + 1.0f;
    }

    void ClampPan(const Rect &Stage, int32_t Width, int32_t Height)
    {
        int32_t LimitX = hz::Max((Width - Stage.Width) / 2, 0);
        int32_t LimitY = hz::Max((Height - Stage.Height) / 2, 0);
        PanX_ = hz::Clamp(PanX_, -LimitX, LimitX);
        PanY_ = hz::Clamp(PanY_, -LimitY, LimitY);
    }

    const hz::image::Bitmap *Picture_ = nullptr;
    hz::String Message_;
    Transition Reveal_;
    bool Fit_ = true;
    float Zoom_ = 1.0f;
    int32_t PanX_ = 0, PanY_ = 0;
    bool Panning_ = false;
    Point Last_{};
};

/* ---- The application -------------------------------------------------------------------------- */

class Photos {
public:
    bool Create(const char *InitialPath);

private:
    void Build();
    void OpenWithDialog();
    void Load(const hz::String &Path);
    void ShowCurrent();
    void Step(int Delta);
    void Rotate(int Delta);
    void RefreshFolder();
    void UpdateTitle();
    void UpdateStatus();
    hz::Vector<MenuItem> FileMenu();
    hz::Vector<MenuItem> ViewMenu();
    void OnKey(KeyEvent &E);

    Window *Main_ = nullptr;
    ImageStage *Stage_ = nullptr;
    TextBlock *Status_ = nullptr;
    hz::String Path_;               /* the picture on screen */
    hz::Vector<hz::String> Siblings_; /* the pictures in Path_'s folder, in listing order */
    size_t Index_ = 0;              /* Path_ inside Siblings_ */
    hz::image::Bitmap Base_, Display_; /* as decoded, and turned for the stage */
    int Turns_ = 0;                 /* quarter turns clockwise, 0..3 */
    hz::image::Format Format_ = hz::image::Format::Unknown;
};

bool Photos::Create(const char *InitialPath)
{
    Main_ = new Window("Photos", 920, 640);
    Main_->SetMinimumSize(560, 400);
    Build();
    Main_->OnClosed = []() { NtUserPostQuitMessage(0); };
    Main_->OnUnhandledKey = [this](KeyEvent &E) { OnKey(E); };
    if (InitialPath && *InitialPath) {
        hz::String Path = hz::fs::Resolve(hz::String("C:\\"), InitialPath);
        if (hz::fs::Exists(Path))
            Load(Path);
        else
            Stage_->SetMessage(hz::String::Format("%s was not found.", Path.CStr()).CStr());
    }
    if (!Main_->Show())
        return false;
    UpdateTitle();
    UpdateStatus();
    return true;
}

void Photos::Build()
{
    Grid *Root = new Grid();
    Root->Rows.Push(GridLength::AutoSize()); /* toolbar */
    Root->Rows.Push(GridLength::Stars(1));   /* stage */
    Root->Rows.Push(GridLength::AutoSize()); /* status bar */

    Grid *Toolbar = Root->Add<Grid>();
    Toolbar->Padding = Thickness(10, 8);
    Toolbar->ColumnSpacing = 12;
    Toolbar->Columns.Push(GridLength::AutoSize());
    Toolbar->Columns.Push(GridLength::Stars(1));

    MenuBar *Menu = Toolbar->Add<MenuBar>();
    Menu->VerticalAlignment = Align::Center;
    Menu->AddMenu("File", [this]() { return FileMenu(); });
    Menu->AddMenu("View", [this]() { return ViewMenu(); });

    StackPanel *Buttons = Toolbar->Add<StackPanel>(Orientation::Horizontal, 6);
    Buttons->GridColumn = 1;
    Buttons->VerticalAlignment = Align::Center;

    Button *Open = Buttons->Add<Button>("Open", ButtonStyle::Subtle);
    Open->Glyph = Icon::Open;
    Open->ToolTip = "Open a picture (Ctrl+O)";
    Open->OnClick = [this]() { OpenWithDialog(); };

    Button *Previous = Buttons->Add<Button>("", ButtonStyle::Subtle);
    Previous->Glyph = Icon::ChevronLeft;
    Previous->ToolTip = "Previous picture (Left)";
    Previous->OnClick = [this]() { Step(-1); };

    Button *Next = Buttons->Add<Button>("", ButtonStyle::Subtle);
    Next->Glyph = Icon::ChevronRight;
    Next->ToolTip = "Next picture (Right)";
    Next->OnClick = [this]() { Step(1); };

    Button *ZoomOut = Buttons->Add<Button>("", ButtonStyle::Subtle);
    ZoomOut->Glyph = Icon::Remove;
    ZoomOut->ToolTip = "Zoom out (Ctrl+-)";
    ZoomOut->OnClick = [this]() { Stage_->ZoomBy(1.0f / kZoomStep); };

    Button *Fit = Buttons->Add<Button>("Fit", ButtonStyle::Subtle);
    Fit->ToolTip = "Fit the picture to the window (Ctrl+0)";
    Fit->OnClick = [this]() { Stage_->FitToWindow(); };

    Button *Actual = Buttons->Add<Button>("100%", ButtonStyle::Subtle);
    Actual->ToolTip = "Actual size: one screen pixel per picture pixel (Ctrl+1)";
    Actual->OnClick = [this]() { Stage_->SetZoom(1.0f); };

    Button *ZoomIn = Buttons->Add<Button>("", ButtonStyle::Subtle);
    ZoomIn->Glyph = Icon::Add;
    ZoomIn->ToolTip = "Zoom in (Ctrl++)";
    ZoomIn->OnClick = [this]() { Stage_->ZoomBy(kZoomStep); };

    Button *RotateLeft = Buttons->Add<Button>("Rotate left", ButtonStyle::Subtle);
    RotateLeft->ToolTip = "Turn a quarter turn to the left (Ctrl+Shift+R)";
    RotateLeft->OnClick = [this]() { Rotate(-1); };

    Button *RotateRight = Buttons->Add<Button>("Rotate right", ButtonStyle::Subtle);
    RotateRight->ToolTip = "Turn a quarter turn to the right (Ctrl+R)";
    RotateRight->OnClick = [this]() { Rotate(1); };

    Stage_ = Root->Add<ImageStage>();
    Stage_->GridRow = 1;
    Stage_->SetMessage("Open a picture to get started (File > Open, or Ctrl+O).");

    Grid *StatusBar = Root->Add<Grid>();
    StatusBar->GridRow = 2;
    StatusBar->Height = 30;
    StatusBar->Padding = Thickness(14, 0);
    Status_ = StatusBar->Add<TextBlock>("", FontRole::Caption);
    Status_->VerticalAlignment = Align::Center;
    Status_->Foreground = Theme::TextSecondary;

    Main_->SetContent(Root);
}

void Photos::OpenWithDialog()
{
    hz::String Folder = Path_.Empty() ? hz::fs::DocumentsFolder() : hz::fs::Parent(Path_);
    ShowFileDialog(Main_, FileDialogMode::Open, Folder.CStr(), nullptr, [this](const hz::String &Path) {
        if (!Path.Empty())
            Load(Path);
    });
}

void Photos::Load(const hz::String &Path)
{
    Path_ = Path;
    Format_ = hz::image::Detect(Path);
    Turns_ = 0;
    Display_.Release();
    STATUS Status = hz::image::Decode(Path, &Base_);
    RefreshFolder();

    if (!HZ_SUCCESS(Status)) {
        const char *Name = hz::image::FormatName(Format_);
        hz::String Text;
        if (Format_ == hz::image::Format::Unknown)
            Text = hz::String::Format("%s is not a picture format this build reads.", hz::fs::FileName(Path).CStr());
        else if (!hz::image::IsSupported(Format_))
            Text = hz::String::Format("%s is %s, which this build recognises but does not decode.",
                                      hz::fs::FileName(Path).CStr(), Name);
        else
            Text = hz::String::Format("%s is a %s file that could not be read (%s).", hz::fs::FileName(Path).CStr(),
                                      Name, hz::image::LastError());
        Stage_->SetMessage(Text.CStr());
        HzPrint("photos: cannot show %s (0x%08X, %s)", Path.CStr(), (uint32_t)Status, hz::image::LastError());
        UpdateTitle();
        UpdateStatus();
        return;
    }

    HzPrint("photos: opened %s (%d x %d, %s)", Path.CStr(), Base_.Width, Base_.Height, hz::image::FormatName(Format_));
    ShowCurrent();
    UpdateTitle();
    UpdateStatus();
}

void Photos::ShowCurrent()
{
    Display_.Release();
    if (!Base_.Pixels) {
        Stage_->SetMessage("No picture open.");
        return;
    }
    if (Turns_ != 0 && !HZ_SUCCESS(hz::image::RotateQuarter(Base_, Turns_, &Display_))) {
        Turns_ = 0; /* turning is a copy; if it cannot be made, show the picture as decoded */
        HzPrint("photos: cannot turn %s", Path_.CStr());
    }
    Stage_->SetPicture(Display_.Pixels ? &Display_ : &Base_);
}

void Photos::Step(int Delta)
{
    if (Siblings_.Size() < 2)
        return;
    int Count = (int)Siblings_.Size();
    int Next = ((int)Index_ + Delta % Count + Count) % Count; /* wraps, like Photos on Windows */
    Index_ = (size_t)Next;
    Load(Siblings_[Index_]);
}

void Photos::Rotate(int Delta)
{
    if (!Base_.Pixels)
        return;
    Turns_ = ((Turns_ + Delta) % 4 + 4) % 4;
    ShowCurrent();
    UpdateStatus();
}

void Photos::RefreshFolder()
{
    Siblings_.Clear();
    Index_ = 0;
    if (Path_.Empty())
        return;
    hz::String Folder = hz::fs::Parent(Path_);
    hz::Vector<hz::fs::Entry> Entries;
    if (!HZ_SUCCESS(hz::fs::List(Folder, Entries)))
        return;
    for (const hz::fs::Entry &Entry : Entries) {
        if (Entry.Directory || !IsPictureName(Entry.Name.CStr()))
            continue;
        Siblings_.Push(hz::fs::Join(Folder, Entry.Name.CStr()));
    }
    for (size_t i = 0; i < Siblings_.Size(); i++) {
        if (hz::fs::EqualNames(Siblings_[i].CStr(), Path_.CStr())) {
            Index_ = i;
            break;
        }
    }
}

void Photos::UpdateTitle()
{
    hz::String Title = Path_.Empty() ? hz::String("Photos")
                                     : hz::String::Format("%s - Photos", hz::fs::FileName(Path_).CStr());
    Main_->SetTitle(Title.CStr());
}

void Photos::UpdateStatus()
{
    if (Path_.Empty()) {
        Status_->SetText("No picture open");
        return;
    }
    hz::String Pixels = Base_.Pixels ? hz::String::Format("%d x %d", Base_.Width, Base_.Height)
                                     : hz::String("not decoded");
    hz::String Position = Siblings_.Size() > 1 ? hz::String::Format("%zu of %zu", Index_ + 1, Siblings_.Size())
                                               : hz::String();
    hz::String Text =
        Position.Empty()
            ? hz::String::Format("%s  |  %s  |  %s", hz::fs::FileName(Path_).CStr(), Pixels.CStr(),
                                 hz::image::FormatName(Format_))
            : hz::String::Format("%s  |  %s  |  %s  |  %s", hz::fs::FileName(Path_).CStr(), Pixels.CStr(),
                                 hz::image::FormatName(Format_), Position.CStr());
    Status_->SetText(Text.CStr());
}

hz::Vector<MenuItem> Photos::FileMenu()
{
    hz::Vector<MenuItem> Items;
    MenuItem Item = MenuItem::Make("Open...", [this]() { OpenWithDialog(); }, "Ctrl+O");
    Item.Glyph = Icon::Open;
    Items.Push(hz::Move(Item));
    Items.Push(MenuItem::MakeSeparator());
    Items.Push(MenuItem::Make("Close", [this]() { Main_->Close(); }, "Ctrl+W"));
    return Items;
}

hz::Vector<MenuItem> Photos::ViewMenu()
{
    hz::Vector<MenuItem> Items;
    Items.Push(MenuItem::Make("Zoom in", [this]() { Stage_->ZoomBy(kZoomStep); }, "Ctrl++"));
    Items.Push(MenuItem::Make("Zoom out", [this]() { Stage_->ZoomBy(1.0f / kZoomStep); }, "Ctrl+-"));
    Items.Push(MenuItem::Make("Fit to window", [this]() { Stage_->FitToWindow(); }, "Ctrl+0"));
    Items.Push(MenuItem::Make("Actual size", [this]() { Stage_->SetZoom(1.0f); }, "Ctrl+1"));
    Items.Push(MenuItem::MakeSeparator());
    Items.Push(MenuItem::Make("Rotate left", [this]() { Rotate(-1); }, "Ctrl+Shift+R"));
    Items.Push(MenuItem::Make("Rotate right", [this]() { Rotate(1); }, "Ctrl+R"));
    Items.Push(MenuItem::MakeSeparator());
    Items.Push(MenuItem::Make("Previous picture", [this]() { Step(-1); }, "Left"));
    Items.Push(MenuItem::Make("Next picture", [this]() { Step(1); }, "Right"));
    return Items;
}

void Photos::OnKey(KeyEvent &E)
{
    if (E.Ctrl() && !E.Alt()) {
        switch (E.Key) {
        case 'O':
            E.Handled = true;
            OpenWithDialog();
            return;
        case 'W':
            E.Handled = true;
            Main_->Close();
            return;
        case 'R':
            E.Handled = true;
            Rotate(E.Shift() ? -1 : 1);
            return;
        case '0':
            E.Handled = true;
            Stage_->FitToWindow();
            return;
        case '1':
            E.Handled = true;
            Stage_->SetZoom(1.0f);
            return;
        default:
            break;
        }
    }
    /* The punctuation keys report a key code on some layouts and a character on others. */
    if (E.Key == '+' || E.Key == '=' || E.Character == '+') {
        E.Handled = true;
        Stage_->ZoomBy(kZoomStep);
        return;
    }
    if (E.Key == '-' || E.Key == '_' || E.Character == '-') {
        E.Handled = true;
        Stage_->ZoomBy(1.0f / kZoomStep);
        return;
    }
    switch (E.Key) {
    case HZ_VK_LEFT:
        E.Handled = true;
        Step(-1);
        return;
    case HZ_VK_RIGHT:
        E.Handled = true;
        Step(1);
        return;
    case HZ_VK_PRIOR:
        E.Handled = true;
        Step(-1);
        return;
    case HZ_VK_NEXT:
        E.Handled = true;
        Step(1);
        return;
    case HZ_VK_SPACE:
        E.Handled = true;
        Step(1);
        return;
    default:
        break;
    }
}

}  // namespace

extern "C" int main(int argc, char **argv)
{
    Photos *App = new Photos();
    if (!App->Create(argc > 1 ? argv[1] : nullptr)) {
        HzPrint("photos: cannot create the window");
        return 1;
    }
    HzPrint("photos: started");
    return RunMessageLoop();
}
