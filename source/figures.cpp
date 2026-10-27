#include "figures.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace sky {

namespace {

bool endsWith(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != suffix[i]) return false;
    }
    return true;
}

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool fileExists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool copyFile(const std::string& from, const std::string& to) {
    FILE* in = std::fopen(from.c_str(), "rb");
    if (!in) return false;
    FILE* out = std::fopen(to.c_str(), "wb");
    if (!out) {
        std::fclose(in);
        return false;
    }
    char buf[4096];
    size_t n;
    bool ok = true;
    while ((n = std::fread(buf, 1, sizeof buf, in)) > 0) {
        if (std::fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    std::fclose(in);
    if (std::fclose(out) != 0) ok = false;
    if (!ok) std::remove(to.c_str());
    return ok;
}

}  // namespace

std::vector<FigureFile> scanFigures(const std::string& dir) { return scanFiguresOfSize(dir, kFigureSize); }

namespace {
void scanSizedRecursive(const std::string& dir, size_t size, std::vector<FigureFile>& out, unsigned depth) {
    if (depth > 8) return; // prevent accidental loops / pathological directory trees
    DIR* d = ::opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name.empty() || name[0] == '.' || name == "..") continue;
        if (endsWith(name, ".bak") || endsWith(name, ".tmp")) continue;
        const std::string path = dir + "/" + name;
        struct stat st;
        if (::stat(path.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { scanSizedRecursive(path, size, out, depth + 1); continue; }
        if (!S_ISREG(st.st_mode) || static_cast<size_t>(st.st_size) != size) continue;
        out.push_back({name, path});
    }
    ::closedir(d);
}
} // namespace

std::vector<FigureFile> scanFiguresOfSize(const std::string& dir, size_t size) {
    std::vector<FigureFile> out;
    scanSizedRecursive(dir, size, out, 0);
    std::sort(out.begin(), out.end(), [](const FigureFile& a, const FigureFile& b) {
        const std::string la = lower(a.path), lb = lower(b.path);
        return la != lb ? la < lb : a.path < b.path;
    });
    return out;
}

bool readFigure(const std::string& path, std::array<uint8_t, kFigureSize>& out) {
    return readFigureBytes(path, out.data(), out.size());
}

bool readFigureBytes(const std::string& path, uint8_t* out, size_t size) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    const size_t n = std::fread(out, 1, size, f);
    // Anything other than exactly `size` bytes is not a figure dump.
    const bool extra = std::fgetc(f) != EOF;
    std::fclose(f);
    return n == size && !extra;
}

bool saveFigure(const std::string& path, const std::array<uint8_t, kFigureSize>& data, std::string& error) {
    return saveFigureBytes(path, data.data(), data.size(), error);
}

bool saveFigureBytes(const std::string& path, const uint8_t* data, size_t size, std::string& error) {
    const std::string bak = path + ".bak";
    const std::string tmp = path + ".tmp";

    if (fileExists(path) && !fileExists(bak)) {
        if (!copyFile(path, bak)) {
            // Never overwrite the only copy of a figure without a backup.
            error = "could not create backup";
            return false;
        }
    }

    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        error = "could not create temp file";
        return false;
    }
    bool ok = std::fwrite(data, 1, size, f) == size;
    if (std::fclose(f) != 0) ok = false;
    if (!ok) {
        std::remove(tmp.c_str());
        error = "write failed (SD card full?)";
        return false;
    }

    // rename() onto an existing file fails on FAT, so remove the old one first.
    // The .bak and .tmp files cover an interruption between these two steps.
    std::remove(path.c_str());
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        error = "rename failed (data kept in .tmp)";
        return false;
    }
    return true;
}

std::string figureUidString(const uint8_t* data) {
    char buf[9];
    std::snprintf(buf, sizeof buf, "%02X%02X%02X%02X", data[3], data[2], data[1], data[0]);
    return buf;
}

bool ensureDirectory(const std::string& dir) {
    std::string cur;
    for (size_t i = 0; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == '/') {
            if (!cur.empty() && cur.back() != ':') {  // skip "sdmc:" and empty prefixes
                ::mkdir(cur.c_str(), 0777);
            }
        }
        if (i < dir.size()) cur += dir[i];
    }
    struct stat st;
    return ::stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

}  // namespace sky
