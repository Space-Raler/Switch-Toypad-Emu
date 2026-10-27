// The startup breadcrumb log must survive a crash: every line is on disk as soon as crumb() returns.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "breadcrumb.h"

static std::string slurp() {
    std::string s;
    if (FILE* f = std::fopen(SKY_CRUMB_PATH, "r")) {
        char b[256];
        size_t n;
        while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
        std::fclose(f);
    }
    return s;
}

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main() {
    std::remove(SKY_CRUMB_PATH);
    sky::crumbReset();
    std::string s = slurp();
    CHECK(s.find("skyportal startup") != std::string::npos);

    sky::crumb("step %d: %s", 1, "sdl init");
    // Read back WITHOUT any flush/close call from the test: a crash right after crumb() must not lose the line.
    s = slurp();
    CHECK(s.find("step 1: sdl init\n") != std::string::npos);

    sky::crumb("step 2 rc=0x%X", 0x2CA02u);
    s = slurp();
    CHECK(s.find("step 1: sdl init\nstep 2 rc=0x2CA02\n") != std::string::npos);  // appended, in order

    // A new launch starts a fresh log instead of mixing with the previous (possibly crashed) run.
    sky::crumbReset();
    s = slurp();
    CHECK(s.find("step 1") == std::string::npos);

    // Never writes past a sane size even if called in a loop.
    for (int i = 0; i < 5000; ++i) sky::crumb("spam %d", i);
    s = slurp();
    CHECK(s.size() <= 16384 + 256);
    std::remove(SKY_CRUMB_PATH);
    std::puts("breadcrumb tests passed");
    return 0;
}
