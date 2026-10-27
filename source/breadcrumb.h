// Startup breadcrumb log: sdmc:/switch/skyportal/startup.log
//
// A crash on the Switch leaves no message, so the app writes one line per startup step. Every call
// opens, appends and CLOSES the file, so a line is on the SD card the moment crumb() returns even if the
// process dies on the very next instruction. After a crash, the LAST line of the log is the step that
// was running. Each launch starts a fresh log. The size is capped so it cannot fill the card.
#pragma once

#include <cstdarg>
#include <cstdio>

#ifdef __SWITCH__
#include <sys/stat.h>
#endif

#ifndef SKY_CRUMB_PATH
#define SKY_CRUMB_PATH "sdmc:/switch/skyportal/startup.log"
#endif

namespace sky {

constexpr long kCrumbMaxBytes = 16384;

inline void crumbReset() {
#ifdef __SWITCH__
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/skyportal", 0777);
#endif
    if (FILE* f = std::fopen(SKY_CRUMB_PATH, "w")) {
        std::fputs("--- skyportal startup (last line = step that was running if the app crashed) ---\n", f);
        std::fclose(f);
    }
}

inline void crumb(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline void crumb(const char* fmt, ...) {
    FILE* f = std::fopen(SKY_CRUMB_PATH, "a");
    if (!f) return;
    std::fseek(f, 0, SEEK_END);
    if (std::ftell(f) < kCrumbMaxBytes) {
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(f, fmt, ap);
        va_end(ap);
        std::fputc('\n', f);
    }
    std::fclose(f);
}

}  // namespace sky
