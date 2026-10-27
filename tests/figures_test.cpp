// Host-side tests for figure file handling.
//   g++ -std=c++17 -Wall -Wextra -Isource tests/figures_test.cpp source/figures.cpp source/portal.cpp -o figures_test
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "figures.h"

using namespace sky;

static int g_checks = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(c)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);                  \
            std::exit(1);                                                     \
        }                                                                     \
    } while (0)

static void writeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    FILE* f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr);
    CHECK(std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size());
    std::fclose(f);
}

static std::vector<uint8_t> readAll(const std::string& path) {
    std::vector<uint8_t> v;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return v;
    int c;
    while ((c = std::fgetc(f)) != EOF) v.push_back(uint8_t(c));
    std::fclose(f);
    return v;
}

static bool exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

int main() {
    char tmpl[] = "/tmp/skyportal_test_XXXXXX";
    CHECK(mkdtemp(tmpl) != nullptr);
    const std::string root = tmpl;
    const std::string dir = root + "/a/b/figures";

    // --- ensureDirectory creates nested directories ----------------------------------
    CHECK(ensureDirectory(dir));
    CHECK(ensureDirectory(dir));  // idempotent

    // --- scan: only exact-size regular files, sorted, junk skipped ---------------------
    std::vector<uint8_t> fig(kFigureSize);
    for (size_t i = 0; i < fig.size(); ++i) fig[i] = uint8_t(i * 3 + 1);
    fig[0] = 0x44; fig[1] = 0x33; fig[2] = 0x22; fig[3] = 0x11;

    writeFile(dir + "/zebra.bin", fig);
    writeFile(dir + "/Apple.sky", fig);
    writeFile(dir + "/mango.dmp", fig);
    writeFile(dir + "/short.bin", std::vector<uint8_t>(1000, 0));
    writeFile(dir + "/long.bin", std::vector<uint8_t>(1025, 0));
    writeFile(dir + "/old.bin.bak", fig);
    writeFile(dir + "/half.bin.tmp", fig);
    writeFile(dir + "/.hidden", fig);
    CHECK(ensureDirectory(dir + "/subdir"));

    auto list = scanFigures(dir);
    CHECK(list.size() == 3);
    CHECK(list[0].name == "Apple.sky" && list[1].name == "mango.dmp" && list[2].name == "zebra.bin");
    CHECK(list[0].path == dir + "/Apple.sky");
    CHECK(scanFigures(root + "/does/not/exist").empty());

    // --- read: exact size only -------------------------------------------------------------
    std::array<uint8_t, kFigureSize> data{};
    CHECK(readFigure(dir + "/zebra.bin", data));
    CHECK(std::memcmp(data.data(), fig.data(), fig.size()) == 0);
    CHECK(!readFigure(dir + "/short.bin", data));
    CHECK(!readFigure(dir + "/long.bin", data));
    CHECK(!readFigure(dir + "/missing.bin", data));

    CHECK(figureUidString(fig.data()) == "11223344");

    // --- save: backup once, atomic swap, no leftovers ------------------------------------------
    const std::string path = dir + "/zebra.bin";
    std::array<uint8_t, kFigureSize> changed = data;
    changed[100] ^= 0xFF;
    std::string err;
    CHECK(saveFigure(path, changed, err));
    CHECK(readAll(path) == std::vector<uint8_t>(changed.begin(), changed.end()));
    CHECK(readAll(path + ".bak") == fig);  // original preserved
    CHECK(!exists(path + ".tmp"));

    // A second save must not overwrite the backup of the *original*.
    changed[200] ^= 0x0F;
    CHECK(saveFigure(path, changed, err));
    CHECK(readAll(path + ".bak") == fig);
    CHECK(readAll(path) == std::vector<uint8_t>(changed.begin(), changed.end()));

    // Saving to a directory that cannot be written reports an error and does not throw.
    CHECK(!saveFigure(root + "/nonexistent_dir/x.bin", changed, err));
    CHECK(!err.empty());

    // Saving a brand-new path works and creates no backup.
    CHECK(saveFigure(dir + "/new.bin", changed, err));
    CHECK(exists(dir + "/new.bin") && !exists(dir + "/new.bin.bak"));

    // Cleanup
    std::string cmd = "rm -rf '" + root + "'";
    CHECK(std::system(cmd.c_str()) == 0);

    std::printf("OK: %d checks passed\n", g_checks);
    return 0;
}
