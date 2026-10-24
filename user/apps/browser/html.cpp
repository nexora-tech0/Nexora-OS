#include "html.h"

namespace nexora::browser {
namespace {

bool IsSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

char Lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
}

bool EqualsIgnoreCase(const char *left, size_t leftLength, const char *right)
{
    size_t rightLength = 0;
    while (right[rightLength])
        ++rightLength;
    if (leftLength != rightLength)
        return false;
    for (size_t i = 0; i < leftLength; ++i)
        if (Lower(left[i]) != right[i])
            return false;
    return true;
}

void Append(char *output, size_t capacity, size_t *length, char value, bool *truncated)
{
    if (*length + 1 >= capacity) {
        *truncated = true;
        return;
    }
    output[(*length)++] = value;
    output[*length] = 0;
}

void AppendText(HtmlDocument *document, char value, bool *pendingSpace, bool inTitle, HtmlLink *link)
{
    if (IsSpace(value)) {
        *pendingSpace = true;
        return;
    }
    if ((unsigned char)value < 0x20 || (unsigned char)value == 0x7f)
        return;
    if (*pendingSpace) {
        if (inTitle) {
            if (document->Title[0])
                Append(document->Title, sizeof(document->Title), &document->TitleLength, ' ', &document->Truncated);
        } else if (document->TextLength && document->Text[document->TextLength - 1] != '\n') {
            Append(document->Text, sizeof(document->Text), &document->TextLength, ' ', &document->Truncated);
        }
        if (link && link->Text[0])
            Append(link->Text, sizeof(link->Text), &link->TextLength, ' ', &document->Truncated);
        *pendingSpace = false;
    }
    if (inTitle) {
        Append(document->Title, sizeof(document->Title), &document->TitleLength, value, &document->Truncated);
    } else {
        Append(document->Text, sizeof(document->Text), &document->TextLength, value, &document->Truncated);
    }
    if (link)
        Append(link->Text, sizeof(link->Text), &link->TextLength, value, &document->Truncated);
}

void AppendBreak(HtmlDocument *document)
{
    if (!document->TextLength || document->Text[document->TextLength - 1] == '\n')
        return;
    Append(document->Text, sizeof(document->Text), &document->TextLength, '\n', &document->Truncated);
}

bool IsBlockTag(const char *name, size_t length)
{
    return EqualsIgnoreCase(name, length, "p") || EqualsIgnoreCase(name, length, "div") ||
           EqualsIgnoreCase(name, length, "section") || EqualsIgnoreCase(name, length, "article") ||
           EqualsIgnoreCase(name, length, "header") || EqualsIgnoreCase(name, length, "footer") ||
           EqualsIgnoreCase(name, length, "li") || EqualsIgnoreCase(name, length, "h1") ||
           EqualsIgnoreCase(name, length, "h2") || EqualsIgnoreCase(name, length, "h3") ||
           EqualsIgnoreCase(name, length, "h4") || EqualsIgnoreCase(name, length, "h5") ||
           EqualsIgnoreCase(name, length, "h6") || EqualsIgnoreCase(name, length, "tr");
}

bool IsNameCharacter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == ':';
}

void DecodeEntity(const char *source, size_t length, size_t *position, HtmlDocument *document,
                  bool *pendingSpace, bool inTitle, HtmlLink *link)
{
    size_t end = *position + 1;
    while (end < length && end - *position <= 12 && source[end] != ';' && source[end] != '<' &&
           !IsSpace(source[end]))
        ++end;
    if (end >= length || source[end] != ';' || end - *position > 12) {
        AppendText(document, source[(*position)++], pendingSpace, inTitle, link);
        return;
    }

    const char *entity = source + *position + 1;
    size_t entityLength = end - *position - 1;
    char decoded = 0;
    if (EqualsIgnoreCase(entity, entityLength, "amp"))
        decoded = '&';
    else if (EqualsIgnoreCase(entity, entityLength, "lt"))
        decoded = '<';
    else if (EqualsIgnoreCase(entity, entityLength, "gt"))
        decoded = '>';
    else if (EqualsIgnoreCase(entity, entityLength, "quot"))
        decoded = '"';
    else if (EqualsIgnoreCase(entity, entityLength, "apos") || EqualsIgnoreCase(entity, entityLength, "#39"))
        decoded = '\'';
    else if (EqualsIgnoreCase(entity, entityLength, "nbsp"))
        decoded = ' ';
    if (decoded) {
        AppendText(document, decoded, pendingSpace, inTitle, link);
    } else {
        document->UnsupportedContent = true;
        for (size_t k = *position; k <= end; ++k)
            AppendText(document, source[k], pendingSpace, inTitle, link);
    }
    *position = end + 1;
}

bool HasStyleAttribute(const char *source, size_t cursor, size_t end)
{
    while (cursor < end) {
        while (cursor < end && (IsSpace(source[cursor]) || source[cursor] == '/'))
            ++cursor;
        size_t nameStart = cursor;
        while (cursor < end && IsNameCharacter(source[cursor]))
            ++cursor;
        size_t nameLength = cursor - nameStart;
        if (!nameLength) {
            ++cursor;
            continue;
        }
        if (EqualsIgnoreCase(source + nameStart, nameLength, "style"))
            return true;
        while (cursor < end && IsSpace(source[cursor]))
            ++cursor;
        if (cursor >= end || source[cursor] != '=')
            continue;
        ++cursor;
        while (cursor < end && IsSpace(source[cursor]))
            ++cursor;
        if (cursor < end && (source[cursor] == '\'' || source[cursor] == '"')) {
            char delimiter = source[cursor++];
            while (cursor < end && source[cursor] != delimiter)
                ++cursor;
            if (cursor < end)
                ++cursor;
        } else {
            while (cursor < end && !IsSpace(source[cursor]) && source[cursor] != '/')
                ++cursor;
        }
    }
    return false;
}

bool FindClosingTag(const char *source, size_t length, size_t start, const char *name, size_t nameLength,
                    size_t *afterClosing)
{
    for (size_t i = start; i + nameLength + 3 <= length; ++i) {
        if (source[i] != '<' || source[i + 1] != '/')
            continue;
        if (!EqualsIgnoreCase(source + i + 2, nameLength, name))
            continue;
        size_t end = i + 2 + nameLength;
        if (end < length && (IsSpace(source[end]) || source[end] == '>')) {
            while (end < length && source[end] != '>')
                ++end;
            if (end < length) {
                *afterClosing = end + 1;
                return true;
            }
        }
    }
    return false;
}

}

bool ParseHtml(const char *source, size_t length, HtmlDocument *document)
{
    if (!source || !document || length > HtmlMaximumSource)
        return false;
    *document = HtmlDocument{};
    bool pendingSpace = false;
    bool inTitle = false;
    HtmlLink *activeLink = nullptr;
    size_t i = 0;

    while (i < length && !document->Truncated) {
        if (source[i] == '&') {
            DecodeEntity(source, length, &i, document, &pendingSpace, inTitle, activeLink);
            continue;
        }
        if (source[i] != '<') {
            AppendText(document, source[i++], &pendingSpace, inTitle, activeLink);
            continue;
        }
        if (i + 3 < length && source[i + 1] == '!' && source[i + 2] == '-' && source[i + 3] == '-') {
            i += 4;
            while (i + 2 < length && !(source[i] == '-' && source[i + 1] == '-' && source[i + 2] == '>'))
                ++i;
            i = i + 2 < length ? i + 3 : length;
            continue;
        }

        size_t tagEnd = i + 1;
        char quote = 0;
        while (tagEnd < length) {
            char c = source[tagEnd];
            if (quote) {
                if (c == quote)
                    quote = 0;
            } else if (c == '\'' || c == '"') {
                quote = c;
            } else if (c == '>') {
                break;
            }
            ++tagEnd;
        }
        if (tagEnd >= length) {
            document->UnsupportedContent = true;
            break;
        }

        size_t cursor = i + 1;
        while (cursor < tagEnd && IsSpace(source[cursor]))
            ++cursor;
        if (cursor < tagEnd && (source[cursor] == '!' || source[cursor] == '?')) {
            i = tagEnd + 1;
            continue;
        }
        bool closing = cursor < tagEnd && source[cursor] == '/';
        if (closing)
            ++cursor;
        while (cursor < tagEnd && IsSpace(source[cursor]))
            ++cursor;
        size_t nameStart = cursor;
        while (cursor < tagEnd && IsNameCharacter(source[cursor]))
            ++cursor;
        size_t nameLength = cursor - nameStart;
        if (!nameLength) {
            document->UnsupportedContent = true;
            i = tagEnd + 1;
            continue;
        }
        const char *name = source + nameStart;
        if (HasStyleAttribute(source, cursor, tagEnd))
            document->UnsupportedContent = true;

        if (EqualsIgnoreCase(name, nameLength, "script") || EqualsIgnoreCase(name, nameLength, "style")) {
            document->UnsupportedContent = true;
            if (!closing) {
                size_t afterClosing;
                if (FindClosingTag(source, length, tagEnd + 1, name, nameLength, &afterClosing)) {
                    i = afterClosing;
                    continue;
                }
                break;
            }
        } else if (EqualsIgnoreCase(name, nameLength, "title")) {
            inTitle = !closing;
            pendingSpace = false;
        } else if (EqualsIgnoreCase(name, nameLength, "br") || EqualsIgnoreCase(name, nameLength, "hr")) {
            if (!inTitle)
                AppendBreak(document);
        } else if (EqualsIgnoreCase(name, nameLength, "img") || EqualsIgnoreCase(name, nameLength, "link")) {
            document->UnsupportedContent = true;
        } else if (EqualsIgnoreCase(name, nameLength, "a")) {
            if (closing) {
                activeLink = nullptr;
            } else if (document->LinkCount < HtmlMaximumLinks) {
                HtmlLink *link = &document->Links[document->LinkCount];
                while (cursor < tagEnd) {
                    while (cursor < tagEnd && (IsSpace(source[cursor]) || source[cursor] == '/'))
                        ++cursor;
                    size_t attributeStart = cursor;
                    while (cursor < tagEnd && IsNameCharacter(source[cursor]))
                        ++cursor;
                    size_t attributeLength = cursor - attributeStart;
                    if (!attributeLength) {
                        ++cursor;
                        continue;
                    }
                    while (cursor < tagEnd && IsSpace(source[cursor]))
                        ++cursor;
                    if (cursor >= tagEnd || source[cursor] != '=') {
                        continue;
                    }
                    ++cursor;
                    while (cursor < tagEnd && IsSpace(source[cursor]))
                        ++cursor;
                    char delimiter = 0;
                    if (cursor < tagEnd && (source[cursor] == '\'' || source[cursor] == '"'))
                        delimiter = source[cursor++];
                    size_t valueStart = cursor;
                    while (cursor < tagEnd && (delimiter ? source[cursor] != delimiter : !IsSpace(source[cursor]) &&
                                                                                           source[cursor] != '/'))
                        ++cursor;
                    if (EqualsIgnoreCase(source + attributeStart, attributeLength, "href")) {
                        size_t valueLength = cursor - valueStart;
                        if (valueLength < sizeof(link->Href)) {
                            for (size_t k = 0; k < valueLength; ++k)
                                link->Href[k] = source[valueStart + k];
                            link->Href[valueLength] = 0;
                        } else {
                            document->Truncated = true;
                        }
                    }
                    if (delimiter && cursor < tagEnd)
                        ++cursor;
                }
                activeLink = link;
                document->LinkCount++;
            } else {
                document->UnsupportedContent = true;
            }
        } else if (IsBlockTag(name, nameLength)) {
            if (!inTitle)
                AppendBreak(document);
        } else if (!EqualsIgnoreCase(name, nameLength, "html") && !EqualsIgnoreCase(name, nameLength, "head") &&
                   !EqualsIgnoreCase(name, nameLength, "body") && !EqualsIgnoreCase(name, nameLength, "meta") &&
                   !EqualsIgnoreCase(name, nameLength, "span") && !EqualsIgnoreCase(name, nameLength, "strong") &&
                   !EqualsIgnoreCase(name, nameLength, "b") && !EqualsIgnoreCase(name, nameLength, "em") &&
                   !EqualsIgnoreCase(name, nameLength, "i") && !EqualsIgnoreCase(name, nameLength, "small") &&
                   !EqualsIgnoreCase(name, nameLength, "ul") && !EqualsIgnoreCase(name, nameLength, "ol") &&
                   !EqualsIgnoreCase(name, nameLength, "main") && !EqualsIgnoreCase(name, nameLength, "nav")) {
            document->UnsupportedContent = true;
        }
        i = tagEnd + 1;
    }

    while (document->TextLength && (document->Text[document->TextLength - 1] == ' ' ||
                                     document->Text[document->TextLength - 1] == '\n'))
        document->Text[--document->TextLength] = 0;
    return true;
}

}
