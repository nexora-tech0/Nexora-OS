/*
 * Nexora OS - user/apps/terminal/parse.h
 * Command-line parsing for the terminal, apart from the window so apptest.exe can check it.
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

}  // namespace term

#endif /* TERMINAL_PARSE_H */
