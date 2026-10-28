#include "community_tls.h"
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <poll.h>
#include <fcntl.h>
#include <algorithm>
#include <climits>
namespace community {
namespace {
// Socket BIO uses MSG_NOSIGNAL so a peer closing during a write cannot terminate the game.
BIO_METHOD* SocketBioMethod() {
    static BIO_METHOD* method = [] {
        auto* m = BIO_meth_new(BIO_TYPE_SOURCE_SINK | BIO_get_new_index(), "community socket");
        BIO_meth_set_create(m, [](BIO* b) { BIO_set_init(b, 1); return 1; });
        BIO_meth_set_destroy(m, [](BIO*) { return 1; });
        BIO_meth_set_ctrl(m, [](BIO*, int op, long, void*) -> long { return op == BIO_CTRL_FLUSH ? 1 : 0; });
        BIO_meth_set_read(m, [](BIO* b, char* out, int size) {
            BIO_clear_retry_flags(b); int fd = int(reinterpret_cast<intptr_t>(BIO_get_data(b)));
            int n = int(recv(fd, out, size, 0)); if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) BIO_set_retry_read(b); return n;
        });
        BIO_meth_set_write(m, [](BIO* b, const char* in, int size) {
            BIO_clear_retry_flags(b); int fd = int(reinterpret_cast<intptr_t>(BIO_get_data(b)));
            int n = int(send(fd, in, size, MSG_NOSIGNAL)); if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) BIO_set_retry_write(b); return n;
        }); return m;
    }(); return method;
}
int64_t NowUs() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
bool Ready(int fd, short events, int64_t deadline) {
    for (;;) {
        int timeout = -1;
        if (deadline) { auto left = deadline - NowUs(); if (left <= 0) return false; timeout = int(std::min<int64_t>(INT_MAX, (left + 999) / 1000)); }
        pollfd p{fd, events, 0}; int n = poll(&p, 1, timeout);
        if (n < 0 && errno == EINTR) continue;
        return n > 0 && (p.revents & events);
    }
}
}
bool Sha256(const void* data, size_t size, uint8_t (&out)[32]) { return SHA256(static_cast<const unsigned char*>(data), size, out) != nullptr; }
TlsStream::~TlsStream() { Close(); }
bool TlsStream::Connect(const std::string& host, uint16_t port, int timeoutMs,
    const std::function<bool(const uint8_t (&)[32])>& verify, std::string& error) {
    Close(); auto deadline = deadlineUs_ ? deadlineUs_ : NowUs() + int64_t(timeoutMs) * 1000;
    addrinfo hints{}, *found = nullptr; hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found)) { error = "unknown host " + host; return false; }
    for (auto* a = found; a; a = a->ai_next) {
        int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol); if (fd < 0) continue;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
#if defined(__APPLE__)
        // Darwin: the per-socket SIGPIPE guard, in addition to MSG_NOSIGNAL.
        { int on = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)); }
        fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
        int rc = connect(fd, a->ai_addr, a->ai_addrlen), status = 0; socklen_t len = sizeof(status);
        if ((rc == 0 || (errno == EINPROGRESS && Ready(fd, POLLOUT, deadline))) && getsockopt(fd, SOL_SOCKET, SO_ERROR, &status, &len) == 0 && status == 0) { socket_ = fd; break; }
        close(fd);
    }
    freeaddrinfo(found);
    if (socket_ < 0) { error = "could not reach " + host; return false; }
    int one = 1; setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    sslContext_ = SSL_CTX_new(TLS_client_method());
    if (!sslContext_ || !SSL_CTX_set_min_proto_version(sslContext_, TLS1_2_VERSION)) { error = "could not initialize TLS"; Close(); return false; }
    // Trust is established exclusively by the mandatory SPKI callback below, before application bytes are sent.
    SSL_CTX_set_verify(sslContext_, SSL_VERIFY_NONE, nullptr);
    ssl_ = SSL_new(sslContext_);
    if (!ssl_ || !SSL_set_tlsext_host_name(ssl_, host.c_str())) { error = "could not initialize TLS stream"; Close(); return false; }
    BIO* bio = BIO_new(SocketBioMethod());
    if (!bio) { error = "could not initialize TLS socket"; Close(); return false; }
    BIO_set_data(bio, reinterpret_cast<void*>(intptr_t(socket_))); SSL_set_bio(ssl_, bio, bio);
    for (;;) {
        int n = SSL_connect(ssl_); if (n == 1) break;
        int e = SSL_get_error(ssl_, n);
        if ((e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) || !Ready(socket_, e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, deadline)) { error = "TLS handshake failed or timed out"; Close(); return false; }
    }
    X509* cert = SSL_get_peer_certificate(ssl_);
    unsigned char* der = nullptr; int bytes = cert ? i2d_X509_PUBKEY(X509_get_X509_PUBKEY(cert), &der) : -1;
    uint8_t fingerprint[32]; bool trusted = bytes > 0 && Sha256(der, size_t(bytes), fingerprint) && verify && verify(fingerprint);
    OPENSSL_free(der); X509_free(cert);
    if (!trusted) { error = "server key was not trusted"; Close(); return false; }
    protocol_ = SSL_get_version(ssl_); return true;
}
bool TlsStream::Send(const void* data, size_t size) {
    std::lock_guard order(sendMutex_); auto* p = static_cast<const unsigned char*>(data);
    while (size) {
        int n, e; { std::lock_guard context(ctxMutex_); if (!ssl_) return false; n = SSL_write(ssl_, p, int(std::min<size_t>(size, 16384))); e = n > 0 ? 0 : SSL_get_error(ssl_, n); }
        if (n > 0) { p += n; size -= n; continue; }
        if ((e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) || !Ready(socket_, e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, deadlineUs_)) return false;
    } return true;
}
int TlsStream::Recv(void* data, size_t size) {
    for (;;) {
        int n, e; { std::lock_guard context(ctxMutex_); if (!ssl_) return -1; n = SSL_read(ssl_, data, int(std::min<size_t>(size, INT_MAX))); e = n > 0 ? 0 : SSL_get_error(ssl_, n); }
        if (n > 0) return n; if (e == SSL_ERROR_ZERO_RETURN) return 0;
        if ((e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) || !Ready(socket_, e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, deadlineUs_)) return -1;
    }
}
bool TlsStream::PeerAddress(sockaddr_storage& addr, int& length) const { socklen_t len = sizeof(addr); bool ok = socket_ >= 0 && getpeername(socket_, reinterpret_cast<sockaddr*>(&addr), &len) == 0; length = int(len); return ok; }
void TlsStream::Shutdown() { if (socket_ >= 0) shutdown(socket_, SHUT_RDWR); }
void TlsStream::Close() { std::lock_guard send(sendMutex_); std::lock_guard context(ctxMutex_); if (ssl_) SSL_free(ssl_); ssl_ = nullptr; if (sslContext_) SSL_CTX_free(sslContext_); sslContext_ = nullptr; if (socket_ >= 0) close(socket_); socket_ = -1; protocol_ = ""; }
}
