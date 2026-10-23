/*
 * Nexora OS - user/apps/paint/paint.cpp
 * A simple drawing pad that exports uncompressed 24-bit BMP files.
 */
#include "hzfile.h"
#include "hzui.h"

using namespace hzui;

namespace {

constexpr int kBitmapWidth = 480;
constexpr int kBitmapHeight = 300;

struct Segment {
    int32_t X0, Y0, X1, Y1;
    Color Ink;
};

class DrawingCanvas final : public Element {
public:
    DrawingCanvas() { Height = 360; }

    void SetInk(Color Ink) { Ink_ = Ink; }
    void Clear()
    {
        Segments_.Clear();
        Invalidate();
    }

    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        P.FillRoundRect(B, Theme::OverlayRadius, 0xFFF7F7F7);
        P.StrokeRoundRect(B, Theme::OverlayRadius, 1, Theme::CardStroke);
        for (const Segment &S : Segments_) {
            float X0 = (float)B.X + (float)S.X0 * (float)B.Width / (float)kBitmapWidth;
            float Y0 = (float)B.Y + (float)S.Y0 * (float)B.Height / (float)kBitmapHeight;
            float X1 = (float)B.X + (float)S.X1 * (float)B.Width / (float)kBitmapWidth;
            float Y1 = (float)B.Y + (float)S.Y1 * (float)B.Height / (float)kBitmapHeight;
            P.Line(X0, Y0, X1, Y1, 5.0f, S.Ink);
        }
    }

    void OnPointerDown(PointerEvent &E) override
    {
        if (E.Button != HZ_MOUSE_LEFT || !Bounds().Contains(E.Position))
            return;
        Drawing_ = true;
        Last_ = Map(E.Position);
        Segments_.Push(Segment{Last_.X, Last_.Y, Last_.X, Last_.Y, Ink_});
        Invalidate();
        E.Handled = true;
    }

    void OnPointerMove(PointerEvent &E) override
    {
        if (!Drawing_ || !(E.Buttons & HZ_MOUSE_LEFT))
            return;
        Point Current = Map(E.Position);
        if (Current == Last_)
            return;
        Segments_.Push(Segment{Last_.X, Last_.Y, Current.X, Current.Y, Ink_});
        Last_ = Current;
        Invalidate();
        E.Handled = true;
    }

    void OnPointerUp(PointerEvent &E) override
    {
        if (E.Button != HZ_MOUSE_LEFT || !Drawing_)
            return;
        Point Current = Map(E.Position);
        if (!(Current == Last_))
            Segments_.Push(Segment{Last_.X, Last_.Y, Current.X, Current.Y, Ink_});
        Last_ = Current;
        Drawing_ = false;
        Invalidate();
        E.Handled = true;
    }

    const hz::Vector<Segment> &Segments() const { return Segments_; }

private:
    Point Map(Point P) const
    {
        const Rect &B = Bounds();
        if (B.Width <= 0 || B.Height <= 0)
            return Point{};
        int32_t X = (P.X - B.X) * kBitmapWidth / B.Width;
        int32_t Y = (P.Y - B.Y) * kBitmapHeight / B.Height;
        return Point{hz::Clamp(X, 0, kBitmapWidth - 1), hz::Clamp(Y, 0, kBitmapHeight - 1)};
    }

    hz::Vector<Segment> Segments_;
    Point Last_{};
    Color Ink_ = 0xFF202020;
    bool Drawing_ = false;
};

void Put16(uint8_t *Out, size_t At, uint16_t Value)
{
    Out[At] = (uint8_t)Value;
    Out[At + 1] = (uint8_t)(Value >> 8);
}

void Put32(uint8_t *Out, size_t At, uint32_t Value)
{
    Put16(Out, At, (uint16_t)Value);
    Put16(Out, At + 2, (uint16_t)(Value >> 16));
}

void Pixel(uint8_t *Pixels, int X, int Y, Color Ink)
{
    if (X < 0 || X >= kBitmapWidth || Y < 0 || Y >= kBitmapHeight)
        return;
    size_t At = ((size_t)Y * kBitmapWidth + (size_t)X) * 3;
    Pixels[At] = (uint8_t)Ink;
    Pixels[At + 1] = (uint8_t)(Ink >> 8);
    Pixels[At + 2] = (uint8_t)(Ink >> 16);
}

void DrawSegment(uint8_t *Pixels, const Segment &S)
{
    int X = S.X0, Y = S.Y0;
    int Dx = S.X1 >= S.X0 ? S.X1 - S.X0 : S.X0 - S.X1;
    int Sx = S.X0 < S.X1 ? 1 : -1;
    int Dy = -(S.Y1 >= S.Y0 ? S.Y1 - S.Y0 : S.Y0 - S.Y1);
    int Sy = S.Y0 < S.Y1 ? 1 : -1;
    int Error = Dx + Dy;
    for (;;) {
        for (int Oy = -2; Oy <= 2; Oy++)
            for (int Ox = -2; Ox <= 2; Ox++)
                if (Ox * Ox + Oy * Oy <= 5)
                    Pixel(Pixels, X + Ox, Y + Oy, S.Ink);
        if (X == S.X1 && Y == S.Y1)
            break;
        int Twice = 2 * Error;
        if (Twice >= Dy) {
            Error += Dy;
            X += Sx;
        }
        if (Twice <= Dx) {
            Error += Dx;
            Y += Sy;
        }
    }
}

class PaintApp {
public:
    bool Create()
    {
        Main_ = new Window("Paint", 760, 520);
        Main_->SetMinimumSize(480, 390);
        StackPanel *Root = new StackPanel(Orientation::Vertical, 8);
        Root->Padding = Thickness(12, 8);
        StackPanel *Toolbar = Root->Add<StackPanel>(Orientation::Horizontal, 6);
        struct Palette {
            const char *Name;
            Color Ink;
        };
        static const Palette Colors[] = {{"Ink", 0xFF202020}, {"Blue", 0xFF2775D2}, {"Red", 0xFFE05252},
                                         {"Green", 0xFF299C68}, {"Gold", 0xFFE8A22A}};
        for (const Palette &ColorChoice : Colors) {
            Button *Choice = Toolbar->Add<Button>(ColorChoice.Name, ButtonStyle::Subtle);
            Choice->OnClick = [this, Ink = ColorChoice.Ink]() {
                Canvas_->SetInk(Ink);
                Status_->SetText("Brush color changed");
            };
        }
        Button *Clear = Toolbar->Add<Button>("Clear", ButtonStyle::Standard);
        Clear->OnClick = [this]() {
            Canvas_->Clear();
            Status_->SetText("Canvas cleared");
            HzPrint("paint: canvas cleared");
        };
        Button *Save = Toolbar->Add<Button>("Save BMP", ButtonStyle::Accent);
        Save->OnClick = [this]() { SaveBitmap(); };
        Canvas_ = Root->Add<DrawingCanvas>();
        Status_ = Root->Add<TextBlock>("Draw with the mouse. Save writes a BMP to Documents.", FontRole::Caption);
        Status_->Foreground = Theme::TextTertiary;
        Main_->SetContent(Root);
        Main_->OnClosed = []() { NtUserPostQuitMessage(0); };
        if (!Main_->Show())
            return false;
        HzPrint("paint: started");
        return true;
    }

private:
    void SaveBitmap()
    {
        constexpr uint32_t Stride = kBitmapWidth * 3;
        constexpr uint32_t DataSize = Stride * kBitmapHeight;
        constexpr uint32_t HeaderSize = 54;
        hz::Vector<uint8_t> File;
        File.Resize(HeaderSize + DataSize);
        uint8_t *Bytes = File.Data();
        Bytes[0] = 'B';
        Bytes[1] = 'M';
        Put32(Bytes, 2, HeaderSize + DataSize);
        Put32(Bytes, 10, HeaderSize);
        Put32(Bytes, 14, 40);
        Put32(Bytes, 18, kBitmapWidth);
        Put32(Bytes, 22, kBitmapHeight);
        Put16(Bytes, 26, 1);
        Put16(Bytes, 28, 24);
        Put32(Bytes, 34, DataSize);
        for (uint32_t Y = 0; Y < kBitmapHeight; Y++)
            for (uint32_t X = 0; X < kBitmapWidth; X++)
                Pixel(Bytes + HeaderSize, (int)X, (int)(kBitmapHeight - 1 - Y), 0xFFFFFFFF);
        for (const Segment &S : Canvas_->Segments())
            DrawSegment(Bytes + HeaderSize, S);

        hz::String Path = hz::fs::Join(hz::fs::DocumentsFolder(), "Nexora-Paint.bmp");
        STATUS Result = hz::fs::WriteAll(Path, (const char *)Bytes, File.Size());
        if (!HZ_SUCCESS(Result)) {
            Status_->SetText(hz::String::Format("Save failed: %s", hz::fs::StatusText(Result)).CStr());
            HzPrint("paint: save failed for %s (0x%08X)", Path.CStr(), (uint32_t)Result);
            return;
        }
        Status_->SetText(hz::String::Format("Saved %s", Path.CStr()).CStr());
        HzPrint("paint: saved %s", Path.CStr());
    }

    Window *Main_ = nullptr;
    DrawingCanvas *Canvas_ = nullptr;
    TextBlock *Status_ = nullptr;
};

}  // namespace

extern "C" int main(int argc, char **argv)
{
    PaintApp App;
    if (!App.Create()) {
        HzPrint("paint: cannot create the window");
        return 1;
    }
    return RunMessageLoop();
}
