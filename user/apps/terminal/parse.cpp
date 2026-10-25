/*
 * Nexora OS - user/apps/terminal/parse.cpp
 * Command-line parsing for the terminal; see parse.h.
 */
#include "parse.h"

namespace term {

namespace {

bool IsSpace(char C)
{
    return C == ' ' || C == '\t';
}

void Trim(hz::String &Text)
{
    size_t Start = 0, End = Text.Length();
    while (Start < End && IsSpace(Text[Start]))
        Start++;
    while (End > Start && IsSpace(Text[End - 1]))
        End--;
    Text = Text.Substring(Start, End - Start);
}

}  // namespace

hz::String Lower(const hz::String &Text)
{
    hz::String Out;
    for (size_t i = 0; i < Text.Length(); i++) {
        char C = Text[i];
        Out.Append(C >= 'A' && C <= 'Z' ? (char)(C + 32) : C);
    }
    return Out;
}

hz::String Quote(const hz::String &Word)
{
    bool Space = Word.Empty();
    for (size_t i = 0; i < Word.Length(); i++)
        Space = Space || IsSpace(Word[i]);
    if (!Space)
        return Word;
    hz::String Out("\"");
    Out.Append(Word);
    Out.Append('"');
    return Out;
}

/* ---- Variables -------------------------------------------------------------------------------- */

namespace {

bool IsNameStart(char C)
{
    return (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || C == '_';
}

bool IsNameChar(char C)
{
    return IsNameStart(C) || (C >= '0' && C <= '9');
}

}  // namespace

size_t Environment::Find(const hz::String &Name) const
{
    if (Name.Empty())
        return hz::NotFound;
    hz::String Wanted = Lower(Name);
    for (size_t i = 0; i < Entries_.Size(); i++) {
        const hz::String &Entry = Entries_[i];
        size_t Equals = Entry.Find("=");
        if (Equals == hz::NotFound)
            continue;
        if (Lower(Entry.Substring(0, Equals)) == Wanted)
            return i;
    }
    return hz::NotFound;
}

bool Environment::Get(const hz::String &Name, hz::String *Value) const
{
    size_t Index = Find(Name);
    if (Index == hz::NotFound)
        return false;
    const hz::String &Entry = Entries_[Index];
    size_t Equals = Entry.Find("=");
    if (Value)
        *Value = Entry.Substring(Equals + 1, Entry.Length() - Equals - 1);
    return true;
}

void Environment::Set(const hz::String &Name, const hz::String &Value)
{
    if (Name.Empty())
        return;
    hz::String Entry = hz::String::Format("%s=%s", Name.CStr(), Value.CStr());
    size_t Index = Find(Name);
    if (Index == hz::NotFound)
        Entries_.Push(hz::Move(Entry));
    else
        Entries_[Index] = hz::Move(Entry);
}

void Environment::Unset(const hz::String &Name)
{
    size_t Index = Find(Name);
    if (Index != hz::NotFound)
        Entries_.Erase(Index);
}

hz::String ExpandVariables(const char *Text, const Environment &Env)
{
    hz::String Result;
    size_t Length = strlen(Text);
    for (size_t i = 0; i < Length;) {
        char C = Text[i];
        /* '\$' and '\%': a literal character, so a file called "100%.txt" can be typed. */
        if (C == '\\' && i + 1 < Length && (Text[i + 1] == '$' || Text[i + 1] == '%')) {
            Result.Append(Text[i + 1]);
            i += 2;
            continue;
        }
        if (C == '$' && i + 1 < Length && (IsNameStart(Text[i + 1]) || Text[i + 1] == '{')) {
            bool Braced = Text[i + 1] == '{';
            size_t Start = i + (Braced ? 2 : 1), End = Start;
            while (End < Length && IsNameChar(Text[End]))
                End++;
            if (Braced && (End >= Length || Text[End] != '}')) {
                Result.Append(C); /* "$ {" is not a reference */
                i++;
                continue;
            }
            hz::String Value;
            if (End > Start && Env.Get(hz::String(Text + Start, End - Start), &Value)) {
                Result.Append(Value);
                i = Braced ? End + 1 : End;
                continue;
            }
            Result.Append(C);
            i++;
            continue;
        }
        if (C == '%' && i + 1 < Length) {
            size_t End = i + 1;
            while (End < Length && IsNameChar(Text[End]))
                End++;
            if (End > i + 1 && End < Length && Text[End] == '%') {
                hz::String Value;
                if (Env.Get(hz::String(Text + i + 1, End - i - 1), &Value)) {
                    Result.Append(Value);
                    i = End + 1;
                    continue;
                }
            }
            Result.Append(C);
            i++;
            continue;
        }
        Result.Append(C);
        i++;
    }
    return Result;
}

bool IsSwitch(const hz::String &Word, const char *Name)
{
    if (Word.Length() < 2)
        return false;
    char Lead = Word[0];
    if (Lead != '-' && Lead != '/')
        return false;
    return Lower(Word.Substring(1, Word.Length() - 1)) == Name;
}

CommandLine Parse(const char *Line)
{
    CommandLine Result;
    size_t Length = strlen(Line);
    /* First find a redirection or '| more' outside quotes. */
    size_t CommandEnd = Length;
    bool Quoted = false;
    for (size_t i = 0; i < Length; i++) {
        if (Line[i] == '"')
            Quoted = !Quoted;
        else if (Line[i] == '|' && !Quoted) {
            CommandEnd = i;
            hz::String Target(Line + i + 1, Length - i - 1);
            Trim(Target);
            if (Lower(Target) != "more") {
                Result.Valid = false;
                Result.Error = "Only '| more' can follow a command.";
                return Result;
            }
            Result.More = true;
            break;
        } else if (Line[i] == '>' && !Quoted) {
            CommandEnd = i;
            size_t j = i + 1;
            if (j < Length && Line[j] == '>') {
                Result.Append = true;
                j++;
            }
            hz::String Target(Line + j, Length - j);
            Trim(Target);
            if (Target.Length() >= 2 && Target[0] == '"' && Target[Target.Length() - 1] == '"')
                Target = Target.Substring(1, Target.Length() - 2);
            if (Target.Empty() || Target.Find(">") != hz::NotFound) {
                Result.Valid = false;
                Result.Error = "The syntax of the redirection is incorrect.";
                return Result;
            }
            Result.Redirect = Target;
            break;
        }
    }
    if (Quoted) {
        Result.Valid = false;
        Result.Error = "A quote is not closed.";
        return Result;
    }
    /* Then split the command part into words. */
    size_t i = 0;
    while (i < CommandEnd) {
        while (i < CommandEnd && IsSpace(Line[i]))
            i++;
        if (i >= CommandEnd)
            break;
        hz::String Word;
        bool InQuote = false;
        while (i < CommandEnd && (InQuote || !IsSpace(Line[i]))) {
            if (Line[i] == '"')
                InQuote = !InQuote;
            else
                Word.Append(Line[i]);
            i++;
        }
        if (Result.Words.Empty()) {
            size_t RestStart = i;
            while (RestStart < CommandEnd && IsSpace(Line[RestStart]))
                RestStart++;
            Result.Rest = hz::String(Line + RestStart, CommandEnd - RestStart);
            /* Trailing spaces before a redirection are not part of the text. */
            size_t End = Result.Rest.Length();
            while (End > 0 && IsSpace(Result.Rest[End - 1]))
                End--;
            Result.Rest = Result.Rest.Substring(0, End);
        }
        Result.Words.Push(hz::Move(Word));
    }
    return Result;
}

}  // namespace term
