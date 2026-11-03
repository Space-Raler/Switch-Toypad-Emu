// Host tests for the local website: HTTP parsing, routing, upload-name rules, and a real
// loopback round trip through WebServer + WebApi with a fake "UI thread".
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "web_api.h"
#include "web_server.h"

using namespace sky;

static int g_checks = 0, g_fail = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static std::string httpRaw(uint16_t port, const std::string& req) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { close(fd); return ""; }
    send(fd, req.data(), req.size(), 0);
    std::string out;
    char b[4096];
    ssize_t n;
    while ((n = recv(fd, b, sizeof b, 0)) > 0) out.append(b, size_t(n));
    close(fd);
    return out;
}

static std::string bodyOf(const std::string& r) {
    const size_t p = r.find("\r\n\r\n");
    return p == std::string::npos ? "" : r.substr(p + 4);
}

int main() {
    // helpers
    CHECK(jsonEscape("a\"b\\c\n") == "a\\\"b\\\\c\\n");
    CHECK(jsonEscape(std::string("\x01")) == "\\u0001");
    CHECK(urlDecode("a%20b+c%2Fd%zz") == "a b c/d%zz");
    auto q = parseQuery("file=sdmc%3A%2Fx%2Fa%20b.bin&slot=3&flag");
    CHECK(q["file"] == "sdmc:/x/a b.bin" && q["slot"] == "3" && q.count("flag"));
    CHECK(sanitizeUploadName("../../etc/passwd") == "passwd");
    CHECK(sanitizeUploadName("C:\\x\\Spyro.sky") == "Spyro.sky");
    CHECK(sanitizeUploadName(".hidden") == "hidden");
    CHECK(sanitizeUploadName("a.bak").empty() && sanitizeUploadName("A.TMP").empty());
    CHECK(sanitizeUploadName("../").empty() && sanitizeUploadName("").empty());
    CHECK(sanitizeUploadName("Spy<ro>|1.bin") == "Spy_ro__1.bin");
    CHECK(sanitizeStaticPath("app.js") == "app.js" && sanitizeStaticPath("img/a.png") == "img/a.png");
    CHECK(sanitizeStaticPath("../a").empty() && sanitizeStaticPath("a/../b").empty() && sanitizeStaticPath(".x").empty());
    CHECK(sanitizeStaticPath("a b").empty() && sanitizeStaticPath("a//b").empty() && sanitizeStaticPath("a/").empty());

    // head parsing
    WebRequest r;
    CHECK(webParseHead("POST /api/load?file=x&slot=2 HTTP/1.1\r\nHost: a\r\nX-Requested-With:  Toypad-Emu \r\nContent-Length: 4", r));
    CHECK(r.method == "POST" && r.path == "/api/load" && r.query == "file=x&slot=2");
    CHECK(r.headers["x-requested-with"] == "Toypad-Emu" && r.headers["content-length"] == "4");
    CHECK(!webParseHead("garbage", r));
    CHECK(!webParseHead("GET nothing HTTP/1.1", r));
    CHECK(!webParseHead("GET / FTP/1.0", r));

    // server + api, with a fake UI thread
    WebApi api([](const std::string& rel, std::string& out) {
        if (rel == "custom.css") { out = "body{}"; return true; }
        return false;
    });
    api.setWaitMs(1500);
    WebServer srv;
    CHECK(srv.start(0, [&](const WebRequest& rq, WebResponse& rs) { api.handle(rq, rs); }));
    const uint16_t port = srv.port();
    CHECK(port != 0);

    std::atomic<bool> stop{false};
    std::string lastOp, lastFile, lastBody;
    std::thread ui([&] {
        while (!stop) {
            WebCommand c;
            while (api.popCommand(c)) {
                lastOp = c.op;
                lastFile = c.args.count("file") ? c.args["file"] : c.args.count("name") ? c.args["name"] : "";
                lastBody = c.body;
                if (c.op == "load" && lastFile == "bad") api.finish(c.ticket, false, "Unknown file");
                else api.finish(c.ticket, true, "did " + c.op);
            }
            usleep(2000);
        }
    });

    std::string res = httpRaw(port, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(res.find("200 OK") != std::string::npos && res.find("<title>Toypad-Emu</title>") != std::string::npos);
    CHECK(res.find("Content-Type: text/html") != std::string::npos);
    CHECK(httpRaw(port, "GET /custom.css HTTP/1.1\r\n\r\n").find("text/css") != std::string::npos);
    CHECK(httpRaw(port, "GET /nope.js HTTP/1.1\r\n\r\n").find("404") != std::string::npos);
    CHECK(httpRaw(port, "GET /../secret HTTP/1.1\r\n\r\n").find("404") != std::string::npos);
    CHECK(bodyOf(httpRaw(port, "GET /api/state HTTP/1.1\r\n\r\n")) == "{\"ready\":false}");
    api.publishState("{\"ready\":true,\"x\":1}");
    api.publishFiles("{\"files\":[]}");
    CHECK(bodyOf(httpRaw(port, "GET /api/state HTTP/1.1\r\n\r\n")) == "{\"ready\":true,\"x\":1}");
    CHECK(bodyOf(httpRaw(port, "GET /api/files HTTP/1.1\r\n\r\n")) == "{\"files\":[]}");

    // POST rules
    CHECK(httpRaw(port, "POST /api/clear HTTP/1.1\r\nContent-Length: 0\r\n\r\n").find("403") != std::string::npos);
    CHECK(httpRaw(port, "POST /api/clear HTTP/1.1\r\nX-Requested-With: other\r\nContent-Length: 0\r\n\r\n").find("403") != std::string::npos);
    CHECK(httpRaw(port, "OPTIONS /api/clear HTTP/1.1\r\nOrigin: http://evil\r\n\r\n").find("405") != std::string::npos);
    CHECK(httpRaw(port, "POST /api/format HTTP/1.1\r\nX-Requested-With: Toypad-Emu\r\nContent-Length: 0\r\n\r\n").find("404") != std::string::npos);
    const std::string H = "X-Requested-With: Toypad-Emu\r\nContent-Length: 0\r\n\r\n";
    res = httpRaw(port, "POST /api/clear HTTP/1.1\r\n" + H);
    CHECK(res.find("200 OK") != std::string::npos && bodyOf(res) == "{\"ok\":true,\"message\":\"did clear\"}" && lastOp == "clear");
    res = httpRaw(port, "POST /api/load?file=sdmc%3A%2Fa%20b.bin&slot=3 HTTP/1.1\r\n" + H);
    CHECK(bodyOf(res).find("\"ok\":true") != std::string::npos && lastFile == "sdmc:/a b.bin");
    res = httpRaw(port, "POST /api/load?file=bad HTTP/1.1\r\n" + H);
    CHECK(bodyOf(res) == "{\"ok\":false,\"message\":\"Unknown file\"}");
    CHECK(httpRaw(port, "POST /api/load HTTP/1.1\r\n" + H).find("400") != std::string::npos);
    CHECK(httpRaw(port, "POST /api/load?file=a&slot=99 HTTP/1.1\r\n" + H).find("400") != std::string::npos);
    CHECK(httpRaw(port, "POST /api/remove?slot=-1 HTTP/1.1\r\n" + H).find("400") != std::string::npos);
    CHECK(httpRaw(port, "POST /api/mode?mode=xbox HTTP/1.1\r\n" + H).find("400") != std::string::npos);
    CHECK(httpRaw(port, "POST /api/mode?mode=lego HTTP/1.1\r\n" + H).find("200 OK") != std::string::npos);
    CHECK(httpRaw(port, "POST /api/move?from=0&to=3 HTTP/1.1\r\n" + H).find("200 OK") != std::string::npos);

    // upload: name is cleaned, body arrives intact
    std::string payload(192, '\0');
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = char(i * 7);
    res = httpRaw(port, "POST /api/upload?name=..%2F..%2Fevil%3F.ldtag HTTP/1.1\r\nX-Requested-With: Toypad-Emu\r\nContent-Length: 192\r\n\r\n" + payload);
    CHECK(res.find("200 OK") != std::string::npos && lastOp == "upload" && lastFile == "evil_.ldtag" && lastBody == payload);
    CHECK(httpRaw(port, "POST /api/upload?name=a.bak HTTP/1.1\r\nX-Requested-With: Toypad-Emu\r\nContent-Length: 192\r\n\r\n" + payload).find("400") != std::string::npos);
    CHECK(httpRaw(port, "POST /api/upload?name=a.bin HTTP/1.1\r\nX-Requested-With: Toypad-Emu\r\nContent-Length: 0\r\n\r\n").find("400") != std::string::npos);
    CHECK(httpRaw(port, "POST /api/upload?name=a.bin HTTP/1.1\r\nX-Requested-With: Toypad-Emu\r\nContent-Length: 999999\r\n\r\n").find("413") != std::string::npos);
    CHECK(httpRaw(port, "BLAH\r\n\r\n").find("400") != std::string::npos);

    // the UI thread not answering gives a timeout, not a hang
    stop = true;
    ui.join();
    api.setWaitMs(100);
    res = httpRaw(port, "POST /api/rescan HTTP/1.1\r\n" + H);
    CHECK(res.find("504") != std::string::npos);

    srv.stop();
    CHECK(!srv.running());
    std::printf("web_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
