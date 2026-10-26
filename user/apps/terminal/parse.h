/*
 * Nexora OS - user/apps/terminal/parse.h
 * Command-line parsing for the terminal, kept apart from the window so it can be exercised on its own.
 * Words are split at spaces and tabs; "double quotes" group words (the quotes are removed);
 * '>' or '>>' outside quotes redirects the output to a file (replace or append).
 */
#ifndef TERMINAL_PARSE_H
#define TERMINAL_PARSE_H

#include "hzcore.h"

namespace term {

struct CommandLine {
    hz::Vector<hz::String> Words;  /* Words[0] is the command */
    hz::String Rest;               /* the text after the command, as typed (without the redirection) */
    hz::String Redirect;           /* file name, or "" */
    bool Append = false;           /* '>>' */
    bool More = false;             /* '| more': page the output (M11; the only pipe there is) */
    bool Valid = true;             /* false: an unterminated quote or a '>' without a file */
    hz::String Error;
};

CommandLine Parse(const char *Line);

/* Lower-case ASCII copy (command names compare case-insensitively). */
hz::String Lower(const hz::String &Text);

/* Quotes a word for a child's command line if it contains a space. */
hz::String Quote(const hz::String &Word);

/*
 * The terminal's variables, so one line can use either convention: 'set NAME=value' (also
 * 'export'), then '%NAME%' (Windows) or $NAME / ${NAME} (Unix) are replaced before the line is
 * parsed. Names compare case-insensitively, as on Windows, because command names already do.
 * A name that is not set is left exactly as it was typed instead of quietly becoming empty, so a
 * misspelling is visible rather than turning into a broken path.
 */
class Environment {
public:
    bool Get(const hz::String &Name, hz::String *Value) const;
    void Set(const hz::String &Name, const hz::String &Value);
    void Unset(const hz::String &Name);
    void Clear() { Entries_.Clear(); }
    size_t Count() const { return Entries_.Size(); }
    const hz::String &EntryAt(size_t Index) const { return Entries_[Index]; } /* "NAME=value" */

private:
    size_t Find(const hz::String &Name) const; /* hz::NotFound when unset */
    hz::Vector<hz::String> Entries_;
};

/* Replaces the references above. '\$' and '\%' give a literal character. */
hz::String ExpandVariables(const char *Text, const Environment &Env);

/* True when Word is the option Name in either style: "-i" or "/i" (Windows accepts both). */
bool IsSwitch(const hz::String &Word, const char *Name);

}  // namespace term

#endif /* TERMINAL_PARSE_H */
