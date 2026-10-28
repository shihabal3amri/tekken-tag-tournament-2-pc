#include "host_random.h"

#if !defined(__linux__) && !defined(__APPLE__)
#error "This random backend requires Linux or macOS"
#endif

#include <cerrno>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#if defined(__linux__)
#include <sys/random.h>
#endif

namespace host_random {
void Fill(void* data, size_t bytes)
{
#if defined(__APPLE__)
    // The kernel CSPRNG; arc4random_buf cannot fail.
    arc4random_buf(data, bytes);
#else
    auto* next = static_cast<unsigned char*>(data);
    while (bytes)
    {
        const ssize_t count = getrandom(next, std::min(bytes, size_t(1 << 20)), 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0)
        {
            std::perror("getrandom");
            std::abort();
        }
        next += count;
        bytes -= size_t(count);
    }
#endif
}
}
