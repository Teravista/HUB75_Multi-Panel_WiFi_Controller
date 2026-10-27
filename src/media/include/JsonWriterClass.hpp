#pragma once
#include <cstddef>

/**
    Builds bounded JSON text for library, preview and status responses.

    The caller supplies the output memory and capacity. The writer appends formatted fields and
    escapes string values while retaining a single failure flag if the response no longer fits.
    This lets response-building code stop relying on partial output after truncation. It owns no
    network connection or allocation and is used only while the caller keeps the borrowed response
    buffer valid.
*/
class JsonWriter {
public:
    char* out;
    size_t capacity;
    size_t used = 0;
    bool good = true;

    // Appends formatted text to the bounded JSON response buffer.
    void AppendFormattedText(const char* format, ...);

    // Appends an escaped JSON string to the response buffer.
    void AppendEscapedString(const char* text);

};
