#include "host_file.h"

namespace {
bool SameAsciiName(const std::filesystem::path& left,
                   const std::filesystem::path& right)
{
    // Compare native code units so an unrelated Unicode host filename never
    // needs conversion through the process's narrow character set on Windows.
    const auto& nativeLeft = left.native();
    const auto& nativeRight = right.native();
    if (nativeLeft.size() != nativeRight.size()) return false;
    for (size_t i = 0; i < nativeLeft.size(); ++i)
    {
        const auto a = nativeLeft[i];
        const auto b = nativeRight[i];
        const auto foldedA = a >= 'A' && a <= 'Z' ? a + ('a' - 'A') : a;
        const auto foldedB = b >= 'A' && b <= 'Z' ? b + ('a' - 'A') : b;
        if (foldedA != foldedB) return false;
    }
    return true;
}

std::filesystem::path FindDiscChild(const std::filesystem::path& parent,
                                    const std::filesystem::path& requested)
{
    std::error_code error;
    const auto exact = parent / requested;
    const auto exactStatus = std::filesystem::symlink_status(exact, error);
    if (!error && std::filesystem::exists(exactStatus))
        return std::filesystem::is_symlink(exactStatus) ? std::filesystem::path{} : exact;
    if (error && error != std::errc::no_such_file_or_directory) return {};

    std::filesystem::path found;
    for (auto it = std::filesystem::directory_iterator(parent, error);
         !error && it != std::filesystem::directory_iterator(); it.increment(error))
    {
        if (!SameAsciiName(it->path().filename(), requested)) continue;
        const auto status = it->symlink_status(error);
        if (error || std::filesystem::is_symlink(status) || !found.empty()) return {};
        found = it->path();
    }
    return error ? std::filesystem::path{} : found;
}
}

namespace host_file {
std::filesystem::path ResolveDiscPath(const std::filesystem::path& root,
                                      const std::filesystem::path& relative,
                                      bool rootFallback)
{
    if (relative.is_absolute() || relative.has_root_name()) return {};
    for (const auto& component : relative)
        if (component == "..") return {};

    std::error_code error;
    const auto base = std::filesystem::canonical(root, error);
    if (error) return {};
    auto current = base;
    bool found = true;
    for (const auto& component : relative)
    {
        if (component == "." || component.empty()) continue;
        const auto next = FindDiscChild(current, component);
        if (next.empty()) { found = false; break; }
        current = next;
    }
    if (found) return current;
    if (!rootFallback || relative.filename().empty()) return {};
    return FindDiscChild(base, relative.filename());
}

uint32_t DosErrorStatus(uint32_t error)
{
    switch (error)
    {
    case 0: return 0;             // successful host flush remains STATUS_SUCCESS
    case 38: return 0xC0000011u;   // ERROR_HANDLE_EOF
    case 6: return 0xC0000008u;    // ERROR_INVALID_HANDLE
    case 87: case 131: return 0xC000000Du; // invalid parameter / negative seek
    case 5: return 0xC0000022u;    // access denied
    case 2: return 0xC0000034u;    // file not found
    case 3: case 267: return 0xC000003Au; // path not found / not a directory
    case 123: return 0xC0000033u;  // invalid name
    case 32: return 0xC0000043u;   // sharing violation
    case 112: return 0xC000007Fu;  // disk full
    case 33: return 0xC0000054u;   // lock violation
    case 23: return 0xC000009Cu;   // CRC / device data error
    case 1117: return 0xC0000185u; // I/O device error
    case 995: return 0xC0000120u;  // operation aborted
    case 8: case 14: return 0xC000009Au; // insufficient resources
    case 183: return 0xC0000035u;  // already exists
    default: return 0xC0000001u;
    }
}
}
