/*
 * Nexora OS - user/tests/apptest/apptest.cpp
 * Application self-test, run by the kernel at every boot after uitest (win32k/w32test.cpp):
 *   - hz::fs paths (resolve, parent, name, extension), wildcards and formatting;
 *   - program manifests: parsing (valid and broken ones) and the installed set on C:;
 *   - the calculator engine (key sequences, errors, number formatting and parsing);
 *   - the terminal's command-line parser (quotes, redirection, errors);
 *   - the text editor's document model (lines, tabs, multi-line edits, undo, find, UTF-8);
 *   - (M10) Setup's builders: CRC-32, GUIDs, the GPT layout and sectors, FAT32 geometry and boot
 *     sectors, the boot configuration, user names; settings files.
 *   - (M11) '| more' parsing, editor word wrap, file trees (copy, move, delete) on a writable
 *     volume when there is one; SHA-256 test vectors and the password hash.
 * The engine, parser and installer sources are compiled in from the applications' folders. The
 * exit code is the number of failed checks.
 */
#include "../../apps/calc/engine.cpp"
#include "../../apps/browser/html.cpp"
#include "../../apps/browser/url.cpp"
#include "../../apps/setup/installer.cpp"
#include "../../apps/terminal/parse.cpp"
#include "hzapps.h"
#include "hzcrypto.h"
#include "hzfile.h"
#include "hzui.h"

namespace {

int g_Checks;
int g_Failures;

void Check(bool Ok, const char *What)
{
    g_Checks++;
    if (!Ok) {
        g_Failures++;
        HzPrint("apptest: FAIL %s", What);
    }
}

void CheckText(const hz::String &Got, const char *Expected, const char *What)
{
    g_Checks++;
    if (Got != Expected) {
        g_Failures++;
        HzPrint("apptest: FAIL %s: got \"%s\", expected \"%s\"", What, Got.CStr(), Expected);
    }
}

/* ---- Paths and formatting ----------------------------------------------------------------------- */

void TestPaths()
{
    using namespace hz::fs;
    hz::String Notes("D:\\Notes");
    CheckText(Resolve(Notes, "..\\x"), "D:\\x", "resolve ..");
    CheckText(Resolve(Notes, "\\a\\b"), "D:\\a\\b", "resolve from the root");
    CheckText(Resolve(hz::String("C:\\"), "d:x"), "D:\\x", "resolve another drive");
    CheckText(Resolve(hz::String("D:\\a"), "D:b"), "D:\\a\\b", "resolve drive-relative");
    CheckText(Resolve(hz::String("C:\\"), "a/./b/../c"), "C:\\a\\c", "resolve slashes, . and ..");
    CheckText(Resolve(hz::String("C:\\a"), "..\\..\\.."), "C:\\", "resolve stops at the root");
    CheckText(Resolve(Notes, "E:\\"), "E:\\", "resolve a root");
    CheckText(Parent(hz::String("D:\\a\\b")), "D:\\a", "parent");
    CheckText(Parent(hz::String("D:\\a")), "D:\\", "parent of a top folder");
    CheckText(Parent(hz::String("D:\\")), "D:\\", "parent of a root");
    CheckText(FileName(hz::String("D:\\a\\b.txt")), "b.txt", "file name");
    CheckText(FileName(hz::String("D:\\a\\")), "a", "file name with a trailing separator");
    CheckText(Extension(hz::String("D:\\x.TXT")), "txt", "extension in lower case");
    CheckText(Extension(hz::String("D:\\.profile")), "", "a leading dot is not an extension");
    CheckText(Extension(hz::String("a.b.c")), "c", "last extension");
    CheckText(Join(hz::String("D:\\"), "x"), "D:\\x", "join at a root");
    CheckText(Join(hz::String("D:\\a"), "x"), "D:\\a\\x", "join");
    Check(IsRoot(hz::String("C:\\")) && !IsRoot(hz::String("C:\\a")), "is root");
    Check(EqualNames("ReadMe.TXT", "readme.txt") && !EqualNames("a", "ab"), "names compare without case");

    Check(MatchPattern("*.txt", "README.TXT"), "pattern *.txt");
    Check(MatchPattern("h?llo.*", "hello.c"), "pattern with ?");
    Check(MatchPattern("*a*b", "xaxb"), "pattern with two stars");
    Check(!MatchPattern("*.txt", "a.txt.bak"), "pattern anchored at the end");
    Check(MatchPattern("*", "") && !MatchPattern("?", ""), "empty names");
    Check(HasWildcards("a*") && !HasWildcards("abc"), "wildcard detection");

    CheckText(FormatSize(0), "0 bytes", "size 0");
    CheckText(FormatSize(1023), "1023 bytes", "size 1023");
    CheckText(FormatSize(1536), "1.5 KB", "size 1.5 KB");
    CheckText(FormatSize(10ull * 1024 * 1024), "10.0 MB", "size 10 MB");
    CheckText(FormatSize(3ull * 512 * 1024 * 1024), "1.5 GB", "size 1.5 GB");
    CheckText(FormatTime(HZ_TIME_UNIX_EPOCH), "1970-01-01 00:00", "time at the Unix epoch");
    CheckText(FormatTime(HZ_TIME_UNIX_EPOCH + 951868800ull * HZ_TIME_UNITS_PER_SECOND + 3600ull * 13 * HZ_TIME_UNITS_PER_SECOND),
              "2000-03-01 13:00", "time after a leap day");
    CheckText(hz::String(StatusText(STATUS_OBJECT_NAME_NOT_FOUND)), "The file was not found", "status text");

    /* Local time (M11) */
    CivilTime T = ToCivil(HZ_TIME_UNIX_EPOCH);
    Check(T.Year == 1970 && T.Month == 1 && T.Day == 1 && T.Hour == 0 && T.Weekday == 4, "civil time at the Unix epoch");
    T = ToCivil(HZ_TIME_UNIX_EPOCH, -60);
    Check(T.Year == 1969 && T.Month == 12 && T.Day == 31 && T.Hour == 23 && T.Weekday == 3, "civil time before the epoch");
    T = ToCivil(HZ_TIME_UNIX_EPOCH + (951868800ull + 13 * 3600 + 59) * HZ_TIME_UNITS_PER_SECOND, 330);
    Check(T.Year == 2000 && T.Month == 3 && T.Day == 1 && T.Hour == 18 && T.Minute == 30 && T.Second == 59 && T.Weekday == 3,
          "civil time with UTC+05:30");
    CheckText(FormatUtcOffset(0), "UTC", "offset UTC");
    CheckText(FormatUtcOffset(330), "UTC+05:30", "offset +05:30");
    CheckText(FormatUtcOffset(-180), "UTC-03:00", "offset -03:00");
    Check(ClampUtcOffset(-9999) == kMinUtcOffset && ClampUtcOffset(9999) == kMaxUtcOffset && ClampUtcOffset(60) == 60,
          "offsets are clamped");
}

/* ---- Manifests ---------------------------------------------------------------------------------- */

void TestManifests()
{
    using namespace hz::apps;
    const char *Good = "# comment\r\nName = Text Editor \r\n description=Edits text\nIcon=Pencil\nColor=#3a6fd8\n"
                       "Path=C:\\Nexora\\System\\notepad.exe\nOpens= .TXT, md ,log\nPinned=yes\nOrder=7\nFuture=1\n";
    App A;
    hz::String Error;
    Check(ParseManifest(Good, strlen(Good), A, &Error), "a valid manifest parses");
    Check(A.Name == "Text Editor" && A.Description == "Edits text" && A.Icon == "Pencil" && A.Color == 0xFF3A6FD8 &&
              A.Path == "C:\\Nexora\\System\\notepad.exe" && A.Pinned && A.Order == 7,
          "manifest fields (trimmed, keys without case, CRLF)");
    Check(A.Opens.Size() == 3 && A.Opens[0] == "txt" && A.Opens[1] == "md" && A.Opens[2] == "log",
          "manifest extensions are lower case without dots");
    const char *NoPath = "Name=X\n";
    Check(!ParseManifest(NoPath, strlen(NoPath), A, &Error) && Error == "Path is missing", "a manifest needs a path");
    const char *BadColor = "Name=X\nPath=C:\\x.exe\nColor=12345\n";
    Check(!ParseManifest(BadColor, strlen(BadColor), A, &Error) && Error.Find("Color") != hz::NotFound,
          "a bad colour is reported");
    const char *NoEquals = "Name=X\nPath=C:\\x.exe\njunk\n";
    Check(!ParseManifest(NoEquals, strlen(NoEquals), A, &Error) && Error.StartsWith("line 3"), "a line without '=' is reported");
    const char *BadOrder = "Name=X\nPath=C:\\x.exe\nOrder=ten\n";
    Check(!ParseManifest(BadOrder, strlen(BadOrder), A, &Error), "a bad order is reported");

    hz::Vector<App> Apps = Load();
    bool Live = hz::fs::IsLiveSystem(); /* Setup is pinned on the live image only */
    Check(Apps.Size() >= 10, "the app catalog contains the baseline programs");
    Check(FindByImage(Apps, "calendar.exe") && FindByImage(Apps, "stopwatch.exe") &&
              FindByImage(Apps, "sysinfo.exe") && FindByImage(Apps, "paint.exe") &&
              FindByImage(Apps, "browser.exe"),
          "new built-in apps are registered");
    Check(Apps.Size() && Apps[0].Id == "setup" && Apps[0].Pinned == Live, "Setup is pinned on live systems only");
    bool Ordered = true, Present = true;
    for (size_t i = 0; i < Apps.Size(); i++) {
        if (i && Apps[i - 1].Order > Apps[i].Order)
            Ordered = false;
        if (!hz::fs::Exists(Apps[i].Path)) {
            Present = false;
            HzPrint("apptest: %s is missing", Apps[i].Path.CStr());
        }
    }
    Check(Ordered && Apps.Size() > 1 && Apps[1].Id == "notepad", "programs are ordered");
    Check(Present, "every manifest names an existing program");
    const App *Text = FindForExtension(Apps, hz::String("txt"));
    Check(Text && Text->Id == "notepad", "text files open in Notepad");
    Check(!FindForExtension(Apps, hz::String("exe")) && !FindForExtension(Apps, hz::String()), "nothing claims .exe");
    const App *Calc = FindByImage(Apps, "C:\\Nexora\\System\\CALC.EXE");
    Check(Calc && Calc->Name == "Calculator", "find a program by its image");
    Check(OpenPath(hz::String("C:\\Nexora\\Fonts\\Inter-Regular.ttf")) == STATUS_NOT_FOUND, "no program opens fonts");
}

/* ---- Browser URL parsing ------------------------------------------------------------------------- */

void TestBrowserUrls()
{
    using namespace nexora::browser;
    Url Address;
    Check(ParseUrl("https://example.com/", &Address) == UrlError::None && Address.valid &&
              Address.scheme == Scheme::Https && IsSecure(Address) && Address.port == 443 &&
              !strcmp(Address.host, "example.com") && !strcmp(Address.path, "/"),
          "browser parses a secure URL");
    Check(ParseUrl("http://example.com:8080/a?q=1", &Address) == UrlError::None && Address.port == 8080 &&
              Address.explicitPort && !strcmp(Address.path, "/a?q=1"),
          "browser parses a URL with port and query");
    Check(ParseUrl("https://example.com?q=1", &Address) == UrlError::None && !strcmp(Address.path, "/?q=1"),
          "browser adds a slash before a query");
    Check(ParseUrl("file:///C:/Users/Ada/page.html", &Address) == UrlError::None &&
              IsLocalFile(Address) && !IsSecure(Address) && !strcmp(Address.path, "C:\\Users\\Ada\\page.html"),
          "browser parses an absolute local file URL");
    Check(ParseUrl("file:///C:/Users/A%20B/page.html", &Address) == UrlError::None &&
              !strcmp(Address.path, "C:\\Users\\A%20B\\page.html"),
          "browser preserves percent-escaped local path bytes for decoding");
    Check(ParseUrl("https://user@example.com/", &Address) == UrlError::InvalidAuthority &&
              ParseUrl("https://example.com:99999/", &Address) == UrlError::InvalidPort &&
              ParseUrl("https://example.com:999999999999999999999999/", &Address) == UrlError::InvalidPort &&
              ParseUrl("file:///etc/passwd", &Address) == UrlError::InvalidAuthority &&
              ParseUrl("file://server/share/page.html", &Address) == UrlError::UnsupportedScheme &&
              ParseUrl("file:///C:/private page.html", &Address) == UrlError::InvalidCharacter,
          "browser rejects unsafe or malformed URL forms");
}

void TestBrowserHtml()
{
    using namespace nexora::browser;
    const char Markup[] =
        "<!doctype html><html><head><title> Nexora   Browser </title></head><body>"
        "<h1>Welcome &amp; enjoy</h1><p>Hello <a href=\"http://example.com/\">world &lt;web&gt;</a>.</p>"
        "<script>secret()</script><style>h1 { color: red; }</style></body></html>";
    HtmlDocument Document;
    Check(ParseHtml(Markup, sizeof(Markup) - 1, &Document), "browser HTML parser accepts a bounded document");
    Check(!strcmp(Document.Title, "Nexora Browser") && !strcmp(Document.Text, "Welcome & enjoy\nHello world <web>."),
          "browser HTML parser extracts title, block text and basic entities");
    Check(Document.LinkCount == 1 && !strcmp(Document.Links[0].Text, "world <web>") &&
              !strcmp(Document.Links[0].Href, "http://example.com/"),
          "browser HTML parser captures link labels and destinations");
    Check(Document.UnsupportedContent && !strcmp(Document.Text, "Welcome & enjoy\nHello world <web>."),
          "browser HTML parser omits script and stylesheet content and reports unsupported features");

    const char Styled[] = "<p style='color:red'>Text &unknown;</p>";
    Check(ParseHtml(Styled, sizeof(Styled) - 1, &Document) && Document.UnsupportedContent &&
              !strcmp(Document.Text, "Text &unknown;"),
          "browser HTML parser flags inline style and preserves unknown entities");
    Check(ParseHtml("<p>unfinished", 13, &Document) && !strcmp(Document.Text, "unfinished"),
          "browser HTML parser tolerates an unclosed element");
    Check(!ParseHtml(nullptr, 0, &Document) && !ParseHtml("x", HtmlMaximumSource + 1, &Document),
          "browser HTML parser rejects invalid and oversized input");

    char LongText[HtmlMaximumText + 20];
    for (size_t i = 0; i < sizeof(LongText); ++i)
        LongText[i] = 'x';
    Check(ParseHtml(LongText, sizeof(LongText), &Document) && Document.Truncated &&
              Document.TextLength == HtmlMaximumText - 1 && Document.Text[Document.TextLength] == 0,
          "browser HTML parser bounds output and reports truncation");
}

/* ---- Calculator --------------------------------------------------------------------------------- */

hz::String Run(const char *Keys, hz::String *Expression = nullptr)
{
    calc::Engine E;
    for (const char *K = Keys; *K; K++)
        E.Key(*K);
    if (Expression)
        *Expression = E.Expression();
    return E.Display();
}

void TestCalculator()
{
    hz::String Expression;
    CheckText(Run("2+3=", &Expression), "5", "calc: 2+3=");
    CheckText(Expression, "2 + 3 =", "calc: the expression line");
    CheckText(Run("2+3=="), "8", "calc: '=' repeats the operation");
    CheckText(Run("10/4="), "2.5", "calc: division");
    CheckText(Run("2*3+4="), "10", "calc: chained operations");
    CheckText(Run("2+3*"), "5", "calc: an operator shows the running result");
    CheckText(Run("5+="), "10", "calc: '=' after an operator uses the shown value");
    CheckText(Run("7-*3="), "21", "calc: the last operator wins");
    CheckText(Run(".1+.2="), "0.3", "calc: 0.1 + 0.2 shows 0.3");
    CheckText(Run("1/3="), "0.333333333333333", "calc: fifteen digits");
    CheckText(Run("2/3="), "0.666666666666667", "calc: rounding");
    CheckText(Run("1/0="), "Cannot divide by zero", "calc: division by zero");
    CheckText(Run("0/0="), "Result is undefined", "calc: zero by zero");
    CheckText(Run("1/0=5"), "5", "calc: a digit clears an error");
    CheckText(Run("9@"), "3", "calc: square root");
    CheckText(Run("2@"), "1.4142135623731", "calc: square root of 2");
    CheckText(Run("3n@"), "Invalid input", "calc: root of a negative");
    CheckText(Run("12q"), "144", "calc: square");
    CheckText(Run("4r"), "0.25", "calc: reciprocal");
    CheckText(Run("50+10%"), "5", "calc: percent of the accumulator");
    CheckText(Run("50+10%="), "55", "calc: percent then equals");
    CheckText(Run("123\b\b"), "1", "calc: backspace");
    CheckText(Run("3n"), "-3", "calc: negate");
    CheckText(Run("3nn"), "3", "calc: negate twice");
    CheckText(Run("1..5"), "1.5", "calc: one decimal point");
    CheckText(Run("12345678901234567890"), "1234567890123456", "calc: the entry is limited to 16 digits");
    CheckText(Run("99999999*99999999="), "9.9999998e+15", "calc: scientific notation");
    CheckText(Run("5+3\x1B"), "0", "calc: Escape clears");
    CheckText(Run("5+3E"), "3", "calc: unknown keys are ignored");
    calc::Engine E;
    E.Key('8');
    E.Key('+');
    E.Key('2');
    E.ClearEntry();
    E.Key('1');
    E.Key('=');
    CheckText(E.Display(), "9", "calc: CE clears the entry only");

    CheckText(calc::FormatNumber(1e-10), "1e-10", "format 1e-10");
    CheckText(calc::FormatNumber(123456789012345.0), "123456789012345", "format 15 digits");
    CheckText(calc::FormatNumber(1234567890123456.0), "1.23456789012346e+15", "format 16 digits");
    CheckText(calc::FormatNumber(-0.5), "-0.5", "format a negative fraction");
    CheckText(calc::FormatNumber(0.000001), "0.000001", "format a small number");
    CheckText(calc::FormatNumber(100), "100", "format trailing zeros");
    double V = 0;
    Check(calc::ParseNumber("3e4", &V) && V == 30000, "parse an exponent");
    Check(calc::ParseNumber("-0.5", &V) && V == -0.5, "parse a negative fraction");
    Check(!calc::ParseNumber("abc", &V) && !calc::ParseNumber("1x", &V) && !calc::ParseNumber("", &V), "reject junk");
    Check(calc::SquareRoot(2.25) == 1.5, "square root");
}

/* ---- Terminal parsing --------------------------------------------------------------------------- */

void TestTerminalParse()
{
    term::CommandLine C = term::Parse("dir \"My Files\" > out.txt");
    Check(C.Valid && C.Words.Size() == 2 && C.Words[0] == "dir" && C.Words[1] == "My Files", "parse quoted words");
    Check(C.Redirect == "out.txt" && !C.Append && C.Rest == "\"My Files\"", "parse a redirection");
    C = term::Parse("  echo  hello   world  ");
    Check(C.Words.Size() == 3 && C.Rest == "hello   world" && C.Redirect.Empty(), "echo keeps the spacing");
    C = term::Parse("echo a >> \"b c.txt\"");
    Check(C.Valid && C.Append && C.Redirect == "b c.txt" && C.Rest == "a", "parse an append to a quoted file");
    C = term::Parse("type \"unclosed");
    Check(!C.Valid && !C.Error.Empty(), "an open quote is an error");
    C = term::Parse("dir >");
    Check(!C.Valid, "a redirection needs a file");
    C = term::Parse("dir > a > b");
    Check(!C.Valid, "one redirection only");
    C = term::Parse("\"C:\\Program Files\\x.exe\" arg");
    Check(C.Words.Size() == 2 && C.Words[0] == "C:\\Program Files\\x.exe" && C.Words[1] == "arg", "a quoted program path");
    C = term::Parse("echo \"a>b\"");
    Check(C.Valid && C.Redirect.Empty() && C.Words[1] == "a>b", "'>' inside quotes is text");
    C = term::Parse("   ");
    Check(C.Valid && C.Words.Empty(), "an empty line");
    CheckText(term::Quote(hz::String("a b")), "\"a b\"", "quote a word with a space");
    CheckText(term::Quote(hz::String("ab")), "ab", "plain words are not quoted");
    CheckText(term::Lower(hz::String("DiR")), "dir", "lower case");
    C = term::Parse("help | MORE ");
    Check(C.Valid && C.More && C.Words.Size() == 1 && C.Words[0] == "help" && C.Rest.Empty(), "parse '| more'");
    C = term::Parse("dir | sort");
    Check(!C.Valid, "only '| more' is a pipe");
    C = term::Parse("echo \"a|b\"");
    Check(C.Valid && !C.More && C.Words[1] == "a|b", "'|' inside quotes is text");
}

/* ---- Editor model --------------------------------------------------------------------------------- */

void TestEditorModel()
{
    hzui::TextEditor *Ed = new hzui::TextEditor();
    Ed->SetText("a\tb\r\nc", 6);
    Check(Ed->LineCount() == 2 && Ed->Line(0) == "a    b" && Ed->Line(1) == "c", "editor: tabs and CRLF are normalized");
    Ed->Select(hzui::TextPosition{0, 1}, hzui::TextPosition{1, 0});
    CheckText(Ed->SelectedText(), "    b\n", "editor: selection across lines");
    Ed->Insert("X\nY\nZ");
    CheckText(Ed->Text(), "aX\nY\nZc", "editor: multi-line replace");
    Check(Ed->Caret() == (hzui::TextPosition{2, 1}) && Ed->Modified, "editor: the caret follows the insertion");
    Check(Ed->Undo(), "editor: undo");
    CheckText(Ed->Text(), "a    b\nc", "editor: undo restores the text");
    Check(Ed->Caret() == (hzui::TextPosition{1, 0}) && Ed->Anchor() == (hzui::TextPosition{0, 1}),
          "editor: undo restores the selection");
    Check(Ed->Redo(), "editor: redo");
    CheckText(Ed->Text(), "aX\nY\nZc", "editor: redo replays");
    Ed->SelectAll();
    Ed->Insert("caf\xC3\xA9 \xE2\x82\xAC");
    Check(Ed->CharacterCount() == 6 && Ed->Text().Length() == 9, "editor: characters count code points");
    Ed->Select(hzui::TextPosition{0, 0}, hzui::TextPosition{0, 0});
    Check(Ed->Find("\xC3\x89", true) == false, "editor: find ignores ASCII case only");
    Check(Ed->Find("CAF"), "editor: find");
    CheckText(Ed->SelectedText(), "caf", "editor: find selects the match");
    Ed->SetText("", 0);
    Check(Ed->LineCount() == 1 && Ed->Text().Empty() && !Ed->CanUndo() && !Ed->Modified, "editor: empty document");
    Ed->Insert("one\n");
    Check(Ed->LineCount() == 2 && Ed->Caret() == (hzui::TextPosition{1, 0}), "editor: a trailing line feed adds a line");

    /* Word wrap (M11): rows at word boundaries, none without it. */
    Ed->Arrange(hzui::Rect{0, 0, 240, 400});
    const char *Long = "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi omicron pi rho";
    Ed->SetText(Long, strlen(Long));
    Check(Ed->RowCount() == 1, "editor: one row per line without word wrap");
    Ed->WordWrap = true;
    size_t Rows = Ed->RowCount();
    Check(Rows >= 3 && Rows <= 8, "editor: a long line wraps onto rows");
    Ed->Insert("\nshort");
    Check(Ed->RowCount() == Rows + 1, "editor: rows follow edits");
    Ed->WordWrap = false;
    Check(Ed->RowCount() == 2, "editor: word wrap off again");
    delete Ed;
}

/* ---- Crypto (M11) ------------------------------------------------------------------------------------ */

void TestCrypto()
{
    using namespace hz::crypto;
    CheckText(Sha256Hex("", 0), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 of nothing");
    CheckText(Sha256Hex("abc", 3), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 of abc");
    const char *Two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"; /* two blocks of padding */
    CheckText(Sha256Hex(Two, strlen(Two)), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
              "SHA-256 of 448 bits");
    Sha256 Million;
    char As[1000];
    memset(As, 'a', sizeof(As));
    for (int i = 0; i < 1000; i++)
        Million.Update(As, sizeof(As)); /* in pieces that straddle blocks */
    uint8_t Digest[kSha256Size];
    Million.Final(Digest);
    CheckText(ToHex(Digest, sizeof(Digest)), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
              "SHA-256 of a million a's");
    hz::String Salt("00112233445566778899aabbccddeeff");
    hz::String Hash = HashPassword(hz::String("horizon"), Salt);
    CheckText(Hash, "74a7db10617d7e8ad736fc64d7815a384a0dab53ee63e1eb9251d4f21bcb8e17", "password hash (10000 rounds)");
    Check(CheckPassword(hz::String("horizon"), Salt, Hash) && !CheckPassword(hz::String("Horizon"), Salt, Hash) &&
              !CheckPassword(hz::String(""), Salt, Hash),
          "password check");
    hz::String A = NewSalt(), B = NewSalt();
    Check(A.Length() == 32 && B.Length() == 32 && A != B, "salts are random");
}

/* ---- File trees (M11) ------------------------------------------------------------------------------ */

void TestFileTrees()
{
    using namespace hz::fs;
    Check(IsInside(hz::String("C:\\a\\b"), hz::String("C:\\a")) && IsInside(hz::String("C:\\a"), hz::String("C:\\")) &&
              !IsInside(hz::String("C:\\ab"), hz::String("C:\\a")) && !IsInside(hz::String("C:\\a"), hz::String("C:\\a")),
          "is inside");
    hz::String Root;
    for (const Volume &V : Volumes()) {
        if (!V.ReadOnly) {
            Root = V.Root;
            break;
        }
    }
    if (Root.Empty()) {
        HzPrint("apptest: no writable volume; file trees not tested");
        return;
    }
    hz::String Top = Join(Root, "apptest-tree");
    DeleteTree(Top); /* a left-over from an interrupted run */
    hz::String A = Join(Top, "A"), B = Join(A, "B");
    bool Made = HZ_SUCCESS(CreateDirectory(Top)) && HZ_SUCCESS(CreateDirectory(A)) && HZ_SUCCESS(CreateDirectory(B)) &&
                HZ_SUCCESS(WriteAll(Join(A, "one.txt"), "one\n", 4)) && HZ_SUCCESS(WriteAll(Join(B, "two.txt"), "two!\n", 5));
    /* A file over a few chunks of the streaming copy. */
    hz::String Big;
    for (int i = 0; i < 70000; i++)
        Big.Append(hz::String::Format("%c%c%c%c%c%c%c%c%c\n", 'a' + i % 26, 'a' + i / 26 % 26, 'a' + i / 676 % 26, '0' + i % 10,
                                      '0' + i / 10 % 10, '0' + i / 100 % 10, '0' + i / 1000 % 10, '0' + i / 10000 % 10, 'x'));
    Made = Made && HZ_SUCCESS(WriteAll(Join(B, "big.dat"), Big.CStr(), Big.Length()));
    Check(Made, "trees: make a test tree");
    hz::String Copy = Join(Top, "A copy");
    Check(HZ_SUCCESS(CopyTree(A, Copy)), "trees: copy a folder");
    hz::String Data;
    Check(HZ_SUCCESS(ReadAll(Join(Copy, "B\\two.txt"), Data)) && Data == "two!\n", "trees: the copy has the nested file");
    Check(HZ_SUCCESS(ReadAll(Join(Copy, "B\\big.dat"), Data)) && Data == Big, "trees: a 700 KB file copies exactly");
    Check(CopyTree(A, Join(B, "inside")) == STATUS_INVALID_PARAMETER, "trees: a folder is not copied into itself");
    Check(CopyTree(A, Copy) == STATUS_OBJECT_NAME_COLLISION, "trees: copy does not merge into an existing folder");
    hz::String Moved = Join(Copy, "moved A");
    Check(HZ_SUCCESS(MoveTree(A, Moved)) && !Exists(A) && HZ_SUCCESS(ReadAll(Join(Moved, "one.txt"), Data)) &&
              Data == "one\n",
          "trees: move a folder into another");
    Check(MoveTree(Copy, Join(Moved, "x")) == STATUS_INVALID_PARAMETER, "trees: a folder is not moved into itself");
    Check(HZ_SUCCESS(DeleteTree(Top)) && !Exists(Top), "trees: delete a folder with everything in it");
}

/* ---- Setup (M10) ------------------------------------------------------------------------------------ */

uint32_t Get32(const uint8_t *P)
{
    uint32_t V;
    memcpy(&V, P, 4);
    return V;
}

uint64_t Get64(const uint8_t *P)
{
    uint64_t V;
    memcpy(&V, P, 8);
    return V;
}

void TestInstaller()
{
    Check(setup::Crc32("123456789", 9) == 0xCBF43926u, "CRC-32 check value");
    Check(setup::Crc32("", 0) == 0, "CRC-32 of nothing");

    setup::Guid G;
    Check(setup::ParseGuid("C12A7328-F81F-11D2-BA4B-00A0C93EC93B", &G) && G == setup::kEfiSystemType,
          "parse a GUID into GPT byte order");
    CheckText(setup::FormatGuid(setup::kBasicDataType), "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", "format a GUID");
    Check(!setup::ParseGuid("C12A7328F81F-11D2-BA4B-00A0C93EC93B0", &G) && !setup::ParseGuid("xyz", &G) &&
              !setup::ParseGuid("C12A7328-F81F-11D2-BA4B-00A0C93EC93BXX", &G),
          "reject malformed GUIDs");
    setup::Guid A = setup::NewGuid(), B = setup::NewGuid();
    Check(!(A == B) && (A.Bytes[7] >> 4) == 4 && (A.Bytes[8] & 0xC0) == 0x80, "random GUIDs: distinct, version 4");
    setup::Guid Back;
    Check(setup::ParseGuid(setup::FormatGuid(A).CStr(), &Back) && Back == A, "GUID text round trip");

    /* A 384 MiB disk */
    const uint64_t Disk = 384ull * 2048;
    setup::Layout L;
    Check(setup::PlanLayout(Disk, &L), "plan a 384 MiB disk");
    Check(L.Esp.FirstLba == 2048 && L.Esp.LastLba == 2048 + 204800 - 1 && L.System.FirstLba == 206848 &&
              L.System.LastLba == 784383 && (L.System.LastLba + 1) % 2048 == 0 && L.System.LastLba <= Disk - 34,
          "partitions are 1 MiB aligned and leave room for the backup GPT");
    Check(!setup::PlanLayout(setup::MinimumDiskSectors() - 1, &L) && setup::PlanLayout(setup::MinimumDiskSectors(), &L),
          "the minimum disk size");
    setup::PlanLayout(Disk, &L);
    L.DiskGuid = setup::NewGuid();
    L.Esp.Unique = setup::NewGuid();
    L.System.Unique = setup::NewGuid();
    static uint8_t Mbr[512], Primary[512], Backup[512], Entries[setup::kGptEntrySectors * 512];
    setup::BuildGpt(L, Mbr, Primary, Entries, Backup);
    Check(Mbr[510] == 0x55 && Mbr[511] == 0xAA && Mbr[446 + 4] == 0xEE && Get32(Mbr + 446 + 8) == 1 &&
              Get32(Mbr + 446 + 12) == Disk - 1,
          "protective MBR");
    uint8_t Copy[512];
    memcpy(Copy, Primary, 512);
    uint32_t Crc = Get32(Copy + 16);
    memset(Copy + 16, 0, 4);
    Check(memcmp(Primary, "EFI PART", 8) == 0 && setup::Crc32(Copy, 92) == Crc, "primary header and its checksum");
    Check(Get64(Primary + 24) == 1 && Get64(Primary + 32) == Disk - 1 && Get64(Primary + 40) == 34 &&
              Get64(Primary + 48) == Disk - 34 && Get64(Primary + 72) == 2 && Get32(Primary + 80) == 128 &&
              Get32(Primary + 84) == 128,
          "primary header fields");
    Check(Get64(Backup + 24) == Disk - 1 && Get64(Backup + 32) == 1 && Get64(Backup + 72) == Disk - 33 &&
              Get32(Backup + 88) == Get32(Primary + 88),
          "backup header fields");
    Check(setup::Crc32(Entries, 128 * 128) == Get32(Primary + 88), "entry array checksum");
    Check(memcmp(Entries, setup::kEfiSystemType.Bytes, 16) == 0 && memcmp(Entries + 16, L.Esp.Unique.Bytes, 16) == 0 &&
              Get64(Entries + 32) == 2048 && memcmp(Entries + 128, setup::kBasicDataType.Bytes, 16) == 0 &&
              Get64(Entries + 128 + 40) == 784383 &&
              memcmp(Entries + 128 + 56, "N\0e\0x\0o\0r\0a\0 \0O\0S\0", 18) == 0,
          "partition entries: types, GUIDs, bounds, UTF-16 names");

    /* FAT32 geometry */
    setup::FatParameters F;
    Check(setup::PlanFat32(204800, "efi", 1, &F) && F.SectorsPerCluster == 2 && F.Clusters >= 65541 &&
              (uint64_t)F.FatSectors * 128 >= F.Clusters + 2 && strcmp(F.Label, "EFI        ") == 0,
          "FAT32 for the 100 MiB ESP: 1 KiB clusters, enough FAT, padded label");
    Check(F.FirstDataSector() + (uint64_t)F.Clusters * F.SectorsPerCluster <= 204800, "the clusters fit the partition");
    Check(setup::PlanFat32(577536, "NEXORAOS", 2, &F) && F.SectorsPerCluster == 8 && F.Clusters >= 65541,
          "FAT32 for a 282 MiB partition: 4 KiB clusters");
    Check(setup::PlanFat32(33554432, "X", 3, &F) && F.SectorsPerCluster == 16, "16 GiB: 8 KiB clusters");
    Check(!setup::PlanFat32(30000, "X", 4, &F), "too small for FAT32");
    setup::PlanFat32(204800, "EFI", 0x12345678, &F);
    static uint8_t Boot[512], Info[512], Fat[512], Root[512];
    setup::BuildBootSector(F, 2048, Boot);
    setup::BuildFsInfo(F, Info);
    setup::BuildFirstFatSector(Fat);
    setup::BuildRootSector(F, Root);
    Check(Boot[0] == 0xEB && Boot[510] == 0x55 && Boot[511] == 0xAA && Boot[13] == 2 && Get32(Boot + 28) == 2048 &&
              Get32(Boot + 32) == 204800 && Get32(Boot + 36) == F.FatSectors && Get32(Boot + 44) == 2 &&
              Get32(Boot + 67) == 0x12345678 && memcmp(Boot + 71, "EFI        ", 11) == 0 &&
              memcmp(Boot + 82, "FAT32   ", 8) == 0,
          "FAT32 boot sector fields");
    Check(Get32(Info) == 0x41615252 && Get32(Info + 484) == 0x61417272 && Get32(Info + 488) == F.Clusters - 1 &&
              Get32(Info + 508) == 0xAA550000,
          "FSInfo sector");
    Check(Get32(Fat) == 0x0FFFFFF8 && Get32(Fat + 8) == 0x0FFFFFFF && Get32(Fat + 12) == 0, "first FAT sector");
    Check(memcmp(Root, "EFI        ", 11) == 0 && Root[11] == 0x08, "root directory label entry");

    /* String::Format used to cut results at 511 bytes (the boot configuration is longer). */
    hz::String Long;
    for (int i = 0; i < 100; i++)
        Long.Append("0123456789");
    hz::String Twice = hz::String::Format("%s|%s", Long.CStr(), Long.CStr());
    Check(Twice.Length() == 2001 && Twice[1000] == '|' && Twice[2000] == '9' && strlen(Twice.CStr()) == 2001,
          "String::Format results longer than 512 bytes");
    hz::String Conf = setup::BootConfiguration(A);
    Check(Conf.Length() > 800 && Conf.Find("module_string: symbols\n\n/Nexora OS (system image") != hz::NotFound &&
              Conf[Conf.Length() - 1] == '\n',
          "the whole boot configuration");
    Check(Conf.Find(hz::String::Format("cmdline: hz.system=%s\n", setup::FormatGuid(A).CStr()).CStr()) != hz::NotFound &&
              Conf.Find("module_string: ramdisk") != hz::NotFound && Conf.Find("path: boot():/boot/hzkrnl.elf") != hz::NotFound,
          "boot configuration names the system partition");

    Check(setup::IsValidUserName("Ada") && setup::IsValidUserName("Mary-Jane O.k_2"), "valid user names");
    Check(!setup::IsValidUserName("") && !setup::IsValidUserName(" Ada") && !setup::IsValidUserName("Ada ") &&
              !setup::IsValidUserName("a/b") && !setup::IsValidUserName("..") &&
              !setup::IsValidUserName("abcdefghijabcdefghijabcdefghijabc"),
          "invalid user names");
}

void TestSettings()
{
    hz::fs::Settings S;
    const char *Text = "# comment\r\nUserName = Ada \r\n; another\nwallpaper=2\nRecent=notepad,files\n";
    Check(hz::fs::Settings::Parse(Text, strlen(Text), S), "parse a settings file");
    Check(S.Get("username") == "Ada" && S.GetInt("Wallpaper") == 2 && S.Get("Recent") == "notepad,files" &&
              S.Get("Missing", "x") == "x" && S.GetInt("UserName", 7) == 7 && S.Has("RECENT"),
          "settings values, case-insensitive keys, defaults");
    S.Set("WALLPAPER", "1");
    S.SetInt("FirstBoot", 0);
    Check(S.Items().Size() == 4 && S.Items()[1].Key == "wallpaper" && S.Items()[1].Value == "1" &&
              S.Items()[3].Key == "FirstBoot",
          "set keeps the order and the key's spelling");
    hz::fs::Settings Again;
    hz::String Saved = S.Text();
    Check(hz::fs::Settings::Parse(Saved.CStr(), Saved.Length(), Again) && Again.Get("UserName") == "Ada" &&
              Again.GetInt("FirstBoot", 5) == 0,
          "settings text round trip");
    Check(!hz::fs::Settings::Parse("novalue\n", 8, Again) && !hz::fs::Settings::Parse("=x\n", 3, Again),
          "reject lines without a key");
}

}  // namespace

extern "C" int main(int argc, char **argv)
{
    TestPaths();
    TestManifests();
    TestBrowserUrls();
    TestBrowserHtml();
    TestCalculator();
    TestTerminalParse();
    TestEditorModel();
    TestInstaller();
    TestSettings();
    TestCrypto();
    TestFileTrees();
    HzPrint("apptest: %d checks, %d failed", g_Checks, g_Failures);
    return g_Failures;
}
