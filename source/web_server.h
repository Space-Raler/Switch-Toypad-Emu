// Tiny HTTP/1.1 server for the local website (plain POSIX sockets, so it builds on the
// Switch with libnx's BSD sockets and on a PC for the host tests).
//
// One connection is served at a time ("Connection: close"); that is plenty for a page that
// polls once a second and keeps the memory use of the app small. The server thread never
// touches app state: it only calls the handler it was given.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

#ifdef __SWITCH__
#include <switch.h>
#else
#include <thread>
#endif

namespace sky {

struct WebRequest {
    std::string method;                         // "GET", "POST", ...
    std::string path;                           // "/api/state" (no query)
    std::string query;                          // "a=1&b=2" (no leading '?')
    std::map<std::string, std::string> headers; // names lower-cased
    std::string body;
};

struct WebResponse {
    int status = 200;
    std::string contentType = "text/plain; charset=utf-8";
    std::string body;
};

using WebHandler = std::function<void(const WebRequest&, WebResponse&)>;

// Parsing / formatting, exposed for the tests.
// `head` is the request line plus headers, without the blank line. Returns false if malformed.
bool webParseHead(const std::string& head, WebRequest& out);
std::string webFormatResponse(const WebResponse& r);
const char* webStatusText(int status);

class WebServer {
public:
    static constexpr size_t kMaxHead = 8192;
    static constexpr size_t kMaxBody = 16384;  // figure dumps are at most 1024 bytes

    WebServer() = default;
    ~WebServer() { stop(); }
    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    // Binds 0.0.0.0:port and starts the server thread. port 0 picks a free port (tests).
    bool start(uint16_t port, WebHandler handler);
    void stop();
    bool running() const { return running_; }
    uint16_t port() const { return port_; }
    const std::string& error() const { return error_; }
    uint32_t served() const { return served_; }

private:
    static void threadEntry(void* self) { static_cast<WebServer*>(self)->run(); }
    void run();
    void serve(int fd);

    WebHandler handler_;
    std::atomic<bool> stopRequested_{false};
    bool running_ = false;
    uint16_t port_ = 0;
    int listenFd_ = -1;
    std::string error_;
    std::atomic<uint32_t> served_{0};
#ifdef __SWITCH__
    Thread thread_{};
#else
    std::thread thread_;
#endif
};

}  // namespace sky
