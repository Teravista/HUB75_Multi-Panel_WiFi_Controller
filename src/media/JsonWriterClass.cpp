#include "media/include/JsonWriterClass.hpp"
#include "config/defines.h"
#include <cstdarg>
#include <cstdio>

/**
    Appends formatted text to the bounded JSON response buffer.

    Writes into the remaining caller-supplied capacity and advances the cursor only when the
    entire formatted fragment fits. A formatting failure or truncation makes the writer unusable
    for the rest of the response. Callers must check good before publishing the result, because an
    overflowing fragment can leave partial text.

    @param format Printf-style format string for the appended JSON text.
*/
void
JsonWriter::AppendFormattedText (
    const char* format,
    ...
    )
{
    va_list args;
    int n;

    if (!good || !out || used >= capacity) {
        good = false;
        return;
    }
    va_start(args, format);
    n = std::vsnprintf(out + used, capacity - used, format, args);
    va_end(args);
    if (n < 0 || size_t(n) >= capacity - used) {
        good = false;
        return;
    }
    used += size_t(n);
} // AppendFormattedText

/**
    Appends an escaped JSON string to the response buffer.

    Surrounds the value with JSON quotes, escapes quotes and backslashes, and emits control bytes
    as Unicode escapes. Other bytes pass through unchanged, preserving an already encoded text
    value. All fragments use the same bounded append path, so overflow propagates through the
    writer's retained failure flag.

    @param text Null-terminated input text.
*/
void
JsonWriter::AppendEscapedString (
    const char* text
    )
{
    unsigned char c;

    AppendFormattedText("\"");
    for (; *text; ++text) {
        c = *text;
        if (c == '"' || c == '\\') {
            AppendFormattedText("\\%c", c);
        } else if (c < ASCII_SPACE) {
            AppendFormattedText("\\u%04x", unsigned(c));
        } else {
            AppendFormattedText("%c", c);
        }
    }

    AppendFormattedText("\"");
} // AppendEscapedString
