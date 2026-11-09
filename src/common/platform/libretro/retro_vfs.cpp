#include "retro_vfs.h"
#include "fs_files.h"
#ifdef __ANDROID__
#include "retro_frontend_stack.h"
#endif

#include <algorithm>
#include <cstdio>
#include <limits>
#include <memory>
#include <cstring>
#include <cerrno>

namespace Libretro
{
namespace
{
struct ReadAPI
{
    retro_vfs_open_t open = nullptr;
    retro_vfs_close_t close = nullptr;
    retro_vfs_size_t size = nullptr;
    retro_vfs_seek_t seek = nullptr;
    retro_vfs_read_t read = nullptr;
};

ReadAPI api;
retro_vfs_interface fullAPI{};

#ifdef __ANDROID__
retro_vfs_interface frontendAPI{};

template<auto Member> struct VFSCallback;

template<typename Result, typename... Args,
    Result (*retro_vfs_interface::*Member)(Args...)>
struct VFSCallback<Member>
{
    static Result Call(Args... args)
    {
        Result result{};
        auto operation = [&] { result = (frontendAPI.*Member)(args...); };
        RetroFrontendStack::Call([](void *data) {
            (*static_cast<decltype(operation) *>(data))();
        }, &operation);

        return result;
    }
};
#endif

class VFSReader final : public FileSys::FileReaderInterface
{
public:
    explicit VFSReader(ReadAPI functions) : functions(functions) {}
    ~VFSReader() override
    {
        if (stream)
            functions.close(stream);
    }

    bool Open(const char *path, ptrdiff_t start, ptrdiff_t length)
    {
        if (!path || start < 0 || length < -1)
            return false;

        stream = functions.open(path, RETRO_VFS_FILE_ACCESS_READ,
            RETRO_VFS_FILE_ACCESS_HINT_NONE);
        if (!stream)
            return false;

        const auto size = functions.size(stream);
        if (size < 0 || size > std::numeric_limits<ptrdiff_t>::max() || start > size)
            return false;

        base = start;
        Length = static_cast<ptrdiff_t>(size) - start;
        if (length >= 0)
            Length = std::min(Length, length);

        return functions.seek(stream, base, RETRO_VFS_SEEK_POSITION_START) == 0;
    }

    ptrdiff_t Tell() const override
    {
        return position;
    }

    ptrdiff_t Seek(ptrdiff_t offset, int origin) override
    {
        ptrdiff_t reference;

        switch (origin)
        {
        case SEEK_SET:
            reference = 0;
            break;
        case SEEK_CUR:
            reference = position;
            break;
        case SEEK_END:
            reference = Length;
            break;
        default:
            return -1;
        }

        // Check before adding, including PTRDIFF_MIN and PTRDIFF_MAX offsets.
        if (offset < -reference || offset > Length - reference)
            return -1;

        const auto target = reference + offset;
        if (functions.seek(stream, base + target, RETRO_VFS_SEEK_POSITION_START)
            != 0)
            return -1;

        position = target;

        return 0;
    }

    ptrdiff_t Read(void *buffer, ptrdiff_t length) override
    {
        if (!buffer || length <= 0)
            return 0;

        const auto requested = std::min(length, Length - position);
        ptrdiff_t total = 0;

        // VFS backends may return a short read before EOF.
        while (total < requested)
        {
            const auto count = functions.read(stream,
                static_cast<char *>(buffer) + total, requested - total);
            if (count <= 0 || count > requested - total)
                break;
            total += static_cast<ptrdiff_t>(count);
            position += static_cast<ptrdiff_t>(count);
        }

        return total;
    }

    char *Gets(char *buffer, ptrdiff_t length) override
    {
        if (!buffer || length <= 0 || position == Length)
            return nullptr;

        ptrdiff_t used = 0;
        while (used < length - 1)
        {
            char c;
            if (Read(&c, 1) != 1)
                break;
            buffer[used++] = c;
            if (c == '\n')
                break;
        }

        buffer[used] = '\0';

        return used || length == 1 ? buffer : nullptr;
    }

private:
    ReadAPI functions;
    retro_vfs_file_handle *stream = nullptr;
    ptrdiff_t base = 0;
    ptrdiff_t position = 0;
};
}

bool InitVFS(retro_environment_t environment)
{
    api = {}; fullAPI = {};
    retro_vfs_interface_info info = {3, nullptr};

    bool v3 = environment && environment(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &info) && info.iface;
    if (!v3)
        info = {1, nullptr};
    if (!environment || (!v3 && !environment(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &info))
        || !info.iface || info.required_interface_version < 1)
        return false;
    const auto &vfs = *info.iface;
    if (!vfs.open || !vfs.close || !vfs.size || !vfs.seek || !vfs.read)
        return false;

    fullAPI.open = vfs.open; fullAPI.close = vfs.close; fullAPI.size = vfs.size;
    fullAPI.tell = vfs.tell; fullAPI.seek = vfs.seek; fullAPI.read = vfs.read;
    fullAPI.write = vfs.write; fullAPI.flush = vfs.flush;
    fullAPI.remove = vfs.remove; fullAPI.rename = vfs.rename;

    if (v3)
    {
        fullAPI.stat = vfs.stat; fullAPI.mkdir = vfs.mkdir;
        fullAPI.opendir = vfs.opendir; fullAPI.readdir = vfs.readdir;
        fullAPI.dirent_get_name = vfs.dirent_get_name;
        fullAPI.dirent_is_dir = vfs.dirent_is_dir; fullAPI.closedir = vfs.closedir;
    }

#ifdef __ANDROID__
    frontendAPI = fullAPI;
#define WRAP_VFS(member) \
    if (frontendAPI.member) \
        fullAPI.member = VFSCallback<&retro_vfs_interface::member>::Call
    WRAP_VFS(open);
    WRAP_VFS(close);
    WRAP_VFS(size);
    WRAP_VFS(tell);
    WRAP_VFS(seek);
    WRAP_VFS(read);
    WRAP_VFS(write);
    WRAP_VFS(flush);
    WRAP_VFS(remove);
    WRAP_VFS(rename);
    WRAP_VFS(stat);
    WRAP_VFS(mkdir);
    WRAP_VFS(opendir);
    WRAP_VFS(readdir);
    WRAP_VFS(dirent_get_name);
    WRAP_VFS(dirent_is_dir);
    WRAP_VFS(closedir);
#undef WRAP_VFS
#endif
    api = {fullAPI.open, fullAPI.close, fullAPI.size, fullAPI.seek, fullAPI.read};
    return true;
}

bool HasVFS()
{
    return api.open != nullptr;
}

FileSys::FileReaderInterface *OpenVFSReader(const char *path, ptrdiff_t start, ptrdiff_t length)
{
    if (!HasVFS())
        return nullptr;

    auto reader = std::make_unique<VFSReader>(api);
    if (!reader->Open(path, start, length))
        return nullptr;

    return reader.release();
}
}

namespace Libretro
{
const retro_vfs_interface *VFS()
{
    return HasVFS() ? &fullAPI : nullptr;
}

bool StatVFS(const char *path, bool *directory, size_t *size)
{
    if (directory)
        *directory = false;

    if (fullAPI.stat)
    {
        int32_t bytes = 0;
        int flags = fullAPI.stat(path, &bytes);
        if (directory) *directory = (flags & RETRO_VFS_STAT_IS_DIRECTORY) != 0;
        if (size) *size = bytes < 0 ? 0 : size_t(bytes);

        return (flags & RETRO_VFS_STAT_IS_VALID) != 0;
    }

    auto file = fullAPI.open(path, RETRO_VFS_FILE_ACCESS_READ, 0);
    if (!file)
        return false;

    auto bytes = fullAPI.size(file);
    fullAPI.close(file);
    if (size)
        *size = bytes < 0 ? 0 : size_t(bytes);

    return bytes >= 0;
}
#ifndef _WIN32
namespace
{
struct Cookie { retro_vfs_interface api; retro_vfs_file_handle *file; };

ssize_t CookieRead(void *opaque, char *data, size_t size)
{
    auto c = static_cast<Cookie *>(opaque); return c->api.read(c->file, data, size);
}

ssize_t CookieWrite(void *opaque, const char *data, size_t size)
{
    auto c = static_cast<Cookie *>(opaque);
    if (!c->api.write)
    { 
        errno = EACCES;
        return -1;
    }

    return c->api.write(c->file, data, size);
}

#ifdef __APPLE__
using CookieOffset = fpos_t;
#else
using CookieOffset = off64_t;
#endif

int CookieSeek(void *opaque, CookieOffset *offset, int origin)
{
    auto c = static_cast<Cookie *>(opaque);
    int whence = origin == SEEK_SET ? RETRO_VFS_SEEK_POSITION_START :
        origin == SEEK_CUR ? RETRO_VFS_SEEK_POSITION_CURRENT : RETRO_VFS_SEEK_POSITION_END;

    auto result = c->api.seek(c->file, *offset, whence);
    if (result < 0)
    {
        return -1;
    }
    if (!c->api.tell)
    {
        errno = ESPIPE; return -1;
    }

    auto position = c->api.tell(c->file);

    if (position < 0)
        return -1;

    *offset = position; return 0;
}

int CookieClose(void *opaque)
{
    std::unique_ptr<Cookie> c(static_cast<Cookie *>(opaque));

    return c->api.close(c->file);
}

} // anonymous namespace

FILE *OpenVFSFile(const char *path, const char *mode)
{
    if (!HasVFS() || !path || !mode)
        return nullptr;

    unsigned access = strchr(mode, '+') ? RETRO_VFS_FILE_ACCESS_READ_WRITE :
        mode[0] == 'r' ? RETRO_VFS_FILE_ACCESS_READ : RETRO_VFS_FILE_ACCESS_WRITE;
    if (mode[0] == 'a' || (mode[0] == 'r' && strchr(mode, '+')))
        access |= RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;

    auto file = fullAPI.open(path, access, 0);
    if (!file)
        return nullptr;

    auto cookie = new Cookie{fullAPI, file};
    if (mode[0] == 'a')
    {
        fullAPI.seek(file, 0, RETRO_VFS_SEEK_POSITION_END);
    }
#if defined(__APPLE__) || (defined(__ANDROID__) && __ANDROID_API__ < 24)
    FILE *stream = funopen(cookie,
        [](void *c, char *buffer, int count) { return int(CookieRead(c, buffer, count)); },
        [](void *c, const char *buffer, int count) { return int(CookieWrite(c, buffer, count)); },
        [](void *c, fpos_t offset, int origin) -> fpos_t {
            CookieOffset position = offset;
            if (CookieSeek(c, &position, origin) != 0)
            {
                return -1;
            }
            if (position > std::numeric_limits<fpos_t>::max())
            {
                errno = EOVERFLOW;
                return -1;
            }
            return static_cast<fpos_t>(position);
        }, CookieClose);
#elif defined(__ANDROID__)
    FILE *stream = funopen64(cookie,
        [](void *c, char *buffer, int count) { return int(CookieRead(c, buffer, count)); },
        [](void *c, const char *buffer, int count) { return int(CookieWrite(c, buffer, count)); },
        [](void *c, fpos64_t offset, int origin) -> fpos64_t {
            CookieOffset position = offset;
            return CookieSeek(c, &position, origin) == 0 ? position : -1;
        }, CookieClose);
#else
    cookie_io_functions_t functions{CookieRead, CookieWrite, CookieSeek, CookieClose};
    FILE *stream = fopencookie(cookie, mode, functions);
#endif
    if (!stream)
        CookieClose(cookie);

    return stream;
}
#else
FILE *OpenVFSFile(const char *, const char *)
{
    // Windows CRT streams cannot wrap frontend VFS handles. Readers and writers
    // use their engine interfaces directly instead.
    errno = ENOSYS;
    return nullptr;
}
#endif

FileSys::FileWriter *OpenVFSWriter(const char *path)
{
    class Writer final : public FileSys::FileWriter
    {
        retro_vfs_interface api;
        retro_vfs_file_handle *file;
    public:
        Writer(retro_vfs_interface api, retro_vfs_file_handle *file) : api(api), file(file) {}
        ~Writer() override { Close(); }

        void Close() override { if (file) api.close(file); file = nullptr; }

        size_t Write(const void *buffer, size_t size) override
        {
            if (!file)
                return 0;

            size_t total = 0;

            while (total < size)
            {
                auto count = api.write(file, static_cast<const char *>(buffer) + total, size - total);
                if (count <= 0 || uint64_t(count) > size - total)
                    break;
                total += size_t(count);
            }

            return total;
        }

        ptrdiff_t Tell() override
        {
            return file && api.tell ? api.tell(file) : -1;
        }

        ptrdiff_t Seek(ptrdiff_t offset, int origin) override
        {
            int whence = origin == SEEK_SET ? RETRO_VFS_SEEK_POSITION_START :
                origin == SEEK_CUR ? RETRO_VFS_SEEK_POSITION_CURRENT :
                origin == SEEK_END ? RETRO_VFS_SEEK_POSITION_END : -1;
            return file && whence >= 0 ? api.seek(file, offset, whence) : -1;
        }
    };

    if (!HasVFS() || !fullAPI.write)
        return nullptr;

    auto file = fullAPI.open(path, RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);

    return file ? new Writer(fullAPI, file) : nullptr;
}
}
