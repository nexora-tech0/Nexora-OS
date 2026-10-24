/*
 * Nexora OS - user/tests/uitest/uitest.cpp
 * UI toolkit self-test, run by the kernel at every boot after wintest (win32k/w32test.cpp):
 *   - hzcore (Vector, String, Function, UniquePtr) and global constructors;
 *   - layout math (StackPanel, Grid, alignment, margins, wrapping text);
 *   - the Painter and controls rendered into an off-screen canvas, checked pixel by pixel;
 *   - a live window driven by injected input: Tab order, text editing, clicks and keys on every
 *     control, the combo box menu, the text box context menu, the virtualized list, tooltips,
 *     cursor shapes, maximize and close requests;
 *   - the multi-line editor and the menu bar in a live window (typing, indentation, undo and
 *     redo, selection and the clipboard, the caret from a click, the wheel), and the dialogs
 *     (message box answers, the owner disabled meanwhile, input box, the file dialog, owned
 *     windows kept off the shell's window list).
 * The exit code is the number of failed checks.
 */
#include "hzui.h"

using namespace hzui;

namespace {

int g_Checks;
int g_Failures;

void Check(bool Ok, const char *What)
{
    g_Checks++;
    if (!Ok) {
        g_Failures++;
        HzPrint("uitest: FAIL %s", What);
    }
}

/* ---- Global constructors -------------------------------------------------------------------- */

struct GlobalProbe {
    int Value;
    GlobalProbe() : Value(1234) {}
};
GlobalProbe g_Probe;
hz::String g_Greeting("constructed before main");

/* ---- Core ------------------------------------------------------------------------------------ */

void TestCore()
{
    Check(g_Probe.Value == 1234 && g_Greeting == "constructed before main", "global constructors ran");

    hz::Vector<hz::String> V;
    for (int i = 0; i < 100; i++)
        V.Push(hz::String::Format("item %d", i));
    V.Insert(0, hz::String("first"));
    V.Erase(50);
    Check(V.Size() == 100 && V[0] == "first" && V[1] == "item 0" && V[50] == "item 50" && V.Back() == "item 99",
          "Vector of String: push, insert, erase");
    hz::Vector<hz::String> Copy = V;
    hz::Vector<hz::String> Moved = hz::Move(Copy);
    Check(Moved.Size() == 100 && Copy.Empty() && Moved.Find(hz::String("item 77")) == 77, "Vector copy, move, find");
    V.Push(V[0]); /* aliasing an element while the vector grows */
    Check(V.Back() == "first", "Vector push of its own element");

    hz::String S("Hello");
    S.Append(", world");
    S.Insert(5, "!!", 2);
    S.Erase(5, 1);
    Check(S == "Hello!, world" && S.Length() == 13, "String append, insert, erase");
    Check(S.Find("world") == 8 && S.Find("nope") == hz::NotFound && S.Substring(7, 2) == " w" &&
              S.ContainsIgnoreCase("WORLD") && S.StartsWith("Hell") && S.Substring(8, 5) == "world",
          "String find, substring, case-insensitive search");
    S.Insert(0, S.CStr() + 8, 5); /* source inside itself */
    Check(S == "worldHello!, world", "String insert from itself");
    hz::String U("a\xC3\xA9z"); /* a, e-acute, z */
    Check(U.NextChar(1) == 3 && U.PreviousChar(3) == 1 && U.CodePointAt(1) == 0xE9 && U.NextChar(4) == 4,
          "String UTF-8 navigation");
    hz::String Built;
    Built.AppendCodePoint(0x20AC);
    Check(Built == "\xE2\x82\xAC", "String UTF-8 encoding");

    int Calls = 0;
    hz::Function<int(int)> Twice = [&Calls](int X) {
        Calls++;
        return X * 2;
    };
    hz::Function<int(int)> Other = Twice;
    Check(Twice(4) == 8 && Other(5) == 10 && Calls == 2, "Function copies share captured references");
    hz::Function<void()> Empty;
    Check(!Empty && (bool)Twice, "Function emptiness");

    hz::UniquePtr<hz::String> P = hz::MakeUnique<hz::String>("owned");
    hz::UniquePtr<hz::String> Q = hz::Move(P);
    Check(!P && Q && *Q == "owned", "UniquePtr move");
}

void TestTypography()
{
    HZ_FONT *Body = GetFont(FontRole::Body);
    HZ_FONT *BodyLarge = GetFont(FontRole::BodyLarge);
    HZ_FONT *Display = GetFont(FontRole::Display);
    HZ_FONT *Mono = GetFont(FontRole::Mono);
    Check(Body && BodyLarge && Display && Mono, "standard font roles load from the packaged font assets");
    if (Body && BodyLarge && Display && Mono) {
        Check(HzFontLineHeight(Body) < HzFontLineHeight(BodyLarge) &&
                  HzFontLineHeight(BodyLarge) < HzFontLineHeight(Display),
              "body, large-body, and display roles follow the typography scale");
        Check(HzFontLineHeight(Mono) > 0, "technical text font has valid metrics");
    }
}

/* ---- Layout -------------------------------------------------------------------------------------- */

void TestLayout()
{
    StackPanel Stack(Orientation::Vertical, 10);
    Stack.Padding = Thickness(5);
    Element *A = Stack.Add<Element>();
    A->Height = 20;
    Element *B = Stack.Add<Element>();
    B->Height = 30;
    B->Margin = Thickness(3);
    Element *Hidden = Stack.Add<Element>();
    Hidden->Height = 50;
    Hidden->SetVisible(false);
    Element *C = Stack.Add<Element>();
    C->Width = 100;
    C->Height = 40;
    C->HorizontalAlignment = Align::Center;
    LayoutTree(&Stack, Rect{0, 0, 300, 200});
    Check(A->Bounds() == Rect{5, 5, 290, 20}, "StackPanel: first child");
    Check(B->Bounds() == Rect{8, 38, 284, 30}, "StackPanel: margins and spacing");
    Check(Hidden->Bounds().Empty() && C->Bounds() == Rect{100, 81, 100, 40}, "StackPanel: hidden children and centring");
    Check(Stack.DesiredSize() == Size{110, 5 + 20 + 10 + 36 + 10 + 40 + 5}, "StackPanel: desired size");

    Grid G;
    G.Columns.Push(GridLength::Pixels(100));
    G.Columns.Push(GridLength::AutoSize());
    G.Columns.Push(GridLength::Stars(1));
    G.Columns.Push(GridLength::Stars(2));
    G.Rows.Push(GridLength::AutoSize());
    G.Rows.Push(GridLength::Stars(1));
    G.ColumnSpacing = 10;
    G.RowSpacing = 4;
    Element *Auto = G.Add<Element>();
    Auto->GridColumn = 1;
    Auto->Width = 50;
    Auto->Height = 25;
    Element *Span = G.Add<Element>();
    Span->GridRow = 1;
    Span->GridColumnSpan = 2;
    LayoutTree(&G, Rect{0, 0, 400, 300});
    const hz::Vector<int32_t> &W = G.ColumnWidths();
    Check(W.Size() == 4 && W[0] == 100 && W[1] == 50 && W[2] == 73 && W[3] == 147, "Grid: pixel, auto and star columns");
    Check(G.RowHeights()[0] == 25 && G.RowHeights()[1] == 300 - 25 - 4, "Grid: auto and star rows");
    Check(Auto->Bounds() == Rect{110, 0, 50, 25} && Span->Bounds() == Rect{0, 29, 160, 271}, "Grid: cells and spans");

    TextBlock Text("The quick brown fox jumps over the lazy dog, again and again and again.");
    Text.Wrap = true;
    Text.Measure(Size{150, Infinite});
    int32_t Line = HzFontLineHeight(GetFont(FontRole::Body));
    Check(Text.DesiredSize().Height >= 3 * Line && Text.DesiredSize().Height % Line == 0 && Text.DesiredSize().Width <= 150,
          "TextBlock: wrapping");
    TextLayout Layout;
    Layout.Layout(GetFont(FontRole::Body), "one\ntwo\n\nfour", 13, 0);
    Check(Layout.Lines.Size() == 4 && Layout.Lines[2].Length == 0 && Layout.Lines[3].Length == 4, "TextLayout: line breaks");
    hz::String Cut = EllipsizeText(GetFont(FontRole::Body), "A rather long piece of text", 27, 80);
    Check(HzMeasureText(GetFont(FontRole::Body), Cut.CStr()) <= 80 && Cut.Find("\xE2\x80\xA6") != hz::NotFound,
          "Ellipsis");
}

/* ---- Off-screen rendering ---------------------------------------------------------------------- */

constexpr int32_t kCanvasW = 240, kCanvasH = 320;
uint32_t g_Pixels[kCanvasW * kCanvasH];

HZ_CANVAS MakeCanvas()
{
    for (uint32_t &P : g_Pixels)
        P = 0xFF000000;
    HZ_CANVAS C = {g_Pixels, kCanvasW, kCanvasH, kCanvasW, {0, 0, kCanvasW, kCanvasH}};
    return C;
}

uint32_t At(int32_t X, int32_t Y)
{
    return g_Pixels[Y * kCanvasW + X];
}

int Lit(const Rect &R)
{
    int Count = 0;
    for (int32_t y = R.Y; y < R.Bottom(); y++)
        for (int32_t x = R.X; x < R.Right(); x++)
            Count += (At(x, y) & 0xFF) > 0x60;
    return Count;
}

bool Near(uint32_t A, uint32_t B, int Tolerance)
{
    for (int Shift = 0; Shift <= 24; Shift += 8) {
        int D = (int)((A >> Shift) & 0xFF) - (int)((B >> Shift) & 0xFF);
        if (D < -Tolerance || D > Tolerance)
            return false;
    }
    return true;
}

void TestPainting()
{
    HZ_CANVAS Canvas = MakeCanvas();
    Painter P(Canvas);
    P.FillRoundRect(Rect{10, 10, 40, 30}, 8, 0xFFFF0000);
    Check(At(30, 25) == 0xFFFF0000 && At(49, 25) == 0xFFFF0000, "rounded rectangle: inside");
    Check(((At(10, 10) >> 16) & 0xFF) < 0x20 && ((At(12, 12) >> 16) & 0xFF) > 0x30, "rounded rectangle: corner coverage");
    P.StrokeRoundRect(Rect{60, 10, 40, 30}, 4, 2, 0xFFFFFFFF);
    Check(At(60, 25) == 0xFFFFFFFF && At(61, 25) == 0xFFFFFFFF && At(62, 25) == 0xFF000000 && At(80, 25) == 0xFF000000,
          "stroke lies inside the edge");
    P.PushClip(Rect{0, 0, 5, 5});
    P.FillRect(Rect{0, 0, 100, 100}, 0xFF0000FF);
    P.PopClip();
    Check(At(4, 4) == 0xFF0000FF && At(5, 5) == 0xFF000000, "clip stack");
    float Points[] = {10, 60, 40, 60, 40, 90};
    P.Polyline(Points, 3, 4, 0x80FFFFFF);
    Check(Near(At(40, 60), At(25, 60), 3) && (At(25, 60) & 0xFF) > 0x60, "translucent polyline joints are blended once");
    P.Text(GetFont(FontRole::Body), 120, 60, "Hz", 2, 0xFFFFFFFF);
    Check(Lit(Rect{118, 40, 30, 25}) > 15, "text");
    P.DrawIcon(Icon::Check, Rect{160, 40, 16, 16}, 0xFFFFFFFF, 1.5f);
    Check(Lit(Rect{160, 40, 16, 16}) > 8, "icon");
    P.FillVerticalGradient(Rect{200, 10, 20, 100}, 0, 0xFF000000, 0xFF0000FF);
    Check((At(210, 12) & 0xFF) < 0x10 && (At(210, 107) & 0xFF) > 0xF0, "vertical gradient");

    Button Accent("OK", ButtonStyle::Accent);
    LayoutTree(&Accent, Rect{10, 150, 100, 32});
    PaintTree(&Accent, P);
    Check(At(14, 155) == Theme::Accent, "accent button");
    CheckBox Box("Checked", true);
    LayoutTree(&Box, Rect{10, 200, 150, 32});
    PaintTree(&Box, P);
    Check(At(11, 216) == Theme::Accent && Lit(Rect{30, 200, 120, 32}) > 20, "checked check box and its label");
    ToggleSwitch Toggle;
    Toggle.SetOn(true, false);
    LayoutTree(&Toggle, Rect{10, 250, 150, 32});
    PaintTree(&Toggle, P);
    Check(At(14, 266) == Theme::Accent && Toggle.IsOn(), "toggle switch on");
    ProgressBar Progress(50);
    LayoutTree(&Progress, Rect{120, 250, 100, 4});
    PaintTree(&Progress, P);
    Check(At(140, 252) == Theme::Accent && At(200, 252) != Theme::Accent, "progress bar");
}

void TestTransitions()
{
    Transition T;
    T.Jump(5);
    Check(T.Value() == 5 && !T.Running(), "transition jump");
    T.To(10, 200);
    Check(T.Value() < 9 && T.Running(), "transition starts where it was");
    NtDelayExecution(230000);
    Check(T.Value() == 10 && !T.Running(), "transition ends at its target");
}

/* ---- Live window --------------------------------------------------------------------------------- */

STATUS SendMouse(int32_t X, int32_t Y, uint16_t Buttons, int16_t Wheel = 0)
{
    HZ_INPUT In;
    memset(&In, 0, sizeof(In));
    In.Type = HZ_INPUT_MOUSE;
    In.Flags = HZ_INPUT_MOUSE_ABSOLUTE;
    In.Mouse.DeltaX = (int16_t)X;
    In.Mouse.DeltaY = (int16_t)Y;
    In.Mouse.Buttons = Buttons;
    In.Mouse.Wheel = Wheel;
    return NtUserSendInput(&In, 1);
}

void Key(uint8_t Vk, uint8_t Character, uint16_t Modifiers)
{
    HZ_INPUT In[2];
    memset(In, 0, sizeof(In));
    for (int i = 0; i < 2; i++) {
        In[i].Type = HZ_INPUT_KEYBOARD;
        In[i].Key.VirtualKey = Vk;
        In[i].Key.Modifiers = Modifiers;
        In[i].Key.Flags = i ? HZ_KEY_BREAK : 0;
        In[i].Key.Character = i ? 0 : Character;
    }
    NtUserSendInput(In, 2);
    PumpMessages();
}

void Type(const char *Text)
{
    for (const char *C = Text; *C; C++) {
        char Ch = *C;
        uint8_t Vk = (Ch >= 'a' && Ch <= 'z') ? (uint8_t)(Ch - 32) : (uint8_t)Ch;
        uint16_t Mods = (Ch >= 'A' && Ch <= 'Z') ? HZ_MOD_SHIFT : 0;
        Key(Vk, (uint8_t)Ch, Mods);
    }
}

void Wait(uint32_t Ms)
{
    uint64_t End = NowUs() + (uint64_t)Ms * 1000;
    while (NowUs() < End) {
        PumpMessages();
        NtDelayExecution(4000);
    }
    PumpMessages();
}

void Settle()
{
    PumpMessages();
    NtUserFlush(2000);
}

uint32_t ScreenPixel(int32_t X, int32_t Y)
{
    uint32_t C = 0;
    NtUserGetScreenPixel(X, Y, &C);
    return C & 0xFFFFFF;
}

uint32_t CursorShape()
{
    HZ_DISPLAY_INFO Info;
    memset(&Info, 0, sizeof(Info));
    NtUserGetDisplayInfo(&Info);
    return Info.CursorShape;
}

HWND WindowAt(int32_t X, int32_t Y)
{
    HWND H = 0;
    NtUserWindowFromPoint(X, Y, &H);
    return H;
}

struct Live {
    Window *W;
    TextBox *Edit;
    Button *Ok, *Disabled, *Tipped;
    CheckBox *Check;
    ToggleSwitch *Toggle;
    Slider *Volume;
    ComboBox *Combo;
    ListView *List;
    int Clicks = 0, Submits = 0, Toggles = 0, ComboChanges = 0;
    hz::String Submitted;
};

Point ScreenOf(Live &L, Element *E, int32_t Dx = -1, int32_t Dy = -1)
{
    Point O = L.W->ScreenOrigin();
    const Rect &B = E->Bounds();
    return Point{O.X + B.X + (Dx < 0 ? B.Width / 2 : Dx), O.Y + B.Y + (Dy < 0 ? B.Height / 2 : Dy)};
}

void ClickAt(Point P, int Count = 1)
{
    SendMouse(P.X, P.Y, 0);
    for (int i = 0; i < Count; i++) {
        SendMouse(P.X, P.Y, HZ_MOUSE_LEFT);
        SendMouse(P.X, P.Y, 0);
    }
    PumpMessages();
}

void RightClickAt(Point P)
{
    SendMouse(P.X, P.Y, 0);
    SendMouse(P.X, P.Y, HZ_MOUSE_RIGHT);
    SendMouse(P.X, P.Y, 0);
    PumpMessages();
}

void BuildLive(Live &L)
{
    L.W = new Window("UI test", 640, 560);
    StackPanel *Root = new StackPanel(Orientation::Vertical, 8);
    Root->Padding = Thickness(16);
    L.Edit = Root->Add<TextBox>("Type here");
    L.Edit->Width = 300;
    L.Edit->HorizontalAlignment = Align::Start;
    L.Edit->OnSubmit = [&L](const hz::String &Text) {
        L.Submits++;
        L.Submitted = Text;
    };
    StackPanel *Row = Root->Add<StackPanel>(Orientation::Horizontal, 8);
    L.Ok = Row->Add<Button>("OK", ButtonStyle::Accent);
    L.Ok->OnClick = [&L]() { L.Clicks++; };
    L.Disabled = Row->Add<Button>("Disabled");
    L.Disabled->SetEnabled(false);
    L.Disabled->OnClick = [&L]() { L.Clicks += 100; };
    L.Check = Row->Add<CheckBox>("Check");
    L.Toggle = Row->Add<ToggleSwitch>();
    L.Toggle->OnToggled = [&L](bool) { L.Toggles++; };
    L.Volume = Root->Add<Slider>(0, 100, 0);
    L.Volume->Width = 300;
    L.Volume->HorizontalAlignment = Align::Start;
    L.Combo = Root->Add<ComboBox>();
    L.Combo->Items.Push("Red");
    L.Combo->Items.Push("Green");
    L.Combo->Items.Push("Blue");
    L.Combo->Width = 200;
    L.Combo->HorizontalAlignment = Align::Start;
    L.Combo->OnSelectionChanged = [&L](size_t) { L.ComboChanges++; };
    L.Tipped = Root->Add<Button>("Hover me");
    L.Tipped->ToolTip = "A tooltip from hzui";
    L.Tipped->HorizontalAlignment = Align::Start;
    L.List = Root->Add<ListView>();
    L.List->Height = 180;
    L.List->ItemText = [](size_t I) { return hz::String::Format("Row %u", (unsigned)I); };
    L.List->SetItemCount(1000);
    L.W->SetContent(Root);
}

void TestFocusAndText(Live &L)
{
    Window &W = *L.W;
    W.SetFocus(nullptr);
    Key(HZ_VK_TAB, '\t', 0);
    Check(W.FocusedElement() == L.Edit && W.FocusVisible(), "Tab focuses the first control");
    Key(HZ_VK_TAB, '\t', 0);
    Check(W.FocusedElement() == L.Ok, "Tab moves to the next control");
    Key(HZ_VK_TAB, '\t', 0);
    Check(W.FocusedElement() == L.Check, "Tab skips disabled controls");
    Key(HZ_VK_TAB, '\t', HZ_MOD_SHIFT);
    Check(W.FocusedElement() == L.Ok, "Shift+Tab moves back");

    ClickAt(ScreenOf(L, L.Edit));
    Check(W.FocusedElement() == L.Edit && !W.FocusVisible(), "clicking focuses without a focus ring");
    Check(CursorShape() == HZ_CURSOR_IBEAM, "I-beam over the text box");
    Type("Hello");
    Check(L.Edit->Text() == "Hello" && L.Edit->CaretPosition() == 5, "typing");
    Key(HZ_VK_LEFT, 0, 0);
    Key(HZ_VK_LEFT, 0, 0);
    Key(HZ_VK_BACK, 0, 0);
    Check(L.Edit->Text() == "Helo" && L.Edit->CaretPosition() == 2, "caret moves and Backspace");
    Key(HZ_VK_END, 0, HZ_MOD_SHIFT);
    Check(L.Edit->SelectionStart() == 2 && L.Edit->SelectionEnd() == 4, "Shift+End selects");
    Type("y");
    Check(L.Edit->Text() == "Hey" && !L.Edit->HasSelection(), "typing replaces the selection");
    Key('A', 0, HZ_MOD_CTRL);
    Key('C', 0, HZ_MOD_CTRL);
    Key(HZ_VK_DELETE, 0, 0);
    Check(L.Edit->Text().Empty() && Clipboard::GetText() == "Hey", "select all, copy, delete");
    Key('V', 0, HZ_MOD_CTRL);
    Key(HZ_VK_HOME, 0, 0);
    Type("Oh ");
    Check(L.Edit->Text() == "Oh Hey" && L.Edit->CaretPosition() == 3, "paste and Home");
    Key(HZ_VK_RIGHT, 0, HZ_MOD_CTRL);
    Check(L.Edit->CaretPosition() == 6, "Ctrl+Right moves by word");
    Key(HZ_VK_BACK, 0, HZ_MOD_CTRL);
    Check(L.Edit->Text() == "Oh ", "Ctrl+Backspace deletes a word");
    Key(HZ_VK_RETURN, '\r', 0);
    Check(L.Submits == 1 && L.Submitted == "Oh ", "Enter submits");

    L.Edit->SetText("alpha beta");
    PumpMessages();
    Point Word = ScreenOf(L, L.Edit, 0, -1);
    Word.X = W.ScreenOrigin().X + (L.Edit->OffsetToX(7) + L.Edit->OffsetToX(8)) / 2;
    ClickAt(Word, 2);
    Check(L.Edit->SelectionStart() == 6 && L.Edit->SelectionEnd() == 10, "double-click selects a word");
    Wait(600); /* no double-click with the next press */
    Point From = Word, To = Word;
    From.X = W.ScreenOrigin().X + L.Edit->OffsetToX(0) + 1;
    To.X = W.ScreenOrigin().X + L.Edit->OffsetToX(5);
    SendMouse(From.X, From.Y, 0);
    SendMouse(From.X, From.Y, HZ_MOUSE_LEFT);
    SendMouse(To.X, To.Y, HZ_MOUSE_LEFT);
    SendMouse(To.X, To.Y, 0);
    PumpMessages();
    Check(L.Edit->SelectionStart() == 0 && L.Edit->SelectionEnd() == 5, "dragging selects");

    L.Edit->SetText("");
    Type("abcdefghijklmnopqrstuvwxyz abcdefghijklmnopqrstuvwxyz abcdefghijklmnopqrstuvwxyz");
    int32_t Caret = L.Edit->OffsetToX(L.Edit->CaretPosition());
    Check(Caret <= L.Edit->Bounds().Right() - 11 && Caret >= L.Edit->Bounds().X + 11, "long text scrolls to the caret");
    Key(HZ_VK_HOME, 0, 0);
    Check(L.Edit->OffsetToX(0) == L.Edit->Bounds().X + 11, "Home scrolls back");

    RightClickAt(ScreenOf(L, L.Edit));
    Check(W.ActiveMenu() != nullptr, "right-click opens the context menu");
    Key(HZ_VK_ESCAPE, 0x1B, 0);
    CollectClosedWindows();
    Check(W.ActiveMenu() == nullptr, "Escape closes it");
    RightClickAt(ScreenOf(L, L.Ok));
    Check(W.ActiveMenu() != nullptr, "right-click on a standard control opens the window context menu");
    Key(HZ_VK_ESCAPE, 0x1B, 0);
    CollectClosedWindows();
    Check(W.ActiveMenu() == nullptr, "the standard context menu closes with Escape");
    L.Edit->SetText("");
}

void TestControls(Live &L)
{
    Window &W = *L.W;
    Point OkLeft = ScreenOf(L, L.Ok, 3, -1);
    SendMouse(OkLeft.X - 200, OkLeft.Y + 200, 0);
    Wait(200);
    Settle();
    Check(ScreenPixel(OkLeft.X, OkLeft.Y) == (Theme::Accent & 0xFFFFFF), "accent button at rest");
    SendMouse(OkLeft.X, OkLeft.Y, 0);
    Wait(200);
    Settle();
    /* Two pixels left of the pointer: the cursor sprite starts at its hotspot. */
    Check(ScreenPixel(OkLeft.X - 2, OkLeft.Y) == (Theme::AccentHover & 0xFFFFFF), "accent button hovered (animated)");
    ClickAt(ScreenOf(L, L.Ok));
    Check(L.Clicks == 1 && W.FocusedElement() == L.Ok, "clicking a button");
    Key(HZ_VK_SPACE, ' ', 0);
    Check(L.Clicks == 2, "Space clicks the focused button");
    ClickAt(ScreenOf(L, L.Disabled));
    Check(L.Clicks == 2, "disabled buttons ignore clicks");
    Point Ok = ScreenOf(L, L.Ok);
    SendMouse(Ok.X, Ok.Y, HZ_MOUSE_LEFT);
    SendMouse(Ok.X + 300, Ok.Y + 300, HZ_MOUSE_LEFT); /* dragged off before release */
    SendMouse(Ok.X + 300, Ok.Y + 300, 0);
    PumpMessages();
    Check(L.Clicks == 2, "a press released elsewhere is not a click");

    ClickAt(ScreenOf(L, L.Check));
    Check(L.Check->IsChecked(), "clicking a check box");
    Key(HZ_VK_SPACE, ' ', 0);
    Check(!L.Check->IsChecked(), "Space toggles it");

    ClickAt(ScreenOf(L, L.Toggle, 10, -1));
    Check(L.Toggle->IsOn() && L.Toggles == 1, "clicking a toggle switch");
    Wait(250);
    Settle();
    Point Track = ScreenOf(L, L.Toggle, 4, -1);
    Check(ScreenPixel(Track.X, Track.Y) == (Theme::Accent & 0xFFFFFF) ||
              ScreenPixel(Track.X, Track.Y) == (Theme::AccentHover & 0xFFFFFF),
          "the toggle track turns accent");

    Rect TrackRect = L.Volume->TrackRect();
    Point O = W.ScreenOrigin();
    ClickAt(Point{O.X + TrackRect.X + TrackRect.Width * 3 / 4, O.Y + TrackRect.Y + 2});
    Check(L.Volume->Value() >= 74 && L.Volume->Value() <= 76, "clicking a slider track");
    SendMouse(O.X + TrackRect.X + TrackRect.Width * 3 / 4, O.Y + TrackRect.Y + 2, HZ_MOUSE_LEFT);
    SendMouse(O.X + TrackRect.X - 50, O.Y + TrackRect.Y + 2, HZ_MOUSE_LEFT);
    SendMouse(O.X + TrackRect.X - 50, O.Y + TrackRect.Y + 2, 0);
    PumpMessages();
    Check(L.Volume->Value() == 0, "dragging the slider past its end");
    Key(HZ_VK_RIGHT, 0, 0);
    Check(L.Volume->Value() == 1, "Right steps the slider");
    Key(HZ_VK_END, 0, 0);
    Check(L.Volume->Value() == 100, "End jumps to the maximum");
}

void TestComboAndMenus(Live &L)
{
    Window &W = *L.W;
    Point Combo = ScreenOf(L, L.Combo);
    ClickAt(Combo);
    Check(W.ActiveMenu() != nullptr, "clicking a combo box opens its list");
    Point Below{Combo.X, ScreenOf(L, L.Combo, -1, L.Combo->Bounds().Height).Y + 2 + 4 + 16};
    HWND Popup = WindowAt(Below.X, Below.Y);
    Check(Popup != 0 && Popup == W.ActiveMenu()->Handle(), "the list is a popup window under it");
    Key(HZ_VK_DOWN, 0, 0);
    Key(HZ_VK_RETURN, '\r', 0);
    CollectClosedWindows();
    Check(L.Combo->SelectedIndex() == 1 && L.ComboChanges == 1 && W.ActiveMenu() == nullptr,
          "Down and Enter choose from the list");

    ClickAt(Combo);
    Check(W.ActiveMenu() != nullptr, "the list opens again");
    Point Third{Below.X, Below.Y + 2 * 32};
    ClickAt(Third);
    CollectClosedWindows();
    Check(L.Combo->SelectedIndex() == 2 && W.ActiveMenu() == nullptr, "clicking an item chooses it");

    ClickAt(Combo);
    int ClicksBefore = L.Clicks;
    ClickAt(ScreenOf(L, L.Ok)); /* the click outside closes the menu and is consumed */
    CollectClosedWindows();
    Check(W.ActiveMenu() == nullptr && L.Combo->SelectedIndex() == 2 && L.Clicks == ClicksBefore,
          "a click outside closes the list without side effects");
    Check(HzIsActiveWindow(W.Handle()), "the owner window stays active");
}

void TestList(Live &L)
{
    Window &W = *L.W;
    ListView *List = L.List;
    Settle();
    W.InvalidateRect(List->Bounds());
    Settle();
    uint32_t Rows = List->RowsPaintedLastFrame();
    Check(Rows >= 4 && Rows <= 7, "only visible rows are painted (virtualization)");
    Point Middle = ScreenOf(L, List);
    SendMouse(Middle.X, Middle.Y, 0, -2);
    Wait(250);
    int32_t Offset = List->ScrollOffset();
    Check(Offset == 2 * 3 * List->ItemHeight / 2, "wheel scrolls the list smoothly to its target");
    Point Row = ScreenOf(L, List, 40, 36 + 18);
    ClickAt(Row);
    size_t Expected = (size_t)((36 + 18 + Offset) / List->ItemHeight);
    Check(List->SelectedIndex() == Expected && W.FocusedElement() == List, "clicking selects the row under the pointer");
    RightClickAt(Row);
    Check(List->SelectedIndex() == Expected && W.ActiveMenu() != nullptr, "right-click selects a list row and opens its context menu");
    Key(HZ_VK_ESCAPE, 0x1B, 0);
    CollectClosedWindows();
    Key(HZ_VK_DOWN, 0, 0);
    Check(List->SelectedIndex() == Expected + 1, "Down selects the next row");
    Key(HZ_VK_END, 0, 0);
    Wait(250);
    Check(List->SelectedIndex() == 999 && List->ScrollOffset() == 1000 * List->ItemHeight - List->ContentBounds().Height,
          "End selects and reveals the last row");
    Key(HZ_VK_HOME, 0, 0);
    Wait(250);
    Check(List->SelectedIndex() == 0 && List->ScrollOffset() == 0, "Home goes back to the first row");
}

void TestToolTip(Live &L)
{
    Point Tip = ScreenOf(L, L.Tipped);
    SendMouse(Tip.X - 2, Tip.Y, 0);
    SendMouse(Tip.X, Tip.Y, 0);
    Wait(900);
    HWND Under = WindowAt(Tip.X + 12, Tip.Y + 24 + 10);
    Check(Under != 0 && Under != L.W->Handle(), "hovering shows the tooltip popup");
    SendMouse(Tip.X, Tip.Y + 120, 0);
    Wait(50);
    CollectClosedWindows();
    Under = WindowAt(Tip.X + 12, Tip.Y + 24 + 10);
    Check(Under == L.W->Handle(), "moving away hides it");
}

void TestWindowStates(Live &L)
{
    Window &W = *L.W;
    HZ_DISPLAY_INFO Info;
    NtUserGetDisplayInfo(&Info);
    int32_t ListWidth = L.List->Bounds().Width;
    HzShowWindow(W.Handle(), HZ_SW_MAXIMIZE);
    Wait(50);
    Settle();
    Check(W.WindowSize() == Size{(int32_t)Info.Width, (int32_t)Info.Height} && W.ScreenOrigin() == Point{0, 0} &&
              L.List->Bounds().Width == ListWidth + (int32_t)Info.Width - 640,
          "maximizing lays the window out again");
    HzShowWindow(W.Handle(), HZ_SW_RESTORE);
    Wait(50);
    Check(W.WindowSize() == Size{640, 560} && W.ScreenOrigin() == Point{100, 60}, "restoring");

    bool Allow = false;
    int Closed = 0;
    W.OnCloseRequested = [&Allow]() { return Allow; };
    W.OnClosed = [&Closed]() { Closed++; };
    NtUserPostMessage(W.Handle(), HZ_WM_CLOSE, 0, 0);
    PumpMessages();
    Check(W.IsOpen() && Closed == 0, "a close request can be refused");
    Allow = true;
    NtUserPostMessage(W.Handle(), HZ_WM_CLOSE, 0, 0);
    PumpMessages();
    Check(!W.IsOpen() && Closed == 1, "closing runs OnClosed");
}

void TestTabsAndScrolling()
{
    Window W("Tabs", 420, 320);
    TabView *Tabs = new TabView();
    Tabs->Padding = Thickness(12);
    StackPanel *First = new StackPanel(Orientation::Vertical, 8);
    TextBox *Edit = First->Add<TextBox>("First page");
    Button *Go = First->Add<Button>("Go");
    Tabs->AddTab("One", First);
    ScrollViewer *Scroll = new ScrollViewer();
    StackPanel *Long = new StackPanel(Orientation::Vertical, 8);
    hz::Vector<Button *> Buttons;
    for (int i = 0; i < 30; i++)
        Buttons.Push(Long->Add<Button>(hz::String::Format("Button %d", i).CStr()));
    Scroll->SetContent(Long);
    Tabs->AddTab("Two", Scroll);
    int Changes = 0;
    Tabs->OnTabChanged = [&Changes](size_t) { Changes++; };
    W.SetContent(Tabs);
    Check(W.ShowAt(150, 120), "show a tab view window");
    Settle();

    Key(HZ_VK_TAB, '\t', 0);
    Check(W.FocusedElement() && W.FocusedElement()->Bounds() == Tabs->HeaderRect(0), "the selected tab header is a tab stop");
    Key(HZ_VK_TAB, '\t', 0);
    Key(HZ_VK_TAB, '\t', 0);
    Check(W.FocusedElement() == Go, "Tab goes from the tab strip into the page");
    Key(HZ_VK_TAB, '\t', 0);
    Check(W.FocusedElement() && W.FocusedElement()->Bounds() == Tabs->HeaderRect(0), "only one header is a tab stop");
    Edit->Focus();
    Key(HZ_VK_TAB, '\t', HZ_MOD_CTRL);
    Check(Tabs->SelectedTab() == 1 && Changes == 1 && !First->IsVisible() && Scroll->IsVisible(),
          "Ctrl+Tab switches tabs");
    Check(W.FocusedElement() && W.FocusedElement()->Bounds() == Tabs->HeaderRect(1),
          "focus hidden with its page moves to the tab strip");
    Key(HZ_VK_RIGHT, 0, 0);
    Check(Tabs->SelectedTab() == 0 && W.FocusedElement() && W.FocusedElement()->Bounds() == Tabs->HeaderRect(0),
          "arrows move between tabs");
    Key(HZ_VK_TAB, '\t', HZ_MOD_CTRL);
    PumpMessages();
    Key(HZ_VK_TAB, '\t', 0);
    Check(W.FocusedElement() == Buttons[0], "Tab enters the scrolled page");
    for (int i = 0; i < 12; i++)
        Key(HZ_VK_TAB, '\t', 0);
    Wait(300);
    Rect View = Scroll->ContentBounds();
    Rect Focused = Buttons[12]->Bounds();
    Check(W.FocusedElement() == Buttons[12] && Scroll->Offset() > 0 && Focused.Y >= View.Y && Focused.Bottom() <= View.Bottom(),
          "keyboard focus scrolls into view");
    int32_t Before = Scroll->Offset();
    Point O = W.ScreenOrigin();
    SendMouse(O.X + View.X + 40, O.Y + View.Y + 40, 0, 3);
    Wait(300);
    Check(Scroll->Offset() == Before - 3 * 64, "the wheel scrolls the viewer");
    Check(Scroll->Extent() == 30 * 32 + 29 * 8, "scroll extent");
    W.Close();
    PumpMessages();
}

/* ---- Editor, menu bar and dialogs (M9) ------------------------------------------------------- */

bool Listed(HWND Window)
{
    HWND Handles[64];
    uint32_t Count = 0;
    NtUserEnumWindows(Handles, 64, &Count);
    for (uint32_t i = 0; i < hz::Min<uint32_t>(Count, 64); i++)
        if (Handles[i] == Window)
            return true;
    return false;
}

void TestEditorLive()
{
    Window W("Editor", 520, 360);
    Grid *Root = new Grid();
    Root->Rows.Push(GridLength::AutoSize());
    Root->Rows.Push(GridLength::Stars(1));
    MenuBar *Menu = Root->Add<MenuBar>();
    Menu->HorizontalAlignment = Align::Start;
    int MenuChoice = 0, MenuBuilt = 0;
    Menu->AddMenu("File", [&MenuChoice, &MenuBuilt]() {
        MenuBuilt++;
        hz::Vector<MenuItem> Items;
        Items.Push(MenuItem::Make("First", [&MenuChoice]() { MenuChoice = 1; }));
        Items.Push(MenuItem::Make("Second", [&MenuChoice]() { MenuChoice = 2; }));
        return Items;
    });
    Menu->AddMenu("Edit", []() { return hz::Vector<MenuItem>(); });
    TextEditor *Ed = Root->Add<TextEditor>();
    Ed->GridRow = 1;
    int Changes = 0;
    Ed->OnChanged = [&Changes]() { Changes++; };
    W.SetContent(Root);
    Check(W.ShowAt(160, 100), "show an editor window");
    Settle();
    W.SetFocus(Ed);

    Type("int main");
    Key(HZ_VK_RETURN, '\r', 0);
    Type("    x");
    Key(HZ_VK_RETURN, '\r', 0);
    Type("y");
    Check(Ed->Text() == "int main\n    x\n    y" && Ed->LineCount() == 3, "editor: typing, Enter keeps the indentation");
    Check(Ed->Modified && Changes >= 5 && Ed->Caret() == (TextPosition{2, 5}), "editor: modified, caret at the end");
    Key('Z', 0, HZ_MOD_CTRL);
    Check(Ed->Text() == "int main\n    x\n    ", "editor: Ctrl+Z undoes the last word");
    Key('Z', 0, HZ_MOD_CTRL);
    Check(Ed->Text() == "int main\n    x", "editor: undo the line break");
    Key('Y', 0, HZ_MOD_CTRL);
    Check(Ed->Text() == "int main\n    x\n    " && Ed->CanRedo(), "editor: Ctrl+Y redoes");
    Key('Z', 0, HZ_MOD_CTRL | HZ_MOD_SHIFT);
    Check(Ed->Text() == "int main\n    x\n    y" && !Ed->CanRedo(), "editor: Ctrl+Shift+Z redoes");

    Key(HZ_VK_HOME, 0, HZ_MOD_CTRL);
    Key(HZ_VK_END, 0, HZ_MOD_SHIFT);
    Check(Ed->SelectedText() == "int main", "editor: Shift+End selects to the line end");
    Key('C', 0, HZ_MOD_CTRL);
    Check(Clipboard::GetText() == "int main", "editor: Ctrl+C copies the selection");
    Key(HZ_VK_END, 0, HZ_MOD_CTRL);
    Key('V', 0, HZ_MOD_CTRL);
    Check(Ed->Text() == "int main\n    x\n    yint main", "editor: Ctrl+V pastes at the caret");
    Key(HZ_VK_BACK, 0, HZ_MOD_CTRL);
    Check(Ed->Text() == "int main\n    x\n    yint ", "editor: Ctrl+Backspace deletes a word");
    Key('A', 0, HZ_MOD_CTRL);
    Check(Ed->SelectionStart() == (TextPosition{0, 0}) && Ed->SelectionEnd() == (TextPosition{2, 9}), "editor: Ctrl+A");
    Key(HZ_VK_DELETE, 0, 0);
    Check(Ed->Text() == "" && Ed->LineCount() == 1, "editor: Delete removes the selection");
    Key('Z', 0, HZ_MOD_CTRL);
    Check(Ed->Text() == "int main\n    x\n    yint ", "editor: undo restores it");

    /* Many lines: the wheel scrolls, a click places the caret. */
    hz::String Long;
    for (int i = 0; i < 200; i++)
        Long.Append(hz::String::Format("line %d\r\n", i));
    Ed->SetText(Long.CStr(), Long.Length());
    Check(Ed->LineCount() == 201 && !Ed->Modified && !Ed->CanUndo() && Ed->Line(5) == "line 5",
          "editor: SetText splits lines, drops carriage returns and resets undo");
    Settle();
    Point O = W.ScreenOrigin();
    const Rect &B = Ed->Bounds();
    SendMouse(O.X + B.X + 60, O.Y + B.Y + 60, 0, -2);
    Wait(100);
    Check(Ed->ScrollY() > 0, "editor: the wheel scrolls");
    int32_t Scrolled = Ed->ScrollY();
    Point Target = Ed->PointOf(TextPosition{10, 3}); /* lines 6 to about 19 are visible */
    ClickAt(Point{O.X + Target.X + 1, O.Y + Target.Y + 4});
    Check(Ed->Caret() == (TextPosition{10, 3}) && Ed->ScrollY() == Scrolled, "editor: a click places the caret");
    Check(Ed->Find("LINE 5", false), "editor: find ignores case");
    Check(Ed->Caret() == (TextPosition{5, 0}) && Ed->Anchor() == (TextPosition{5, 6}), "editor: find backwards");
    Check(Ed->Find("199") && Ed->SelectedText() == "199" && Ed->ScrollY() > Scrolled, "editor: find scrolls to the match");
    Check(!Ed->Find("missing"), "editor: find reports no match");
    Settle();

    /* The menu bar */
    ClickAt(Point{O.X + Menu->Bounds().X + 20, O.Y + Menu->Bounds().Y + 16});
    Check(MenuBuilt == 1 && W.ActiveMenu() != nullptr, "menu bar: a click opens the menu");
    Key(HZ_VK_DOWN, 0, 0);
    Key(HZ_VK_DOWN, 0, 0);
    Key(HZ_VK_RETURN, '\r', 0);
    PumpMessages();
    Check(MenuChoice == 2 && W.ActiveMenu() == nullptr, "menu bar: keys choose an item");
    W.Close();
    PumpMessages();
    CollectClosedWindows();
}

void TestDialogs()
{
    Window W("Owner", 600, 420);
    StackPanel *Root = new StackPanel(Orientation::Vertical, 8);
    Root->Padding = Thickness(16);
    TextBox *Edit = Root->Add<TextBox>("Owner text");
    int OwnerKeys = 0;
    W.OnUnhandledKey = [&OwnerKeys](KeyEvent &E) {
        if (E.Key == HZ_VK_F1)
            OwnerKeys++;
    };
    Edit->SetText("C:\\Nexora\\System", false); /* before any layout */
    W.SetContent(Root);
    Check(W.ShowAt(120, 90), "show the owner window");
    Settle();
    Check(Edit->OffsetToX(0) >= Edit->Bounds().X && Edit->OffsetToX(0) < Edit->Bounds().X + 20,
          "text set before the first layout shows from its start");
    Edit->SetText("", false);
    W.SetFocus(Edit);
    Check(Listed(W.Handle()), "the owner is an app window");

    /* Message box: Enter answers the first button. */
    int Answer = -5;
    MessageBox(&W, "Question", "Save the changes?", "Save|Don't save|Cancel", [&Answer](int Choice) { Answer = Choice; });
    Settle();
    HWND Active = 0;
    HZ_WINDOW_INFO Info;
    HWND Handles[64];
    uint32_t Count = 0;
    NtUserEnumWindows(Handles, 64, &Count);
    bool DialogListed = false;
    for (uint32_t i = 0; i < hz::Min<uint32_t>(Count, 64); i++) {
        if (HZ_SUCCESS(NtUserGetWindowInfo(Handles[i], &Info)) && strcmp(Info.Title, "Question") == 0)
            DialogListed = true;
    }
    Check(!DialogListed, "an owned dialog is not on the window list");
    Check(!Root->IsEnabled(), "the owner's content is disabled while the dialog is open");
    Key(HZ_VK_F1, 0, 0); /* goes to the active dialog */
    Check(OwnerKeys == 0, "the owner gets no keys meanwhile");
    Key(HZ_VK_RETURN, '\r', 0);
    Settle();
    Check(Answer == 0, "message box: Enter picks the first button");
    Check(Root->IsEnabled() && W.FocusedElement() == Edit && W.IsActive(), "the owner is enabled and active again");
    (void)Active;

    Answer = -5;
    MessageBox(&W, "Question", "Delete it?", "Delete|Cancel", [&Answer](int Choice) { Answer = Choice; });
    Settle();
    Key(HZ_VK_ESCAPE, 0, 0);
    Settle();
    Check(Answer == 1, "message box: Escape picks the last button");

    Answer = -5;
    MessageBox(&W, "Note", "Done.", nullptr, [&Answer](int Choice) { Answer = Choice; });
    Settle();
    HWND Note = 0;
    NtUserWindowFromPoint(W.ScreenOrigin().X + 300, W.ScreenOrigin().Y + 200, &Note);
    Check(Note != 0 && Note != W.Handle(), "message box: centred over its owner");
    NtUserPostMessage(Note, HZ_WM_CLOSE, 0, 0);
    Wait(50);
    Settle();
    Check(Answer == -1, "message box: closing it answers -1");

    /* Input box */
    hz::String Typed("(none)");
    InputBox(&W, "Rename", "New name", "report.txt", "Rename", [&Typed](const hz::String &Text) { Typed = Text; });
    Settle();
    Type("notes"); /* replaces the selected "report" */
    Key(HZ_VK_RETURN, '\r', 0);
    Settle();
    Check(Typed == "notes.txt", "input box: the name is selected without its extension");
    Typed = "(none)";
    InputBox(&W, "Rename", "New name", "x", nullptr, [&Typed](const hz::String &Text) { Typed = Text; });
    Settle();
    Key(HZ_VK_ESCAPE, 0, 0);
    Settle();
    Check(Typed == "(none)", "input box: Escape cancels");

    /* File dialog on the system volume */
    hz::String Chosen("(none)");
    ShowFileDialog(&W, FileDialogMode::Open, "C:\\Nexora", nullptr, [&Chosen](const hz::String &Path) { Chosen = Path; });
    Settle();
    Type("Apps");
    Key(HZ_VK_RETURN, '\r', 0); /* a folder name navigates */
    Settle();
    Type("notepad.app");
    Key(HZ_VK_RETURN, '\r', 0);
    Settle();
    Check(Chosen == "C:\\Nexora\\Apps\\notepad.app", "file dialog: navigate into a folder and choose a file");
    Chosen = "(none)";
    ShowFileDialog(&W, FileDialogMode::Open, "C:\\Nexora", nullptr, [&Chosen](const hz::String &Path) { Chosen = Path; });
    Settle();
    Key(HZ_VK_ESCAPE, 0, 0);
    Settle();
    Check(Chosen == "", "file dialog: Escape cancels with an empty path");
    Check(Root->IsEnabled() && W.IsActive(), "the owner is usable after the dialogs");
    Key(HZ_VK_F1, 0, 0);
    Check(OwnerKeys == 1, "the owner gets its keys back");
    W.Close();
    PumpMessages();
    CollectClosedWindows();
}

void TestLive()
{
    HZ_DISPLAY_INFO Info;
    memset(&Info, 0, sizeof(Info));
    NtUserGetDisplayInfo(&Info);
    if (Info.Width < 800 || Info.Height < 640) {
        HzPrint("uitest: display too small for the live tests");
        return;
    }
    Live L;
    BuildLive(L);
    Check(L.W->ShowAt(100, 60), "show a window");
    Settle();
    Check(L.W->PaintCount() > 0 && L.Edit->Bounds() == Rect{16, 32 + 16, 300, 32}, "first paint and layout");
    TestFocusAndText(L);
    TestControls(L);
    TestComboAndMenus(L);
    TestList(L);
    TestToolTip(L);
    TestWindowStates(L);
    delete L.W;
    CollectClosedWindows();
    TestTabsAndScrolling();
    TestEditorLive();
    TestDialogs();
}

}  // namespace

extern "C" int main(int argc, char **argv)
{
    HZ_POINT Cursor = {};
    NtUserGetCursorPos(&Cursor);
    TestCore();
    TestTypography();
    TestLayout();
    TestPainting();
    TestTransitions();
    TestLive();
    SendMouse(Cursor.X, Cursor.Y, 0); /* leave the cursor where it was */
    HzPrint("uitest: %d checks, %d failed", g_Checks, g_Failures);
    return g_Failures;
}
