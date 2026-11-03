#include "web_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace sky {

namespace {

uint64_t nowMs() {
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

// Reads at least one byte into `buf` or fails when the deadline passes / the peer closes.
bool recvSome(int fd, std::string& buf, uint64_t deadlineMs) {
    for (;;) {
        const uint64_t now = nowMs();
        if (now >= deadlineMs) return false;
        pollfd p{fd, POLLIN, 0};
        const int rc = poll(&p, 1, int(std::min<uint64_t>(deadlineMs - now, 500)));
        if (rc < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (rc == 0) continue;
        char tmp[2048];
        const ssize_t n = recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) return false;
        buf.append(tmp, size_t(n));
        return true;
    }
}

bool sendAll(int fd, const std::string& data, uint64_t deadlineMs) {
    size_t off = 0;
    while (off < data.size()) {
        const uint64_t now = nowMs();
        if (now >= deadlineMs) return false;
        pollfd p{fd, POLLOUT, 0};
        const int rc = poll(&p, 1, int(std::min<uint64_t>(deadlineMs - now, 500)));
        if (rc < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (rc == 0) continue;
        const ssize_t n = send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += size_t(n);
    }
    return true;
}

}  // namespace

const char* webStatusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "Status";
    }
}

bool webParseHead(const std::string& head, WebRequest& out) {
    out = WebRequest{};
    size_t eol = head.find('\n');
    const std::string line = trim(head.substr(0, eol));
    const size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos || sp1 == 0) return false;
    const size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return false;
    out.method = line.substr(0, sp1);
    std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::string version = line.substr(sp2 + 1);
    if (version.compare(0, 5, "HTTP/") != 0) return false;
    if (target.empty() || target[0] != '/') return false;
    const size_t q = target.find('?');
    if (q == std::string::npos) {
        out.path = target;
    } else {
        out.path = target.substr(0, q);
        out.query = target.substr(q + 1);
    }
    while (eol != std::string::npos && eol + 1 < head.size()) {
        const size_t start = eol + 1;
        eol = head.find('\n', start);
        const std::string h = head.substr(start, eol == std::string::npos ? std::string::npos : eol - start);
        const size_t colon = h.find(':');
        if (colon == std::string::npos || colon == 0) continue;
        out.headers[lower(trim(h.substr(0, colon)))] = trim(h.substr(colon + 1));
    }
    return true;
}

std::string webFormatResponse(const WebResponse& r) {
    std::string s = "HTTP/1.1 " + std::to_string(r.status) + " " + webStatusText(r.status) + "\r\n";
    s += "Content-Type: " + r.contentType + "\r\n";
    s += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
    s += "Cache-Control: no-store\r\n";
    s += "X-Content-Type-Options: nosniff\r\n";
    s += "Connection: close\r\n\r\n";
    s += r.body;
    return s;
}

bool WebServer::start(uint16_t port, WebHandler handler) {
    if (running_) return true;
    handler_ = std::move(handler);
    error_.clear();

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error_ = "socket() failed";
        return false;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        error_ = "bind() failed (port in use?)";
        close(fd);
        return false;
    }
    if (listen(fd, 4) != 0) {
        error_ = "listen() failed";
        close(fd);
        return false;
    }
    sockaddr_in bound{};
    socklen_t len = sizeof bound;
    port_ = (getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) == 0) ? ntohs(bound.sin_port) : port;

    listenFd_ = fd;
    stopRequested_ = false;
#ifdef __SWITCH__
    Result rc = threadCreate(&thread_, &WebServer::threadEntry, this, nullptr, 0x10000, 0x2C, -2);
    if (R_SUCCEEDED(rc)) rc = threadStart(&thread_);
    if (R_FAILED(rc)) {
        error_ = "could not start the web thread";
        close(fd);
        listenFd_ = -1;
        return false;
    }
#else
    thread_ = std::thread([this] { run(); });
#endif
    running_ = true;
    return true;
}

void WebServer::stop() {
    if (!running_) return;
    stopRequested_ = true;
#ifdef __SWITCH__
    threadWaitForExit(&thread_);
    threadClose(&thread_);
#else
    if (thread_.joinable()) thread_.join();
#endif
    if (listenFd_ >= 0) close(listenFd_);
    listenFd_ = -1;
    running_ = false;
}

void WebServer::run() {
    while (!stopRequested_) {
        pollfd p{listenFd_, POLLIN, 0};
        const int rc = poll(&p, 1, 200);
        if (rc <= 0) continue;
        const int c = accept(listenFd_, nullptr, nullptr);
        if (c < 0) continue;
        serve(c);
        close(c);
    }
}

void WebServer::serve(int fd) {
    const uint64_t deadline = nowMs() + 6000;
    WebResponse res;
    auto fail = [&](int status, const char* msg) {
        res = WebResponse{};
        res.status = status;
        res.body = msg;
    };

    std::string buf;
    size_t headEnd = std::string::npos;
    while ((headEnd = buf.find("\r\n\r\n")) == std::string::npos) {
        if (buf.size() > kMaxHead) break;
        if (!recvSome(fd, buf, deadline)) break;
    }

    WebRequest req;
    if (headEnd == std::string::npos) {
        fail(buf.size() > kMaxHead ? 431 : 408, "bad or incomplete request");
    } else if (!webParseHead(buf.substr(0, headEnd), req)) {
        fail(400, "malformed request");
    } else {
        size_t want = 0;
        const auto cl = req.headers.find("content-length");
        if (cl != req.headers.end()) {
            char* end = nullptr;
            const unsigned long long v = std::strtoull(cl->second.c_str(), &end, 10);
            if (end == cl->second.c_str() || v > kMaxBody) {
                fail(v > kMaxBody ? 413 : 400, "bad Content-Length");
                want = size_t(-1);
            } else {
                want = size_t(v);
            }
        }
        if (want != size_t(-1)) {
            std::string body = buf.substr(headEnd + 4);
            bool ok = true;
            while (body.size() < want && ok) ok = recvSome(fd, body, deadline);
            if (!ok) {
                fail(408, "request body incomplete");
            } else {
                req.body = body.substr(0, want);
                handler_(req, res);
            }
        }
    }
    ++served_;
    sendAll(fd, webFormatResponse(res), nowMs() + 6000);
}

}  // namespace sky
