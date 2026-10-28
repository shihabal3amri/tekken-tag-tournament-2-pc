#include "host_storage.h"

#if !defined(__linux__) && !defined(__APPLE__)
#error "This save storage backend requires Linux or macOS"
#endif

#include <cerrno>
#include <atomic>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace host_storage {
namespace {
#if defined(__linux__)
constexpr int DirectoryPathFlags = DirectoryPathFlags;
#else
constexpr int DirectoryPathFlags = O_SEARCH; // macOS: lookup-only directory descriptor
#endif
}

uint32_t Error(const std::error_code& error)
{
    if (!error) return 0;
    switch (error.value())
    {
    case ENOENT: return 2;     // ERROR_FILE_NOT_FOUND
    case EIO: return 1117;     // ERROR_IO_DEVICE
    case EACCES: case EPERM: case ELOOP: case EROFS: return 5;
    case EBADF: return 6;
    case ENOMEM: return 8;
    case EINVAL: return 87;
    case ENOSPC: case EDQUOT: return 112;
    case EBUSY: return 170;
    case ENOTEMPTY: return 145; // ERROR_DIR_NOT_EMPTY
    case EEXIST: return 183;
    case ENOTDIR: return 267;
    default: return 31;       // ERROR_GEN_FAILURE
    }
}

uint32_t SaveBase(std::filesystem::path& root)
{
    if (const char* overridePath = std::getenv("TTT2_SAVE_ROOT"); overridePath && *overridePath)
        root = overridePath;
#if defined(__APPLE__)
    else if (const char* home = std::getenv("HOME");
             home && *home && std::filesystem::path(home).is_absolute())
        root = std::filesystem::path(home) / "Library" / "Application Support" /
               "TTT2Recomp" / "saves";
    else return 3;             // ERROR_PATH_NOT_FOUND
#else
    else
    {
        const char* data = std::getenv("XDG_DATA_HOME");
        const char* home = std::getenv("HOME");
        if (data && *data && std::filesystem::path(data).is_absolute())
            root = std::filesystem::path(data) / "ttt2-recomp" / "saves";
        else if (home && *home && std::filesystem::path(home).is_absolute())
            root = std::filesystem::path(home) / ".local" / "share" / "ttt2-recomp" / "saves";
        else return 3;         // ERROR_PATH_NOT_FOUND
    }
#endif
    return root.is_absolute() ? 0 : 123; // ERROR_INVALID_NAME
}

uint32_t RejectLinks(const std::filesystem::path& path)
{
    std::filesystem::path part;
    for (const auto& component : path)
    {
        part /= component;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(part, error);
        if (error == std::errc::no_such_file_or_directory) continue;
        if (error) return Error(error);
        if (std::filesystem::is_symlink(status)) return 5; // ERROR_ACCESS_DENIED
    }
    return 0;
}

uint32_t CreateDirectories(const std::filesystem::path& path)
{
    if (!path.is_absolute()) return 123; // ERROR_INVALID_NAME
    int directory = open("/", DirectoryPathFlags | O_CLOEXEC);
    if (directory < 0) return Error(std::error_code(errno, std::generic_category()));
    for (const auto& component : path.relative_path())
    {
        if (component == ".") continue;
        if (component == "..") { close(directory); return 5; }
        int next = openat(directory, component.c_str(),
                          DirectoryPathFlags | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT)
        {
            if (mkdirat(directory, component.c_str(), 0700) < 0 && errno != EEXIST)
            {
                const uint32_t error = Error(std::error_code(errno, std::generic_category()));
                close(directory);
                return error;
            }
            next = openat(directory, component.c_str(),
                          DirectoryPathFlags | O_NOFOLLOW | O_CLOEXEC);
        }
        if (next < 0)
        {
            const uint32_t error = Error(std::error_code(errno, std::generic_category()));
            close(directory);
            return error;
        }
        struct stat info{};
        if (fstat(next, &info) < 0)
        {
            const uint32_t error = Error(std::error_code(errno, std::generic_category()));
            close(next);
            close(directory);
            return error;
        }
        if (!S_ISDIR(info.st_mode)) { close(next); close(directory); return 5; }
        close(directory);
        directory = next;
    }
    close(directory);
    return 0;
}

namespace {
uint32_t NativeError() { return Error(std::error_code(errno, std::generic_category())); }

int Sync(int fd)
{
    int result;
#if defined(__APPLE__)
    // macOS fsync only reaches the drive's cache; F_FULLFSYNC flushes it.
    // Some file systems do not support it, so fall back to fsync.
    do { result = fcntl(fd, F_FULLFSYNC); } while (result < 0 && errno == EINTR);
    if (result == 0) return 0;
#endif
    do { result = fsync(fd); } while (result < 0 && errno == EINTR);
    return result;
}

// Resolve each parent below a stable directory descriptor. O_NOFOLLOW prevents
// a replaced save component from redirecting reads or writes outside the store.
// The final descriptor must support fsync after an atomic metadata rename;
// Linux rejects fsync on an O_PATH descriptor with EBADF.
uint32_t OpenParent(const std::filesystem::path& path, int& directory)
{
    if (!path.is_absolute()) return 123;
    int fd = open("/", DirectoryPathFlags | O_CLOEXEC);
    if (fd < 0) return NativeError();
    for (const auto& component : path.parent_path().relative_path())
    {
        if (component == ".") continue;
        if (component == "..") { close(fd); return 5; }
        const int next = openat(fd, component.c_str(),
                                DirectoryPathFlags | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0)
        {
            const uint32_t error = NativeError();
            close(fd);
            return error;
        }
        close(fd);
        fd = next;
    }
    const int readable = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (readable < 0)
    {
        const uint32_t error = NativeError();
        close(fd);
        return error;
    }
    close(fd);
    directory = readable;
    return 0;
}

bool SameFile(const struct stat& left, const struct stat& right)
{
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

uint32_t RemoveContents(int directory)
{
    const int listing = openat(directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (listing < 0) return NativeError();
    DIR* stream = fdopendir(listing);
    if (!stream) { const uint32_t error = NativeError(); close(listing); return error; }
    std::vector<std::string> names;
    for (;;)
    {
        errno = 0;
        dirent* entry = readdir(stream);
        if (!entry)
        {
            const uint32_t error = errno ? NativeError() : 0;
            closedir(stream);
            if (error) return error;
            break;
        }
        const std::string name = entry->d_name;
        if (name != "." && name != "..") names.push_back(name);
    }
    for (const std::string& name : names)
    {
        struct stat before{};
        if (fstatat(directory, name.c_str(), &before, AT_SYMLINK_NOFOLLOW) < 0)
            return NativeError();
        if (S_ISLNK(before.st_mode)) return 5; // Never follow or delete save links.
        if (S_ISDIR(before.st_mode))
        {
            const int child = openat(directory, name.c_str(),
                                     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) return NativeError();
            struct stat opened{};
            if (fstat(child, &opened) < 0)
            {
                const uint32_t error = NativeError();
                close(child);
                return error;
            }
            if (!SameFile(before, opened)) { close(child); return 5; }
            const uint32_t error = RemoveContents(child);
            close(child);
            if (error) return error;
        }
        else if (!S_ISREG(before.st_mode)) return 5;
        struct stat current{};
        if (fstatat(directory, name.c_str(), &current, AT_SYMLINK_NOFOLLOW) < 0)
            return NativeError();
        if (!SameFile(before, current) || before.st_mode != current.st_mode) return 5;
        if (unlinkat(directory, name.c_str(), S_ISDIR(before.st_mode) ? AT_REMOVEDIR : 0) < 0)
            return NativeError();
    }
    return 0;
}
} // namespace

uint32_t RemoveTree(const std::filesystem::path& path)
{
    const auto name = path.filename();
    if (name.empty() || name == "." || name == "..") return 123;
    int parent = -1;
    if (const uint32_t error = OpenParent(path, parent)) return error;
    const int directory = openat(parent, name.c_str(),
                                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0)
    {
        const uint32_t error = NativeError();
        close(parent);
        return error;
    }
    struct stat opened{};
    uint32_t error = fstat(directory, &opened) < 0 ? NativeError() : RemoveContents(directory);
    close(directory);
    if (!error)
    {
        struct stat current{};
        if (fstatat(parent, name.c_str(), &current, AT_SYMLINK_NOFOLLOW) < 0)
            error = NativeError();
        else if (!SameFile(opened, current) || !S_ISDIR(current.st_mode)) error = 5;
        else if (unlinkat(parent, name.c_str(), AT_REMOVEDIR) < 0) error = NativeError();
    }
    close(parent);
    return error;
}

uint32_t ReadSmallFile(const std::filesystem::path& path, std::string& data,
                       size_t maxBytes)
{
    data.clear();
    int parent = -1;
    if (const uint32_t error = OpenParent(path, parent)) return error;
    // A FIFO in place of a metadata file must not block before fstat rejects it.
    const int fd = openat(parent, path.filename().c_str(),
                          O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    const uint32_t openError = fd < 0 ? NativeError() : 0;
    close(parent);
    if (openError) return openError;
    struct stat info{};
    if (fstat(fd, &info) < 0)
    {
        const uint32_t error = NativeError();
        close(fd);
        return error;
    }
    if (!S_ISREG(info.st_mode) || info.st_size < 0 || uint64_t(info.st_size) > maxBytes)
    {
        close(fd);
        return 1392; // ERROR_FILE_CORRUPT
    }
    data.resize(size_t(info.st_size));
    size_t done = 0;
    while (done < data.size())
    {
        const ssize_t got = read(fd, data.data() + done, data.size() - done);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0)
        {
            const uint32_t error = got == 0 ? 1392 : NativeError();
            data.clear();
            close(fd);
            return error;
        }
        done += size_t(got);
    }
    close(fd);
    return 0;
}

uint32_t ReadExact(const std::filesystem::path& path, void* data, size_t bytes)
{
    int parent = -1;
    if (const uint32_t error = OpenParent(path, parent)) return error;
    const int fd = openat(parent, path.filename().c_str(),
                          O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    const uint32_t openError = fd < 0 ? NativeError() : 0;
    close(parent);
    if (openError) return openError;
    struct stat info{};
    if (fstat(fd, &info) < 0)
    {
        const uint32_t error = NativeError();
        close(fd);
        return error;
    }
    if (!S_ISREG(info.st_mode) || info.st_size < 0 || uint64_t(info.st_size) != bytes)
    {
        close(fd);
        return 1392; // ERROR_FILE_CORRUPT
    }
    size_t done = 0;
    while (done < bytes)
    {
        const ssize_t got = read(fd, static_cast<uint8_t*>(data) + done, bytes - done);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0)
        {
            const uint32_t error = got == 0 ? 1392 : NativeError();
            close(fd);
            return error;
        }
        done += size_t(got);
    }
    close(fd);
    return 0;
}

uint32_t ListDirectories(const std::filesystem::path& root,
                         std::vector<std::filesystem::path>& paths)
{
    paths.clear();
    int parent = -1;
    const uint32_t parentError = OpenParent(root, parent);
    if (parentError == 2 || parentError == 3) return 0;
    if (parentError) return parentError;
    const int fd = openat(parent, root.filename().c_str(),
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    const uint32_t openError = fd < 0 ? NativeError() : 0;
    close(parent);
    if (openError == 2 || openError == 3) return 0;
    if (openError) return openError;
    DIR* stream = fdopendir(fd);
    if (!stream) { const uint32_t error = NativeError(); close(fd); return error; }
    for (;;)
    {
        errno = 0;
        dirent* entry = readdir(stream);
        if (!entry)
        {
            const uint32_t error = errno ? NativeError() : 0;
            closedir(stream);
            if (error) paths.clear();
            return error;
        }
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        struct stat info{};
        if (fstatat(fd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) < 0)
        {
            const uint32_t error = NativeError();
            closedir(stream);
            paths.clear();
            return error;
        }
        if (S_ISLNK(info.st_mode))
        {
            closedir(stream);
            paths.clear();
            return 5; // ERROR_ACCESS_DENIED
        }
        if (S_ISDIR(info.st_mode)) paths.push_back(root / name);
    }
}

uint32_t WriteAtomic(const std::filesystem::path& destination,
                     const void* data, size_t bytes)
{
    int parent = -1;
    if (const uint32_t error = OpenParent(destination, parent)) return error;
    static std::atomic<uint64_t> sequence{0};
    std::string temporary;
    int fd = -1;
    for (unsigned attempt = 0; attempt < 128; ++attempt)
    {
        temporary = ".ttt2-tmp." + std::to_string(getpid()) + "." +
                    std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
        fd = openat(parent, temporary.c_str(),
                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0)
    {
        const uint32_t error = NativeError();
        close(parent);
        return error;
    }
    size_t done = 0;
    uint32_t error = 0;
    while (done < bytes)
    {
        const ssize_t written = write(fd, static_cast<const uint8_t*>(data) + done, bytes - done);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { error = written == 0 ? 29 : NativeError(); break; }
        done += size_t(written);
    }
    if (!error && Sync(fd) < 0) error = NativeError();
    // Linux closes the descriptor even when close reports EINTR. Retrying
    // could close a newly reused descriptor, so treat that result as closed.
    if (close(fd) < 0 && errno != EINTR && !error) error = NativeError();
    if (!error && renameat(parent, temporary.c_str(),
                           parent, destination.filename().c_str()) < 0)
        error = NativeError();
    if (!error && Sync(parent) < 0) error = NativeError();
    if (error) unlinkat(parent, temporary.c_str(), 0);
    close(parent);
    return error;
}
} // namespace host_storage
