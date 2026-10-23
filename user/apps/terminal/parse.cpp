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
