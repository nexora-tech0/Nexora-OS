#include "http.h"

namespace nexora::browser {
namespace {

char Lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
}

bool EqualIgnoreCase(const char *left, size_t leftLength, const char *right)
{
    size_t rightLength = 0;
    while (right[rightLength])
        ++rightLength;
    if (leftLength != rightLength)
        return false;
    for (size_t i = 0; i < leftLength; ++i)
        if (Lower(left[i]) != Lower(right[i]))
            return false;
    return true;
}

bool IsSpace(char c)
{
    return c == ' ' || c == '\t';
}

bool IsTokenCharacter(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           c == '!' || c == '#' || c == '$' || c == '%' || c == '&' || c == '\'' || c == '*' ||
           c == '+' || c == '-' || c == '.' || c == '^' || c == '_' || c == '`' || c == '|' || c == '~';
}

bool FindCrlf(const char *text, size_t length, size_t start, size_t *end)
{
    for (size_t i = start; i + 1 < length; ++i) {
        if (text[i] == '\r' && text[i + 1] == '\n') {
            *end = i;
            return true;
        }
        if (text[i] == '\n' || text[i] == '\r')
            return false;
    }
    return false;
}

bool ParseDecimal(const char *text, size_t length, size_t maximum, size_t *value)
{
    if (!length)
        return false;
    size_t result = 0;
    for (size_t i = 0; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9')
            return false;
        size_t digit = (size_t)(text[i] - '0');
        if (result > (maximum - digit) / 10)
            return false;
        result = result * 10 + digit;
    }
    *value = result;
    return true;
}

int Hex(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool Append(char *output, size_t capacity, size_t *length, char value)
{
    if (*length + 1 >= capacity)
        return false;
    output[(*length)++] = value;
    output[*length] = 0;
    return true;
}

bool AppendBytes(char *output, size_t capacity, size_t *length, const char *source, size_t count)
{
    if (count >= capacity || *length >= capacity - count)
        return false;
    for (size_t i = 0; i < count; ++i)
        output[(*length)++] = source[i];
    output[*length] = 0;
    return true;
}

void Trim(const char **value, size_t *length)
{
    while (*length && IsSpace((*value)[0])) {
        ++*value;
        --*length;
    }
    while (*length && IsSpace((*value)[*length - 1]))
        --*length;
}

HttpParseError ParseChunked(const char *wire, size_t wireLength, size_t position, char *body,
                            size_t bodyCapacity, size_t *bodyLength)
{
    size_t outputLength = 0;
    size_t trailerBytes = 0;
    size_t trailerCount = 0;
    for (;;) {
        size_t lineEnd = 0;
        if (!FindCrlf(wire, wireLength, position, &lineEnd))
            return wireLength - position > 128 ? HttpParseError::InvalidFraming : HttpParseError::Incomplete;
        if (lineEnd == position || lineEnd - position > 64)
            return HttpParseError::InvalidFraming;
        size_t digits = position;
        while (digits < lineEnd && wire[digits] != ';')
            ++digits;
        if (digits == position)
            return HttpParseError::InvalidFraming;
        for (size_t i = digits; i < lineEnd; ++i)
            if ((unsigned char)wire[i] < 0x20 && wire[i] != '\t')
                return HttpParseError::InvalidFraming;
            else if ((unsigned char)wire[i] >= 0x7f)
                return HttpParseError::InvalidFraming;
        size_t chunkLength = 0;
        for (size_t i = position; i < digits; ++i) {
            int digit = Hex(wire[i]);
            if (digit < 0 || chunkLength > (HtmlMaximumSource - (size_t)digit) / 16)
                return HttpParseError::TooLarge;
            chunkLength = chunkLength * 16 + (size_t)digit;
        }
        position = lineEnd + 2;
        if (!chunkLength) {
            for (;;) {
                if (position >= wireLength)
                    return HttpParseError::Incomplete;
                if (wire[position] == '\r') {
                    if (position + 1 >= wireLength)
                        return HttpParseError::Incomplete;
                    if (wire[position + 1] != '\n')
                        return HttpParseError::InvalidFraming;
                    position += 2;
                    if (position != wireLength)
                        return HttpParseError::InvalidFraming;
                    *bodyLength = outputLength;
                    return HttpParseError::None;
                }
                if (++trailerCount > 32 || position - (lineEnd + 2) > 4096)
                    return HttpParseError::TooLarge;
                size_t trailerEnd = 0;
                if (!FindCrlf(wire, wireLength, position, &trailerEnd))
                    return HttpParseError::Incomplete;
                size_t colon = position;
                while (colon < trailerEnd && wire[colon] != ':') {
                    if (!IsTokenCharacter((unsigned char)wire[colon]))
                        return HttpParseError::InvalidHeader;
                    ++colon;
                }
                if (colon == position || colon == trailerEnd)
                    return HttpParseError::InvalidHeader;
                for (size_t i = colon + 1; i < trailerEnd; ++i)
                    if (((unsigned char)wire[i] < 0x20 && wire[i] != '\t') ||
                        (unsigned char)wire[i] == 0x7f)
                        return HttpParseError::InvalidHeader;
                trailerBytes += trailerEnd + 2 - position;
                if (trailerBytes > 4096)
                    return HttpParseError::TooLarge;
                position = trailerEnd + 2;
            }
        }
        if (chunkLength > bodyCapacity - 1 - outputLength)
            return HttpParseError::TooLarge;
        if (chunkLength > wireLength - position || wireLength - position - chunkLength < 2)
            return HttpParseError::Incomplete;
        for (size_t i = 0; i < chunkLength; ++i)
            body[outputLength++] = wire[position + i];
        body[outputLength] = 0;
        position += chunkLength;
        if (wire[position] != '\r' || wire[position + 1] != '\n')
            return HttpParseError::InvalidFraming;
        position += 2;
    }
}

bool NormalizePath(const char *input, size_t pathLength, const char *suffix, size_t suffixLength,
                   char *output, size_t capacity)
{
    output[0] = 0;
    size_t outputLength = 0;
    if (!Append(output, capacity, &outputLength, '/'))
        return false;
    size_t position = 0;
    while (position < pathLength) {
        while (position < pathLength && input[position] == '/')
            ++position;
        size_t end = position;
        while (end < pathLength && input[end] != '/')
            ++end;
        size_t segmentLength = end - position;
        if (segmentLength == 1 && input[position] == '.') {
        } else if (segmentLength == 2 && input[position] == '.' && input[position + 1] == '.') {
            if (outputLength > 1) {
                if (output[outputLength - 1] == '/')
                    --outputLength;
                while (outputLength > 1 && output[outputLength - 1] != '/')
                    --outputLength;
                output[outputLength] = 0;
            }
        } else if (segmentLength) {
            if (outputLength > 1 && output[outputLength - 1] != '/' &&
                !Append(output, capacity, &outputLength, '/'))
                return false;
            if (!AppendBytes(output, capacity, &outputLength, input + position, segmentLength))
                return false;
        }
        position = end;
    }
    bool directoryReference = pathLength && input[pathLength - 1] == '/';
    if (pathLength >= 2 && input[pathLength - 1] == '.' && input[pathLength - 2] == '/')
        directoryReference = true;
    if (pathLength >= 3 && input[pathLength - 1] == '.' && input[pathLength - 2] == '.' &&
        input[pathLength - 3] == '/')
        directoryReference = true;
    if (directoryReference && output[outputLength - 1] != '/' &&
        !Append(output, capacity, &outputLength, '/'))
        return false;
    if (!AppendBytes(output, capacity, &outputLength, suffix, suffixLength))
        return false;
    return true;
}

}

HttpParseError ParseHttpResponse(const char *wire, size_t wireLength, char *body, size_t bodyCapacity,
                                size_t *bodyLength, HttpResponse *response)
{
    if (!wire || !body || bodyCapacity < 1 || !bodyLength || !response)
        return HttpParseError::InvalidFraming;
    *bodyLength = 0;
    body[0] = 0;
    *response = HttpResponse{};
    size_t headerEnd = 0;
    bool hasHeaderEnd = false;
    for (size_t i = 0; i + 3 < wireLength && i < HttpMaximumHeader; ++i)
        if (wire[i] == '\r' && wire[i + 1] == '\n' && wire[i + 2] == '\r' && wire[i + 3] == '\n') {
            headerEnd = i;
            hasHeaderEnd = true;
            break;
        }
    if (!hasHeaderEnd) {
        if (wireLength > HttpMaximumHeader)
            return HttpParseError::TooLarge;
        return HttpParseError::Incomplete;
    }
    if (headerEnd + 4 > HttpMaximumHeader)
        return HttpParseError::TooLarge;

    size_t lineEnd = 0;
    if (!FindCrlf(wire, headerEnd + 2, 0, &lineEnd) || lineEnd < 12 ||
        !((lineEnd >= 9 && wire[0] == 'H' && wire[1] == 'T' && wire[2] == 'T' && wire[3] == 'P' &&
           wire[4] == '/' && (wire[5] == '1') && (wire[6] == '.') &&
           (wire[7] == '0' || wire[7] == '1') && wire[8] == ' ') &&
          lineEnd >= 9))
        return HttpParseError::InvalidStatus;
    if (wire[9] < '1' || wire[9] > '5' || wire[10] < '0' || wire[10] > '9' ||
        wire[11] < '0' || wire[11] > '9' || (lineEnd > 12 && wire[12] != ' '))
        return HttpParseError::InvalidStatus;
    response->StatusCode = (unsigned short)((wire[9] - '0') * 100 + (wire[10] - '0') * 10 + wire[11] - '0');

    bool hasContentLength = false;
    size_t contentLength = 0;
    bool hasTransferEncoding = false;
    size_t position = lineEnd + 2;
    size_t headerCount = 0;
    while (position < headerEnd) {
        if (++headerCount > 64 || !FindCrlf(wire, headerEnd + 2, position, &lineEnd))
            return HttpParseError::InvalidHeader;
        size_t colon = position;
        while (colon < lineEnd && wire[colon] != ':') {
            if (!IsTokenCharacter((unsigned char)wire[colon]))
                return HttpParseError::InvalidHeader;
            ++colon;
        }
        if (colon == position || colon == lineEnd)
            return HttpParseError::InvalidHeader;
        const char *value = wire + colon + 1;
        size_t valueLength = lineEnd - colon - 1;
        for (size_t i = 0; i < valueLength; ++i)
            if (((unsigned char)value[i] < 0x20 && value[i] != '\t') ||
                (unsigned char)value[i] == 0x7f)
                return HttpParseError::InvalidHeader;
        Trim(&value, &valueLength);
        const char *name = wire + position;
        size_t nameLength = colon - position;
        if (EqualIgnoreCase(name, nameLength, "content-length")) {
            size_t parsed = 0;
            if (!ParseDecimal(value, valueLength, (size_t)-1, &parsed))
                return HttpParseError::InvalidFraming;
            if (parsed > HtmlMaximumSource)
                return HttpParseError::TooLarge;
            if (hasContentLength && parsed != contentLength)
                return HttpParseError::InvalidFraming;
            hasContentLength = true;
            contentLength = parsed;
        } else if (EqualIgnoreCase(name, nameLength, "transfer-encoding")) {
            if (hasTransferEncoding)
                return HttpParseError::InvalidFraming;
            hasTransferEncoding = true;
            if (!EqualIgnoreCase(value, valueLength, "chunked"))
                return HttpParseError::UnsupportedTransferEncoding;
        } else if (EqualIgnoreCase(name, nameLength, "location")) {
            if (valueLength >= sizeof(response->Location))
                return HttpParseError::TooLarge;
            for (size_t i = 0; i < valueLength; ++i)
                response->Location[i] = value[i];
            response->Location[valueLength] = 0;
        } else if (EqualIgnoreCase(name, nameLength, "content-type")) {
            size_t mediaLength = 0;
            while (mediaLength < valueLength && value[mediaLength] != ';')
                ++mediaLength;
            while (mediaLength && IsSpace(value[mediaLength - 1]))
                --mediaLength;
            if (mediaLength >= sizeof(response->ContentType))
                return HttpParseError::TooLarge;
            for (size_t i = 0; i < mediaLength; ++i)
                response->ContentType[i] = Lower(value[i]);
            response->ContentType[mediaLength] = 0;
        } else if (EqualIgnoreCase(name, nameLength, "content-encoding") &&
                   !EqualIgnoreCase(value, valueLength, "identity")) {
            response->UnsupportedContentEncoding = true;
        }
        position = lineEnd + 2;
    }
    if (hasContentLength && hasTransferEncoding)
        return HttpParseError::InvalidFraming;

    position = headerEnd + 4;
    if (hasTransferEncoding) {
        response->Framing = HttpBodyFraming::Chunked;
        return ParseChunked(wire, wireLength, position, body, bodyCapacity, bodyLength);
    }
    if (hasContentLength) {
        response->Framing = HttpBodyFraming::ContentLength;
        if (contentLength >= bodyCapacity)
            return HttpParseError::TooLarge;
        if (wireLength - position < contentLength)
            return HttpParseError::Incomplete;
        if (wireLength - position != contentLength)
            return HttpParseError::InvalidFraming;
        for (size_t i = 0; i < contentLength; ++i)
            body[i] = wire[position + i];
        body[contentLength] = 0;
        *bodyLength = contentLength;
        return HttpParseError::None;
    }

    response->Framing = HttpBodyFraming::CloseDelimited;
    size_t closeLength = wireLength - position;
    if (closeLength >= bodyCapacity)
        return HttpParseError::TooLarge;
    for (size_t i = 0; i < closeLength; ++i)
        body[i] = wire[position + i];
    body[closeLength] = 0;
    *bodyLength = closeLength;
    return HttpParseError::None;
}

bool FormatHttpUrl(const Url &url, char *output, size_t capacity)
{
    if (!url.valid || (url.scheme != Scheme::Http && url.scheme != Scheme::Https) || !output || capacity < 1)
        return false;
    output[0] = 0;
    size_t length = 0;
    const char *scheme = url.scheme == Scheme::Http ? "http://" : "https://";
    for (size_t i = 0; scheme[i]; ++i)
        if (!Append(output, capacity, &length, scheme[i]))
            return false;
    for (size_t i = 0; url.host[i]; ++i)
        if (!Append(output, capacity, &length, url.host[i]))
            return false;
    if (url.explicitPort) {
        char digits[5] = {};
        unsigned int port = url.port;
        size_t count = 0;
        do {
            digits[count++] = (char)('0' + port % 10);
            port /= 10;
        } while (port && count < sizeof(digits));
        if (!Append(output, capacity, &length, ':'))
            return false;
        while (count)
            if (!Append(output, capacity, &length, digits[--count]))
                return false;
    }
    for (size_t i = 0; url.path[i]; ++i)
        if (!Append(output, capacity, &length, url.path[i]))
            return false;
    return true;
}

bool FormatHttpRequestTarget(const Url &url, char *output, size_t capacity)
{
    if (!url.valid || !output || capacity < 2)
        return false;
    output[0] = 0;
    size_t length = 0;
    const char *path = url.path;
    while (*path && *path != '#')
        if (!Append(output, capacity, &length, *path++))
            return false;
    if (!length && !Append(output, capacity, &length, '/'))
        return false;
    return true;
}

bool ParseIpv4Literal(const char *host, unsigned char octets[4])
{
    if (!host || !octets)
        return false;
    const char *cursor = host;
    for (size_t part = 0; part < 4; ++part) {
        unsigned int value = 0;
        size_t digits = 0;
        while (*cursor >= '0' && *cursor <= '9') {
            if (++digits > 3 || (digits > 1 && value == 0))
                return false;
            value = value * 10 + (unsigned int)(*cursor++ - '0');
            if (value > 255)
                return false;
        }
        if (!digits)
            return false;
        octets[part] = (unsigned char)value;
        if (part < 3) {
            if (*cursor++ != '.')
                return false;
        } else if (*cursor) {
            return false;
        }
    }
    return true;
}

bool IsIpv4LiteralCandidate(const char *host)
{
    if (!host)
        return false;
    bool hasDot = false;
    for (const unsigned char *cursor = (const unsigned char *)host; *cursor; ++cursor) {
        if (*cursor == '.') {
            hasDot = true;
        } else if (*cursor < '0' || *cursor > '9') {
            return false;
        }
    }
    return hasDot;
}

bool ResolveHttpReference(const Url &base, const char *reference, Url *resolved, char *absolute,
                          size_t absoluteCapacity)
{
    if (!base.valid || (base.scheme != Scheme::Http && base.scheme != Scheme::Https) ||
        !reference || !resolved || !absolute || absoluteCapacity < 1)
        return false;
    size_t length = 0;
    while (reference[length] && length < HttpMaximumHeader)
        ++length;
    if (length == HttpMaximumHeader)
        return false;
    for (size_t i = 0; i < length; ++i)
        if ((unsigned char)reference[i] <= 0x20 || (unsigned char)reference[i] == 0x7f)
            return false;
    if ((length >= 7 && reference[0] == 'h' && reference[1] == 't' && reference[2] == 't' &&
         reference[3] == 'p' && reference[4] == ':' && reference[5] == '/' && reference[6] == '/') ||
        (length >= 8 && reference[0] == 'h' && reference[1] == 't' && reference[2] == 't' &&
         reference[3] == 'p' && reference[4] == 's' && reference[5] == ':' &&
         reference[6] == '/' && reference[7] == '/')) {
        if (length >= absoluteCapacity)
            return false;
        for (size_t i = 0; i <= length; ++i)
            absolute[i] = reference[i];
        return ParseUrl(absolute, resolved) == UrlError::None;
    }
    if (length >= 2 && reference[0] == '/' && reference[1] == '/') {
        char withScheme[sizeof(Url::host) + sizeof(Url::path) + 16];
        const char *scheme = base.scheme == Scheme::Http ? "http:" : "https:";
        size_t schemeLength = base.scheme == Scheme::Http ? 5 : 6;
        if (schemeLength + length >= sizeof(withScheme))
            return false;
        for (size_t i = 0; i < schemeLength; ++i)
            withScheme[i] = scheme[i];
        for (size_t i = 0; i <= length; ++i)
            withScheme[schemeLength + i] = reference[i];
        return ParseUrl(withScheme, resolved) == UrlError::None &&
               FormatHttpUrl(*resolved, absolute, absoluteCapacity);
    }

    size_t firstDelimiter = length;
    for (size_t i = 0; i < length; ++i)
        if (reference[i] == '/' || reference[i] == '?' || reference[i] == '#') {
            firstDelimiter = i;
            break;
        }
    for (size_t i = 0; i < firstDelimiter; ++i)
        if (reference[i] == ':')
            return false;

    size_t referencePathLength = 0;
    while (referencePathLength < length && reference[referencePathLength] != '?' &&
           reference[referencePathLength] != '#')
        ++referencePathLength;
    size_t basePathLength = 0;
    while (base.path[basePathLength] && base.path[basePathLength] != '?' &&
           base.path[basePathLength] != '#')
        ++basePathLength;

    char combined[sizeof(base.path) * 2];
    size_t combinedLength = 0;
    char suffix[sizeof(base.path) * 2];
    size_t suffixLength = 0;
    if (!referencePathLength) {
        if (!AppendBytes(combined, sizeof(combined), &combinedLength, base.path, basePathLength))
            return false;
        if (reference[0] == '?') {
            if (!AppendBytes(suffix, sizeof(suffix), &suffixLength, reference, length))
                return false;
        } else if (reference[0] == '#') {
            size_t baseQueryEnd = basePathLength;
            while (base.path[baseQueryEnd] && base.path[baseQueryEnd] != '#')
                ++baseQueryEnd;
            if (base.path[basePathLength] == '?' &&
                !AppendBytes(suffix, sizeof(suffix), &suffixLength, base.path + basePathLength,
                             baseQueryEnd - basePathLength))
                return false;
            if (!AppendBytes(suffix, sizeof(suffix), &suffixLength, reference, length))
                return false;
        } else {
            size_t baseSuffix = basePathLength;
            while (base.path[baseSuffix])
                ++baseSuffix;
            if (!AppendBytes(suffix, sizeof(suffix), &suffixLength, base.path + basePathLength,
                             baseSuffix - basePathLength))
                return false;
        }
    } else {
        if (!AppendBytes(suffix, sizeof(suffix), &suffixLength, reference + referencePathLength,
                         length - referencePathLength))
            return false;
        if (reference[0] == '/') {
            if (!AppendBytes(combined, sizeof(combined), &combinedLength, reference, referencePathLength))
                return false;
        } else {
            size_t directoryLength = basePathLength;
            while (directoryLength && base.path[directoryLength - 1] != '/')
                --directoryLength;
            if (!AppendBytes(combined, sizeof(combined), &combinedLength, base.path, directoryLength) ||
                !AppendBytes(combined, sizeof(combined), &combinedLength, reference, referencePathLength))
                return false;
        }
    }
    combined[combinedLength] = 0;
    suffix[suffixLength] = 0;

    char path[sizeof(base.path)];
    if (!NormalizePath(combined, combinedLength, suffix, suffixLength, path, sizeof(path)))
        return false;
    size_t pathLength = 0;
    while (path[pathLength])
        ++pathLength;

    char candidate[sizeof(base.host) + sizeof(base.path) + 32];
    size_t candidateLength = 0;
    const char *scheme = base.scheme == Scheme::Http ? "http://" : "https://";
    for (size_t i = 0; scheme[i]; ++i)
        if (!Append(candidate, sizeof(candidate), &candidateLength, scheme[i]))
            return false;
    for (size_t i = 0; base.host[i]; ++i)
        if (!Append(candidate, sizeof(candidate), &candidateLength, base.host[i]))
            return false;
    if (base.explicitPort) {
        if (!Append(candidate, sizeof(candidate), &candidateLength, ':'))
            return false;
        char digits[5];
        size_t count = 0;
        unsigned int port = base.port;
        do {
            digits[count++] = (char)('0' + port % 10);
            port /= 10;
        } while (port && count < sizeof(digits));
        while (count)
            if (!Append(candidate, sizeof(candidate), &candidateLength, digits[--count]))
                return false;
    }
    if (!AppendBytes(candidate, sizeof(candidate), &candidateLength, path, pathLength))
        return false;
    if (ParseUrl(candidate, resolved) != UrlError::None)
        return false;
    return FormatHttpUrl(*resolved, absolute, absoluteCapacity);
}

}
