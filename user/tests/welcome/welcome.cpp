/*
 * Nexora OS - user/tests/welcome/welcome.cpp
 * The welcome program, started by the shell after unlocking (and from Start), built with hzui: a
 * Mica window whose tabs show the toolkit (a text box that echoes what is typed, every control, a
 * filtered list of 500 items, an about page). Closing the window ends the program. Typed text, clearing and window moves are logged (the
 * "desktop" smoke test drives them through the real PS/2 devices).
 */
#include "hzui.h"
#include "hzversion.h"

using namespace hzui;

namespace {

/* ---- Helpers ------------------------------------------------------------------------------------- */

Element *Card(Element *Content)
{
    Element *Box = new Element();
    Box->Background = Theme::CardBackground;
    Box->BorderColor = Theme::CardStroke;
    Box->CornerRadius = Theme::OverlayRadius;
    Box->Padding = Thickness(16);
    Box->AddChild(Content);
    return Box;
}

TextBlock *Label(Element *Parent, const char *Text, FontRole Role = FontRole::Body, Color C = Theme::TextPrimary)
{
    TextBlock *T = Parent->Add<TextBlock>(Text, Role);
    T->Foreground = C;
    return T;
}

TextBlock *Paragraph(Element *Parent, const char *Text, Color C = Theme::TextSecondary)
{
    TextBlock *T = Label(Parent, Text, FontRole::Body, C);
    T->Wrap = true;
    return T;
}

StackPanel *Section(StackPanel *Page, const char *Title)
{
    Label(Page, Title, FontRole::BodyStrong);
    StackPanel *Body = new StackPanel(Orientation::Vertical, 8);
    Page->AddChild(Card(Body));
    return Body;
}

/* ---- The welcome window's tabs ------------------------------------------------------------------- */

Element *BuildStart(TextBox **Edit)
{
    StackPanel *Page = new StackPanel(Orientation::Vertical, 12);

    StackPanel *Echo = new StackPanel(Orientation::Vertical, 8);
    Label(Echo, "Type something", FontRole::BodyStrong);
    Grid *Row = Echo->Add<Grid>();
    Row->Columns.Push(GridLength::Stars(1));
    Row->Columns.Push(GridLength::AutoSize());
    Row->ColumnSpacing = 8;
    TextBox *Box = Row->Add<TextBox>("Your keystrokes appear here, Enter confirms");
    Button *Clear = Row->Add<Button>("Clear");
    Clear->GridColumn = 1;
    TextBlock *Result = Label(Echo, "Nothing confirmed yet.", FontRole::Caption, Theme::TextSecondary);
    Box->OnSubmit = [Result](const hz::String &Text) {
        HzPrint("welcome: you typed \"%s\"", Text.CStr());
        Result->SetText(hz::String::Format("You typed \"%s\".", Text.CStr()).CStr());
    };
    Clear->OnClick = [Box, Result]() {
        Box->SetText("");
        Result->SetText("Cleared.");
        HzPrint("welcome: cleared");
    };
    Page->AddChild(Card(Echo));

    StackPanel *News = new StackPanel(Orientation::Vertical, 6);
    Label(News, "New in this milestone", FontRole::BodyStrong);
    Paragraph(News, "Nexora OS installs itself: on the live system, \"Install Nexora OS\" in Start partitions a disk, "
                    "copies the system and its boot loader, and the computer then starts from the disk. Settings, "
                    "your name and your documents are kept between restarts.");
    Paragraph(News, "Programs: Notepad, Files, Terminal, Calculator, Settings and Task Manager, all in Start "
                    "(press the logo key). Press Tab to move between controls and Ctrl+Tab to switch tabs.");
    Page->AddChild(Card(News));
    *Edit = Box;
    return Page;
}

Element *BuildControls(Window *Owner)
{
    ScrollViewer *Scroll = new ScrollViewer();
    StackPanel *Page = new StackPanel(Orientation::Vertical, 8);
    Page->Padding = Thickness(0, 0, 16, 16);
    Scroll->SetContent(Page);

    StackPanel *Buttons = Section(Page, "Buttons");
    StackPanel *Row = Buttons->Add<StackPanel>(Orientation::Horizontal, 8);
    Row->Add<Button>("Accent button", ButtonStyle::Accent)->ToolTip = "The main action of a page";
    Row->Add<Button>("Standard")->ToolTip = "Any other action";
    Button *Subtle = Row->Add<Button>("Subtle", ButtonStyle::Subtle);
    Subtle->Glyph = Icon::Star;
    Row->Add<Button>("Disabled")->SetEnabled(false);
    Button *More = Row->Add<Button>("", ButtonStyle::Subtle);
    More->Glyph = Icon::More;
    More->ToolTip = "More options";
    More->OnClick = [Owner, More]() {
        hz::Vector<MenuItem> Items;
        Items.Push(MenuItem::Make("New window", []() { HzPrint("welcome: menu new window"); }, "Ctrl+N"));
        MenuItem Settings = MenuItem::Make("Settings", []() { HzPrint("welcome: menu settings"); });
        Settings.Glyph = Icon::Settings;
        Items.Push(hz::Move(Settings));
        Items.Push(MenuItem::MakeSeparator());
        Items.Push(MenuItem::Make("About Nexora OS", []() { HzPrint("welcome: menu about"); }));
        Point O = Owner->ScreenOrigin();
        ShowMenu(Owner, Point{O.X + More->Bounds().X, O.Y + More->Bounds().Bottom() + 4}, hz::Move(Items));
    };

    StackPanel *Toggles = Section(Page, "Switches and options");
    Grid *Pairs = Toggles->Add<Grid>();
    Pairs->Columns.Push(GridLength::Stars(1));
    Pairs->Columns.Push(GridLength::Stars(1));
    StackPanel *Left = Pairs->Add<StackPanel>(Orientation::Vertical, 4);
    Label(Left, "Night light", FontRole::Body, Theme::TextSecondary);
    ToggleSwitch *Night = Left->Add<ToggleSwitch>();
    Night->OnToggled = [](bool On) { HzPrint("welcome: night light %s", On ? "on" : "off"); };
    Label(Left, "Notifications", FontRole::Body, Theme::TextSecondary);
    Left->Add<ToggleSwitch>()->SetOn(true, false);
    StackPanel *Right = Pairs->Add<StackPanel>(Orientation::Vertical, 0);
    Right->GridColumn = 1;
    Right->Add<CheckBox>("Show hidden files");
    Right->Add<CheckBox>("Round window corners", true);
    Right->Add<CheckBox>("Unavailable option")->SetEnabled(false);

    StackPanel *Choices = Section(Page, "Choices");
    Grid *ChoiceGrid = Choices->Add<Grid>();
    ChoiceGrid->Columns.Push(GridLength::Stars(1));
    ChoiceGrid->Columns.Push(GridLength::Stars(1));
    StackPanel *Radios = ChoiceGrid->Add<StackPanel>(Orientation::Vertical, 0);
    Radios->Add<RadioButton>("Balanced", "power", true);
    Radios->Add<RadioButton>("Best performance", "power");
    Radios->Add<RadioButton>("Battery saver", "power");
    StackPanel *ComboColumn = ChoiceGrid->Add<StackPanel>(Orientation::Vertical, 4);
    ComboColumn->GridColumn = 1;
    Label(ComboColumn, "Accent colour", FontRole::Body, Theme::TextSecondary);
    ComboBox *Combo = ComboColumn->Add<ComboBox>();
    Combo->Items.Push("Nexora orange");
    Combo->Items.Push("Twilight violet");
    Combo->Items.Push("Ocean blue");
    Combo->Items.Push("Forest green");
    Combo->SetSelectedIndex(0, false);
    Combo->HorizontalAlignment = Align::Start;
    Combo->Width = 200;

    StackPanel *Values = Section(Page, "Values");
    TextBlock *VolumeLabel = Label(Values, "Volume: 40", FontRole::Body, Theme::TextSecondary);
    Slider *Volume = Values->Add<Slider>(0, 100, 40);
    Volume->Width = 280;
    Volume->HorizontalAlignment = Align::Start;
    ProgressBar *Progress = Values->Add<ProgressBar>(40);
    Progress->Width = 280;
    Progress->HorizontalAlignment = Align::Start;
    Volume->OnValueChanged = [VolumeLabel, Progress](float V) {
        VolumeLabel->SetText(hz::String::Format("Volume: %d", (int)V).CStr());
        Progress->SetValue(V);
    };
    Label(Values, "Working on it", FontRole::Body, Theme::TextSecondary);
    ProgressBar *Busy = Values->Add<ProgressBar>();
    Busy->Width = 280;
    Busy->HorizontalAlignment = Align::Start;
    Busy->SetIndeterminate(true);
    return Scroll;
}

const char *const kAdjectives[] = {"Amber", "Bright", "Calm", "Distant", "Early", "Gentle", "Hidden", "Ivory",
                                   "Jade", "Lunar", "Misty", "Northern", "Quiet", "Rosy", "Silver", "Tidal",
                                   "Velvet", "Warm", "Young", "Zephyr"};
const char *const kNouns[] = {"harbour", "meadow", "summit", "valley", "lagoon", "canyon", "orchard", "glacier",
                              "island", "forest", "dune", "river", "beacon", "garden", "plateau"};

struct ListPage {
    hz::Vector<hz::String> All;
    hz::Vector<size_t> Shown;
    ListView *List;
    TextBlock *Count;

    void Filter(const char *Text)
    {
        Shown.Clear();
        for (size_t i = 0; i < All.Size(); i++)
            if (All[i].ContainsIgnoreCase(Text))
                Shown.Push(i);
        List->SetItemCount(Shown.Size());
        Count->SetText(hz::String::Format("%u of %u places", (unsigned)Shown.Size(), (unsigned)All.Size()).CStr());
    }
};

ListPage g_List;

Element *BuildList()
{
    for (unsigned i = 0; i < 500; i++)
        g_List.All.Push(hz::String::Format("%s %s %03u", kAdjectives[i % 20], kNouns[(i / 3) % 15], i + 1));
    Grid *Page = new Grid();
    Page->Rows.Push(GridLength::AutoSize());
    Page->Rows.Push(GridLength::AutoSize());
    Page->Rows.Push(GridLength::Stars(1));
    Page->RowSpacing = 8;
    Page->Padding = Thickness(0, 0, 0, 16);
    TextBox *Search = Page->Add<TextBox>("Search places");
    Search->HorizontalAlignment = Align::Start;
    Search->Width = 320;
    g_List.Count = Page->Add<TextBlock>("", FontRole::Caption);
    g_List.Count->Foreground = Theme::TextSecondary;
    g_List.Count->GridRow = 1;
    ListView *List = Page->Add<ListView>();
    List->GridRow = 2;
    List->Background = Theme::CardBackground;
    List->BorderColor = Theme::CardStroke;
    List->CornerRadius = Theme::OverlayRadius;
    List->Padding = Thickness(0, 4);
    List->PaintItem = [List](Painter &P, const Rect &R, size_t Row) {
        const hz::String &Name = g_List.All[g_List.Shown[Row]];
        P.DrawIcon(Icon::Home, Rect{R.X, R.Y + (R.Height - 16) / 2, 16, 16}, Theme::TextSecondary);
        P.TextLine(GetFont(FontRole::Body), Rect{R.X + 28, R.Y, R.Width - 28, R.Height}, Name.CStr(),
                   List->IsEnabled() ? Theme::TextPrimary : Theme::TextDisabled);
    };
    List->OnItemInvoked = [](size_t Row) { HzPrint("welcome: opened %s", g_List.All[g_List.Shown[Row]].CStr()); };
    g_List.List = List;
    Search->OnTextChanged = [](const hz::String &Text) { g_List.Filter(Text.CStr()); };
    g_List.Filter("");
    return Page;
}

Element *BuildAbout(Window *Owner)
{
    StackPanel *Page = new StackPanel(Orientation::Vertical, 12);
    StackPanel *Body = new StackPanel(Orientation::Vertical, 8);
    Label(Body, "Nexora OS " HZ_VERSION_STRING " (" HZ_MILESTONE ")", FontRole::Subtitle);
    Paragraph(Body, "An original hobby operating system for x86-64 PCs. Its kernel is layered like NT: a HAL, "
                    "the kernel and executive, an I/O manager with drivers and file systems, and win32k, which "
                    "owns windows, input and composition. Applications draw their own content with hzui.");
    Paragraph(Body, "Interface text is set in Inter, under the SIL Open Font License. The licenses of every "
                    "third-party component are in C:\\Nexora\\Licenses.");
    Button *Close = Body->Add<Button>("Close this window");
    Close->HorizontalAlignment = Align::Start;
    Close->OnClick = [Owner]() { Owner->Close(); };
    Page->AddChild(Card(Body));
    return Page;
}

}  // namespace

extern "C" int main(int argc, char **argv)
{
    HZ_DISPLAY_INFO Display;
    if (!HZ_SUCCESS(NtUserGetDisplayInfo(&Display)) || Display.Width < 800 || Display.Height < 600) {
        HzPrint("welcome: no usable display");
        return 1;
    }

    Window Welcome("Welcome", 760, 520);
    Welcome.SetMinimumSize(560, 420);
    Grid *Root = new Grid();
    Root->Rows.Push(GridLength::AutoSize());
    Root->Rows.Push(GridLength::Stars(1));
    Root->Padding = Thickness(24, 12, 24, 0);
    Root->RowSpacing = 12;
    StackPanel *Header = Root->Add<StackPanel>(Orientation::Vertical, 4);
    Label(Header, "Welcome to Nexora OS", FontRole::Title);
    Paragraph(Header, "An original hobby operating system with an NT-style kernel, drawn by its own compositor.");
    TabView *Tabs = Root->Add<TabView>();
    Tabs->GridRow = 1;
    TextBox *Edit = nullptr;
    Tabs->AddTab("Start", BuildStart(&Edit));
    Tabs->AddTab("Controls", BuildControls(&Welcome));
    Tabs->AddTab("List", BuildList());
    Tabs->AddTab("About", BuildAbout(&Welcome));
    Tabs->OnTabChanged = [](size_t Index) { HzPrint("welcome: tab %u", (unsigned)Index); };
    Welcome.SetContent(Root);
    Welcome.SetFocus(Edit);
    Welcome.OnClosed = []() { NtUserPostQuitMessage(0); };
    Welcome.OnMoved = [](int32_t X, int32_t Y) { HzPrint("welcome: moved to (%d, %d)", X, Y); };

    if (!Welcome.Show()) {
        HzPrint("welcome: cannot create the window");
        return 1;
    }
    HzPrint("welcome: desktop windows created");
    return RunMessageLoop();
}
