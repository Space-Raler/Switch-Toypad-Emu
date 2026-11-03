#include "web_api.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace sky {

namespace {

const char* const kJson = "application/json; charset=utf-8";

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string lowerCopy(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool endsWithNoCase(const std::string& s, const char* suffix) {
    const std::string a = lowerCopy(s), b(suffix);
    return a.size() >= b.size() && a.compare(a.size() - b.size(), b.size(), b) == 0;
}

void sleepMs(unsigned ms) {
#ifdef __SWITCH__
    svcSleepThread(int64_t(ms) * 1000000LL);
#else
    usleep(ms * 1000u);
#endif
}

void jsonReply(WebResponse& res, int status, bool ok, const std::string& message) {
    res.status = status;
    res.contentType = kJson;
    res.body = std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"message\":\"" + jsonEscape(message) + "\"}";
}

bool parseIntArg(const std::map<std::string, std::string>& a, const char* key, int lo, int hi, int& out) {
    const auto it = a.find(key);
    if (it == a.end() || it->second.empty() || it->second.size() > 4) return false;
    char* end = nullptr;
    const long v = std::strtol(it->second.c_str(), &end, 10);
    if (*end != '\0' || v < lo || v > hi) return false;
    out = int(v);
    return true;
}

}  // namespace

std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (const unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o += char(c);  // UTF-8 bytes pass through
                }
        }
    }
    return o;
}

std::string urlDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hexVal(s[i + 1]) >= 0 && hexVal(s[i + 2]) >= 0) {
            o += char(hexVal(s[i + 1]) * 16 + hexVal(s[i + 2]));
            i += 2;
        } else if (s[i] == '+') {
            o += ' ';
        } else {
            o += s[i];
        }
    }
    return o;
}

std::map<std::string, std::string> parseQuery(const std::string& q) {
    std::map<std::string, std::string> m;
    size_t i = 0;
    while (i <= q.size()) {
        size_t amp = q.find('&', i);
        if (amp == std::string::npos) amp = q.size();
        const std::string part = q.substr(i, amp - i);
        if (!part.empty()) {
            const size_t eq = part.find('=');
            if (eq == std::string::npos) m[urlDecode(part)] = "";
            else m[urlDecode(part.substr(0, eq))] = urlDecode(part.substr(eq + 1));
        }
        i = amp + 1;
    }
    return m;
}

std::string sanitizeUploadName(const std::string& raw) {
    size_t cut = raw.find_last_of("/\\");
    std::string n = cut == std::string::npos ? raw : raw.substr(cut + 1);
    std::string o;
    for (const unsigned char c : n) {
        const bool ok = std::isalnum(c) || c == ' ' || c == '.' || c == '_' || c == '-' || c == '(' || c == ')' ||
                        c == '+' || c == '\'';
        o += ok ? char(c) : '_';
    }
    while (!o.empty() && (o.front() == '.' || o.front() == ' ')) o.erase(o.begin());
    while (!o.empty() && (o.back() == ' ' || o.back() == '.')) o.pop_back();
    if (o.size() > 96) o.resize(96);
    if (o.empty()) return "";
    if (endsWithNoCase(o, ".bak") || endsWithNoCase(o, ".tmp")) return "";
    return o;
}

std::string sanitizeStaticPath(const std::string& raw) {
    if (raw.empty() || raw.size() > 128) return "";
    std::string seg, out;
    auto flush = [&]() -> bool {
        if (seg.empty() || seg == "." || seg == ".." || seg[0] == '.') return false;
        if (!out.empty()) out += '/';
        out += seg;
        seg.clear();
        return true;
    };
    for (const unsigned char c : raw) {
        if (c == '/') {
            if (!flush()) return "";
        } else if (std::isalnum(c) || c == '.' || c == '_' || c == '-') {
            seg += char(c);
        } else {
            return "";
        }
    }
    if (!flush()) return "";
    return out;
}

const char* webContentTypeFor(const std::string& path) {
    if (endsWithNoCase(path, ".html") || endsWithNoCase(path, ".htm")) return "text/html; charset=utf-8";
    if (endsWithNoCase(path, ".js")) return "text/javascript; charset=utf-8";
    if (endsWithNoCase(path, ".css")) return "text/css; charset=utf-8";
    if (endsWithNoCase(path, ".json")) return kJson;
    if (endsWithNoCase(path, ".svg")) return "image/svg+xml";
    if (endsWithNoCase(path, ".png")) return "image/png";
    if (endsWithNoCase(path, ".jpg") || endsWithNoCase(path, ".jpeg")) return "image/jpeg";
    if (endsWithNoCase(path, ".ico")) return "image/x-icon";
    if (endsWithNoCase(path, ".txt")) return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

// ---------------------------------------------------------------------------
// UI thread side
// ---------------------------------------------------------------------------

void WebApi::publishState(std::string json) {
    WebLock lock(mutex_);
    state_ = std::move(json);
}

void WebApi::publishFiles(std::string json) {
    WebLock lock(mutex_);
    files_ = std::move(json);
}

bool WebApi::popCommand(WebCommand& out) {
    WebLock lock(mutex_);
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

void WebApi::finish(uint32_t ticket, bool ok, const std::string& message) {
    WebLock lock(mutex_);
    results_[ticket] = WebResult{ok, message};
    while (results_.size() > 32) results_.erase(results_.begin());  // nobody is waiting for these any more
}

// ---------------------------------------------------------------------------
// Web thread side
// ---------------------------------------------------------------------------

bool WebApi::waitFor(uint32_t ticket, WebResult& out) {
    for (uint32_t waited = 0; waited <= waitMs_; waited += 10) {
        {
            WebLock lock(mutex_);
            const auto it = results_.find(ticket);
            if (it != results_.end()) {
                out = it->second;
                results_.erase(it);
                return true;
            }
        }
        sleepMs(10);
    }
    return false;
}

void WebApi::handle(const WebRequest& req, WebResponse& res) {
    const bool isGet = req.method == "GET";
    const bool isPost = req.method == "POST";
    if (!isGet && !isPost) {
        jsonReply(res, 405, false, "method not allowed");
        return;
    }

    if (req.path.compare(0, 5, "/api/") != 0) {
        // The page, then anything else from the custom website folder.
        if (!isGet) { jsonReply(res, 405, false, "method not allowed"); return; }
        std::string rel = req.path == "/" ? "index.html" : sanitizeStaticPath(req.path.substr(1));
        if (rel.empty()) { jsonReply(res, 404, false, "not found"); return; }
        std::string data;
        if (loader_ && loader_(rel, data)) {
            res.status = 200;
            res.contentType = webContentTypeFor(rel);
            res.body = std::move(data);
            return;
        }
        if (rel == "index.html" || rel == "index.htm") {
            res.status = 200;
            res.contentType = "text/html; charset=utf-8";
            res.body = kWebPage;
            return;
        }
        jsonReply(res, 404, false, "not found");
        return;
    }

    const std::string op = req.path.substr(5);
    if (isGet) {
        if (op != "state" && op != "files") { jsonReply(res, 404, false, "not found"); return; }
        WebLock lock(mutex_);
        res.status = 200;
        res.contentType = kJson;
        const std::string& src = op == "state" ? state_ : files_;
        res.body = src.empty() ? "{\"ready\":false}" : src;
        return;
    }

    // POST: a custom header forces a CORS preflight for other websites, which this server
    // never answers, so a page on the internet cannot drive the Switch from your browser.
    const auto xr = req.headers.find("x-requested-with");
    if (xr == req.headers.end() || xr->second != "Toypad-Emu") {
        jsonReply(res, 403, false, "missing X-Requested-With header");
        return;
    }

    static const char* const kOps[] = {"load", "remove", "move", "clear", "mode", "favorite", "rescan", "replug", "upload"};
    if (std::find_if(std::begin(kOps), std::end(kOps), [&](const char* o) { return op == o; }) == std::end(kOps)) {
        jsonReply(res, 404, false, "unknown action");
        return;
    }

    WebCommand cmd;
    cmd.op = op;
    cmd.args = parseQuery(req.query);
    int n = 0;
    if (op == "load" || op == "favorite") {
        const auto it = cmd.args.find("file");
        if (it == cmd.args.end() || it->second.empty() || it->second.size() > 512) {
            jsonReply(res, 400, false, "missing file");
            return;
        }
        if (op == "load" && cmd.args.count("slot") && !parseIntArg(cmd.args, "slot", 0, 15, n)) {
            jsonReply(res, 400, false, "bad slot");
            return;
        }
    } else if (op == "remove") {
        if (!parseIntArg(cmd.args, "slot", 0, 15, n)) { jsonReply(res, 400, false, "bad slot"); return; }
    } else if (op == "move") {
        if (!parseIntArg(cmd.args, "from", 0, 15, n) || !parseIntArg(cmd.args, "to", 0, 15, n)) {
            jsonReply(res, 400, false, "bad position");
            return;
        }
    } else if (op == "mode") {
        const auto it = cmd.args.find("mode");
        if (it == cmd.args.end() || (it->second != "skylanders" && it->second != "infinity" && it->second != "lego")) {
            jsonReply(res, 400, false, "bad mode");
            return;
        }
    } else if (op == "upload") {
        const auto it = cmd.args.find("name");
        const std::string clean = it == cmd.args.end() ? "" : sanitizeUploadName(it->second);
        if (clean.empty()) { jsonReply(res, 400, false, "bad file name"); return; }
        if (req.body.empty()) { jsonReply(res, 400, false, "empty file"); return; }
        cmd.args["name"] = clean;
        cmd.body = req.body;
    }

    {
        WebLock lock(mutex_);
        if (queue_.size() >= 32) { jsonReply(res, 503, false, "busy, try again"); return; }
        cmd.ticket = nextTicket_++;
        queue_.push_back(cmd);
    }
    WebResult result;
    if (!waitFor(cmd.ticket, result)) {
        jsonReply(res, 504, false, "the Switch app did not answer in time");
        return;
    }
    jsonReply(res, 200, result.ok, result.message);
}

}  // namespace sky
