#include "host_udp.h"

#if !defined(__linux__) && !defined(__APPLE__)
#error "This UDP backend requires Linux or macOS"
#endif

#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace host_udp {
struct Socket { int native = -1; };

Socket* BindLoopback(uint16_t firstPort, uint16_t attempts, uint16_t& boundPort)
{
    boundPort = 0;
#if defined(__APPLE__)
    // macOS has no SOCK_CLOEXEC. The shader prebuild child is the only exec.
    const int native = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (native < 0) return nullptr;
    fcntl(native, F_SETFD, FD_CLOEXEC);
    // kern.ipc.maxsockbuf is 8 MiB by default and caps the buffer below that.
    // A refused size leaves the system default in place, as on Linux.
#else
    const int native = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
    if (native < 0) return nullptr;
#endif
    int big = 8 << 20;
    setsockopt(native, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
    setsockopt(native, SOL_SOCKET, SO_SNDBUF, &big, sizeof(big));
    for (uint16_t index = 0; index < attempts; ++index)
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(uint16_t(firstPort + index));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(native, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0)
        { boundPort = uint16_t(firstPort + index); break; }
    }
    if (!boundPort) { close(native); return nullptr; }
    return new Socket{native};
}

int Receive(Socket* socket, void* data, size_t capacity, uint16_t& fromPort)
{
    sockaddr_in from{};
    socklen_t fromLength = sizeof(from);
    const ssize_t count = recvfrom(socket->native, data, capacity, 0,
                                   reinterpret_cast<sockaddr*>(&from), &fromLength);
    if (count < 0) return errno == EBADF || errno == ENOTSOCK ? -1 : 0;
    fromPort = ntohs(from.sin_port);
    return int(count);
}

bool Send(Socket* socket, uint16_t toPort, const void* data, size_t bytes)
{
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(toPort);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ssize_t sent;
    do {
        sent = sendto(socket->native, data, bytes, 0,
                      reinterpret_cast<sockaddr*>(&to), sizeof(to));
    } while (sent < 0 && errno == EINTR);
    return sent == ssize_t(bytes);
}
}
