#pragma once
#include <filesystem>
#include <string>

// A path or other wide text for the log (a narrow byte stream read as UTF-8), as UTF-8. Nothing in the port calls
// setlocale, so in the CRT's "C" locale %ls ends the whole line, newline included, at the first character above U+00FF
// (a Cyrillic, Greek, Polish, Turkish, Arabic or CJK user name, install folder or device name), and path::string()
// converts to the ANSI code page and throws where that has no mapping: MEASURED 2026-11-02, a save root under such a
// user name aborted the game's save-loading job (kernel_io.cpp). "?" for text that is not valid UTF-16 (an unpaired
// surrogate in a file name). It allocates: crash handlers convert into static buffers instead (main.cpp's [fatal]).
inline std::string LogUtf8(const std::filesystem::path& text)
{
    try
    {
        const std::u8string utf8 = text.u8string();
        return std::string(utf8.begin(), utf8.end());
    }
    catch (...)
    {
        return "?";
    }
}
