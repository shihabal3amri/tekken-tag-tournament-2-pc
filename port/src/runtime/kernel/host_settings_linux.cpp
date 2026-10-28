#include "host_settings_platform.h"
#include "../host_storage.h"

#if !defined(__linux__) && !defined(__APPLE__)
#error "This settings backend requires Linux or macOS"
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace host_settings::platform {
namespace {
std::string Trim(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

uint32_t Lines(const std::filesystem::path& path, std::vector<std::string>& lines)
{
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    if (error) return host_storage::Error(error);
    std::string data;
    if (const uint32_t status = host_storage::ReadSmallFile(absolute, data, 1024 * 1024))
        return status;
    std::istringstream file(data);
    for (std::string line; std::getline(file, line);) lines.push_back(std::move(line));
    return 0;
}

std::string Section(const std::string& line)
{
    const std::string clean = Trim(line);
    if (clean.size() < 2 || clean.front() != '[' || clean.back() != ']') return {};
    return Lower(Trim(clean.substr(1, clean.size() - 2)));
}

std::string Key(const std::string& line)
{
    const auto equal = line.find('=');
    if (equal == std::string::npos) return {};
    return Lower(Trim(line.substr(0, equal)));
}
} // namespace

std::filesystem::path SettingsOverride()
{
    const char* configured = std::getenv("TTT2_SETTINGS_INI");
    return configured && *configured ? std::filesystem::path(configured) :
                                       std::filesystem::path{};
}

std::filesystem::path DefaultSettingsFolder()
{
#if defined(__APPLE__)
    // Settings sit beside the saves: ~/Library/Application Support/TTT2Recomp.
    const char* home = std::getenv("HOME");
    if (home && *home && std::filesystem::path(home).is_absolute())
        return std::filesystem::path(home) / "Library" / "Application Support" / "TTT2Recomp";
    return std::filesystem::path(".") / "TTT2Recomp";
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    if (xdg && *xdg && std::filesystem::path(xdg).is_absolute())
        return std::filesystem::path(xdg) / "ttt2-recomp";
    if (home && *home && std::filesystem::path(home).is_absolute())
        return std::filesystem::path(home) / ".config" / "ttt2-recomp";
    return std::filesystem::path(".") / "ttt2-recomp";
#endif
}

std::filesystem::path DefaultDiagnosticFolder(const std::filesystem::path& settingsPath)
{
    return settingsPath.parent_path() / "debug";
}

std::string Read(const std::filesystem::path& path, const char* section,
                 const char* key, const char* fallback)
{
    const std::string wantedSection = Lower(section), wantedKey = Lower(key);
    std::string currentSection;
    std::string result = fallback;
    std::vector<std::string> lines;
    if (Lines(path, lines)) return result;
    for (const auto& line : lines)
    {
        if (const std::string name = Section(line); !name.empty()) currentSection = name;
        else if (currentSection == wantedSection && Key(line) == wantedKey)
            result = Trim(line.substr(line.find('=') + 1));
    }
    return result;
}

void WriteMany(const std::filesystem::path& path, const char* section,
               std::initializer_list<std::pair<const char*, std::string>> values)
{
    std::vector<std::string> lines;
    const uint32_t readError = Lines(path, lines);
    if (readError && readError != 2 && readError != 3)
    {
        std::fprintf(stderr, "[settings] cannot read settings (%u); update skipped\n", readError);
        return;
    }
    const std::string wantedSection = Lower(section);
    std::string currentSection;
    bool sectionFound = false;
    size_t sectionEnd = lines.size();
    std::vector<std::string> missing;
    for (const auto& [key, value] : values) missing.emplace_back(key);
    for (size_t lineNumber = 0; lineNumber < lines.size(); ++lineNumber)
    {
        auto& line = lines[lineNumber];
        if (const std::string name = Section(line); !name.empty())
        {
            if (currentSection == wantedSection) sectionEnd = lineNumber;
            currentSection = name;
            sectionFound |= name == wantedSection;
            continue;
        }
        if (currentSection != wantedSection) continue;
        for (size_t i = 0; i < missing.size(); ++i)
            if (!missing[i].empty() && Key(line) == Lower(missing[i]))
            {
                line = std::string((values.begin() + i)->first) + "=" + (values.begin() + i)->second;
                missing[i].clear();
            }
    }
    if (!sectionFound) lines.emplace_back("[" + std::string(section) + "]");
    else if (currentSection == wantedSection) sectionEnd = lines.size();
    std::vector<std::string> additions;
    for (size_t i = 0; i < missing.size(); ++i)
        if (!missing[i].empty()) additions.emplace_back(std::string((values.begin() + i)->first) + "=" + (values.begin() + i)->second);
    lines.insert(lines.begin() + (sectionFound ? sectionEnd : lines.size()), additions.begin(), additions.end());
    std::ostringstream output;
    for (const auto& line : lines) output << line << '\n';
    const std::string data = output.str();
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    if (error)
    {
        std::fprintf(stderr, "[settings] cannot resolve settings path: %s\n", error.message().c_str());
        return;
    }
    const uint32_t directoryError = host_storage::CreateDirectories(absolute.parent_path());
    if (directoryError)
    {
        std::fprintf(stderr, "[settings] cannot create settings directory (%u)\n", directoryError);
        return;
    }
    const uint32_t writeError = host_storage::WriteAtomic(
        absolute, data.data(), data.size());
    if (writeError)
        std::fprintf(stderr, "[settings] cannot replace settings (%u)\n", writeError);
}
} // namespace host_settings::platform
