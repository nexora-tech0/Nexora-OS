/*
 * Nexora OS - user/apps/terminal/terminal.cpp
 * Terminal: a command prompt. Built-in commands work on the file systems and the process list;
 * anything else runs as a program with its standard output and error going through a pipe that
 * a reader thread drains into the window (posted as messages, so the view is only touched on the
 * window's thread). '>' and '>>' send a command's output to a file. Programs that have a
 * manifest (the windowed apps) are started without waiting. Commands and output lines are
 * mirrored to the kernel log ("terminal: > dir", "terminal| ...") for the smoke tests.
 */
#include "console.h"
#include "hzapps.h"
#include "hzcrypto.h"
#include "hzfile.h"
#include "hzversion.h"
#include "parse.h"

namespace term {

namespace {

constexpr uint32_t kMsgOutput = HZ_WM_USER + 1; /* LParam = hz::String * (the receiver frees it) */
constexpr uint32_t kMsgExited = HZ_WM_USER + 2; /* WParam = exit status */
constexpr const char *kSystemFolder = "C:\\Nexora\\System";
constexpr STATUS kInterruptStatus = (STATUS)0xC000013A;

struct ReaderContext {
    HANDLE Read;
    HANDLE Process;
    HWND Window;
};

uint32_t ReaderThread(void *Parameter)
{
    ReaderContext *C = static_cast<ReaderContext *>(Parameter);
    char Buffer[2048];
    for (;;) {
        uint32_t Got = 0;
        STATUS Status = NtReadFile(C->Read, Buffer, sizeof(Buffer), nullptr, &Got);
        if (!HZ_SUCCESS(Status))
            break; /* STATUS_END_OF_FILE once every writer has closed */
        if (Got) {
            hz::String *Chunk = new hz::String(Buffer, Got);
            if (!HZ_SUCCESS(NtUserPostMessage(C->Window, kMsgOutput, 0, reinterpret_cast<uint64_t>(Chunk))))
                delete Chunk;
        }
    }
    NtWaitForSingleObject(C->Process, 0, nullptr);
    HZ_PROCESS_BASIC_INFORMATION Info = {};
    NtQueryInformationProcess(C->Process, HZ_PROCESS_BASIC_INFORMATION_CLASS, &Info, sizeof(Info));
    NtUserPostMessage(C->Window, kMsgExited, (uint64_t)(uint32_t)Info.ExitStatus, 0);
    return 0;
}

hz::String Thousands(uint64_t Value)
{
    char Digits[32];
    int Count = 0;
    do {
        Digits[Count++] = (char)('0' + Value % 10);
        Value /= 10;
    } while (Value);
    hz::String Out;
    for (int i = Count - 1; i >= 0; i--) {
        Out.Append(Digits[i]);
        if (i && i % 3 == 0)
            Out.Append(',');
    }
    return Out;
}

hz::String Pad(const hz::String &Text, size_t Width, bool Right)
{
    hz::String Out;
    if (Right)
        for (size_t i = Text.Length(); i < Width; i++)
            Out.Append(' ');
    Out.Append(Text);
    if (!Right)
        for (size_t i = Text.Length(); i < Width; i++)
            Out.Append(' ');
    return Out;
}

hz::String FormatDuration(uint64_t Microseconds)
{
    uint64_t Seconds = Microseconds / 1000000;
    if (Seconds >= 86400)
        return hz::String::Format("%llu d %llu:%02llu:%02llu", (unsigned long long)(Seconds / 86400),
                                  (unsigned long long)(Seconds / 3600 % 24), (unsigned long long)(Seconds / 60 % 60),
                                  (unsigned long long)(Seconds % 60));
    return hz::String::Format("%llu:%02llu:%02llu", (unsigned long long)(Seconds / 3600),
                              (unsigned long long)(Seconds / 60 % 60), (unsigned long long)(Seconds % 60));
}

class Terminal;

class TerminalWindow final : public Window {
public:
    TerminalWindow(Terminal *Owner) : Window("Terminal", 900, 560), Owner_(Owner) {}
    int64_t HandleMessage(uint32_t Message, uint64_t WParam, uint64_t LParam) override;

private:
    Terminal *Owner_;
};

class Terminal {
public:
    bool Create();
    void OnOutput(hz::String *Chunk);
    void OnExited(STATUS Status);

private:
    void Run(const hz::String &Line);
    void Out(const char *Text, size_t Length);
    void Out(const hz::String &Text) { Out(Text.CStr(), Text.Length()); }
    void Line(const hz::String &Text);
    void Line(const char *Text) { Line(hz::String(Text)); }
    void Error(const char *Text) { Line(hz::String(Text)); }
    void FileError(const char *What, const hz::String &Path, STATUS Status);
    void FinishCommand();
    void ReadyForInput();
    /* '| more' */
    void ShowPage(size_t Rows);
    void OnPagerKey(KeyEvent &E);
    void EndPaging();
    void UpdatePrompt();
    hz::String Path(const hz::String &Argument) const { return hz::fs::Resolve(Cwd_, Argument.CStr()); }
    hz::String FindProgram(const hz::String &Name) const;
    bool StartChild(const hz::String &Image, const CommandLine &Command);
    hz::String Complete(const hz::String &Input);

    /* Built-in commands */
    void Help();
    void Dir(const CommandLine &C);
    void ChangeDirectory(const CommandLine &C);
    void Type(const CommandLine &C);
    void CopyFile(const CommandLine &C);
    void DeleteFiles(const CommandLine &C);
    void MakeDirectory(const CommandLine &C);
    void RemoveDirectory(const CommandLine &C);
    void RenameFile(const CommandLine &C);
    void Memory();
    void Processes();
    void Kill(const CommandLine &C);
    void Start(const CommandLine &C);
    void Volumes();
    void ShowHistory();
    void HeadTail(const CommandLine &C, bool Tail);
    void WordCount(const CommandLine &C);
    void Grep(const CommandLine &C);
    void Touch(const CommandLine &C);
    void Move(const CommandLine &C);
    void Stat(const CommandLine &C);
    void PathPart(const CommandLine &C, bool Parent);
    void Tree(const CommandLine &C);
    void FindFiles(const CommandLine &C);
    void SortLines(const CommandLine &C);
    void SetVariable(const CommandLine &C);
    void ListEnvironment();
    void LinuxHelp();
    bool UnavailableLinuxCommand(const hz::String &Name);

    TerminalWindow *Main_ = nullptr;
    ConsoleView *View_ = nullptr;
    hz::String Cwd_;
    /* Variables for '%NAME%' and $NAME expansion; USER, HOME and PATH are set at start-up. */
    Environment Env_;
    /* Redirection of the current command */
    bool Capturing_ = false;
    hz::String Captured_;
    hz::String RedirectPath_;
    bool RedirectAppend_ = false;
    bool Paging_ = false;
    hz::Vector<hz::String> PagerLines_;
    size_t PagerNext_ = 0;
    hz::String LogLine_;
    /* The running program */
    HANDLE Process_ = 0, Read_ = 0, Thread_ = 0;
    uint32_t ProcessId_ = 0;
    hz::String ProcessName_;
    ReaderContext *Reader_ = nullptr;
};

int64_t TerminalWindow::HandleMessage(uint32_t Message, uint64_t WParam, uint64_t LParam)
{
    if (Message == kMsgOutput) {
        Owner_->OnOutput(reinterpret_cast<hz::String *>(LParam));
        return 0;
    }
    if (Message == kMsgExited) {
        Owner_->OnExited((STATUS)(uint32_t)WParam);
        return 0;
    }
    return Window::HandleMessage(Message, WParam, LParam);
}

/* ---- Set-up ------------------------------------------------------------------------------------- */

bool Terminal::Create()
{
    Cwd_ = hz::fs::DocumentsFolder(); /* the user's documents, else the first writable volume */
    /* A small starting environment, so both conventions work from the first line: %USERNAME% and
       $USER, %USERPROFILE% and $HOME, and a PATH that names the program folder. */
    hz::String User = "Guest";
    hz::fs::Settings Shell;
    if (HZ_SUCCESS(Shell.Load(hz::fs::SettingsPath("shell")))) {
        hz::String Saved = Shell.Get("UserName", "");
        if (!Saved.Empty())
            User = Saved;
    }
    Env_.Set("USERNAME", User);
    Env_.Set("USER", User);
    Env_.Set("HOME", Cwd_);
    Env_.Set("USERPROFILE", Cwd_);
    Env_.Set("OS", "Nexora OS");
    Env_.Set("PATH", "C:\\Nexora\\System");
    hz::fs::Settings Environment;
    if (HZ_SUCCESS(Environment.Load(hz::fs::SettingsPath("environment"))))
        for (const hz::fs::Settings::Item &Item : Environment.Items())
            Env_.Set(Item.Key, Item.Value);
    Main_ = new TerminalWindow(this);
    Main_->SetMinimumSize(420, 240);
    Main_->Background = Theme::SolidBackground;
    View_ = new ConsoleView();
    View_->OnCommand = [this](const hz::String &Line) { Run(Line); };
    View_->OnBusyKey = [this](KeyEvent &E) { OnPagerKey(E); };
    View_->OnInterrupt = [this]() {
        if (Paging_ && !Process_) {
            View_->DropPartial();
            Out("^C\n", 3);
            EndPaging();
            return;
        }
        if (Process_) {
            NtTerminateProcess(Process_, kInterruptStatus);
            View_->Write("^C\n");
            HzLog("terminal: interrupted %s", ProcessName_.CStr());
        }
    };
    View_->OnComplete = [this](const hz::String &Input) { return Complete(Input); };
    Main_->SetContent(View_);
    Main_->OnClosed = [this]() {
        if (Process_)
            NtTerminateProcess(Process_, kInterruptStatus);
        NtUserPostQuitMessage(0);
    };
    View_->Write("Nexora OS Terminal " HZ_VERSION_STRING ". Type 'help' for the commands.\n\n");
    UpdatePrompt();
    if (!Main_->Show())
        return false;
    Main_->SetFocus(View_);
    return true;
}

void Terminal::UpdatePrompt()
{
    View_->SetPrompt(hz::String::Format("%s> ", Cwd_.CStr()));
}

/* ---- Output ------------------------------------------------------------------------------------- */

void Terminal::Out(const char *Text, size_t Length)
{
    if (Capturing_) {
        Captured_.Append(Text, Length);
        return;
    }
    View_->Write(Text, Length);
    /* Mirror complete lines to the kernel log. */
    for (size_t i = 0; i < Length; i++) {
        if (Text[i] == '\n') {
            HzLog("terminal| %s", LogLine_.CStr());
            LogLine_ = hz::String();
        } else if (Text[i] != '\r' && LogLine_.Length() < 200) {
            LogLine_.Append(Text[i]);
        }
    }
}

void Terminal::Line(const hz::String &Text)
{
    Out(Text);
    Out("\n", 1);
}

void Terminal::FileError(const char *What, const hz::String &Path, STATUS Status)
{
    Line(hz::String::Format("%s %s: %s.", What, Path.CStr(), hz::fs::StatusText(Status)));
}

void Terminal::FinishCommand()
{
    if (Capturing_ && Paging_) {
        Capturing_ = false;
        PagerLines_.Clear();
        size_t From = 0;
        for (size_t i = 0; i <= Captured_.Length(); i++) {
            if (i == Captured_.Length() ? i > From : Captured_[i] == '\n') {
                PagerLines_.Push(Captured_.Substring(From, i - From));
                From = i + 1;
            }
        }
        Captured_ = hz::String();
        PagerNext_ = 0;
        HzLog("terminal: paging %zu lines", PagerLines_.Size());
        ShowPage(View_->VisibleRows() - 1);
        return;
    }
    if (Capturing_) {
        Capturing_ = false;
        hz::String Data;
        STATUS Status = STATUS_SUCCESS;
        if (RedirectAppend_ && hz::fs::Exists(RedirectPath_))
            Status = hz::fs::ReadAll(RedirectPath_, Data);
        if (HZ_SUCCESS(Status)) {
            Data.Append(Captured_);
            Status = hz::fs::WriteAll(RedirectPath_, Data.CStr(), Data.Length());
        }
        if (!HZ_SUCCESS(Status))
            FileError("Cannot write", RedirectPath_, Status);
        else
            HzLog("terminal: wrote %zu bytes to %s", Captured_.Length(), RedirectPath_.CStr());
        Captured_ = hz::String();
    }
    ReadyForInput();
}

void Terminal::ReadyForInput()
{
    if (!LogLine_.Empty()) {
        HzLog("terminal| %s", LogLine_.CStr());
        LogLine_ = hz::String();
    }
    View_->SetInputEnabled(true);
    UpdatePrompt();
}

/* ---- '| more' ----------------------------------------------------------------------------------- */

/* Writes lines until Rows rows are used (at least one line), then "-- More --" if lines remain. */
void Terminal::ShowPage(size_t Rows)
{
    size_t Used = 0;
    while (PagerNext_ < PagerLines_.Size()) {
        size_t Need = View_->RowsOfText(PagerLines_[PagerNext_]);
        if (Used > 0 && Used + Need > Rows)
            break;
        Line(PagerLines_[PagerNext_++]);
        Used += Need;
    }
    if (PagerNext_ >= PagerLines_.Size()) {
        EndPaging();
        return;
    }
    View_->Write("-- More (Space: next page, Enter: next line, Q: quit) --");
    HzLog("terminal: more (%zu of %zu lines shown)", PagerNext_, PagerLines_.Size());
}

void Terminal::OnPagerKey(KeyEvent &E)
{
    if (!Paging_ || Process_)
        return;
    size_t Rows;
    if (E.Key == HZ_VK_SPACE)
        Rows = View_->VisibleRows() - 1;
    else if (E.Key == HZ_VK_RETURN || E.Key == HZ_VK_DOWN)
        Rows = 1;
    else if (E.Key == 'Q' || E.Key == HZ_VK_ESCAPE)
        Rows = 0;
    else
        return;
    E.Handled = true;
    View_->DropPartial();
    if (Rows == 0) {
        HzLog("terminal: more ended at line %zu", PagerNext_);
        EndPaging();
        return;
    }
    ShowPage(Rows);
}

void Terminal::EndPaging()
{
    Paging_ = false;
    PagerLines_.Clear();
    PagerNext_ = 0;
    ReadyForInput();
}

/* ---- Commands ----------------------------------------------------------------------------------- */

void Terminal::Run(const hz::String &Text)
{
    HzLog("terminal: > %s", Text.CStr());
    /* %NAME% and $NAME are replaced before parsing, so one line can mix both conventions. */
    hz::String Expanded = ExpandVariables(Text.CStr(), Env_);
    CommandLine C = Parse(Expanded.CStr());
    if (!C.Valid) {
        Error(C.Error.CStr());
        FinishCommand();
        return;
    }
    if (C.Words.Empty()) {
        FinishCommand();
        return;
    }
    if (!C.Redirect.Empty()) {
        Capturing_ = true;
        Captured_ = hz::String();
        RedirectPath_ = Path(C.Redirect);
        RedirectAppend_ = C.Append;
    } else if (C.More) {
        Capturing_ = Paging_ = true;
        Captured_ = hz::String();
    }
    hz::String Name = Lower(C.Words[0]);
    View_->SetInputEnabled(false);

    if (Name.Length() == 2 && Name[1] == ':') { /* "d:" changes the drive */
        hz::String Root = hz::fs::Resolve(Cwd_, hz::String::Format("%c:\\", Name[0]).CStr());
        bool Directory = false;
        if (hz::fs::Exists(Root, &Directory) && Directory)
            Cwd_ = Root;
        else
            Error("The system cannot find the drive specified.");
    } else if (Name == "help" || Name == "?") {
        if (C.Words.Size() > 1 && Lower(C.Words[1]) == "linux")
            LinuxHelp();
        else
            Help();
    } else if (Name == "cls" || Name == "clear") {
        View_->Clear();
    } else if (Name == "ver") {
        Line("Nexora OS " HZ_VERSION_STRING " (" HZ_MILESTONE ")");
    } else if (Name == "echo") {
        Line(C.Rest);
    } else if (Name == "uptime") {
        uint64_t Uptime = 0;
        NtQueryUptime(&Uptime);
        Line(hz::String::Format("Up %s", FormatDuration(Uptime).CStr()));
    } else if (Name == "date" || Name == "time") {
        uint64_t Now = 0;
        NtQuerySystemTime(&Now);
        Line(hz::String::Format("%s UTC", hz::fs::FormatTime(Now).CStr()));
    } else if (Name == "dir" || Name == "ls" || Name == "vdir" || Name == "v") {
        Dir(C);
    } else if (Name == "cd" || Name == "chdir") {
        ChangeDirectory(C);
    } else if (Name == "type" || Name == "cat") {
        Type(C);
    } else if (Name == "copy" || Name == "cp") {
        CopyFile(C);
    } else if (Name == "mv") {
        Move(C);
    } else if (Name == "del" || Name == "erase" || Name == "rm") {
        DeleteFiles(C);
    } else if (Name == "mkdir" || Name == "md") {
        MakeDirectory(C);
    } else if (Name == "rmdir" || Name == "rd") {
        RemoveDirectory(C);
    } else if (Name == "ren" || Name == "rename") {
        RenameFile(C);
    } else if (Name == "mem") {
        Memory();
    } else if (Name == "free") {
        Memory();
    } else if (Name == "ps" || Name == "tasklist") {
        Processes();
    } else if (Name == "kill" || Name == "taskkill") {
        Kill(C);
    } else if (Name == "start") {
        Start(C);
    } else if (Name == "vol" || Name == "volumes") {
        Volumes();
    } else if (Name == "df") {
        Volumes();
    } else if (Name == "pwd") {
        Line(Cwd_);
    } else if (Name == "head") {
        HeadTail(C, false);
    } else if (Name == "tail") {
        HeadTail(C, true);
    } else if (Name == "more") {
        Error("Use '<command> | more' to page command output.");
    } else if (Name == "wc") {
        WordCount(C);
    } else if (Name == "grep") {
        Grep(C);
    } else if (Name == "touch") {
        Touch(C);
    } else if (Name == "stat") {
        Stat(C);
    } else if (Name == "basename") {
        PathPart(C, false);
    } else if (Name == "dirname") {
        PathPart(C, true);
    } else if (Name == "sha256sum") {
        if (C.Words.Size() != 2) {
            Error("Use: sha256sum <file>");
        } else {
            hz::String File = Path(C.Words[1]), Data;
            STATUS Status = hz::fs::ReadAll(File, Data);
            if (!HZ_SUCCESS(Status))
                FileError("Cannot read", File, Status);
            else
                Line(hz::String::Format("%s  %s", hz::crypto::Sha256Hex(Data.CStr(), Data.Length()).CStr(),
                                        C.Words[1].CStr()));
        }
    } else if (Name == "uname") {
        Line(C.Words.Size() > 1 && C.Words[1] == "-a"
                 ? hz::String::Format("NexoraOS %s %s x86_64", HZ_VERSION_STRING, HZ_MILESTONE)
                 : hz::String("NexoraOS"));
    } else if (Name == "hostname") {
        Line("NexoraOS");
    } else if (Name == "whoami" || Name == "who" || Name == "users") {
        hz::fs::Settings Settings;
        STATUS Status = Settings.Load(hz::fs::SettingsPath("shell"));
        if (!HZ_SUCCESS(Status) && !hz::fs::IsLiveSystem())
            Line(hz::String::Format("Cannot read the account settings (0x%08X).", (uint32_t)Status));
        else {
            hz::String User = Settings.Get("UserName", "Guest");
            Line(User.Empty() ? "Guest" : User);
        }
    } else if (Name == "id") {
        Line("POSIX user and group IDs are not implemented; use whoami to display the Nexora account.");
    } else if (Name == "tree") {
        Tree(C);
    } else if (Name == "find") {
        FindFiles(C);
    } else if (Name == "sort") {
        SortLines(C);
    } else if (Name == "set" || Name == "export") {
        SetVariable(C);
    } else if (Name == "unset") {
        if (C.Words.Size() < 2) {
            Error("Use: unset NAME [NAME ...]");
        } else {
            for (size_t i = 1; i < C.Words.Size(); i++)
                Env_.Unset(C.Words[i]);
        }
    } else if (Name == "env" || Name == "printenv") {
        ListEnvironment();
    } else if (Name == "which" || Name == "whereis" || Name == "where") {
        if (C.Words.Size() != 2) {
            Error("Use: which <program>");
        } else {
            hz::String Image = FindProgram(C.Words[1]);
            if (Image.Empty())
                Line(hz::String::Format("%s: program not found.", C.Words[1].CStr()));
            else
                Line(Image);
        }
    } else if (Name == "true") {
        /* A successful no-op, as in the standard utility. */
    } else if (Name == "false") {
        Line("false: Nexora Terminal does not expose POSIX exit statuses for built-ins.");
    } else if (Name == "sudo" || Name == "su" || Name == "doas") {
        Line("Privilege elevation is unavailable: Nexora OS has no POSIX users, groups, or sudo service.");
    } else if (Name == "apt" || Name == "apt-get" || Name == "aptitude" || Name == "dpkg" ||
               Name == "rpm" || Name == "dnf" || Name == "yum") {
        Line("Package management is unavailable: this build has no package database or installer service.");
    } else if (Name == "systemctl" || Name == "service" || Name == "cron" || Name == "crontab") {
        Line("Linux services and scheduled jobs are unavailable: Nexora OS has no Linux service manager.");
    } else if (Name == "curl" || Name == "wget" || Name == "ping" || Name == "ssh" || Name == "scp" ||
               Name == "sftp" || Name == "ifconfig" || Name == "ifup" || Name == "ifdown" || Name == "iw" ||
               Name == "ip" || Name == "nc" || Name == "netstat" || Name == "ss" || Name == "traceroute") {
        Line("This network utility is unavailable: Nexora OS does not yet provide its required user-mode network tools.");
    } else if (Name == "mount" || Name == "umount" || Name == "fdisk" || Name == "cfdisk" || Name == "mkfs" ||
               Name == "dd" || Name == "chroot" || Name == "modprobe" || Name == "rmmod") {
        Line("This privileged device or filesystem operation is unavailable in Nexora OS.");
    } else if (Name == "history") {
        ShowHistory();
    } else if (Name == "lock" || Name == "shutdown" || Name == "restart" || Name == "reboot") {
        /* The shell does these: shutting down asks the programs to close first. */
        HWND Shell = 0;
        uint64_t Request = Name == "lock" ? HZ_SHELL_REQUEST_LOCK : HZ_SHELL_REQUEST_SHUTDOWN;
        uint64_t Detail = Name == "restart" || Name == "reboot" ? HZ_SHUTDOWN_REBOOT : HZ_SHUTDOWN_POWER_OFF;
        if (HZ_SUCCESS(NtUserGetShellWindow(&Shell)) && Shell)
            NtUserPostMessage(Shell, HZ_WM_SHELLREQUEST, Request, Detail);
        else
            Error("The shell is not running.");
    } else if (Name == "exit") {
        FinishCommand();
        Main_->Close();
        return;
    } else {
        hz::String Image = FindProgram(C.Words[0]);
        if (Image.Empty()) {
            if (!UnavailableLinuxCommand(Name))
                Line(hz::String::Format("'%s' is not a command or a program.", C.Words[0].CStr()));
        } else if (StartChild(Image, C)) {
            return; /* finishes when the program exits */
        }
    }
    FinishCommand();
}

void Terminal::Help()
{
    static const char *const Lines[] = {
        "Commands (names are not case-sensitive):",
        "  cd [path]            show or change the current folder; 'D:' changes the drive",
        "  cls                  clear the screen (Ctrl+L)",
        "  copy <from> <to>     copy a file",
        "  date                 the date and time (UTC)",
        "  del <file|pattern>   delete files (* and ? patterns)",
        "  dir/ls [path]        list a folder (Linux ls alias)",
        "  echo <text>          print text (variables are expanded first)",
        "  env                  list the variables (same as 'set' with no argument)",
        "  exit                 close the terminal",
        "  export NAME=value    set a variable (alias for set)",
        "  find [path] <pattern>  search folders recursively for matching names",
        "  free                 physical memory use (alias for mem)",
        "  grep [-i|-n] <text> <file>  search text files for a literal string",
        "  head/tail [-n|-/n N] <file>  show the first or last N lines",
        "  help [linux]         this list or Linux command availability",
        "  history              commands typed so far (Up and Down recall them)",
        "  kill <pid or name>   end a process (by name: every one with that name)",
        "  lock                 lock the computer (the lock screen)",
        "  mem                  memory use",
        "  mkdir <path>         create a folder",
        "  mv <from> <to>       move a file or folder",
        "  ps                   processes (Linux ps alias)",
        "  pwd                  print the current folder",
        "  ren <path> <name>    rename a file or folder",
        "  restart              restart (programs are asked to close first)",
        "  rmdir <path>         remove an empty folder",
        "  set [NAME=value]     list the variables, set one, or clear one with 'set NAME='",
        "  sha256sum <file>     SHA-256 digest of a file",
        "  shutdown             shut down (programs are asked to close first)",
        "  sort <file>          sort the lines of a file (or of 'command | sort')",
        "  start <program|file> start a program or open a file (no waiting; arguments follow)",
        "  stat <path>          show file or folder metadata",
        "  touch <file>         create an empty file if it does not exist",
        "  tree [path]          folders and files under a path, indented",
        "  type <file>          print a text file",
        "  uname/whoami         Nexora OS and account information",
        "  unset NAME           remove a variable",
        "  uptime               time since start-up",
        "  ver                  the Nexora OS version",
        "  vol/df               volumes and free space",
        "  wc <file>            count lines, words and bytes",
        "  where <program>      locate an installed Nexora program (alias for which)",
        "Options take either style: '-i' as on Unix or '/i' as on Windows.",
        "Variables are set with 'set NAME=value' (or 'export') and read as %NAME% or $NAME;",
        "'\\$' and '\\%' give a literal character. A name that is not set is left as typed.",
        "Anything else runs as a program from the current folder or C:\\Nexora\\System;",
        "its output appears here. 'command > file' and '>> file' write the output to a file;",
        "'command | more' shows it a page at a time. Ctrl+C ends a running program.",
        "Tab completes file names.",
    };
    for (const char *L : Lines)
        Line(L);
}

void Terminal::Dir(const CommandLine &C)
{
    hz::String Target = C.Words.Size() > 1 ? Path(C.Words[1]) : Cwd_;
    hz::String Pattern;
    if (hz::fs::HasWildcards(hz::fs::FileName(Target).CStr())) {
        Pattern = hz::fs::FileName(Target);
        Target = hz::fs::Parent(Target);
    } else {
        bool Directory = false;
        if (hz::fs::Exists(Target, &Directory) && !Directory) { /* a single file */
            Pattern = hz::fs::FileName(Target);
            Target = hz::fs::Parent(Target);
        }
    }
    hz::Vector<hz::fs::Entry> Entries;
    STATUS Status = hz::fs::List(Target, Entries);
    if (!HZ_SUCCESS(Status)) {
        FileError("Cannot list", Target, Status);
        return;
    }
    hz::fs::Volume V;
    bool HaveVolume = HZ_SUCCESS(hz::fs::QueryVolume(Target, V));
    if (HaveVolume)
        Line(hz::String::Format(" Volume in drive %c is %s (%s%s)", Target[0], V.Label.Empty() ? "unnamed" : V.Label.CStr(),
                                V.FileSystem.CStr(), V.ReadOnly ? ", read-only" : ""));
    Line(hz::String::Format(" Directory of %s", Target.CStr()));
    Line("");
    uint64_t Files = 0, Folders = 0, Bytes = 0;
    for (const hz::fs::Entry &E : Entries) {
        if (!Pattern.Empty() && !hz::fs::MatchPattern(Pattern.CStr(), E.Name.CStr()))
            continue;
        hz::String When = hz::fs::FormatTime(E.Modified);
        if (When.Empty())
            When = "                ";
        if (E.Directory) {
            Folders++;
            Line(hz::String::Format("%s    <DIR>          %s", When.CStr(), E.Name.CStr()));
        } else {
            Files++;
            Bytes += E.Size;
            Line(hz::String::Format("%s    %s %s", When.CStr(), Pad(Thousands(E.Size), 14, true).CStr(), E.Name.CStr()));
        }
    }
    Line(hz::String::Format("%s file(s) %s bytes", Pad(Thousands(Files), 16, true).CStr(),
                            Pad(Thousands(Bytes), 14, true).CStr()));
    if (HaveVolume)
        Line(hz::String::Format("%s folder(s) %s bytes free", Pad(Thousands(Folders), 14, true).CStr(),
                                Pad(Thousands(V.FreeBytes), 14, true).CStr()));
    else
        Line(hz::String::Format("%s folder(s)", Pad(Thousands(Folders), 14, true).CStr()));
}

void Terminal::ChangeDirectory(const CommandLine &C)
{
    if (C.Words.Size() < 2) {
        Line(Cwd_);
        return;
    }
    hz::String Target = Path(C.Words.Size() > 2 ? C.Rest : C.Words[1]); /* "cd My Notes" works unquoted */
    bool Directory = false;
    if (!hz::fs::Exists(Target, &Directory)) {
        Error("The system cannot find the path specified.");
        return;
    }
    if (!Directory) {
        Error("The directory name is invalid.");
        return;
    }
    Cwd_ = Target;
}

void Terminal::Type(const CommandLine &C)
{
    if (C.Words.Size() < 2) {
        Error("The syntax of the command is incorrect.");
        return;
    }
    for (size_t i = 1; i < C.Words.Size(); i++) {
        hz::String File = Path(C.Words[i]);
        hz::String Text;
        STATUS Status = hz::fs::ReadAll(File, Text, 1024 * 1024);
        if (!HZ_SUCCESS(Status)) {
            FileError("Cannot read", File, Status);
            continue;
        }
        Out(Text);
        if (!Text.Empty() && Text[Text.Length() - 1] != '\n')
            Out("\n", 1);
    }
}

void Terminal::CopyFile(const CommandLine &C)
{
    if (C.Words.Size() != 3) {
        Error("The syntax of the command is incorrect. Use: copy <from> <to>");
        return;
    }
    hz::String From = Path(C.Words[1]), To = Path(C.Words[2]);
    bool Directory = false;
    if (hz::fs::Exists(To, &Directory) && Directory)
        To = hz::fs::Join(To, hz::fs::FileName(From).CStr());
    STATUS Status = hz::fs::Copy(From, To);
    if (!HZ_SUCCESS(Status)) {
        FileError("Cannot copy", From, Status);
        return;
    }
    Line("        1 file(s) copied.");
}

void Terminal::DeleteFiles(const CommandLine &C)
{
    if (C.Words.Size() < 2) {
        Error("The syntax of the command is incorrect.");
        return;
    }
    hz::String Target = Path(C.Words[1]);
    hz::String Name = hz::fs::FileName(Target);
    if (!hz::fs::HasWildcards(Name.CStr())) {
        bool Directory = false;
        if (hz::fs::Exists(Target, &Directory) && Directory) {
            Error("That is a folder; use rmdir.");
            return;
        }
        STATUS Status = hz::fs::Delete(Target);
        if (!HZ_SUCCESS(Status))
            FileError("Cannot delete", Target, Status);
        return;
    }
    hz::String Folder = hz::fs::Parent(Target);
    hz::Vector<hz::fs::Entry> Entries;
    STATUS Status = hz::fs::List(Folder, Entries);
    if (!HZ_SUCCESS(Status)) {
        FileError("Cannot list", Folder, Status);
        return;
    }
    size_t Deleted = 0;
    for (const hz::fs::Entry &E : Entries) {
        if (E.Directory || !hz::fs::MatchPattern(Name.CStr(), E.Name.CStr()))
            continue;
        hz::String File = hz::fs::Join(Folder, E.Name.CStr());
        Status = hz::fs::Delete(File);
        if (HZ_SUCCESS(Status))
            Deleted++;
        else
            FileError("Cannot delete", File, Status);
    }
    Line(hz::String::Format("%zu file(s) deleted.", Deleted));
}

void Terminal::MakeDirectory(const CommandLine &C)
{
    if (C.Words.Size() < 2) {
        Error("The syntax of the command is incorrect.");
        return;
    }
    hz::String Target = Path(C.Words[1]);
    STATUS Status = hz::fs::CreateDirectory(Target);
    if (!HZ_SUCCESS(Status))
        FileError("Cannot create", Target, Status);
}

void Terminal::RemoveDirectory(const CommandLine &C)
{
    if (C.Words.Size() < 2) {
        Error("The syntax of the command is incorrect.");
        return;
    }
    hz::String Target = Path(C.Words[1]);
    bool Directory = false;
    if (hz::fs::Exists(Target, &Directory) && !Directory) {
        Error("That is not a folder; use del.");
        return;
    }
    STATUS Status = hz::fs::Delete(Target);
    if (!HZ_SUCCESS(Status))
        FileError("Cannot remove", Target, Status);
}

void Terminal::RenameFile(const CommandLine &C)
{
    if (C.Words.Size() != 3) {
        Error("The syntax of the command is incorrect. Use: ren <path> <new name>");
        return;
    }
    hz::String Target = Path(C.Words[1]);
    STATUS Status = hz::fs::Rename(Target, C.Words[2].CStr());
    if (!HZ_SUCCESS(Status))
        FileError("Cannot rename", Target, Status);
}

void Terminal::Memory()
{
    HZ_SYSTEM_BASIC_INFORMATION Info = {};
    STATUS Status = NtQuerySystemInformation(HZ_SYSTEM_BASIC_INFORMATION_CLASS, &Info, sizeof(Info), nullptr);
    if (!HZ_SUCCESS(Status)) {
        Line(hz::String::Format("Cannot query the system (0x%08X).", (uint32_t)Status));
        return;
    }
    uint64_t Total = Info.TotalPhysicalPages * Info.PageSize, Free = Info.AvailablePages * Info.PageSize;
    Line(hz::String::Format("Physical memory: %s total, %s in use, %s available", hz::fs::FormatSize(Total).CStr(),
                            hz::fs::FormatSize(Total - Free).CStr(), hz::fs::FormatSize(Free).CStr()));
    Line(hz::String::Format("Page size %u bytes; %u processes, %u threads; %u processor(s)", Info.PageSize,
                            Info.ProcessCount, Info.ThreadCount, Info.ProcessorCount));
}

void Terminal::Processes()
{
    static HZ_SYSTEM_PROCESS_INFORMATION Buffer[128];
    uint32_t Returned = 0;
    STATUS Status =
        NtQuerySystemInformation(HZ_SYSTEM_PROCESS_INFORMATION_CLASS, Buffer, sizeof(Buffer), &Returned);
    if (!HZ_SUCCESS(Status)) {
        Line(hz::String::Format("Cannot list the processes (0x%08X).", (uint32_t)Status));
        return;
    }
    size_t Count = Returned / sizeof(HZ_SYSTEM_PROCESS_INFORMATION);
    Line("  PID  PARENT  THREADS  HANDLES     MEMORY    CPU TIME  NAME");
    for (size_t i = 0; i < Count; i++) {
        const HZ_SYSTEM_PROCESS_INFORMATION &P = Buffer[i];
        Line(hz::String::Format("%5u  %6u  %7u  %7u  %9s  %10s  %s", P.ProcessId, P.ParentProcessId, P.ThreadCount,
                                P.HandleCount, hz::fs::FormatSize(P.ResidentPages * 4096).CStr(),
                                FormatDuration(P.CpuTimeUs).CStr(), P.ImageName));
    }
}

void Terminal::Kill(const CommandLine &C)
{
    uint32_t Pid = 0;
    bool Valid = C.Words.Size() == 2 && C.Words[1].Length() > 0;
    bool Numeric = Valid;
    for (size_t i = 0; Numeric && i < C.Words[1].Length(); i++) {
        char D = C.Words[1][i];
        Numeric = D >= '0' && D <= '9' && Pid < 100000000;
        Pid = Pid * 10 + (uint32_t)(D - '0');
    }
    if (Valid && !Numeric) { /* by image name: every process with that name (M11) */
        static HZ_SYSTEM_PROCESS_INFORMATION Buffer[128];
        uint32_t Returned = 0;
        NtQuerySystemInformation(HZ_SYSTEM_PROCESS_INFORMATION_CLASS, Buffer, sizeof(Buffer), &Returned);
        hz::String Name = C.Words[1];
        if (hz::fs::Extension(Name).Empty())
            Name.Append(".exe");
        uint32_t Matches = 0;
        for (size_t i = 0; i < hz::Min<size_t>(Returned / sizeof(Buffer[0]), 128); i++) {
            if (!hz::fs::EqualNames(Buffer[i].ImageName, Name.CStr()))
                continue;
            Matches++;
            CommandLine One;
            One.Words.Push(C.Words[0]);
            One.Words.Push(hz::String::Format("%u", Buffer[i].ProcessId));
            Kill(One);
        }
        if (!Matches)
            Line(hz::String::Format("No process is named %s.", Name.CStr()));
        return;
    }
    if (!Valid) {
        Error("Use: kill <process id or name> (see ps).");
        return;
    }
    if (Pid == (uint32_t)HzCurrentPeb()->ProcessId) {
        Error("That is this terminal; use exit.");
        return;
    }
    HANDLE Process = 0;
    STATUS Status = NtOpenProcess(&Process, PROCESS_TERMINATE, Pid);
    if (HZ_SUCCESS(Status)) {
        Status = NtTerminateProcess(Process, 1);
        NtClose(Process);
    }
    if (HZ_SUCCESS(Status))
        Line(hz::String::Format("Process %u ended.", Pid));
    else if (Status == STATUS_INVALID_CID)
        Line(hz::String::Format("There is no process %u.", Pid));
    else if (Status == STATUS_ACCESS_DENIED)
        Line(hz::String::Format("Process %u cannot be ended (access denied).", Pid));
    else
        Line(hz::String::Format("Cannot end process %u (0x%08X).", Pid, (uint32_t)Status));
}

void Terminal::Start(const CommandLine &C)
{
    if (C.Words.Size() < 2) {
        Error("Use: start <program or file> [arguments]");
        return;
    }
    hz::String Target = C.Words[1];
    hz::String Image = FindProgram(Target);
    if (Image.Empty()) { /* a document? */
        hz::String File = Path(Target);
        hz::String Program;
        uint32_t Pid = 0;
        STATUS Status = hz::apps::OpenPath(File, &Pid, &Program);
        if (HZ_SUCCESS(Status))
            Line(hz::String::Format("Opened %s with %s (process %u).", File.CStr(), Program.CStr(), Pid));
        else if (Status == STATUS_NOT_FOUND && hz::fs::Exists(File))
            Line(hz::String::Format("No program opens %s.", File.CStr()));
        else if (Status == STATUS_DLL_NOT_FOUND || Status == STATUS_NOT_SUPPORTED ||
                 Status == STATUS_INVALID_IMAGE_NOT_MZ || Status == STATUS_INVALID_IMAGE_FORMAT) {
            hz::String Report;
            if (HZ_SUCCESS(hz::apps::QueryCompatibilityReport(File, Report)))
                Line(Report.CStr());
            else
                Line(hz::String::Format("Cannot load %s (0x%08X); its compatibility report could not be read.",
                                        File.CStr(), (uint32_t)Status));
        } else
            Line(hz::String::Format("'%s' is not a program or a file.", Target.CStr()));
        return;
    }
    hz::String CommandText = hz::fs::FileName(Image);
    for (size_t i = 2; i < C.Words.Size(); i++) {
        CommandText.Append(' ');
        CommandText.Append(Quote(C.Words[i]));
    }
    HANDLE Process = 0;
    uint32_t Pid = 0;
    STATUS Status = NtCreateProcess(Image.CStr(), CommandText.CStr(), &Process, &Pid);
    if (!HZ_SUCCESS(Status)) {
        hz::String Report;
        if ((Status == STATUS_DLL_NOT_FOUND || Status == STATUS_NOT_SUPPORTED ||
             Status == STATUS_INVALID_IMAGE_NOT_MZ || Status == STATUS_INVALID_IMAGE_FORMAT) &&
            HZ_SUCCESS(hz::apps::QueryCompatibilityReport(Image, Report)))
            Line(Report.CStr());
        else
            Line(hz::String::Format("Cannot start %s (0x%08X).", Image.CStr(), (uint32_t)Status));
        return;
    }
    NtClose(Process);
    Line(hz::String::Format("Started %s (process %u).", CommandText.CStr(), Pid));
}

void Terminal::Volumes()
{
    Line("Drive  Label         File system  Size        Free        ");
    for (const hz::fs::Volume &V : hz::fs::Volumes()) {
        Line(hz::String::Format("%c:     %s %s %s %s%s", V.Root[0], Pad(V.Label.Empty() ? hz::String("-") : V.Label, 13, false).CStr(),
                                Pad(V.FileSystem, 12, false).CStr(), Pad(hz::fs::FormatSize(V.TotalBytes), 11, false).CStr(),
                                Pad(hz::fs::FormatSize(V.FreeBytes), 11, false).CStr(), V.ReadOnly ? " read-only" : ""));
    }
}

void Terminal::ShowHistory()
{
    const hz::Vector<hz::String> &History = View_->History();
    for (size_t i = 0; i < History.Size(); i++)
        Line(hz::String::Format("%4zu  %s", i + 1, History[i].CStr()));
}

void Terminal::HeadTail(const CommandLine &C, bool Tail)
{
    size_t Count = 10, FileIndex = 1;
    if (C.Words.Size() > 1 && IsSwitch(C.Words[1], "n")) {
        if (C.Words.Size() < 4) {
            Error(Tail ? "Use: tail [-n count] <file>" : "Use: head [-n count] <file>");
            return;
        }
        Count = 0;
        for (size_t i = 0; i < C.Words[2].Length(); i++) {
            char Digit = C.Words[2][i];
            if (Digit < '0' || Digit > '9' || Count > 10000) {
                Error("The line count must be between 0 and 10000.");
                return;
            }
            Count = Count * 10 + (size_t)(Digit - '0');
        }
        if (Count > 10000) {
            Error("The line count must be between 0 and 10000.");
            return;
        }
        FileIndex = 3;
    }
    if (C.Words.Size() != FileIndex + 1) {
        Error(Tail ? "Use: tail [-n count] <file>" : "Use: head [-n count] <file>");
        return;
    }
    hz::String File = Path(C.Words[FileIndex]), Text;
    STATUS Status = hz::fs::ReadAll(File, Text, 1024 * 1024);
    if (!HZ_SUCCESS(Status)) {
        FileError("Cannot read", File, Status);
        return;
    }
    hz::Vector<hz::String> Lines;
    size_t Start = 0;
    for (size_t i = 0; i < Text.Length(); i++) {
        if (Text[i] == '\n') {
            Lines.Push(Text.Substring(Start, i - Start));
            Start = i + 1;
        }
    }
    if (Start < Text.Length())
        Lines.Push(Text.Substring(Start));
    size_t First = Tail && Lines.Size() > Count ? Lines.Size() - Count : 0;
    size_t End = Tail ? Lines.Size() : hz::Min(Count, Lines.Size());
    for (size_t i = First; i < End; i++)
        Line(Lines[i]);
}

void Terminal::WordCount(const CommandLine &C)
{
    if (C.Words.Size() != 2) {
        Error("Use: wc <file>");
        return;
    }
    hz::String File = Path(C.Words[1]), Text;
    STATUS Status = hz::fs::ReadAll(File, Text, 1024 * 1024);
    if (!HZ_SUCCESS(Status)) {
        FileError("Cannot read", File, Status);
        return;
    }
    uint64_t Lines = 0, Words = 0;
    bool InWord = false;
    for (size_t i = 0; i < Text.Length(); i++) {
        char Character = Text[i];
        if (Character == '\n')
            Lines++;
        bool Space = Character == ' ' || Character == '\t' || Character == '\r' || Character == '\n';
        if (!Space && !InWord)
            Words++;
        InWord = !Space;
    }
    Line(hz::String::Format("%llu %llu %zu %s", (unsigned long long)Lines, (unsigned long long)Words,
                            Text.Length(), C.Words[1].CStr()));
}

void Terminal::Grep(const CommandLine &C)
{
    bool IgnoreCase = false, ShowNumbers = false;
    size_t Index = 1;
    while (Index < C.Words.Size()) {
        /* Both conventions are accepted: '-i' as on Unix, '/i' as on Windows. */
        if (IsSwitch(C.Words[Index], "i"))
            IgnoreCase = true;
        else if (IsSwitch(C.Words[Index], "n"))
            ShowNumbers = true;
        else if (IsSwitch(C.Words[Index], "in") || IsSwitch(C.Words[Index], "ni")) {
            IgnoreCase = true;
            ShowNumbers = true;
        } else
            break;
        Index++;
    }
    if (C.Words.Size() < Index + 2) {
        Error("Use: grep [-i|-n] <text> <file>");
        return;
    }
    const hz::String Pattern = C.Words[Index++];
    for (; Index < C.Words.Size(); Index++) {
        hz::String File = Path(C.Words[Index]), Text;
        STATUS Status = hz::fs::ReadAll(File, Text, 1024 * 1024);
        if (!HZ_SUCCESS(Status)) {
            FileError("Cannot read", File, Status);
            continue;
        }
        size_t Start = 0, LineNumber = 1, Matches = 0;
        for (size_t i = 0; i <= Text.Length(); i++) {
            if (i != Text.Length() && Text[i] != '\n')
                continue;
            hz::String Content = Text.Substring(Start, i - Start);
            bool Match = IgnoreCase ? Content.ContainsIgnoreCase(Pattern.CStr())
                                    : Content.Find(Pattern.CStr()) != hz::NotFound;
            if (Match) {
                Line(ShowNumbers ? hz::String::Format("%zu:%s", LineNumber, Content.CStr()) : Content);
                Matches++;
            }
            Start = i + 1;
            LineNumber++;
        }
        if (!Matches)
            Line(hz::String::Format("No matches in %s.", C.Words[Index].CStr()));
    }
}

void Terminal::Touch(const CommandLine &C)
{
    if (C.Words.Size() < 2) {
        Error("Use: touch <file> [file ...]");
        return;
    }
    for (size_t i = 1; i < C.Words.Size(); i++) {
        hz::String File = Path(C.Words[i]);
        bool Directory = false;
        if (hz::fs::Exists(File, &Directory)) {
            if (Directory)
                Line(hz::String::Format("touch: %s is a folder.", C.Words[i].CStr()));
            continue;
        }
        STATUS Status = hz::fs::WriteAll(File, "", 0);
        if (!HZ_SUCCESS(Status))
            FileError("Cannot create", File, Status);
    }
}

void Terminal::Move(const CommandLine &C)
{
    if (C.Words.Size() != 3) {
        Error("Use: mv <from> <to>");
        return;
    }
    hz::String From = Path(C.Words[1]), To = Path(C.Words[2]);
    bool Directory = false;
    if (hz::fs::Exists(To, &Directory) && Directory)
        To = hz::fs::Join(To, hz::fs::FileName(From).CStr());
    STATUS Status = hz::fs::MoveTree(From, To);
    if (!HZ_SUCCESS(Status))
        FileError("Cannot move", From, Status);
}

void Terminal::Stat(const CommandLine &C)
{
    if (C.Words.Size() != 2) {
        Error("Use: stat <path>");
        return;
    }
    hz::String File = Path(C.Words[1]);
    hz::fs::Entry Entry;
    STATUS Status = hz::fs::Stat(File, Entry);
    if (!HZ_SUCCESS(Status)) {
        FileError("Cannot inspect", File, Status);
        return;
    }
    Line(hz::String::Format("  File: %s", File.CStr()));
    Line(hz::String::Format("  Type: %s", Entry.Directory ? "directory" : "file"));
    if (!Entry.Directory)
        Line(hz::String::Format("  Size: %llu bytes", (unsigned long long)Entry.Size));
    Line(hz::String::Format("Modify: %s", hz::fs::FormatTime(Entry.Modified).CStr()));
}

void Terminal::PathPart(const CommandLine &C, bool Parent)
{
    if (C.Words.Size() != 2) {
        Error(Parent ? "Use: dirname <path>" : "Use: basename <path>");
        return;
    }
    hz::String File = Path(C.Words[1]);
    Line(Parent ? hz::fs::Parent(File) : hz::fs::FileName(File));
}

/* Byte order, so 'sort' is predictable: digits, then upper-case, then lower-case. */
static int32_t CompareText(const hz::String &A, const hz::String &B)
{
    size_t Limit = A.Length() < B.Length() ? A.Length() : B.Length();
    for (size_t i = 0; i < Limit; i++) {
        uint8_t X = (uint8_t)A[i], Y = (uint8_t)B[i];
        if (X != Y)
            return X < Y ? -1 : 1;
    }
    if (A.Length() == B.Length())
        return 0;
    return A.Length() < B.Length() ? -1 : 1;
}

/* 'tree [path]': the folders and files under a path, indented. The walk stops at a fixed depth and
   says so, rather than appearing to have listed everything. */
void Terminal::Tree(const CommandLine &C)
{
    hz::String Root = C.Words.Size() > 1 ? Path(C.Words[1]) : Cwd_;
    bool Directory = false;
    if (!hz::fs::Exists(Root, &Directory) || !Directory) {
        Error("tree: the path is not a folder.");
        return;
    }
    constexpr size_t kMaxDepth = 6;
    size_t Folders = 0, Files = 0;
    bool Truncated = false;
    Line(Root);
    hz::Function<void(const hz::String &, const hz::String &, size_t)> Walk;
    Walk = [&](const hz::String &Folder, const hz::String &Prefix, size_t Depth) {
        if (Depth >= kMaxDepth) {
            Truncated = true;
            return;
        }
        hz::Vector<hz::fs::Entry> Entries;
        if (!HZ_SUCCESS(hz::fs::List(Folder, Entries))) {
            Line(hz::String::Format("%s    (cannot be listed)", Prefix.CStr()));
            return;
        }
        for (size_t i = 0; i < Entries.Size(); i++) {
            const hz::fs::Entry &E = Entries[i];
            bool Last = i + 1 == Entries.Size();
            Line(hz::String::Format("%s%s%s", Prefix.CStr(), Last ? "`-- " : "|-- ", E.Name.CStr()));
            if (!E.Directory) {
                Files++;
                continue;
            }
            Folders++;
            hz::String Deeper = hz::String::Format("%s%s", Prefix.CStr(), Last ? "    " : "|   ");
            Walk(hz::fs::Join(Folder, E.Name.CStr()), Deeper, Depth + 1);
        }
    };
    Walk(Root, "", 0);
    Line("");
    Line(hz::String::Format("%zu folder(s), %zu file(s)", Folders, Files));
    if (Truncated)
        Line(hz::String::Format("Stopped at %zu levels deep; deeper folders were not listed.", kMaxDepth));
}

/* 'find [path] <pattern>': every file or folder whose name matches a * and ? pattern. This is the
   Unix spelling of 'dir /s /b', so a Windows habit works too. */
void Terminal::FindFiles(const CommandLine &C)
{
    if (C.Words.Size() < 2) {
        Error("Use: find [path] <pattern>");
        return;
    }
    bool HavePath = C.Words.Size() > 2;
    hz::String Root = HavePath ? Path(C.Words[1]) : Cwd_;
    hz::String Pattern = HavePath ? C.Words[2] : C.Words[1];
    bool Directory = false;
    if (!hz::fs::Exists(Root, &Directory) || !Directory) {
        Error("find: the search path is not a folder.");
        return;
    }
    constexpr size_t kMaxDepth = 8;
    size_t Matches = 0;
    bool Truncated = false;
    hz::Function<void(const hz::String &, size_t)> Walk;
    Walk = [&](const hz::String &Folder, size_t Depth) {
        if (Depth >= kMaxDepth) {
            Truncated = true;
            return;
        }
        hz::Vector<hz::fs::Entry> Entries;
        if (!HZ_SUCCESS(hz::fs::List(Folder, Entries)))
            return;
        for (const hz::fs::Entry &E : Entries) {
            hz::String Full = hz::fs::Join(Folder, E.Name.CStr());
            if (hz::fs::MatchPattern(Pattern.CStr(), E.Name.CStr())) {
                Line(Full);
                Matches++;
            }
            if (E.Directory)
                Walk(Full, Depth + 1);
        }
    };
    Walk(Root, 0);
    Line(hz::String::Format("%zu match(es) for '%s' under %s", Matches, Pattern.CStr(), Root.CStr()));
    if (Truncated)
        Line(hz::String::Format("Stopped at %zu levels deep.", kMaxDepth));
}

/* 'sort [file]': the lines of a file, or of the output captured by a pipe, in byte order. */
void Terminal::SortLines(const CommandLine &C)
{
    hz::String Text;
    if (C.Words.Size() > 1) {
        hz::String File = Path(C.Words[1]);
        STATUS Status = hz::fs::ReadAll(File, Text, 4 * 1024 * 1024);
        if (!HZ_SUCCESS(Status)) {
            FileError("Cannot read", File, Status);
            return;
        }
    } else if (Capturing_) {
        Text = Captured_;
    } else {
        Error("Use: sort <file> (or pipe another command into it)");
        return;
    }
    hz::Vector<hz::String> Lines;
    size_t Start = 0;
    for (size_t i = 0; i <= Text.Length(); i++) {
        if (i != Text.Length() && Text[i] != '\n')
            continue;
        hz::String Line = Text.Substring(Start, i - Start);
        if (!Line.Empty())
            Lines.Push(hz::Move(Line));
        Start = i + 1;
    }
    /* Insertion sort: terminal output is short, and it keeps the comparison in one place. */
    for (size_t i = 1; i < Lines.Size(); i++) {
        hz::String Key = Lines[i];
        size_t j = i;
        while (j > 0 && CompareText(Lines[j - 1], Key) > 0) {
            Lines[j] = hz::Move(Lines[j - 1]);
            j--;
        }
        Lines[j] = hz::Move(Key);
    }
    for (const hz::String &Text2 : Lines)
        Line(Text2);
}

/* 'set' alone lists the variables; 'set NAME=value' stores one and 'set NAME=' clears it.
   'export NAME=value' is accepted as the same thing, so a Unix habit still works. */
void Terminal::SetVariable(const CommandLine &C)
{
    if (C.Words.Size() < 2) {
        ListEnvironment();
        return;
    }
    hz::String Assignment = C.Rest;
    size_t Equals = Assignment.Find("=");
    if (Equals == hz::NotFound) {
        Error("Use: set NAME=value (or set NAME= to clear it)");
        return;
    }
    hz::String Name = Assignment.Substring(0, Equals);
    hz::String Value = Assignment.Substring(Equals + 1, Assignment.Length() - Equals - 1);
    if (Name.Empty()) {
        Error("The variable name is empty.");
        return;
    }
    if (Value.Empty())
        Env_.Unset(Name);
    else
        Env_.Set(Name, Value);
}

void Terminal::ListEnvironment()
{
    for (size_t i = 0; i < Env_.Count(); i++)
        Line(Env_.EntryAt(i));
}

void Terminal::LinuxHelp()
{
    Line("Linux A-Z command names are not a Linux compatibility layer.");
    Line("Native Nexora commands and aliases: cat, cd, clear, cp, df, env, export, find, free, grep,");
    Line("head, history, kill, ls, mkdir, more, mv, printenv, ps, pwd, rm, rmdir, set, sha256sum,");
    Line("sort, stat, tail, touch, tree, uname, unset, uptime, wc, where, which, who, whoami. These");
    Line("use Nexora OS paths, APIs, and permissions.");
    Line("sudo/su, package managers, Linux services, POSIX shell syntax, and device/network");
    Line("administration are unavailable; use 'help' for supported Nexora commands.");
}

bool Terminal::UnavailableLinuxCommand(const hz::String &Name)
{
    static const char *const LinuxCommandNames[] = {
        "&", "alias", "apropos", "apt", "apt-get", "aptitude", "aspell", "at", "awk", "base32", "base64",
        "bash", "bc", "bg", "bind", "break", "builtin", "bzip2", "cal", "caller", "case", "cfdisk", "chattr",
        "chgrp", "chmod", "chown", "chpasswd", "chroot", "chkconfig", "cksum", "clear_console", "cmp", "comm",
        "command", "continue", "cpio", "cron", "crontab", "csplit", "curl", "cut", "dc", "dd", "ddrescue",
        "ddrescuelog", "declare", "diff", "diff3", "dig", "dircolors", "dirs", "dos2unix", "dmesg", "dpkg",
        "du", "eject", "enable", "env", "ethtool", "eval", "exec", "expand", "export", "expr", "fdformat",
        "fdisk", "fg", "file", "find", "fmt", "fold", "for", "format", "fsck", "ftp", "function", "fuser",
        "gawk", "getopts", "getfacl", "groupadd", "groupdel", "groupmod", "groups", "gzip", "hash", "htop",
        "iconv", "if", "ifconfig", "ifdown", "ifup", "import", "install", "iostat", "ip", "iw", "jobs", "join",
        "killall", "klist", "less", "let", "link", "ln", "local", "locate", "login", "logname", "logout", "look",
        "lpc", "lpr", "lprint", "lprintd", "lprintq", "lprm", "lsattr", "lsblk", "lsof", "lspci", "make", "man",
        "mandoc", "mapfile", "md5sum", "cal", "mkfifo", "mkfile", "mkisofs", "mknod", "mktemp", "modprobe", "most",
        "mtools", "mtr", "mmv", "nc", "netstat", "nft", "nice", "nl", "nohup", "notify-send", "nslookup", "op",
        "open", "openssl", "passwd", "paste", "pathchk", "perf", "ping", "pgrep", "pkill", "popd", "pr", "printcap",
        "printenv", "printf", "pushd", "pv", "quota", "quotacheck", "ram", "rar", "rcp", "read", "readarray",
        "readonly", "reboot", "renice", "remsync", "return", "rev", "rg", "rmmod", "rsync", "screen", "scp",
        "sdiff", "sed", "select", "seq", "set", "setfacl", "shift", "shopt", "shuf", "sleep", "slocate", "sntp",
        "sort", "source", "split", "ss", "ssh", "strace", "sum", "suspend", "sync", "tabs", "tac", "tar", "tee",
        "test", "times", "timeout", "tmux", "top", "tput", "traceroute", "trap", "tr", "tsort", "tty", "ulimit",
        "umask", "umount", "unexpand", "uniq", "units", "unix2dos", "unrar", "unset", "unshar", "until", "useradd",
        "userdel", "usermod", "uuencode", "uudecode", "uuidgen", "vi", "vmstat", "w", "wait", "watch", "wget",
        "wl-copy", "wl-paste", "while", "write", "xargs", "xclip", "xdg-open", "xxd", "xz", "yes", "zip", ".",
        "!!", "#",
    };
    for (const char *Command : LinuxCommandNames)
        if (Name == Command) {
            Line(hz::String::Format("'%s' is listed in the Linux command reference but is not implemented in Nexora OS.",
                                    Name.CStr()));
            Line("This build has no Linux userspace/ELF ABI or compatible subsystem for that command.");
            return true;
        }
    return false;
}

/* ---- Programs ----------------------------------------------------------------------------------- */

hz::String Terminal::FindProgram(const hz::String &Name) const
{
    hz::String WithExe = Name;
    hz::String Extension = hz::fs::Extension(Name);
    if (hz::fs::EqualNames(Extension.CStr(), "nxe"))
        WithExe = hz::apps::ResolveToolImageName(Name);
    else if (Extension.Empty())
        WithExe.Append(".exe");
    else if (!hz::fs::EqualNames(Extension.CStr(), "exe"))
        return hz::String();
    bool HasPath = WithExe.Find("\\") != hz::NotFound || WithExe.Find(":") != hz::NotFound;
    hz::String Candidate = Path(WithExe);
    bool Directory = false;
    if (hz::fs::Exists(Candidate, &Directory) && !Directory)
        return Candidate;
    if (HasPath)
        return hz::String();
    /* PATH is a semicolon-separated list, matching the Windows command convention. */
    hz::String SearchPath;
    if (Env_.Get("PATH", &SearchPath)) {
        size_t Begin = 0;
        while (Begin <= SearchPath.Length()) {
            size_t End = SearchPath.Find(";", Begin);
            if (End == hz::NotFound)
                End = SearchPath.Length();
            hz::String Folder = SearchPath.Substring(Begin, End - Begin);
            if (!Folder.Empty()) {
                Candidate = hz::fs::Join(Folder, WithExe.CStr());
                if (hz::fs::Exists(Candidate, &Directory) && !Directory)
                    return Candidate;
            }
            if (End == SearchPath.Length())
                break;
            Begin = End + 1;
        }
    }
    /* Keep built-in programs reachable even if PATH was cleared or edited incorrectly. */
    Candidate = hz::fs::Join(hz::String(kSystemFolder), WithExe.CStr());
    if (hz::fs::Exists(Candidate, &Directory) && !Directory)
        return Candidate;
    return hz::String();
}

bool Terminal::StartChild(const hz::String &Image, const CommandLine &C)
{
    hz::String CommandText = hz::fs::FileName(Image);
    for (size_t i = 1; i < C.Words.Size(); i++) {
        CommandText.Append(' ');
        CommandText.Append(Quote(C.Words[i]));
    }
    /* Windowed programs (those with a manifest) run on their own. */
    hz::Vector<hz::apps::App> Apps = hz::apps::Load();
    if (const hz::apps::App *App = hz::apps::FindByImage(Apps, Image.CStr())) {
        HANDLE Process = 0;
        uint32_t Pid = 0;
        STATUS Status = NtCreateProcess(Image.CStr(), CommandText.CStr(), &Process, &Pid);
        if (HZ_SUCCESS(Status)) {
            NtClose(Process);
            Line(hz::String::Format("Started %s (process %u).", App->Name.CStr(), Pid));
        } else {
            hz::String Report;
            if ((Status == STATUS_DLL_NOT_FOUND || Status == STATUS_NOT_SUPPORTED ||
                 Status == STATUS_INVALID_IMAGE_NOT_MZ || Status == STATUS_INVALID_IMAGE_FORMAT) &&
                HZ_SUCCESS(hz::apps::QueryCompatibilityReport(Image, Report)))
                Line(Report.CStr());
            else
                Line(hz::String::Format("Cannot start %s (0x%08X).", Image.CStr(), (uint32_t)Status));
        }
        return false;
    }
    HANDLE Read = 0, Write = 0;
    STATUS Status = NtCreatePipe(&Read, &Write, 0);
    if (!HZ_SUCCESS(Status)) {
        Line(hz::String::Format("Cannot create a pipe (0x%08X).", (uint32_t)Status));
        return false;
    }
    HZ_PROCESS_PARAMETERS Parameters = {};
    Parameters.StandardOutput = Write;
    Parameters.StandardError = Write;
    HANDLE Process = 0;
    uint32_t Pid = 0;
    Status = NtCreateProcessEx(Image.CStr(), CommandText.CStr(), &Parameters, &Process, &Pid);
    NtClose(Write); /* the child has its own handle; end of file comes when it exits */
    if (!HZ_SUCCESS(Status)) {
        NtClose(Read);
        hz::String Report;
        if ((Status == STATUS_DLL_NOT_FOUND || Status == STATUS_NOT_SUPPORTED ||
             Status == STATUS_INVALID_IMAGE_NOT_MZ || Status == STATUS_INVALID_IMAGE_FORMAT) &&
            HZ_SUCCESS(hz::apps::QueryCompatibilityReport(Image, Report)))
            Line(Report.CStr());
        else
            Line(hz::String::Format("Cannot start %s (0x%08X).", Image.CStr(), (uint32_t)Status));
        return false;
    }
    Reader_ = new ReaderContext{Read, Process, Main_->Handle()};
    HANDLE Thread = 0;
    Status = HzCreateThread(ReaderThread, Reader_, &Thread, nullptr);
    if (!HZ_SUCCESS(Status)) {
        NtTerminateProcess(Process, kInterruptStatus);
        NtClose(Process);
        NtClose(Read);
        delete Reader_;
        Reader_ = nullptr;
        Line(hz::String::Format("Cannot start the reader thread (0x%08X).", (uint32_t)Status));
        return false;
    }
    Process_ = Process;
    Read_ = Read;
    Thread_ = Thread;
    ProcessId_ = Pid;
    ProcessName_ = hz::fs::FileName(Image);
    HzLog("terminal: running %s (process %u)", CommandText.CStr(), Pid);
    return true;
}

void Terminal::OnOutput(hz::String *Chunk)
{
    Out(Chunk->CStr(), Chunk->Length());
    delete Chunk;
}

void Terminal::OnExited(STATUS Status)
{
    if (!Process_)
        return;
    NtWaitForSingleObject(Thread_, 0, nullptr);
    NtClose(Thread_);
    NtClose(Read_);
    NtClose(Process_);
    delete Reader_;
    Reader_ = nullptr;
    Process_ = Read_ = Thread_ = 0;
    if (!Capturing_ && !LogLine_.Empty()) /* the output did not end with a line feed */
        Out("\n", 1);
    /* "[exit code N]" goes to the screen even when the output is redirected. */
    bool WasCapturing = Capturing_;
    Capturing_ = false;
    if (Status == kInterruptStatus)
        Line(hz::String::Format("[%s ended]", ProcessName_.CStr()));
    else
        Line(((uint32_t)Status & 0xC0000000u) == 0xC0000000u
                 ? hz::String::Format("[ended by an error: 0x%08X]", (uint32_t)Status)
                 : hz::String::Format("[exit code %d]", (int)Status));
    Capturing_ = WasCapturing;
    FinishCommand();
}

/* ---- Completion ----------------------------------------------------------------------------------- */

hz::String Terminal::Complete(const hz::String &Input)
{
    /* Complete the last word as a file or folder name in its folder. */
    size_t Start = Input.Length();
    while (Start > 0 && Input[Start - 1] != ' ')
        Start--;
    hz::String Word = Input.Substring(Start);
    hz::String Full = Path(Word.Empty() ? hz::String(".") : Word);
    hz::String Folder = Word.Empty() || Word[Word.Length() - 1] == '\\' ? Full : hz::fs::Parent(Full);
    hz::String Prefix = Word.Empty() || Word[Word.Length() - 1] == '\\' ? hz::String() : hz::fs::FileName(Full);
    hz::Vector<hz::fs::Entry> Entries;
    if (!HZ_SUCCESS(hz::fs::List(Folder, Entries)))
        return Input;
    hz::String Match;
    bool Directory = false;
    size_t Matches = 0;
    for (const hz::fs::Entry &E : Entries) {
        if (E.Name.Length() < Prefix.Length())
            continue;
        if (!hz::fs::EqualNames(E.Name.Substring(0, Prefix.Length()).CStr(), Prefix.CStr()))
            continue;
        if (Matches == 0) {
            Match = E.Name;
            Directory = E.Directory;
        } else { /* keep the common prefix */
            size_t Common = 0;
            while (Common < Match.Length() && Common < E.Name.Length() &&
                   hz::fs::EqualNames(Match.Substring(Common, 1).CStr(), E.Name.Substring(Common, 1).CStr()))
                Common++;
            Match = Match.Substring(0, Common);
        }
        Matches++;
    }
    if (Matches == 0)
        return Input;
    size_t Keep = Word.Length() - Prefix.Length(); /* the folder part as typed */
    hz::String Out = Input.Substring(0, Start);
    Out.Append(Word.Substring(0, Keep));
    Out.Append(Match);
    if (Matches == 1 && Directory)
        Out.Append('\\');
    return Out;
}

}  // namespace

}  // namespace term

extern "C" int main(int argc, char **argv)
{
    term::Terminal *App = new term::Terminal();
    if (!App->Create()) {
        HzPrint("terminal: cannot create the window");
        return 1;
    }
    HzPrint("terminal: started");
    return hzui::RunMessageLoop();
}
