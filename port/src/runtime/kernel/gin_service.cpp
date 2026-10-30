// Namco's GIN web service for the game (gin_service.h).
//
// The game speaks HTTP/1.1 to the service peer: one request per connection, and it reads the response until the
// connection closes (sub_82A5E360 parses on the zero-length receive; MEASURED in phase 0 S4). A worker thread
// collects each whole request (head + Content-Length body) and answers it:
//  - through the community server that is the network, when that server answers GIN (WELCOME flag; the request
//    travels in a GIN_REQUEST frame, server/PROTOCOL.md "GIN"); the game's first request of an ONLINE MODE visit
//    (its /connect) is also where the saved server gets connected (server_ui::WaitForNetwork);
//  - otherwise here: a GIN login, empty settings (no news), and empty answers to everything else - the replies the
//    game was measured to take (no team, no ghosts, no replays).
// Research (TTT2_GIN_STUB=1): every request is logged with its head and body ([gin] lines, whole requests in
// C:/ttt2/re/srv_tools/gin_log/), and a local answer can be replaced without a rebuild by
// C:/ttt2/re/srv_tools/gin_replies/<METHOD>_<path with '/' as '_'>.txt (the whole response) or .json (the body).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "gin_service.h"
#include "community_net.h"
#include "net_transport.h"
#include "server_ui.h"
#include "xsocket_service.h"
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::mutex g_newsMutex;
std::vector<std::string> g_news;
std::shared_ptr<community::Client> g_newsServer;         // whose news they are

// ---- a server's GIN answer, rebuilt before the game reads it (COMMUNITY_SERVER_PLAN.md 7b S6: everything a server
// sends is bounded and checked first). The body must be JSON (16 levels at most); it is written again with only
// printable ASCII in its strings - no quotes, backslashes or the game's text markup (~ % < > &) - and each string at
// most the largest the game's schemas hold (a ghost's or replay's data, 0x16760). Anything else: no answer. The one
// exception: ghost-data and replay-data are base64 with '+' written %2B (MEASURED 2026-10-22: the game uploads them so
// and gets back what it wrote), so their values keep %XX escapes (hex) - never text the game shows.
constexpr size_t kMaxGinString = 0x16760;

struct JsonCleaner
{
    const std::string& in;
    size_t at = 0;
    std::string out;
    int depth = 0;
    bool data = false;   // the string being read is ghost-data's or replay-data's value

    void Space() { while (at < in.size() && (in[at] == ' ' || in[at] == '\t' || in[at] == '\r' || in[at] == '\n')) ++at; }
    bool Literal(const char* word)
    {
        const size_t n = std::strlen(word);
        if (in.compare(at, n, word) != 0) return false;
        out += word;
        at += n;
        return true;
    }
    bool String()
    {
        if (at >= in.size() || in[at] != '"') return false;
        ++at;
        out += '"';
        size_t kept = 0;
        while (at < in.size() && in[at] != '"')
        {
            char c = in[at++];
            if (c == '\\')
            {
                if (at >= in.size()) return false;
                const char e = in[at++];
                if (e == 'u')
                {
                    if (at + 4 > in.size()) return false;
                    unsigned v = 0;
                    for (int i = 0; i < 4; ++i)
                    {
                        const char h = in[at++];
                        const int d = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
                        if (d < 0) return false;
                        v = v * 16 + unsigned(d);
                    }
                    c = v < 0x80 ? char(v) : 0;
                }
                else if (e == 'n' || e == 't' || e == 'r') c = ' ';
                else if (e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f') c = e == '/' ? '/' : 0;
                else return false;
            }
            else if (static_cast<unsigned char>(c) < 0x20) return false;
            if (data && c == '%' && in[at - 1] == '%' && at + 1 < in.size() && std::isxdigit(static_cast<unsigned char>(in[at])) &&
                std::isxdigit(static_cast<unsigned char>(in[at + 1])) && kept + 3 <= kMaxGinString)
            {
                out.append(in, at - 1, 3);
                at += 2;
                kept += 3;
                continue;
            }
            if (c >= 0x20 && c < 0x7F && !std::strchr("\"\\~%<>&", c) && kept < kMaxGinString)
            {
                out += c;
                ++kept;
            }
        }
        if (at >= in.size()) return false;
        ++at;
        out += '"';
        return true;
    }
    // JSON's number grammar: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
    bool Number()
    {
        const size_t start = at;
        auto digit = [&] { return at < in.size() && in[at] >= '0' && in[at] <= '9'; };
        auto digits = [&] { if (!digit()) return false; while (digit()) ++at; return true; };
        if (at < in.size() && in[at] == '-') ++at;
        if (at < in.size() && in[at] == '0') ++at;
        else if (!digits()) return false;
        if (at < in.size() && in[at] == '.') { ++at; if (!digits()) return false; }
        if (at < in.size() && (in[at] == 'e' || in[at] == 'E'))
        {
            ++at;
            if (at < in.size() && (in[at] == '+' || in[at] == '-')) ++at;
            if (!digits()) return false;
        }
        if (at - start > 32) return false;
        out.append(in, start, at - start);
        return true;
    }
    bool Value()
    {
        Space();
        if (at >= in.size()) return false;
        const char c = in[at];
        if (c == '"') return String();
        if (c == '{' || c == '[')
        {
            if (++depth > 16) return false;
            const char close = c == '{' ? '}' : ']';
            out += c;
            ++at;
            Space();
            if (at < in.size() && in[at] == close) { out += close; ++at; --depth; return true; }
            for (;;)
            {
                Space();
                bool dataKey = false;
                if (c == '{')
                {
                    const size_t key = out.size();
                    if (!String()) return false;
                    dataKey = out.compare(key, std::string::npos, "\"ghost-data\"") == 0 ||
                              out.compare(key, std::string::npos, "\"replay-data\"") == 0;
                    Space();
                    if (at >= in.size() || in[at] != ':') return false;
                    out += ':';
                    ++at;
                }
                data = dataKey;
                const bool ok = Value();
                data = false;
                if (!ok) return false;
                Space();
                if (at >= in.size()) return false;
                if (in[at] == ',') { out += ','; ++at; continue; }
                if (in[at] != close) return false;
                out += close;
                ++at;
                --depth;
                return true;
            }
        }
        if (c == 't') return Literal("true");
        if (c == 'f') return Literal("false");
        if (c == 'n') return Literal("null");
        return Number();
    }
};

// The server's HTTP answer -> the same answer rebuilt from its status and its JSON body (false: not one).
bool CleanGinResponse(const std::string& raw, std::string& clean)
{
    if (raw.size() < 12 || raw.compare(0, 7, "HTTP/1.") != 0) return false;
    const int status = std::atoi(raw.c_str() + 9);
    const size_t head = raw.find("\r\n\r\n");
    if (status < 100 || status > 599 || head == std::string::npos) return false;
    const std::string body = raw.substr(head + 4);
    std::string json;
    if (!body.empty())
    {
        JsonCleaner j{ body };
        if (!j.Value()) return false;
        j.Space();
        if (j.at != body.size()) return false;
        json = std::move(j.out);
    }
    clean = "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " Error") +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(json.size()) + "\r\nConnection: close\r\n\r\n" + json;
    return true;
}

bool Research()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_GIN_STUB"); return e && *e == '1'; }();
    return on;
}

constexpr size_t kMaxRequest = 256u << 10;           // what the server takes (and far more than the game sends)

struct Event { enum Kind { Open, Data, Close } kind; uint32_t conn; std::string bytes; };

class Service final : public xsvc::Handler
{
public:
    void Open(uint32_t conn, uint16_t port) override { Push({ Event::Open, conn, std::to_string(port) }); }
    void Data(uint32_t conn, const uint8_t* data, size_t size) override
    {
        Push({ Event::Data, conn, std::string(reinterpret_cast<const char*>(data), size) });
    }
    void Close(uint32_t conn) override { Push({ Event::Close, conn, {} }); }

    void Run()
    {
        for (;;)
        {
            Event e;
            {
                std::unique_lock l(m_);
                cv_.wait(l, [&] { return !queue_.empty(); });
                e = std::move(queue_.front());
                queue_.pop_front();
            }
            switch (e.kind)
            {
            case Event::Open:
                buffers_[e.conn].clear();
                break;
            case Event::Data:
            {
                std::string& b = buffers_[e.conn];
                b += e.bytes;
                if (b.size() > kMaxRequest)
                {
                    std::fprintf(stderr, "[gin] a request over %zu bytes: connection closed\n", kMaxRequest);
                    b.clear();
                    xsvc::Finish(gin_service::kGinPeer, e.conn);
                    break;
                }
                while (Serve(e.conn)) {}
                break;
            }
            case Event::Close:
                buffers_.erase(e.conn);
                break;
            }
        }
    }

private:
    void Push(Event e)
    {
        {
            std::lock_guard l(m_);
            queue_.push_back(std::move(e));
        }
        cv_.notify_one();
    }

    // One whole request in the connection's buffer: answer it. False when more bytes are needed.
    bool Serve(uint32_t conn)
    {
        std::string& b = buffers_[conn];
        const size_t end = b.find("\r\n\r\n");
        if (end == std::string::npos) return false;
        const std::string head = b.substr(0, end);
        size_t length = 0;
        {
            std::istringstream lines(head);
            for (std::string line; std::getline(lines, line);)
                if (_strnicmp(line.c_str(), "Content-Length:", 15) == 0) length = std::strtoul(line.c_str() + 15, nullptr, 10);
        }
        if (length > kMaxRequest) length = kMaxRequest;
        if (b.size() < end + 4 + length) return false;
        const std::string request = b.substr(0, end + 4 + length);
        b.erase(0, end + 4 + length);

        const size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
        const std::string method = head.substr(0, sp1), target = sp1 == std::string::npos ? std::string() : head.substr(sp1 + 1, sp2 - sp1 - 1);
        const std::string endpoint = Endpoint(target);
        ++requests_;
        if (Research()) LogRequest(head, request.substr(end + 4), method, target);

        std::string response, via = "here";
        const ULONGLONG asked = GetTickCount64();
        if (Forward(request, response))
        {
            via = "the server";
            if (endpoint == "settings") KeepNews(response);
        }
        else response = Local(method, target);
        const std::string status = response.substr(0, response.find("\r\n"));
        std::fprintf(stderr, "[gin] %s %s -> %s (%s, %zu bytes, %llu ms)\n", method.c_str(), endpoint.c_str(), via.c_str(),
                     status.c_str(), response.size(), (unsigned long long)(GetTickCount64() - asked));
        xsvc::Reply(gin_service::kGinPeer, conn, reinterpret_cast<const uint8_t*>(response.data()), response.size());
        xsvc::Finish(gin_service::kGinPeer, conn);   // the game reads until the connection closes
        return true;
    }

    static std::string Endpoint(const std::string& target)
    {
        std::string path = target.substr(0, target.find('?'));
        const std::string prefix = "/2.2/ttt2/xbox/";
        return path.rfind(prefix, 0) == 0 ? path.substr(prefix.size()) : path;
    }

    // Through the community server that is the network (connecting the saved one first when ONLINE MODE opens).
    static bool Forward(const std::string& request, std::string& response)
    {
        if (!server_ui::Enabled()) return false;
        std::shared_ptr<community::Client> server = net::Community();
        if (!server || !server->Connected())
        {
            if (!server_ui::WaitForNetwork(15000)) return false;
            server = net::Community();
            if (!server || !server->Connected()) return false;
        }
        std::vector<uint8_t> out;
        if (!server->GinRequest(std::vector<uint8_t>(request.begin(), request.end()), out, 15000)) return false;
        if (!CleanGinResponse(std::string(out.begin(), out.end()), response))
        {
            std::fprintf(stderr, "[gin] the server's answer (%zu bytes) is not an HTTP answer with a JSON body: not given to the game\n", out.size());
            return false;
        }
        return true;
    }

    // The server's news from its settings answer: messages-news-en, else messages-motd-en (strings the server
    // already cleaned: printable ASCII, no quotes or backslashes - anything else is dropped here too).
    static void KeepNews(const std::string& response)
    {
        const size_t body = response.find("\r\n\r\n");
        if (body == std::string::npos) return;
        const std::string json = response.substr(body + 4);
        std::vector<std::string> lines = Strings(json, "messages-news-en");
        if (lines.empty()) lines = Strings(json, "messages-motd-en");
        std::lock_guard l(g_newsMutex);
        g_news = std::move(lines);
        g_newsServer = net::Community();
        std::fprintf(stderr, "[gin] the server's news: %zu lines\n", g_news.size());
    }

    // The strings of a JSON array "key": [...] (at most 10, printable ASCII).
    static std::vector<std::string> Strings(const std::string& json, const std::string& key)
    {
        std::vector<std::string> out;
        size_t at = json.find("\"" + key + "\"");
        if (at == std::string::npos) return out;
        at = json.find('[', at);
        const size_t end = at == std::string::npos ? at : json.find(']', at);
        if (end == std::string::npos) return out;
        for (size_t i = at + 1; i < end && out.size() < 10;)
        {
            const size_t open = json.find('"', i);
            if (open == std::string::npos || open >= end) break;
            const size_t close = json.find('"', open + 1);
            if (close == std::string::npos || close > end) break;
            std::string s;
            for (size_t k = open + 1; k < close && s.size() < 119; ++k)
                if (json[k] >= 0x20 && json[k] < 0x7F && !std::strchr("\\~%<>&", json[k])) s += json[k];
            if (!s.empty()) out.push_back(std::move(s));
            i = close + 1;
        }
        return out;
    }

    static std::string ReadFile(const std::string& path)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) return {};
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }

    static std::string Http(const std::string& body)
    {
        return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
               "\r\nConnection: close\r\n\r\n" + body;
    }

    // Steam (or a server without GIN): a login, no news, nothing else.
    std::string Local(const std::string& method, const std::string& target)
    {
        const std::string endpoint = Endpoint(target);
        if (Research())
        {
            std::string key = method + "_" + target.substr(0, target.find('?'));
            for (char& c : key) if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?') c = '_';
            const std::string dir = "C:/ttt2/re/srv_tools/gin_replies/";
            std::string whole = ReadFile(dir + key + ".txt");
            if (!whole.empty()) return whole;
            const std::string body = ReadFile(dir + key + ".json");
            if (!body.empty()) return Http(body);
        }
        // the game's GIN library takes a reply only with "message" "OK" (reading data back) or "Created" (an upload),
        // and an upload's reply must have nothing else
        if (endpoint == "connect") return Http(R"({"message":"OK","session_id":"local"})");
        if (method == "POST") return Http(R"({"message":"Created"})");
        return Http(R"({"message":"OK","data":{}})");
    }

    void LogRequest(const std::string& head, const std::string& body, const std::string& method, const std::string& target)
    {
        const size_t eol = head.find("\r\n");
        std::fprintf(stderr, "[gin] #%u %s\n", requests_, head.substr(0, eol).c_str());
        std::istringstream lines(head.substr(eol == std::string::npos ? head.size() : eol + 2));
        for (std::string line; std::getline(lines, line);)
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::fprintf(stderr, "[gin]     %s\n", line.c_str());
        }
        if (!body.empty()) std::fprintf(stderr, "[gin]     body (%zu): %.1500s\n", body.size(), body.c_str());
        std::string name = method + "_" + target.substr(0, target.find('?'));
        for (char& c : name) if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?') c = '_';
        CreateDirectoryA("C:/ttt2/re/srv_tools/gin_log", nullptr);
        char path[MAX_PATH];
        std::snprintf(path, sizeof(path), "C:/ttt2/re/srv_tools/gin_log/%03u%s.txt", requests_, name.c_str());
        std::ofstream(path, std::ios::binary) << head << "\r\n\r\n" << body;
    }

    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Event> queue_;
    std::map<uint32_t, std::string> buffers_;      // worker thread only
    unsigned requests_ = 0;
};

} // namespace

namespace gin_service {

bool Enabled() { return server_ui::Enabled() || Research(); }

std::vector<std::string> News()
{
    const std::shared_ptr<community::Client> server = net::Community();
    std::lock_guard l(g_newsMutex);
    if (!server || !server->Connected() || server != g_newsServer) return {};
    return g_news;
}

void Start()
{
    static const bool started = [] {
        static Service service;
        xsvc::Register(kGinPeer, &service);
        std::thread([] { service.Run(); }).detach();
        std::fprintf(stderr, "[gin] GIN service up (%s)\n", server_ui::Enabled() ? "community servers, else answered here" : "research");
        return true;
    }();
    (void)started;
}

} // namespace gin_service
