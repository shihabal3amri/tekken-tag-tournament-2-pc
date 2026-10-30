// TLS client stream for the community server's control connection (server/PROTOCOL.md), on Windows' own
// SChannel - no third-party crypto in the game. The server's certificate is self-signed: instead of a CA, the
// caller pins the SHA-256 of the server key (SubjectPublicKeyInfo), trust on first use
// (port/COMMUNITY_SERVER_PLAN.md 7b S1). TLS 1.2 minimum.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#define SECURITY_WIN32
#include <security.h>
#ifndef SCHANNEL_USE_BLACKLISTS
#define SCHANNEL_USE_BLACKLISTS                // SCH_CREDENTIALS / TLS_PARAMETERS (TLS 1.3)
#endif
#include <subauth.h>
#include <schannel.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace community {

class TlsStream
{
public:
    TlsStream() = default;
    TlsStream(const TlsStream&) = delete;
    TlsStream& operator=(const TlsStream&) = delete;
    ~TlsStream();

    // Connects and runs the handshake. `verify` gets the server key's fingerprint and decides whether to go on.
    bool Connect(const std::string& host, uint16_t port, int timeoutMs,
                 const std::function<bool(const uint8_t (&fingerprint)[32])>& verify, std::string& error);
    // Sends the whole buffer (one thread at a time; safe while another thread is in Recv).
    bool Send(const void* data, size_t size);
    // Blocks for decrypted bytes: > 0 bytes read, 0 = the server closed, < 0 = error.
    int Recv(void* data, size_t size);
    // Shuts the connection down (unblocks Recv).
    void Close();
    // Unblocks Recv / Send without releasing the socket (Close does, once no thread can be inside them).
    void Shutdown();
    // Absolute deadline (steady clock, microseconds) for Connect and the following Recv calls; 0 = none.
    void SetDeadline(int64_t deadlineUs) { deadlineUs_ = deadlineUs; }
    // The server's address (for the UDP side).
    bool PeerAddress(sockaddr_storage& addr, int& length) const;
    bool Connected() const { return socket_ != INVALID_SOCKET; }
    const char* ProtocolName() const { return protocol_; }

private:
    bool Handshake(const std::string& host, std::string& error);
    bool SendRaw(const void* data, size_t size);
    bool RecvSome(std::vector<uint8_t>& buffer);    // raw bytes, within the deadline
    int64_t deadlineUs_ = 0;

    SOCKET socket_ = INVALID_SOCKET;
    CredHandle cred_{};
    CtxtHandle ctx_{};
    bool haveCred_ = false, haveCtx_ = false;
    SecPkgContext_StreamSizes sizes_{};
    std::mutex ctxMutex_;                 // SChannel calls on the context
    std::mutex sendMutex_;                // encrypt + send in order
    std::vector<uint8_t> in_;             // encrypted bytes not yet decrypted
    std::vector<uint8_t> plain_;          // decrypted bytes not yet returned
    size_t plainAt_ = 0;
    const char* protocol_ = "";
};

// SHA-256 with Windows CNG.
bool Sha256(const void* data, size_t size, uint8_t (&out)[32]);

} // namespace community
