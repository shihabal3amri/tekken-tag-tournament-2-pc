#include "host_file.h"

#if !defined(__linux__) && !defined(__APPLE__)
#error "This file backend requires Linux or macOS"
#endif

#include <cerrno>
#include <algorithm>
#include <dirent.h>
#include <fcntl.h>
#if defined(__linux__)
#include <linux/fs.h>
#else
#include <stdio.h> // renameatx_np, RENAME_EXCL
#endif
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

namespace host_file {
struct File {
    int fd = -1;
    int parent = -1; // held for save-file rename and delete disposition
    std::string name;
    dev_t device = 0;
    ino_t inode = 0;
    uint8_t desired = 0;
    uint8_t share = 0;
    bool tracked = false;
    bool readable = true;
};

namespace {
constexpr uint32_t SharingViolation = 0xC0000043u;
constexpr uint8_t ShareRead = 1, ShareWrite = 2, ShareDelete = 4;
std::mutex g_openMutex;
std::vector<File*> g_openFiles;
struct DeleteMark
{
    File* owner = nullptr;
    int parent = -1; // independent descriptor: the owner may close first
    std::string name;
    dev_t device = 0;
    ino_t inode = 0;
};
std::vector<DeleteMark> g_deleteMarks;

#if defined(__linux__)
constexpr int DirectoryPathFlags = O_PATH | O_DIRECTORY;
// A descriptor used only for fstat of a save entry.
constexpr int MetadataFlags = O_PATH;
#else
// macOS has no O_PATH. O_SEARCH opens a directory for lookups only; a save
// entry is opened read-only and non-blocking (a FIFO is rejected by fstat).
constexpr int DirectoryPathFlags = O_SEARCH;
constexpr int MetadataFlags = O_RDONLY | O_NONBLOCK;
#endif

bool DeletePending(dev_t device, ino_t inode)
{
    return std::any_of(g_deleteMarks.begin(), g_deleteMarks.end(),
        [=](const DeleteMark& mark) {
            return mark.device == device && mark.inode == inode;
        });
}

uint8_t DesiredAccess(uint32_t access)
{
    // The Windows adapter adds FILE_READ_ATTRIBUTES to every native open.
    uint8_t desired = ShareRead;
    if (access & 0x50000116u) desired |= ShareWrite;
    if (access & 0x10010000u) desired |= ShareDelete;
    return desired;
}

bool ShareConflict(dev_t device, ino_t inode, uint8_t desired, uint8_t share)
{
    for (const File* other : g_openFiles)
        if (other->device == device && other->inode == inode &&
            ((desired & ~other->share) || (other->desired & ~share)))
            return true;
    return false;
}

uint32_t Status(int error);

uint32_t DosError(int error)
{
    switch (error)
    {
    case 0: return 0;
    case ENOENT: return 2;
    case EIO: return 1117;
    case EACCES: case EPERM: case ELOOP: case EROFS: return 5;
    case EBADF: return 6;
    case ENOMEM: return 8;
    case EINVAL: return 87;
    case ENOSPC: case EDQUOT: return 112;
    case EBUSY: return 170;
    case ENOTEMPTY: return 145;
    case EEXIST: return 183;
    case ENOTDIR: return 267;
    default: return 31;
    }
}

bool SameGuestName(const std::string& left, const std::string& right)
{
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i)
    {
        const unsigned char a = left[i], b = right[i];
        const unsigned char foldedA = a >= 'A' && a <= 'Z' ? a + ('a' - 'A') : a;
        const unsigned char foldedB = b >= 'A' && b <= 'Z' ? b + ('a' - 'A') : b;
        if (foldedA != foldedB) return false;
    }
    return true;
}

// Guest save names are ASCII and case-insensitive. Scan through the pinned
// container descriptor so a differently cased open finds the existing inode.
// The caller holds g_openMutex across this lookup and its open or rename.
uint32_t ExistingName(int parent, const std::string& requested,
                      std::string& actual, bool& exists)
{
    actual = requested;
    exists = false;
    const int fd = openat(parent, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return Status(errno);
    DIR* directory = fdopendir(fd);
    if (!directory) { const int error = errno; close(fd); return Status(error); }
    for (;;)
    {
        errno = 0;
        dirent* entry = readdir(directory);
        if (!entry)
        {
            const int error = errno;
            closedir(directory);
            return error ? Status(error) : 0;
        }
        if (!SameGuestName(entry->d_name, requested)) continue;
        if (exists && actual != entry->d_name)
        {
            closedir(directory);
            return 0xC0000035u; // ambiguous pre-existing case variants
        }
        actual = entry->d_name;
        exists = true;
    }
}

uint32_t Status(int error)
{
    return DosErrorStatus(DosError(error));
}

uint32_t OpenDirectoryNoFollow(const std::filesystem::path& path, int& directory)
{
    if (!path.is_absolute()) return 0xC0000033u;
    int fd = open("/", DirectoryPathFlags | O_CLOEXEC);
    if (fd < 0) return Status(errno);
    for (const auto& component : path.relative_path())
    {
        if (component == ".") continue;
        if (component == "..") { close(fd); return 0xC0000022u; }
        const int next = openat(fd, component.c_str(),
                                DirectoryPathFlags | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0)
        {
            const uint32_t status = Status(errno);
            close(fd);
            return status;
        }
        struct stat info{};
        if (fstat(next, &info) < 0)
        {
            const uint32_t status = Status(errno);
            close(next);
            close(fd);
            return status;
        }
        if (!S_ISDIR(info.st_mode))
        {
            close(next);
            close(fd);
            return 0xC0000022u;
        }
        close(fd);
        fd = next;
    }
    directory = fd;
    return 0;
}
} // namespace

OpenResult OpenDisc(const std::filesystem::path& path)
{
    // Resolve the parent through directory descriptors. The shared path
    // resolver rejects links, but a directory could be replaced before this
    // open; a plain pathname open would then follow the replacement.
    int parent = -1;
    if (OpenDirectoryNoFollow(path.parent_path(), parent))
        return {nullptr, 0xC0000034u, 0, "disc-parent"};
    const int fd = openat(parent, path.filename().c_str(),
                          O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    close(parent);
    if (fd < 0) return {nullptr, 0xC0000034u, 0, "disc-open"};
    struct stat info{};
    if (fstat(fd, &info) < 0 || !S_ISREG(info.st_mode))
    {
        close(fd);
        return {nullptr, 0xC0000034u, 0, "disc-type"};
    }
    auto* file = new File;
    file->fd = fd;
    return {file, 0, 1, ""};
}

uint32_t QueryAttributes(const std::filesystem::path& path,
                         const std::filesystem::path& container,
                         uint64_t& size, bool& directory)
{
    size = 0;
    directory = false;
    if (path.empty()) return 0xC0000034u; // STATUS_OBJECT_NAME_NOT_FOUND
    if (!path.is_absolute() || (!container.empty() &&
        path != container && path.parent_path() != container))
        return 0xC0000033u; // STATUS_OBJECT_NAME_INVALID
    int fd = -1;
    if (!container.empty() && path == container)
    {
        if (const uint32_t status = OpenDirectoryNoFollow(container, fd)) return status;
    }
    else
    {
        int parent = -1;
        if (const uint32_t status = OpenDirectoryNoFollow(path.parent_path(), parent))
            return status;
        std::string name = path.filename().string();
        if (!container.empty())
        {
            // Match the Windows metadata open under the same pinned directory
            // used by OpenSave. Keep the guest-open lock through the sharing
            // and delete-pending checks as well as the descriptor open.
            std::lock_guard openLock(g_openMutex);
            const std::string requested = name;
            bool exists = false;
            if (const uint32_t status = ExistingName(parent, requested, name, exists))
            {
                close(parent);
                return status;
            }
            if (!exists) { close(parent); return 0xC0000034u; }
            fd = openat(parent, name.c_str(), MetadataFlags | O_NOFOLLOW | O_CLOEXEC);
            const int openError = errno;
            close(parent);
            if (fd < 0) return Status(openError);
            struct stat info{};
            const int result = fstat(fd, &info);
            const int statError = errno;
            close(fd);
            if (result < 0) return Status(statError);
            if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) return 0xC0000022u;
            if (DeletePending(info.st_dev, info.st_ino))
                return 0xC0000056u; // STATUS_DELETE_PENDING
            if (ShareConflict(info.st_dev, info.st_ino, ShareRead,
                              ShareRead | ShareWrite | ShareDelete))
                return SharingViolation;
            if (info.st_size < 0) return 0xC000009Cu;
            directory = S_ISDIR(info.st_mode);
            size = directory ? 0 : uint64_t(info.st_size);
            return 0;
        }
        else fd = openat(parent, name.c_str(), MetadataFlags | O_NOFOLLOW | O_CLOEXEC);
        const int error = errno;
        close(parent);
        if (fd < 0) return Status(error);
    }
    struct stat info{};
    const int result = fstat(fd, &info);
    const int error = errno;
    close(fd);
    if (result < 0) return Status(error);
    if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) return 0xC0000022u;
    if (info.st_size < 0) return 0xC000009Cu;
    directory = S_ISDIR(info.st_mode);
    size = directory ? 0 : uint64_t(info.st_size);
    return 0;
}

OpenResult OpenSave(const std::filesystem::path& path,
                    const std::filesystem::path& container,
                    uint32_t access, uint32_t share, uint32_t disposition,
                    uint32_t options, uint32_t attributes,
                    bool hasAllocation, uint64_t allocation)
{
    (void)attributes; (void)hasAllocation; (void)allocation;
    if (share & ~uint32_t(ShareRead | ShareWrite | ShareDelete))
        return {nullptr, 0xC000000Du, 0, "share"};
    int parent = -1;
    if (const uint32_t status = OpenDirectoryNoFollow(container, parent))
        return {nullptr, status, 0, "container"};
    if (path == container)
    {
        if (!(options & 1) || disposition != 1 || (access & 0x50010116u))
        {
            close(parent);
            return {nullptr, 0xC0000022u, 0, "root-access"};
        }
        // Keep a readable descriptor to the verified directory. Enumeration
        // must not resolve its pathname again after a rename or replacement.
        const int listing = openat(parent, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        const int error = errno;
        close(parent);
        if (listing < 0) return {nullptr, Status(error), 0, "root-list"};
        auto* file = new File;
        file->fd = listing;
        return {file, 0, 1, ""};
    }
    if (options & 1 || path.parent_path() != container)
    {
        close(parent);
        return {nullptr, 0xC0000002u, 0, "nested"};
    }
    std::string name = path.filename().string();
    if (name.empty() || name == "." || name == "..")
    {
        close(parent);
        return {nullptr, 0xC0000033u, 0, "name"};
    }
    const uint8_t desired = DesiredAccess(access);
    const bool write = (desired & ShareWrite) != 0;
    // FILE_READ_ATTRIBUTES is added by the Windows adapter, but it does not
    // grant FILE_READ_DATA. Keep write-only guest opens write-only on Linux.
    const bool readData = (access & 0x92000001u) != 0; // GENERIC_READ/ALL, MAXIMUM_ALLOWED, READ_DATA
    if ((disposition == 0 || disposition == 4 || disposition == 5) && !write)
    {
        close(parent);
        return {nullptr, 0xC0000022u, 0, "truncate-access"};
    }
    // Reject non-regular entries after fstat without waiting on a FIFO open.
    const int baseFlags = (write ? (readData ? O_RDWR : O_WRONLY) : O_RDONLY) |
                          O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC;
    int fd = -1;
    uint32_t information = 1;
    bool truncate = false;
    // Serialize the open, sharing decision, and any truncation against other
    // guest handles. O_TRUNC here would destroy data before the share check.
    std::lock_guard openLock(g_openMutex);
    const std::string requestedName = name;
    bool existed = false;
    if (const uint32_t status = ExistingName(parent, requestedName, name, existed))
    {
        close(parent);
        return {nullptr, status, 0, "save-name"};
    }
    switch (disposition)
    {
    case 0: // FILE_SUPERSEDE
    {
        // The directory scan may race a creator or remover outside this
        // process. Let the exclusive create decide FILE_CREATED versus
        // FILE_SUPERSEDED instead of relying on the earlier observation.
        fd = openat(parent, name.c_str(), baseFlags | O_CREAT | O_EXCL, 0600);
        if (fd < 0 && errno == EEXIST)
        {
            fd = openat(parent, name.c_str(), baseFlags);
            information = 0;
        }
        else information = 2;
        truncate = true;
        break;
    }
    case 1: fd = openat(parent, name.c_str(), baseFlags); break; // FILE_OPEN
    case 2: if (existed) { close(parent); return {nullptr, 0xC0000035u, 0, "save-exists"}; }
            fd = openat(parent, name.c_str(), baseFlags | O_CREAT | O_EXCL, 0600);
            information = 2; break; // FILE_CREATE
    case 3: // FILE_OPEN_IF
        fd = existed ? openat(parent, name.c_str(), baseFlags) :
            openat(parent, name.c_str(), baseFlags | O_CREAT | O_EXCL, 0600);
        if (existed && fd < 0 && errno == ENOENT)
        {
            fd = openat(parent, name.c_str(), baseFlags | O_CREAT | O_EXCL, 0600);
            if (fd >= 0) information = 2;
            else if (errno == EEXIST) fd = openat(parent, name.c_str(), baseFlags);
        }
        if (!existed && fd < 0 && errno == EEXIST)
            fd = openat(parent, name.c_str(), baseFlags);
        else if (!existed) information = 2;
        break;
    case 4: fd = openat(parent, name.c_str(), baseFlags);
            information = 3; truncate = true; break; // FILE_OVERWRITE
    case 5: // FILE_OVERWRITE_IF
        if (existed)
        {
            fd = openat(parent, name.c_str(), baseFlags);
            information = 3;
            truncate = true;
            if (fd < 0 && errno == ENOENT)
            {
                fd = openat(parent, name.c_str(), baseFlags | O_CREAT | O_EXCL, 0600);
                if (fd >= 0) { information = 2; truncate = false; }
                else if (errno == EEXIST) fd = openat(parent, name.c_str(), baseFlags);
            }
        }
        else
        {
            fd = openat(parent, name.c_str(), baseFlags | O_CREAT | O_EXCL, 0600);
            if (fd < 0 && errno == EEXIST)
            {
                fd = openat(parent, name.c_str(), baseFlags);
                information = 3;
                truncate = true;
            }
            else information = 2;
        }
        break;
    default: close(parent); return {nullptr, 0xC000000Du, 0, "disposition"};
    }
    if (fd < 0)
    {
        const uint32_t status = Status(errno);
        close(parent);
        return {nullptr, status, 0, "save-open"};
    }
    struct stat info{};
    if (fstat(fd, &info) < 0 || !S_ISREG(info.st_mode))
    {
        close(fd);
        close(parent);
        return {nullptr, 0xC0000022u, 0, "child-type"};
    }
    if (DeletePending(info.st_dev, info.st_ino))
    {
        close(fd);
        close(parent);
        return {nullptr, 0xC0000056u, 0, "delete-pending"};
    }
    if (ShareConflict(info.st_dev, info.st_ino, desired, uint8_t(share)))
    {
        close(fd);
        close(parent);
        return {nullptr, SharingViolation, 0, "share-conflict"};
    }
    if (truncate && ftruncate(fd, 0) != 0)
    {
        const uint32_t status = Status(errno);
        close(fd);
        close(parent);
        return {nullptr, status, 0, "truncate"};
    }
    auto* file = new File;
    file->fd = fd;
    file->parent = parent;
    file->name = name;
    file->device = info.st_dev;
    file->inode = info.st_ino;
    file->desired = desired;
    file->share = uint8_t(share);
    file->tracked = true;
    file->readable = readData;
    g_openFiles.push_back(file);
    return {file, 0, information, ""};
}

void Close(File* file)
{
    if (!file) return;
    std::lock_guard openLock(g_openMutex);
    for (DeleteMark& mark : g_deleteMarks)
        if (mark.owner == file) mark.owner = nullptr;
    if (file->fd >= 0) close(file->fd);
    if (file->parent >= 0) close(file->parent);
    if (file->tracked) std::erase(g_openFiles, file);
    const bool last = std::none_of(g_openFiles.begin(), g_openFiles.end(),
        [file](const File* other) {
            return other->device == file->device && other->inode == file->inode;
        });
    if (file->tracked && last)
        for (size_t i = 0; i < g_deleteMarks.size();)
        {
            DeleteMark& mark = g_deleteMarks[i];
            if (mark.device != file->device || mark.inode != file->inode) { ++i; continue; }
            struct stat named{};
            // A replacement at this path must never be removed with the old file.
            if (fstatat(mark.parent, mark.name.c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0 &&
                named.st_dev == mark.device && named.st_ino == mark.inode)
                unlinkat(mark.parent, mark.name.c_str(), 0);
            close(mark.parent);
            g_deleteMarks.erase(g_deleteMarks.begin() + i);
        }
    delete file;
}

uint32_t Flush(File* file)
{
    int result;
    do { result = fsync(file->fd); } while (result < 0 && errno == EINTR);
    return result == 0 ? 0 : DosError(errno);
}

uint32_t Seek(File* file, int64_t offset, Origin origin, uint64_t& position)
{
    const int whence = origin == Origin::Begin ? SEEK_SET :
                       origin == Origin::End ? SEEK_END : SEEK_CUR;
    const off_t result = lseek(file->fd, off_t(offset), whence);
    if (result < 0) return Status(errno);
    position = uint64_t(result);
    return 0;
}

uint32_t Read(File* file, void* buffer, uint32_t bytes, uint32_t& readBytes)
{
    if (!file->readable) return 0xC0000022u; // STATUS_ACCESS_DENIED
    ssize_t result;
    do { result = read(file->fd, buffer, bytes); } while (result < 0 && errno == EINTR);
    if (result < 0) return Status(errno);
    readBytes = uint32_t(result);
    return 0;
}

uint32_t Write(File* file, const void* buffer, uint32_t bytes, uint32_t& written)
{
    ssize_t result;
    do { result = write(file->fd, buffer, bytes); } while (result < 0 && errno == EINTR);
    if (result < 0) return Status(errno);
    written = uint32_t(result);
    return 0;
}

uint32_t Size(File* file, uint64_t& size)
{
    struct stat info{};
    if (fstat(file->fd, &info) < 0) return Status(errno);
    size = uint64_t(info.st_size);
    return 0;
}

uint32_t SetSize(File* file, uint64_t size)
{
    if (size > INT64_MAX) return 0xC000000Du;
    int result;
    do { result = ftruncate(file->fd, off_t(size)); }
    while (result < 0 && errno == EINTR);
    return result == 0 ? 0 : Status(errno);
}

uint32_t SetDeleteDisposition(File* file, bool remove)
{
    if (file->parent < 0) return 0xC0000022u;
    std::lock_guard openLock(g_openMutex);
    for (size_t i = 0; i < g_deleteMarks.size(); ++i)
        if (g_deleteMarks[i].owner == file)
        {
            if (!remove)
            {
                close(g_deleteMarks[i].parent);
                g_deleteMarks.erase(g_deleteMarks.begin() + i);
            }
            return 0;
        }
    if (!remove) return 0;
    const int parent = fcntl(file->parent, F_DUPFD_CLOEXEC, 3);
    if (parent < 0) return Status(errno);
    g_deleteMarks.push_back(DeleteMark{file, parent, file->name, file->device, file->inode});
    return 0;
}

uint32_t Rename(File* file, const std::filesystem::path& target, bool replace)
{
    if (file->parent < 0 || target.filename().empty()) return 0xC0000022u;
    const std::string requested = target.filename().string();
    std::string name;
    std::lock_guard openLock(g_openMutex);
    bool targetExists = false;
    if (const uint32_t status = ExistingName(file->parent, requested, name, targetExists))
        return status;
    struct stat opened{}, named{}, destination{}, sourceDirectory{};
    if (fstat(file->fd, &opened) != 0 ||
        fstat(file->parent, &sourceDirectory) != 0 ||
        fstatat(file->parent, file->name.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
        opened.st_dev != named.st_dev || opened.st_ino != named.st_ino)
        return 0xC0000034u;
    if (targetExists &&
        fstatat(file->parent, name.c_str(), &destination, AT_SYMLINK_NOFOLLOW) == 0)
    {
        if (S_ISLNK(destination.st_mode)) return 0xC0000022u;
        if (destination.st_dev == opened.st_dev && destination.st_ino == opened.st_ino)
            name = requested; // allow a case-only rename of this file
        for (const File* other : g_openFiles)
            if (other != file && other->device == destination.st_dev &&
                other->inode == destination.st_ino &&
                !(other->share & ShareDelete))
                return SharingViolation;
    }
    else if (targetExists) name = requested; // removed between lookup and stat
#if defined(__linux__)
    const unsigned flags = replace ? 0 : RENAME_NOREPLACE;
    if (syscall(SYS_renameat2, file->parent, file->name.c_str(),
                file->parent, name.c_str(), flags) < 0)
        return Status(errno);
#else
    const unsigned flags = replace ? 0 : RENAME_EXCL;
    if (renameatx_np(file->parent, file->name.c_str(),
                     file->parent, name.c_str(), flags) < 0)
        return Status(errno);
#endif
    const std::string oldName = file->name;
    for (File* other : g_openFiles)
    {
        if (other->device != opened.st_dev || other->inode != opened.st_ino ||
            other->name != oldName) continue;
        struct stat directory{};
        if (fstat(other->parent, &directory) == 0 &&
            directory.st_dev == sourceDirectory.st_dev &&
            directory.st_ino == sourceDirectory.st_ino)
            other->name = name;
    }
    for (DeleteMark& mark : g_deleteMarks)
    {
        if (mark.device != opened.st_dev || mark.inode != opened.st_ino ||
            mark.name != oldName) continue;
        struct stat directory{};
        if (fstat(mark.parent, &directory) == 0 &&
            directory.st_dev == sourceDirectory.st_dev &&
            directory.st_ino == sourceDirectory.st_ino)
            mark.name = name;
    }
    return 0;
}

uint32_t ListRegularFiles(File* directory, std::vector<DirectoryEntry>& entries)
{
    entries.clear();
    if (!directory || directory->tracked) return 0xC0000103u;
    struct stat root{};
    if (fstat(directory->fd, &root) < 0) return Status(errno);
    if (!S_ISDIR(root.st_mode)) return 0xC0000103u;
    const int listing = openat(directory->fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (listing < 0) return Status(errno);
    DIR* stream = fdopendir(listing);
    if (!stream)
    {
        const int error = errno;
        close(listing);
        return Status(error);
    }
    uint32_t status = 0;
    for (;;)
    {
        errno = 0;
        dirent* entry = readdir(stream);
        if (!entry) { if (errno) status = Status(errno); break; }
        const std::string name = entry->d_name;
        if (name.empty() || name[0] == '.') continue;
        if (std::any_of(name.begin(), name.end(),
                        [](unsigned char byte) { return byte >= 0x80; })) continue;
        struct stat info{};
        if (fstatat(directory->fd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) < 0)
        { status = Status(errno); break; }
        if (S_ISDIR(info.st_mode)) continue;
        if (!S_ISREG(info.st_mode) || info.st_size < 0)
        { status = 0xC0000022u; break; }
        entries.push_back({name, uint64_t(info.st_size)});
    }
    closedir(stream);
    if (status) entries.clear();
    return status;
}
} // namespace host_file
