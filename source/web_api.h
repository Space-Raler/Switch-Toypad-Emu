// Routing and the hand-over between the web thread and the app's UI thread.
//
// The web thread never touches app state. It
//   * serves the page and the two JSON snapshots the UI thread published, and
//   * turns POST /api/<op> into a WebCommand that the UI thread runs between frames,
//     then waits (briefly) for the result.
// Pure C++ (no libnx) so it is covered by the host tests.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#ifndef __SWITCH__
#include <mutex>
#endif

#include "web_server.h"

namespace sky {

// Small RAII lock that works on the Switch (libnx Mutex) and on a PC.
class WebMutex {
public:
#ifdef __SWITCH__
    void lock() { mutexLock(&m_); }
    void unlock() { mutexUnlock(&m_); }
private:
    Mutex m_{};
#else
    void lock() { m_.lock(); }
    void unlock() { m_.unlock(); }
private:
    std::mutex m_;
#endif
};

class WebLock {
public:
    explicit WebLock(WebMutex& m) : m_(m) { m_.lock(); }
    ~WebLock() { m_.unlock(); }
    WebLock(const WebLock&) = delete;
    WebLock& operator=(const WebLock&) = delete;
private:
    WebMutex& m_;
};

struct WebCommand {
    uint32_t ticket = 0;
    std::string op;                           // load remove move clear mode favorite rescan replug upload
    std::map<std::string, std::string> args;  // decoded query parameters
    std::string body;                         // upload payload
};

struct WebResult {
    bool ok = true;
    std::string message;
};

// Helpers (exposed for tests).
std::string jsonEscape(const std::string& s);
std::string urlDecode(const std::string& s);
std::map<std::string, std::string> parseQuery(const std::string& q);
// Keeps a plain file name: no folders, no leading dots, no .bak/.tmp. Empty = rejected.
std::string sanitizeUploadName(const std::string& raw);
// Same thing, applied to a relative path under the web folder. Empty = rejected.
std::string sanitizeStaticPath(const std::string& raw);
const char* webContentTypeFor(const std::string& path);

class WebApi {
public:
    // Reads a file of the custom website folder (sdmc:/switch/Toypad-Emu/web/<rel>); false if absent.
    using FileLoader = std::function<bool(const std::string& rel, std::string& out)>;

    explicit WebApi(FileLoader loader = nullptr) : loader_(std::move(loader)) {}

    // Web thread.
    void handle(const WebRequest& req, WebResponse& res);

    // UI thread.
    void publishState(std::string json);
    void publishFiles(std::string json);
    bool popCommand(WebCommand& out);
    void finish(uint32_t ticket, bool ok, const std::string& message);

    // How long a POST waits for the UI thread (ms). Tests shorten it.
    void setWaitMs(uint32_t ms) { waitMs_ = ms; }

private:
    bool waitFor(uint32_t ticket, WebResult& out);

    FileLoader loader_;
    WebMutex mutex_;
    std::string state_;
    std::string files_;
    std::deque<WebCommand> queue_;
    std::map<uint32_t, WebResult> results_;
    uint32_t nextTicket_ = 1;
    uint32_t waitMs_ = 2500;
};

// The page that is served when there is no sdmc:/switch/Toypad-Emu/web/index.html.
extern const char* const kWebPage;

}  // namespace sky
