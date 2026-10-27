// Figure dump files on the SD card: listing, loading and saving.
//
// POSIX only (dirent/stat/stdio), so it works under libnx's devoptab and on a
// PC for tests.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "portal.h"

namespace sky {

struct FigureFile {
    std::string name;  // file name only
    std::string path;  // full path
};

// Same as the functions below, for dumps of any fixed size (the Disney Infinity
// base uses 320-byte figure files). The kFigureSize functions call these.
std::vector<FigureFile> scanFiguresOfSize(const std::string& dir, size_t size);
bool readFigureBytes(const std::string& path, uint8_t* out, size_t size);
bool saveFigureBytes(const std::string& path, const uint8_t* data, size_t size, std::string& error);

// Lists regular files of exactly kFigureSize bytes in `dir` (any extension,
// except .bak/.tmp and dot-files), sorted case-insensitively by name.
std::vector<FigureFile> scanFigures(const std::string& dir);

// Reads a whole figure dump. Returns false on I/O error or wrong size.
bool readFigure(const std::string& path, std::array<uint8_t, kFigureSize>& out);

// Saves a figure dump. The first time a file is saved, the original is kept as
// "<path>.bak". The new data is written to "<path>.tmp" and then swapped in, so
// an interrupted save never leaves a half-written dump under the real name.
// `error` receives a short reason on failure.
bool saveFigure(const std::string& path, const std::array<uint8_t, kFigureSize>& data, std::string& error);

// "1234ABCD" style UID of a dump (first four bytes, as the game sees it).
std::string figureUidString(const uint8_t* data);

// Creates `dir` and its parents. Returns true if it exists afterwards.
bool ensureDirectory(const std::string& dir);

}  // namespace sky
