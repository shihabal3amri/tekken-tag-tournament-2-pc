// SChannel TLS client (community_tls.h).
#include "community_tls.h"
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ws2_32.lib")

namespace community {
namespace {

constexpr size_t kMaxRecord = 16384 + 2048;        // TLS record plus SChannel's header / trailer room
constexpr size_t kMaxBuffered = 4 * kMaxRecord;

std::string Win32Message(long code)
{
    char text[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, DWORD(code), 0, text, sizeof(text), nullptr);
    std::string s = text;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '.')) s.pop_back();
    char hex[16];
    std::snprintf(hex, sizeof(hex), "0x%08lX", static_cast<unsigned long>(code));
    return s.empty() ? hex : s + " (" + hex + ")";
}

// Connect with a timeout; blocking socket afterwards.
SOCKET ConnectTcp(const std::string& host, uint16_t port, int timeoutMs, std::string& error)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* found = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &found) != 0 || !found)
    {
        error = "unknown host " + host;
        return INVALID_SOCKET;
    }
    SOCKET s = INVALID_SOCKET;
    for (addrinfo* a = found; a && s == INVALID_SOCKET; a = a->ai_next)
    {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        int rc = connect(s, a->ai_addr, int(a->ai_addrlen));
        if (rc == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
        {
            fd_set w, e;
            FD_ZERO(&w);
            FD_ZERO(&e);
            FD_SET(s, &w);
            FD_SET(s, &e);
            timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
            rc = select(0, nullptr, &w, &e, &tv) == 1 && FD_ISSET(s, &w) ? 0 : SOCKET_ERROR;
        }
        if (rc != 0)
        {
            closesocket(s);
            s = INVALID_SOCKET;
            continue;
        }
        nonBlocking = 0;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        BOOL noDelay = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    }
    freeaddrinfo(found);
    if (s == INVALID_SOCKET) error = "could not reach " + host + ":" + service;
    return s;
}

int64_t NowUs()
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return int64_t(double(c.QuadPart) * 1e6 / double(f.QuadPart));
}

} // namespace

bool TlsStream::RecvSome(std::vector<uint8_t>& buffer)
{
    if (buffer.size() >= kMaxBuffered) return false;
    DWORD ms = 0;
    if (deadlineUs_)
    {
        const int64_t left = deadlineUs_ - NowUs();
        if (left <= 0) return false;
        ms = DWORD((std::max)(int64_t(1), left / 1000));
    }
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
    uint8_t chunk[8192];
    const int n = recv(socket_, reinterpret_cast<char*>(chunk), sizeof(chunk), 0);
    if (n <= 0) return false;
    buffer.insert(buffer.end(), chunk, chunk + n);
    return true;
}

bool Sha256(const void* data, size_t size, uint8_t (&out)[32])
{
    return BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, static_cast<PUCHAR>(const_cast<void*>(data)), ULONG(size), out, 32) == 0;
}

TlsStream::~TlsStream() { Close(); }

bool TlsStream::Connect(const std::string& host, uint16_t port, int timeoutMs,
                        const std::function<bool(const uint8_t (&)[32])>& verify, std::string& error)
{
    Close();
    if (!deadlineUs_) deadlineUs_ = NowUs() + int64_t(timeoutMs) * 1000;
    const int64_t left = (deadlineUs_ - NowUs()) / 1000;
    if (left <= 0) { error = "timed out"; return false; }
    socket_ = ConnectTcp(host, port, int(left), error);
    if (socket_ == INVALID_SOCKET) return false;
    DWORD ms = DWORD(left);
    setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
    if (!Handshake(host, error)) { Close(); return false; }

    // The server's key: the SubjectPublicKeyInfo of its certificate, re-encoded as DER, hashed.
    PCCERT_CONTEXT cert = nullptr;
    if (QueryContextAttributesW(&ctx_, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert) != SEC_E_OK || !cert)
    {
        error = "the server sent no certificate";
        Close();
        return false;
    }
    BYTE* spki = nullptr;
    DWORD spkiSize = 0;
    const BOOL encoded = CryptEncodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO, &cert->pCertInfo->SubjectPublicKeyInfo,
                                             CRYPT_ENCODE_ALLOC_FLAG, nullptr, &spki, &spkiSize);
    CertFreeCertificateContext(cert);
    uint8_t fingerprint[32] = {};
    const bool hashed = encoded && Sha256(spki, spkiSize, fingerprint);
    if (spki) LocalFree(spki);
    if (!hashed)
    {
        error = "could not read the server's key";
        Close();
        return false;
    }
    if (!verify(fingerprint))
    {
        error = "server key refused";
        Close();
        return false;
    }
    SecPkgContext_ConnectionInfo info{};
    if (QueryContextAttributesW(&ctx_, SECPKG_ATTR_CONNECTION_INFO, &info) == SEC_E_OK)
        protocol_ = (info.dwProtocol & SP_PROT_TLS1_3_CLIENT) ? "TLS 1.3" : (info.dwProtocol & SP_PROT_TLS1_2_CLIENT) ? "TLS 1.2" : "old TLS";
    if (!(info.dwProtocol & (SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_3_CLIENT)))
    {
        error = "the server negotiated an old TLS version";
        Close();
        return false;
    }
    if (QueryContextAttributesW(&ctx_, SECPKG_ATTR_STREAM_SIZES, &sizes_) != SEC_E_OK)
    {
        error = "TLS stream sizes";
        Close();
        return false;
    }
    return true;
}

bool TlsStream::Handshake(const std::string& host, std::string& error)
{
    // TLS 1.2 + 1.3 with SCH_CREDENTIALS (Windows 10 1809+), else TLS 1.2 with the older SCHANNEL_CRED.
    TLS_PARAMETERS tlsParams{};
    tlsParams.grbitDisabledProtocols = ~DWORD(SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_3_CLIENT);
    SCH_CREDENTIALS creds{};
    creds.dwVersion = SCH_CREDENTIALS_VERSION;
    creds.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;
    creds.cTlsParameters = 1;
    creds.pTlsParameters = &tlsParams;
    TimeStamp expiry{};
    SECURITY_STATUS st = AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND, nullptr, &creds,
                                                   nullptr, nullptr, &cred_, &expiry);
    if (st != SEC_E_OK)
    {
        SCHANNEL_CRED old{};
        old.dwVersion = SCHANNEL_CRED_VERSION;
        old.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
        old.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;
        st = AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND, nullptr, &old, nullptr,
                                       nullptr, &cred_, &expiry);
    }
    if (st != SEC_E_OK)
    {
        error = "TLS credentials: " + Win32Message(st);
        return false;
    }
    haveCred_ = true;

    std::wstring target(host.begin(), host.end());
    const DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_REQ_EXTENDED_ERROR |
                        ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_MANUAL_CRED_VALIDATION;
    in_.clear();
    bool first = true;
    for (int rounds = 0; rounds < 64; ++rounds)
    {
        SecBuffer inBufs[2] = { { ULONG(in_.size()), SECBUFFER_TOKEN, in_.empty() ? nullptr : in_.data() }, { 0, SECBUFFER_EMPTY, nullptr } };
        SecBufferDesc inDesc{ SECBUFFER_VERSION, 2, inBufs };
        SecBuffer outBufs[1] = { { 0, SECBUFFER_TOKEN, nullptr } };
        SecBufferDesc outDesc{ SECBUFFER_VERSION, 1, outBufs };
        DWORD outFlags = 0;
        st = InitializeSecurityContextW(&cred_, first ? nullptr : &ctx_, first ? target.data() : nullptr, flags, 0, 0,
                                        first ? nullptr : &inDesc, 0, &ctx_, &outDesc, &outFlags, nullptr);
        if (first && (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED || st == SEC_E_INCOMPLETE_MESSAGE)) haveCtx_ = true;
        first = false;
        if (outBufs[0].pvBuffer)
        {
            const bool sent = outBufs[0].cbBuffer == 0 || SendRaw(outBufs[0].pvBuffer, outBufs[0].cbBuffer);
            FreeContextBuffer(outBufs[0].pvBuffer);
            if (!sent) { error = "the connection closed during the TLS handshake"; return false; }
        }
        if (st == SEC_E_INCOMPLETE_MESSAGE)
        {
            if (!RecvSome(in_)) { error = "the connection closed during the TLS handshake"; return false; }
            continue;
        }
        if (st != SEC_E_OK && st != SEC_I_CONTINUE_NEEDED)
        {
            error = "TLS handshake: " + Win32Message(st);
            return false;
        }
        // keep what the call did not consume
        if (inBufs[1].BufferType == SECBUFFER_EXTRA && inBufs[1].cbBuffer)
            in_.erase(in_.begin(), in_.end() - inBufs[1].cbBuffer);
        else if (!in_.empty())
            in_.clear();
        if (st == SEC_E_OK) return true;
        if (in_.empty() && !RecvSome(in_)) { error = "the connection closed during the TLS handshake"; return false; }
    }
    error = "TLS handshake did not finish";
    return false;
}

bool TlsStream::SendRaw(const void* data, size_t size)
{
    const char* p = static_cast<const char*>(data);
    while (size)
    {
        const int n = send(socket_, p, int((std::min)(size, size_t(1) << 20)), 0);
        if (n <= 0) return false;
        p += n;
        size -= size_t(n);
    }
    return true;
}

bool TlsStream::Send(const void* data, size_t size)
{
    std::lock_guard sl(sendMutex_);
    if (socket_ == INVALID_SOCKET) return false;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    std::vector<uint8_t> record;
    while (size)
    {
        const size_t chunk = (std::min)(size, size_t(sizes_.cbMaximumMessage));
        record.resize(sizes_.cbHeader + chunk + sizes_.cbTrailer);
        std::memcpy(record.data() + sizes_.cbHeader, p, chunk);
        SecBuffer bufs[4] = {
            { sizes_.cbHeader, SECBUFFER_STREAM_HEADER, record.data() },
            { ULONG(chunk), SECBUFFER_DATA, record.data() + sizes_.cbHeader },
            { sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, record.data() + sizes_.cbHeader + chunk },
            { 0, SECBUFFER_EMPTY, nullptr },
        };
        SecBufferDesc desc{ SECBUFFER_VERSION, 4, bufs };
        SECURITY_STATUS st;
        {
            std::lock_guard cl(ctxMutex_);
            if (!haveCtx_) return false;
            st = EncryptMessage(&ctx_, 0, &desc, 0);
        }
        if (st != SEC_E_OK) return false;
        if (!SendRaw(record.data(), bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer)) return false;
        p += chunk;
        size -= chunk;
    }
    return true;
}

int TlsStream::Recv(void* data, size_t size)
{
    for (;;)
    {
        if (plainAt_ < plain_.size())
        {
            const size_t n = (std::min)(size, plain_.size() - plainAt_);
            std::memcpy(data, plain_.data() + plainAt_, n);
            plainAt_ += n;
            if (plainAt_ == plain_.size()) { plain_.clear(); plainAt_ = 0; }
            return int(n);
        }
        if (socket_ == INVALID_SOCKET) return -1;
        if (!in_.empty())
        {
            SecBuffer bufs[4] = { { ULONG(in_.size()), SECBUFFER_DATA, in_.data() }, {}, {}, {} };
            SecBufferDesc desc{ SECBUFFER_VERSION, 4, bufs };
            SECURITY_STATUS st;
            {
                std::lock_guard cl(ctxMutex_);
                if (!haveCtx_) return -1;
                st = DecryptMessage(&ctx_, &desc, 0, nullptr);
            }
            if (st == SEC_E_OK)
            {
                size_t extra = 0;
                for (const SecBuffer& b : bufs)
                {
                    if (b.BufferType == SECBUFFER_DATA && b.cbBuffer)
                        plain_.insert(plain_.end(), static_cast<uint8_t*>(b.pvBuffer), static_cast<uint8_t*>(b.pvBuffer) + b.cbBuffer);
                    if (b.BufferType == SECBUFFER_EXTRA) extra = b.cbBuffer;
                }
                in_.erase(in_.begin(), in_.end() - extra);
                continue;
            }
            if (st == SEC_I_CONTEXT_EXPIRED) return 0;          // close_notify
            if (st != SEC_E_INCOMPLETE_MESSAGE) return -1;      // tampering, renegotiation (refused), anything else
        }
        if (!RecvSome(in_)) return in_.empty() ? 0 : -1;
    }
}

bool TlsStream::PeerAddress(sockaddr_storage& addr, int& length) const
{
    length = sizeof(addr);
    return socket_ != INVALID_SOCKET && getpeername(socket_, reinterpret_cast<sockaddr*>(&addr), &length) == 0;
}

void TlsStream::Shutdown()
{
    const SOCKET s = socket_;
    if (s != INVALID_SOCKET) shutdown(s, SD_BOTH);
}

void TlsStream::Close()
{
    std::lock_guard sl(sendMutex_);
    if (socket_ != INVALID_SOCKET)
    {
        shutdown(socket_, SD_BOTH);
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
    std::lock_guard cl(ctxMutex_);
    if (haveCtx_) { DeleteSecurityContext(&ctx_); haveCtx_ = false; }
    if (haveCred_) { FreeCredentialsHandle(&cred_); haveCred_ = false; }
}

} // namespace community
