// Skylanders Portal for Switch: graphical UI (SDL2) and app logic.
//
// Put figure dumps (exactly 1024 bytes each) in sdmc:/switch/skyportal/figures/,
// plug the Switch into the console/PC running the game with a USB cable, then
// load figures onto the "portal" from this screen (buttons or touch).
//
// Disney Infinity and LEGO Dimensions modes (L, or the "Switch mode" button): the Switch
// re-enumerates as the selected USB device. LEGO profiles are 192-byte .ldtag files.
//
// The old text-mode UI is kept in extras/main_console.cpp.txt as a fallback.
#include <switch.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "breadcrumb.h"
#include "figures.h"
#include "infinity/infinity_figures.h"
#include "usb_device.h"
#include "lego_dimensions_function.h"

using namespace sky;

namespace {

const char* const kBaseDir = "sdmc:/switch/skyportal";
const char* const kFigureDir = "sdmc:/switch/skyportal/figures";
const char* const kInfinityDir = "sdmc:/switch/skyportal/infinity";
const char* const kLegoDir = "sdmc:/switch/skyportal/lego_uids";
const char* const kModeFile = "sdmc:/switch/skyportal/mode.txt";
const char* const kFavoritesFile = "sdmc:/switch/skyportal/favorites.txt";

// Disney Infinity base: nine slots. The screen shows them as characters first, then the play
// set, then the discs; this is the base slot number behind each on-screen position (same
// order as the FlipBase Flipper app).
constexpr int kInfSlots = INF_SLOT_COUNT;
const int kInfRowSlot[kInfSlots] = {3, 6, 0, 1, 2, 4, 5, 7, 8};
const char* const kInfRowLabel[kInfSlots] = {"Player 1",  "Player 2",  "Play set",  "Disc 2",   "Disc 3",
                                             "P1 disc 1", "P1 disc 2", "P2 disc 1", "P2 disc 2"};

constexpr int kW = 1280;
constexpr int kH = 720;

// ---------------------------------------------------------------------------
// Small drawing toolkit on top of SDL2 + SDL2_ttf
// ---------------------------------------------------------------------------

struct Color {
    uint8_t r, g, b, a;
};

constexpr Color kBg{16, 22, 40, 255};
constexpr Color kPanel{27, 36, 66, 255};
constexpr Color kPanelHi{40, 54, 96, 255};
constexpr Color kText{235, 240, 255, 255};
constexpr Color kDim{140, 155, 190, 255};
constexpr Color kAccent{79, 172, 254, 255};
constexpr Color kGreen{72, 207, 140, 255};
constexpr Color kAmber{255, 193, 69, 255};
constexpr Color kRed{255, 99, 99, 255};
constexpr Color kDark{12, 16, 30, 255};

enum class Font { Small = 0, Body = 1, Title = 2 };

bool inside(const SDL_Rect& r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

class Gfx {
public:
    bool init() {
        crumb("gfx: SDL_Init");
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) { crumb("gfx: SDL_Init FAILED: %s", SDL_GetError()); return false; }
        crumb("gfx: SDL_CreateWindow");
        win_ = SDL_CreateWindow("Skylanders Portal", 0, 0, kW, kH, SDL_WINDOW_SHOWN);
        if (!win_) { crumb("gfx: SDL_CreateWindow FAILED: %s", SDL_GetError()); return false; }
        crumb("gfx: SDL_CreateRenderer (accelerated)");
        ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!ren_) { crumb("gfx: SDL_CreateRenderer FAILED: %s", SDL_GetError()); return false; }
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);

        crumb("gfx: TTF_Init");
        if (TTF_Init() != 0) { crumb("gfx: TTF_Init FAILED: %s", TTF_GetError()); return false; }
        crumb("gfx: plInitialize");
        if (R_SUCCEEDED(plInitialize(PlServiceType_User))) {
            plReady_ = true;
            PlFontData fd;
            crumb("gfx: plGetSharedFontByType");
            if (R_SUCCEEDED(plGetSharedFontByType(&fd, PlSharedFontType_Standard))) {
                const int sizes[3] = {18, 22, 34};
                for (int i = 0; i < 3; ++i) {
                    crumb("gfx: TTF_OpenFontRW size %d", sizes[i]);
                    fonts_[i] = TTF_OpenFontRW(SDL_RWFromMem(fd.address, fd.size), 1, sizes[i]);
                    if (!fonts_[i]) crumb("gfx: font %d FAILED: %s", sizes[i], TTF_GetError());
                }
            } else {
                crumb("gfx: shared font unavailable (no text will be drawn)");
            }
        } else {
            crumb("gfx: plInitialize failed (no text will be drawn)");
        }
        crumb("gfx: ready");
        return true;  // missing fonts only means no text; the app still runs
    }

    void shutdown() {
        clearCache();
        for (auto& f : fonts_) {
            if (f) TTF_CloseFont(f);
            f = nullptr;
        }
        if (plReady_) plExit();
        plReady_ = false;
        TTF_Quit();
        if (ren_) SDL_DestroyRenderer(ren_);
        if (win_) SDL_DestroyWindow(win_);
        ren_ = nullptr;
        win_ = nullptr;
        SDL_Quit();
    }

    bool hasFont() const { return fonts_[1] != nullptr; }

    void begin() {
        setColor(kBg);
        SDL_RenderClear(ren_);
        if (cache_.size() > 600) clearCache();
    }
    void end() { SDL_RenderPresent(ren_); }

    void setColor(Color c) { SDL_SetRenderDrawColor(ren_, c.r, c.g, c.b, c.a); }

    void fill(SDL_Rect r, Color c) {
        setColor(c);
        SDL_RenderFillRect(ren_, &r);
    }

    void round(SDL_Rect r, int rad, Color c) {
        if (rad * 2 > r.h) rad = r.h / 2;
        if (rad * 2 > r.w) rad = r.w / 2;
        setColor(c);
        SDL_Rect mid{r.x, r.y + rad, r.w, r.h - 2 * rad};
        SDL_RenderFillRect(ren_, &mid);
        for (int i = 0; i < rad; ++i) {
            const float dy = float(rad - i) - 0.5f;
            const int inset = rad - int(std::sqrt(float(rad * rad) - dy * dy));
            SDL_Rect t{r.x + inset, r.y + i, r.w - 2 * inset, 1};
            SDL_Rect b{r.x + inset, r.y + r.h - 1 - i, r.w - 2 * inset, 1};
            SDL_RenderFillRect(ren_, &t);
            SDL_RenderFillRect(ren_, &b);
        }
    }

    // Rounded card with a border of `border` pixels.
    void card(SDL_Rect r, int rad, Color fill_, Color borderColor, int border) {
        round(r, rad, borderColor);
        SDL_Rect in{r.x + border, r.y + border, r.w - 2 * border, r.h - 2 * border};
        round(in, std::max(0, rad - border), fill_);
    }

    int textWidth(Font f, const std::string& s) {
        TTF_Font* font = fonts_[int(f)];
        if (!font || s.empty()) return 0;
        int w = 0, h = 0;
        TTF_SizeUTF8(font, s.c_str(), &w, &h);
        return w;
    }

    // Shortens `s` with "..." so it fits in maxW pixels.
    std::string fit(Font f, std::string s, int maxW) {
        if (textWidth(f, s) <= maxW) return s;
        while (!s.empty()) {
            // drop one whole UTF-8 character
            do {
                s.pop_back();
            } while (!s.empty() && (uint8_t(s.back()) & 0xC0) == 0x80);
            if (textWidth(f, s + "...") <= maxW) return s + "...";
        }
        return "...";
    }

    // Draws text with its top-left at (x, y). Returns the width drawn.
    int text(Font f, const std::string& s, int x, int y, Color c) {
        const Cached* t = get(f, s, c);
        if (!t || !t->tex) return 0;
        SDL_Rect dst{x, y, t->w, t->h};
        SDL_RenderCopy(ren_, t->tex, nullptr, &dst);
        return t->w;
    }

    void textRight(Font f, const std::string& s, int xRight, int y, Color c) {
        text(f, s, xRight - textWidth(f, s), y, c);
    }

    void textCenter(Font f, const std::string& s, SDL_Rect r, Color c) {
        const Cached* t = get(f, s, c);
        if (!t || !t->tex) return;
        SDL_Rect dst{r.x + (r.w - t->w) / 2, r.y + (r.h - t->h) / 2, t->w, t->h};
        SDL_RenderCopy(ren_, t->tex, nullptr, &dst);
    }

private:
    struct Cached {
        SDL_Texture* tex = nullptr;
        int w = 0, h = 0;
    };

    const Cached* get(Font f, const std::string& s, Color c) {
        TTF_Font* font = fonts_[int(f)];
        if (!font || s.empty()) return nullptr;
        char key[24];
        std::snprintf(key, sizeof key, "%d|%02X%02X%02X|", int(f), c.r, c.g, c.b);
        const std::string k = std::string(key) + s;
        auto it = cache_.find(k);
        if (it != cache_.end()) return &it->second;

        SDL_Color sc{c.r, c.g, c.b, 255};
        SDL_Surface* surf = TTF_RenderUTF8_Blended(font, s.c_str(), sc);
        Cached entry;
        if (surf) {
            entry.tex = SDL_CreateTextureFromSurface(ren_, surf);
            entry.w = surf->w;
            entry.h = surf->h;
            SDL_FreeSurface(surf);
        }
        return &(cache_[k] = entry);
    }

    void clearCache() {
        for (auto& kv : cache_)
            if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
        cache_.clear();
    }

    SDL_Window* win_ = nullptr;
    SDL_Renderer* ren_ = nullptr;
    TTF_Font* fonts_[3] = {nullptr, nullptr, nullptr};
    bool plReady_ = false;
    std::unordered_map<std::string, Cached> cache_;
};

// ---------------------------------------------------------------------------
// App
// ---------------------------------------------------------------------------

enum class Pane { Files, Slots };
enum class Page { Main, Debug };
enum class HitKind { File, Slot, Favorite, FilterGame, Rescan, ClearAll, ToggleDebug, Replug, SwitchMode };

struct Hit {
    SDL_Rect rect;
    HitKind kind;
    int index;
};

struct SlotView {
    bool present = false;
    uint8_t status = 0;
    bool dirty = false;
    std::string tag;
    std::string uid;
    std::string name;  // Infinity: figure name from the table
};

const char* speedName(UsbDeviceSpeed s) {
    switch (s) {
        case UsbDeviceSpeed_Low: return "low";
        case UsbDeviceSpeed_Full: return "full";
        case UsbDeviceSpeed_High: return "high";
        case UsbDeviceSpeed_Super: return "super";
        default: return "-";
    }
}

const char* usbStateName(UsbState s) {
    switch (s) {
        case UsbState_Detached: return "Detached";
        case UsbState_Attached: return "Attached";
        case UsbState_Powered: return "Powered";
        case UsbState_Default: return "Reset";
        case UsbState_Address: return "Addressed";
        case UsbState_Configured: return "Configured";
        case UsbState_Suspended: return "Suspended";
    }
    return "?";
}

std::string baseName(const std::string& path) {
    const size_t p = path.find_last_of('/');
    return p == std::string::npos ? path : path.substr(p + 1);
}

std::string stripExt(const std::string& name) {
    const size_t p = name.find_last_of('.');
    return (p == std::string::npos || p == 0) ? name : name.substr(0, p);
}

// The public Flipper LDToyPad project provides character/vehicle IDs and a
// protocol emulator, not a folder of raw NFC dump files. Seed a small, synthetic
// test library on first run so the mode can be exercised without proprietary dumps.
struct LegoSample { uint32_t id; const char* name; bool vehicle; };
const LegoSample kLegoSamples[] = {
    {1,"Batman",false},{2,"Gandalf",false},{3,"Wyldstyle",false},{16,"Emmet",false},
    {21,"Homer_Simpson",false},{31,"Marty_McFly",false},{52,"Harry_Potter",false},
    {57,"Sonic_the_Hedgehog",false},{68,"Chase_McCain",false},{61,"ET",false},
    {1006,"Batmobile",true},{1012,"DeLorean_Time_Machine",true},{1033,"Emmets_Excavator",true},
    {1066,"Mystery_Machine",true},{1120,"Ecto_1",true},{1218,"Sonic_Speedster",true}
};
uint32_t legoUidHash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15;
    x *= 0x846ca68bu; x ^= x >> 16; return x;
}
void ensureLegoSampleProfiles() {
    ensureDirectory(kLegoDir);
    if (!scanFiguresOfSize(kLegoDir, 192).empty()) return;
    for (const auto& sample : kLegoSamples) {
        std::array<uint8_t,192> raw{};
        const uint32_t h = legoUidHash(sample.id ^ (sample.vehicle ? 0x4c445632u : 0x4c443031u));
        raw[0] = 0x04;
        for (int i = 0; i < 5; ++i) raw[size_t(i+1)] = uint8_t(legoUidHash(h + uint32_t(i)*0x9e3779b9u) >> ((i%4)*8));
        raw[6] = 0x80;
        raw[7] = sample.vehicle ? 1 : 0;
        raw[8] = uint8_t(sample.id); raw[9] = uint8_t(sample.id >> 8);
        raw[10] = uint8_t(sample.id >> 16); raw[11] = uint8_t(sample.id >> 24);
        if (sample.vehicle) {
            // LEGO Dimensions vehicle token layout used by the public emulator:
            // upgrades at byte 140, vehicle ID at 144, second upgrades at 148,
            // and the big-endian constant 1 at 152.
            raw[12+144] = uint8_t(sample.id); raw[12+145] = uint8_t(sample.id >> 8);
            raw[12+152] = 0; raw[12+153] = 1;
        }
        char filename[128];
        std::snprintf(filename, sizeof filename, "%s/%04u_%s.ldtag", kLegoDir, unsigned(sample.id), sample.name);
        std::string error;
        (void)saveFigureBytes(filename, raw.data(), raw.size(), error);
    }
}

std::string hexString(const uint8_t* d, size_t n) {
    std::string s;
    char b[3];
    for (size_t i = 0; i < n; ++i) {
        std::snprintf(b, sizeof b, "%02X", d[i]);
        s += b;
    }
    return s;
}

Mode readSavedMode() {
    FILE* f = std::fopen(kModeFile, "r");
    if (!f) return Mode::Skylanders;
    char line[32] = {};
    const bool ok = std::fgets(line, sizeof line, f) != nullptr;
    std::fclose(f);
    if (!ok) return Mode::Skylanders;
    if (std::strncmp(line, "infinity", 8) == 0) return Mode::Infinity;
    if (std::strncmp(line, "lego", 4) == 0) return Mode::LegoDimensions;
    return Mode::Skylanders;
}

void saveMode(Mode m) {
    FILE* f = std::fopen(kModeFile, "w");
    if (!f) return;
    std::fputs(m == Mode::Infinity ? "infinity\n" : m == Mode::LegoDimensions ? "lego\n" : "skylanders\n", f);
    std::fclose(f);
}

class App {
public:
    App() : usb_(shared_) {}
    int run();

private:
    // logic (unchanged from the console version)
    void rescan();
    void applyGameFilter();
    void loadFavorites();
    void saveFavorites();
    bool isFavorite(const std::string& path) const;
    void toggleFavorite();
    void loadSelectedFile();
    void removeSlot(int slot);
    void removeAll();
    void replugUsb();
    // Disney Infinity mode
    void loadSelectedInfinity(int forcedUiSlot);
    void loadSelectedLego(int forcedSlot);
    int pickInfinitySlot(uint32_t figureId, const char** kind);  // call with shared_.mutex held
    void removeInfinity(int uiSlot);
    void activateSlot(int uiSlot);
    void requestMode(Mode m);
    void applyMode();
    bool inf() const { return mode_ == Mode::Infinity; }
    bool legoMode() const { return mode_ == Mode::LegoDimensions; }
    int slotCount() const { return legoMode() ? int(lego::kPositionCount) : inf() ? kInfSlots : kSlots; }
    int slotCols() const { return inf() ? 3 : legoMode() ? 4 : 4; }
    const char* figureDir() const { return legoMode() ? kLegoDir : inf() ? kInfinityDir : kFigureDir; }
    size_t figureSize() const { return legoMode() ? size_t(192) : inf() ? size_t(INF_FIGURE_SIZE) : kFigureSize; }
    const char* deviceName() const { return legoMode() ? "ToyPad" : inf() ? "base" : "portal"; }
    void flushDirty();
    void setMessage(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    SlotView viewSlot(int i);

    // input
    void handleInput(u64 down);
    void pollTouch();
    void handleTap(int x, int y);
    void ensureVisible();
    void clampScroll();

    // drawing
    void draw();
    void drawHeader(const UsbDiag& d);
    void drawFilesPanel();
    void drawSlotsPanel();
    void drawDebug(const UsbDiag& d);
    void drawFooter();
    int pill(int xRight, int y, const std::string& label, Color c);
    void button(SDL_Rect r, const std::string& label, HitKind kind, int index = 0, Color c = kPanelHi);

    Gfx gfx_;
    Shared shared_;
    UsbDevice usb_;
    Result usbStartResult_ = 0;

    Mode mode_ = Mode::Skylanders;
    Mode pendingMode_ = Mode::Skylanders;
    bool modePending_ = false;
    std::string infPath_[kInfSlots];  // file behind each base slot (UI thread only)
    std::string infName_[kInfSlots];  // figure name behind each base slot
    std::string legoPath_[lego::kPositionCount];

    std::vector<FigureFile> allFiles_;
    std::vector<FigureFile> files_;
    std::vector<std::string> favorites_;
    int gameFilter_ = 0; // 0=all, 1..N game/folder filter
    std::vector<std::string> gameFilters_;
    int fileCursor_ = 0;
    int slotCursor_ = 0;
    int listScroll_ = 0;
    Pane pane_ = Pane::Files;
    Page page_ = Page::Main;
    bool quit_ = false;

    char message_[96] = "";
    int messageFrames_ = 0;

    // per-frame snapshot and touch hit regions
    SlotView slots_[kSlots];
    std::vector<Hit> hits_;

    // touch tracking
    bool wasTouching_ = false;
    bool dragged_ = false;
    bool touchInList_ = false;
    int touchStartY_ = 0, startScroll_ = 0, lastX_ = 0, lastY_ = 0;

    static constexpr int kRowH = 44;
    static constexpr int kRows = 10;
    const SDL_Rect listRect_{40, 144, 488, kRowH * kRows};
};

void App::setMessage(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(message_, sizeof message_, fmt, ap);
    va_end(ap);
    messageFrames_ = 60 * 6;
}

SlotView App::viewSlot(int i) {
    if (legoMode()) {
        SlotView v; if (i < 0 || i >= int(lego::kPositionCount)) return v;
        ScopedLock lock(shared_.mutex); v.present = shared_.lego.occupied(size_t(i));
        if (v.present) { v.uid = hexString(shared_.lego.uid(size_t(i)).data(), 7); v.tag = legoPath_[i]; }
        return v;
    }
    if (inf()) {
        SlotView v;
        if (i < 0 || i >= kInfSlots) return v;
        const int bs = kInfRowSlot[i];
        ScopedLock lock(shared_.mutex);
        const InfFigure& f = shared_.inf.figures[bs];
        v.present = f.present;
        v.status = f.present ? 1 : 0;
        v.dirty = f.dirty;
        v.tag = infPath_[bs];
        v.name = infName_[bs];
        if (v.present) v.uid = hexString(f.data, 7);
        return v;
    }
    ScopedLock lock(shared_.mutex);
    const Slot& s = shared_.portal.slot(i);
    SlotView v;
    v.present = s.present();
    v.status = s.status;
    v.dirty = s.dirty;
    v.tag = s.tag;
    if (v.present) v.uid = figureUidString(s.data.data());
    return v;
}

void App::loadFavorites() {
    favorites_.clear();
    FILE* f = std::fopen(kFavoritesFile, "r");
    if (!f) return;
    char line[1024];
    while (std::fgets(line, sizeof line, f)) {
        size_t n = std::strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        if (n) favorites_.emplace_back(line);
    }
    std::fclose(f);
}

void App::saveFavorites() {
    FILE* f = std::fopen(kFavoritesFile, "w");
    if (!f) { setMessage("Could not save favorites"); return; }
    for (const auto& path : favorites_) std::fprintf(f, "%s\n", path.c_str());
    std::fclose(f);
}

bool App::isFavorite(const std::string& path) const {
    return std::find(favorites_.begin(), favorites_.end(), path) != favorites_.end();
}

void App::toggleFavorite() {
    if (files_.empty() || fileCursor_ < 0 || fileCursor_ >= int(files_.size())) {
        setMessage("Select a figure first"); return;
    }
    const std::string path = files_[size_t(fileCursor_)].path;
    auto it = std::find(favorites_.begin(), favorites_.end(), path);
    if (it == favorites_.end()) { favorites_.push_back(path); setMessage("Favorited %s", baseName(path).c_str()); }
    else { favorites_.erase(it); setMessage("Unfavorited %s", baseName(path).c_str()); }
    saveFavorites();
}

void App::applyGameFilter() {
    files_.clear();
    gameFilters_.clear();
    gameFilters_.push_back("All games");
    if (inf()) {
        // Folder names in imported libraries commonly identify the Infinity game.
        gameFilters_.insert(gameFilters_.end(), {"Disney Infinity 1.0", "Disney Infinity 2.0", "Disney Infinity 3.0", "Favorites"});
    } else if (!legoMode()) {
        gameFilters_.insert(gameFilters_.end(), {"Spyro's Adventure", "Giants", "Swap Force", "Trap Team", "SuperChargers", "Imaginators", "Favorites"});
    }
    const std::string wanted = gameFilter_ >= 0 && gameFilter_ < int(gameFilters_.size()) ? gameFilters_[size_t(gameFilter_)] : "All games";
    for (const auto& f : allFiles_) {
        if (wanted == "Favorites") { if (isFavorite(f.path)) files_.push_back(f); continue; }
        if (wanted == "All games") { files_.push_back(f); continue; }
        std::string hay = f.path + " " + f.name;
        std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c){ return char(std::tolower(c)); });
        std::string needle = wanted;
        std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c){ return char(std::tolower(c)); });
        // Accept common abbreviated folder/name variants as well as full game names.
        bool match = hay.find(needle) != std::string::npos;
        if (!inf()) {
            if (wanted == "Spyro's Adventure") match = match || hay.find("spyro") != std::string::npos || hay.find("ssa") != std::string::npos;
            if (wanted == "Giants") match = match || hay.find("giants") != std::string::npos;
            if (wanted == "Swap Force") match = match || hay.find("swap") != std::string::npos;
            if (wanted == "Trap Team") match = match || hay.find("trap") != std::string::npos;
            if (wanted == "SuperChargers") match = match || hay.find("supercharger") != std::string::npos;
            if (wanted == "Imaginators") match = match || hay.find("imaginator") != std::string::npos;
        }
        if (match) files_.push_back(f);
    }
    fileCursor_ = files_.empty() ? 0 : std::min(fileCursor_, int(files_.size()) - 1);
    listScroll_ = 0;
    clampScroll();
}

void App::rescan() {
    ensureDirectory(figureDir());
    if (legoMode()) ensureLegoSampleProfiles();
    loadFavorites();
    allFiles_ = scanFiguresOfSize(figureDir(), figureSize());
    applyGameFilter();
    setMessage("%d figure file(s) found", int(files_.size()));
}

void App::loadSelectedFile() {
    if (legoMode()) { loadSelectedLego(-1); return; }
    if (inf()) {
        loadSelectedInfinity(-1);
        return;
    }
    if (files_.empty()) {
        setMessage("No figure files. Copy 1024-byte dumps to %s", kFigureDir);
        return;
    }
    const FigureFile& f = files_[size_t(fileCursor_)];

    std::array<uint8_t, kFigureSize> data;
    if (!readFigure(f.path, data)) {
        setMessage("Could not read %s (must be exactly %d bytes)", f.name.c_str(), int(kFigureSize));
        return;
    }

    int slot = -1;
    {
        ScopedLock lock(shared_.mutex);
        // The game would see two figures with one identity; that confuses it and
        // two slots would fight over the same file when saving.
        const std::string uid = figureUidString(data.data());
        for (int i = 0; i < kSlots; ++i) {
            const Slot& s = shared_.portal.slot(i);
            if (s.present() && (s.tag == f.path || figureUidString(s.data.data()) == uid)) {
                setMessage("%s is already on the portal (slot %d)", f.name.c_str(), i);
                return;
            }
        }
        slot = shared_.portal.loadFigure(data.data(), data.size(), f.path);
    }
    if (slot < 0) setMessage("Portal is full (%d figures)", kSlots);
    else setMessage("Placed %s in slot %d", f.name.c_str(), slot);
}

void App::replugUsb() {
    usb_.requestReconnect();
    setMessage("Re-plugging USB: the console should see the %s arrive again", deviceName());
}

void App::flushDirty() {
    // LEGO tag writes (most notably vehicle upgrades) are persisted to the same
    // .ldtag profile that was loaded into each ToyPad position.
    for (int i = 0; i < int(lego::kPositionCount); ++i) {
        lego::TagProfile profile{};
        bool have = false;
        {
            ScopedLock lock(shared_.mutex);
            have = shared_.lego.takeDirty(size_t(i), profile);
        }
        if (!have) continue;
        const std::string path = legoPath_[i];
        if (path.empty()) continue;
        std::array<uint8_t,192> raw{};
        std::copy(profile.uid.begin(), profile.uid.end(), raw.begin());
        raw[7] = profile.vehicle ? 1 : 0;
        raw[8] = uint8_t(profile.figureId); raw[9] = uint8_t(profile.figureId >> 8);
        raw[10] = uint8_t(profile.figureId >> 16); raw[11] = uint8_t(profile.figureId >> 24);
        std::copy(profile.memory.begin(), profile.memory.end(), raw.begin()+12);
        std::string err;
        if (!saveFigureBytes(path, raw.data(), raw.size(), err)) {
            ScopedLock lock(shared_.mutex);
            shared_.lego.markDirty(size_t(i));
            setMessage("SAVE FAILED %s: %s", baseName(path).c_str(), err.c_str());
        } else {
            setMessage("Saved LEGO profile %s", baseName(path).c_str());
        }
    }
    for (int i = 0; i < kSlots; ++i) {
        std::array<uint8_t, kFigureSize> data;
        std::string tag;
        bool have;
        {
            ScopedLock lock(shared_.mutex);
            have = shared_.portal.takeDirty(i, data, tag);
        }
        if (!have) continue;
        std::string err;
        if (saveFigure(tag, data, err)) {
            setMessage("Saved %s", baseName(tag).c_str());
        } else {
            ScopedLock lock(shared_.mutex);
            shared_.portal.markDirty(i);  // retry on the next flush
            setMessage("SAVE FAILED %s: %s", baseName(tag).c_str(), err.c_str());
        }
    }

    // Infinity base figures (always checked, so a mode switch never loses the game's changes).
    for (int bs = 0; bs < kInfSlots; ++bs) {
        uint8_t data[INF_FIGURE_SIZE];
        bool have;
        {
            ScopedLock lock(shared_.mutex);
            have = inf_base_take_dirty(&shared_.inf, uint8_t(bs), data);
        }
        if (!have) continue;
        const std::string path = infPath_[bs];
        if (path.empty()) continue;  // nowhere to save it to
        std::string err;
        if (saveFigureBytes(path, data, sizeof data, err)) {
            setMessage("Saved %s", baseName(path).c_str());
        } else {
            ScopedLock lock(shared_.mutex);
            shared_.inf.figures[bs].dirty = true;  // retry on the next flush
            setMessage("SAVE FAILED %s: %s", baseName(path).c_str(), err.c_str());
        }
    }
}

void App::loadSelectedLego(int forcedSlot) {
    if (files_.empty()) { setMessage("Copy 192-byte .ldtag profiles to %s", kLegoDir); return; }
    const FigureFile& f = files_[size_t(fileCursor_)];
    std::array<uint8_t,192> raw{};
    if (!readFigureBytes(f.path, raw.data(), raw.size())) { setMessage("LEGO profile must be exactly 192 bytes"); return; }
    lego::TagProfile profile{};
    std::copy(raw.begin(), raw.begin()+7, profile.uid.begin());
    profile.vehicle = raw[7] != 0;
    profile.figureId = uint32_t(raw[8]) | (uint32_t(raw[9])<<8) | (uint32_t(raw[10])<<16) | (uint32_t(raw[11])<<24);
    std::copy(raw.begin()+12, raw.end(), profile.memory.begin());
    int slot = forcedSlot;
    if (slot < 0) { for (int i=0;i<int(lego::kPositionCount);++i) { ScopedLock lock(shared_.mutex); if (!shared_.lego.occupied(size_t(i))) { slot=i; break; } } }
    if (slot < 0 || slot >= int(lego::kPositionCount)) { setMessage("ToyPad is full"); return; }
    { ScopedLock lock(shared_.mutex); if (shared_.lego.occupied(size_t(slot))) { setMessage("Position %d is occupied", slot+1); return; } if(!shared_.lego.placeProfile(size_t(slot), profile)) { setMessage("Could not place profile"); return; } }
    legoPath_[slot] = f.path; setMessage("Placed %s at position %d", f.name.c_str(), slot+1);
}

void App::removeSlot(int slot) {
    if (legoMode()) {
        ScopedLock lock(shared_.mutex); if (shared_.lego.remove(size_t(slot))) { legoPath_[slot].clear(); setMessage("Removed token from position %d", slot + 1); } return;
    }
    if (inf()) {
        removeInfinity(slot);
        return;
    }
    if (!viewSlot(slot).present) return;
    flushDirty();  // removeFigure() discards unsaved changes, so save first
    ScopedLock lock(shared_.mutex);
    if (shared_.portal.slot(slot).dirty) {
        setMessage("Slot %d has unsaved changes that could not be saved; not removed", slot);
        return;
    }
    shared_.portal.removeFigure(slot);
    setMessage("Removed figure from slot %d", slot);
}

void App::removeAll() {
    if (legoMode()) { flushDirty(); ScopedLock lock(shared_.mutex); for (int i=0;i<int(lego::kPositionCount);++i) if(shared_.lego.dirty(size_t(i))) { setMessage("Unsaved tag writes in position %d; ToyPad not cleared", i+1); return; } for (int i=0;i<int(lego::kPositionCount);++i) { shared_.lego.remove(size_t(i)); legoPath_[i].clear(); } setMessage("ToyPad cleared"); return; }
    if (inf()) {
        for (int i = 0; i < kInfSlots; ++i) removeInfinity(i);
        setMessage("Base cleared");
        return;
    }
    flushDirty();
    ScopedLock lock(shared_.mutex);
    for (int i = 0; i < kSlots; ++i) {
        if (shared_.portal.slot(i).dirty) {
            setMessage("Unsaved changes in slot %d could not be saved; nothing removed", i);
            return;
        }
    }
    shared_.portal.removeAll();
    setMessage("Portal cleared");
}

// ---------------------------------------------------------------------------
// Disney Infinity mode
// ---------------------------------------------------------------------------

// Which base slot a figure goes to when the player just taps it. Figure numbers come in
// millions: 1.xxx.xxx characters, 2.xxx.xxx play sets, 3.xxx.xxx and up power discs.
// A figure the table does not know goes to the highlighted slot, if that is empty.
// Call with shared_.mutex held. Returns the base slot, or -1 (and says what ran out in *kind).
int App::pickInfinitySlot(uint32_t figureId, const char** kind) {
    static const int kCharacter[] = {3, 6};
    static const int kPlaySet[] = {0};
    static const int kDisc[] = {4, 5, 7, 8, 1, 2};
    const int* list = nullptr;
    int n = 0;
    if (figureId == 0) {
        const int bs = kInfRowSlot[slotCursor_ % kInfSlots];
        *kind = "highlighted";
        return shared_.inf.figures[bs].present ? -1 : bs;
    }
    const uint32_t category = figureId / 1000000u;
    if (category == 1) {
        list = kCharacter;
        n = 2;
        *kind = "player";
    } else if (category == 2) {
        list = kPlaySet;
        n = 1;
        *kind = "play set";
    } else {
        list = kDisc;
        n = 6;
        *kind = "disc";
    }
    for (int i = 0; i < n; ++i)
        if (!shared_.inf.figures[list[i]].present) return list[i];
    return -1;
}

// Puts the highlighted file on the base. forcedUiSlot (an on-screen position) overrides the
// automatic choice; -1 picks the slot from the figure type.
void App::loadSelectedInfinity(int forcedUiSlot) {
    if (files_.empty()) {
        setMessage("No figure files. Copy 320-byte dumps to %s", kInfinityDir);
        return;
    }
    const FigureFile& f = files_[size_t(fileCursor_)];

    uint8_t data[INF_FIGURE_SIZE];
    if (!readFigureBytes(f.path, data, sizeof data)) {
        setMessage("Could not read %s (must be exactly %d bytes)", f.name.c_str(), int(INF_FIGURE_SIZE));
        return;
    }
    const uint32_t id = inf_figure_decode_number(data);
    const InfFigureInfo* info = inf_figure_lookup(id);

    ScopedLock lock(shared_.mutex);
    // The game would see one figure twice, and two slots would fight over one file when saving.
    for (int bs = 0; bs < kInfSlots; ++bs) {
        const InfFigure& cur = shared_.inf.figures[bs];
        if (cur.present && (infPath_[bs] == f.path || std::memcmp(cur.data, data, 7) == 0)) {
            setMessage("%s is already on the base", f.name.c_str());
            return;
        }
    }

    int bs = -1;
    const char* kind = "";
    if (forcedUiSlot >= 0) {
        bs = kInfRowSlot[forcedUiSlot % kInfSlots];
        if (shared_.inf.figures[bs].present) {
            setMessage("%s is taken", kInfRowLabel[forcedUiSlot % kInfSlots]);
            return;
        }
    } else {
        bs = pickInfinitySlot(info ? id : 0, &kind);
        if (bs < 0) {
            if (info) setMessage("No free %s slot", kind);
            else setMessage("Unknown figure: highlight an empty slot first (Y), then place it");
            return;
        }
    }
    inf_base_load_figure(&shared_.inf, uint8_t(bs), data);
    infPath_[bs] = f.path;
    infName_[bs] = info ? info->name : "Unknown figure";
    int ui = 0;
    for (int i = 0; i < kInfSlots; ++i)
        if (kInfRowSlot[i] == bs) ui = i;
    setMessage("Placed %s on %s", infName_[bs].c_str(), kInfRowLabel[ui]);
}

void App::removeInfinity(int uiSlot) {
    if (uiSlot < 0 || uiSlot >= kInfSlots) return;
    const int bs = kInfRowSlot[uiSlot];
    {
        ScopedLock lock(shared_.mutex);
        if (!shared_.inf.figures[bs].present) return;
    }
    flushDirty();  // saves what the game changed before the figure goes away
    ScopedLock lock(shared_.mutex);
    if (shared_.inf.figures[bs].dirty) {
        setMessage("%s has unsaved changes that could not be saved; not removed", kInfRowLabel[uiSlot]);
        return;
    }
    inf_base_remove_figure(&shared_.inf, uint8_t(bs));
    infPath_[bs].clear();
    infName_[bs].clear();
    setMessage("Took the figure off %s", kInfRowLabel[uiSlot]);
}

// Tap / A on an on-screen slot. Skylanders: take the figure off. Infinity: take it off, or
// (empty slot) put the highlighted file there.
void App::activateSlot(int uiSlot) {
    if (legoMode()) {
        static int selectedLegoPosition = -1;
        if (uiSlot < 0 || uiSlot >= int(lego::kPositionCount)) return;
        if (selectedLegoPosition >= 0 && selectedLegoPosition != uiSlot) {
            bool moved = false;
            {
                ScopedLock lock(shared_.mutex);
                moved = shared_.lego.move(size_t(selectedLegoPosition), size_t(uiSlot));
            }
            if (moved) {
                legoPath_[uiSlot] = legoPath_[selectedLegoPosition];
                legoPath_[selectedLegoPosition].clear();
                selectedLegoPosition = -1;
                setMessage("Quick-swapped token to pad %d", uiSlot + 1);
                return;
            }
        }
        if (viewSlot(uiSlot).present) {
            if (selectedLegoPosition == uiSlot) {
                selectedLegoPosition = -1;
                removeSlot(uiSlot);
                return;
            }
            selectedLegoPosition = uiSlot; slotCursor_ = uiSlot;
            setMessage("Selected pad %d — tap an empty pad to move it, tap again to remove", uiSlot+1);
            return;
        }
        if (selectedLegoPosition >= 0 && selectedLegoPosition != uiSlot) {
            // Selection path above handles moves; if it failed, keep the source selected.
            setMessage("Cannot move to an occupied pad"); return;
        }
        loadSelectedLego(uiSlot); return;
    }
    if (!inf()) {
        removeSlot(uiSlot);
        return;
    }
    if (viewSlot(uiSlot).present) removeInfinity(uiSlot);
    else loadSelectedInfinity(uiSlot);
}

void App::requestMode(Mode m) {
    if (modePending_ || m == mode_) return;
    pendingMode_ = m;
    modePending_ = true;
    setMessage("Switching to %s ... (the console sees an unplug and a plug-in)", modeName(m));
}

// Runs from the main loop after a frame has been drawn, so the message above is on screen.
void App::applyMode() {
    modePending_ = false;
    flushDirty();
    usb_.stop();
    svcSleepThread(1200000000LL);  // stay unplugged long enough for the console to notice

    mode_ = pendingMode_;
    saveMode(mode_);
    fileCursor_ = 0;
    slotCursor_ = 0;
    listScroll_ = 0;
    pane_ = Pane::Files;
    page_ = Page::Main;
    rescan();

    usbStartResult_ = usb_.start(mode_);
    if (R_FAILED(usbStartResult_)) setMessage("USB start failed: 0x%X", usbStartResult_);
    else setMessage("Now a %s. %d file(s) found", modeName(mode_),
                    int(files_.size()));
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void App::clampScroll() {
    const int maxScroll = std::max(0, int(files_.size()) - kRows);
    listScroll_ = std::max(0, std::min(listScroll_, maxScroll));
}

void App::ensureVisible() {
    if (fileCursor_ < listScroll_) listScroll_ = fileCursor_;
    if (fileCursor_ >= listScroll_ + kRows) listScroll_ = fileCursor_ - kRows + 1;
    clampScroll();
}

void App::handleInput(u64 down) {
    if (down & HidNpadButton_Plus) quit_ = true;
    if (down & HidNpadButton_L) requestMode(legoMode() ? Mode::Skylanders : inf() ? Mode::LegoDimensions : Mode::Infinity);
    if (down & HidNpadButton_X) page_ = (page_ == Page::Main) ? Page::Debug : Page::Main;
    if (down & HidNpadButton_B) page_ = Page::Main;
    if (page_ == Page::Debug) {
        if (down & HidNpadButton_A) replugUsb();
        return;
    }

    if (down & HidNpadButton_Y) pane_ = (pane_ == Pane::Files) ? Pane::Slots : Pane::Files;
    if (down & HidNpadButton_R) rescan();
    if (down & HidNpadButton_Minus) removeAll();

    int dy = 0;
    if (pane_ == Pane::Files) {
        if (down & HidNpadButton_Up) dy = -1;
        if (down & HidNpadButton_Down) dy = 1;
        if (down & HidNpadButton_Left) dy = -5;
        if (down & HidNpadButton_Right) dy = 5;
        const int n = int(files_.size());
        if (dy != 0 && n > 0) {
            fileCursor_ = (fileCursor_ + dy % n + n) % n;
            ensureVisible();
        }
    } else {
        // slots are drawn as a 4x4 grid (Infinity: 3x3)
        const int cols = slotCols(), n = slotCount();
        if (down & HidNpadButton_Up) dy = -cols;
        if (down & HidNpadButton_Down) dy = cols;
        if (down & HidNpadButton_Left) dy = -1;
        if (down & HidNpadButton_Right) dy = 1;
        if (dy != 0) slotCursor_ = (slotCursor_ + dy + n) % n;
    }

    if (down & HidNpadButton_A) {
        if (pane_ == Pane::Files) loadSelectedFile();
        else activateSlot(slotCursor_);
    }
}

void App::pollTouch() {
    HidTouchScreenState ts = {};
    const size_t count = hidGetTouchScreenStates(&ts, 1);
    const bool touching = count > 0 && ts.count > 0;
    int x = lastX_, y = lastY_;
    if (touching) {
        x = int(ts.touches[0].x);
        y = int(ts.touches[0].y);
    }

    if (touching && !wasTouching_) {
        dragged_ = false;
        touchStartY_ = y;
        startScroll_ = listScroll_;
        touchInList_ = page_ == Page::Main && inside(listRect_, x, y);
    }
    if (touching && touchInList_) {
        const int dy = y - touchStartY_;
        if (std::abs(dy) > 14) dragged_ = true;
        if (dragged_) {
            listScroll_ = startScroll_ - dy / kRowH;
            clampScroll();
        }
    }
    if (!touching && wasTouching_ && !dragged_) handleTap(lastX_, lastY_);

    if (touching) {
        lastX_ = x;
        lastY_ = y;
    }
    wasTouching_ = touching;
}

void App::handleTap(int x, int y) {
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (!inside(it->rect, x, y)) continue;
        switch (it->kind) {
            case HitKind::File:
                pane_ = Pane::Files;
                fileCursor_ = it->index;
                loadSelectedFile();
                break;
            case HitKind::Slot:
                pane_ = Pane::Slots;
                slotCursor_ = it->index;
                activateSlot(it->index);
                break;
            case HitKind::Favorite: toggleFavorite(); break;
            case HitKind::FilterGame: gameFilter_ = gameFilters_.empty() ? 0 : (gameFilter_ + 1) % int(gameFilters_.size()); applyGameFilter(); setMessage("Filter: %s", gameFilters_[size_t(gameFilter_)].c_str()); break;
            case HitKind::Rescan: rescan(); break;
            case HitKind::ClearAll: removeAll(); break;
            case HitKind::ToggleDebug: page_ = (page_ == Page::Main) ? Page::Debug : Page::Main; break;
            case HitKind::Replug: replugUsb(); break;
            case HitKind::SwitchMode: requestMode(legoMode() ? Mode::Skylanders : inf() ? Mode::LegoDimensions : Mode::Infinity); break;
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

int App::pill(int xRight, int y, const std::string& label, Color c) {
    const int w = gfx_.textWidth(Font::Small, label) + 44;
    const SDL_Rect r{xRight - w, y, w, 36};
    gfx_.round(r, 18, kPanel);
    gfx_.round({r.x + 12, r.y + 12, 12, 12}, 6, c);
    gfx_.text(Font::Small, label, r.x + 32, r.y + 7, kText);
    return r.x;
}

void App::button(SDL_Rect r, const std::string& label, HitKind kind, int index, Color c) {
    gfx_.round(r, 10, c);
    gfx_.textCenter(Font::Small, label, r, kText);
    hits_.push_back({r, kind, index});
}

void App::drawHeader(const UsbDiag& d) {
    const std::string title = legoMode() ? "LEGO Dimensions ToyPad" : inf() ? "Infinity Base" : "Skylanders Portal";
    gfx_.text(Font::Title, title, 32, 14, kText);
    button({32 + gfx_.textWidth(Font::Title, title) + 18, 18, 172, 36}, "Switch mode (L)", HitKind::SwitchMode);

    // Debug / Back button, far right
    button({1148, 18, 108, 36}, page_ == Page::Main ? "Debug (X)" : "Back (X)", HitKind::ToggleDebug);

    bool active;
    uint8_t r, g, b;
    {
        ScopedLock lock(shared_.mutex);
        if (legoMode()) { active = shared_.lego.hostActive(); r = g = b = 0; } else if (inf()) {
            active = shared_.inf.handshake_seen;  // the game has started talking to the base
            r = g = b = 0;
        } else {
            active = shared_.portal.activated();
            r = shared_.portal.ledR();
            g = shared_.portal.ledG();
            b = shared_.portal.ledB();
        }
    }

    int x = 1130;
    x = pill(x, 18, active ? "Game: connected" : "Game: waiting", active ? kGreen : kDim) - 12;

    std::string usbLabel;
    Color usbColor = kAmber;
    if (R_FAILED(usbStartResult_)) {
        char buf[48];
        std::snprintf(buf, sizeof buf, "USB failed (2%03d-%04d)", R_MODULE(usbStartResult_),
                      R_DESCRIPTION(usbStartResult_));
        usbLabel = buf;
        usbColor = kRed;
    } else if (d.state == UsbState_Configured) {
        usbLabel = std::string("Console linked (") + speedName(d.speed) + " speed)";
        usbColor = kGreen;
    } else if (d.state == UsbState_Detached) {
        usbLabel = "No cable detected";
        usbColor = kRed;
    } else {
        usbLabel = "Cable in, waiting for console";
    }
    x = pill(x, 18, usbLabel, usbColor) - 12;

    // Ring LED colour as the game last set it (the Infinity base has no ring to show)
    if (!inf() && !legoMode()) {
        gfx_.round({x - 36, 18, 36, 36}, 18, kPanel);
        gfx_.round({x - 30, 24, 24, 24}, 12, Color{r, g, b, 255});
    }
}

void App::drawFilesPanel() {
    const bool activePane = pane_ == Pane::Files;
    gfx_.card({24, 88, 520, 560}, 16, kPanel, activePane ? kAccent : kPanel, 2);
    gfx_.text(Font::Body, "Your figures", 44, 100, kText);
    gfx_.textRight(Font::Small, std::to_string(files_.size()) + " on SD card", 524, 106, kDim);

    if (files_.empty()) {
        gfx_.text(Font::Body, "No figure files yet", 44, 170, kAmber);
        gfx_.text(Font::Small, legoMode() ? "Built-in test profiles are generated automatically." : inf() ? "Copy 320-byte figure dumps to:" : "Copy 1024-byte figure dumps to:", 44, 210, kDim);
        gfx_.text(Font::Small, legoMode() ? "sd:/switch/skyportal/lego_uids/ (.ldtag profiles)" : inf() ? "sd:/switch/skyportal/infinity/" : "sd:/switch/skyportal/figures/", 44, 236, kText);
        gfx_.text(Font::Small, "then tap Rescan.", 44, 262, kDim);
    }

    const int n = int(files_.size());
    for (int row = 0; row < kRows; ++row) {
        const int i = listScroll_ + row;
        if (i >= n) break;
        const SDL_Rect r{listRect_.x, listRect_.y + row * kRowH, listRect_.w - 14, kRowH - 4};
        const bool sel = (i == fileCursor_);
        if (sel) {
            gfx_.round(r, 10, kPanelHi);
            if (activePane) gfx_.round({r.x, r.y + 6, 5, r.h - 12}, 2, kAccent);
        }

        // is this file currently on the portal / base?
        bool onPortal = false;
        for (int k = 0; k < slotCount(); ++k)
            if (slots_[k].present && slots_[k].tag == files_[size_t(i)].path) onPortal = true;

        int maxW = r.w - 28;
        if (onPortal) maxW -= 96;
        const std::string name = gfx_.fit(Font::Body, stripExt(files_[size_t(i)].name), maxW);
        gfx_.text(Font::Body, name, r.x + 16, r.y + 6, sel ? kText : Color{200, 210, 235, 255});
        if (onPortal) {
            const SDL_Rect chip{r.x + r.w - 92, r.y + 8, 82, 24};
            gfx_.round(chip, 12, Color{30, 90, 66, 255});
            gfx_.textCenter(Font::Small, legoMode() ? "on ToyPad" : inf() ? "on base" : "on portal", chip, kGreen);
        }
        hits_.push_back({r, HitKind::File, i});
    }

    // scrollbar
    if (n > kRows) {
        const int trackH = kRowH * kRows;
        const int thumbH = std::max(30, trackH * kRows / n);
        const int thumbY = listRect_.y + (trackH - thumbH) * listScroll_ / (n - kRows);
        gfx_.round({listRect_.x + listRect_.w - 6, listRect_.y, 6, trackH}, 3, kDark);
        gfx_.round({listRect_.x + listRect_.w - 6, thumbY, 6, thumbH}, 3, kDim);
    }

    button({44, 604, 130, 36}, "Rescan (R)", HitKind::Rescan);
    button({184, 604, 150, 36}, files_.empty() ? "Favorite (*)" : (isFavorite(files_[size_t(fileCursor_)].path) ? "★ Favorited" : "☆ Favorite"), HitKind::Favorite, 0, kPanelHi);
    if (!legoMode()) button({344, 604, 184, 36}, "Filter: " + (gameFilters_.empty() ? std::string("All games") : gameFilters_[size_t(std::max(0, std::min(gameFilter_, int(gameFilters_.size())-1)))]), HitKind::FilterGame);
}

void App::drawSlotsPanel() {
    const bool activePane = pane_ == Pane::Slots;
    gfx_.card({564, 88, 692, 560}, 16, kPanel, activePane ? kAccent : kPanel, 2);

    const int total = slotCount();
    int count = 0;
    for (int k = 0; k < total; ++k) count += slots_[k].present ? 1 : 0;
    gfx_.text(Font::Body, legoMode() ? "On the ToyPad" : inf() ? "On the base" : "On the portal", 584, 100, kText);
    gfx_.textRight(Font::Small, std::to_string(count) + " / " + std::to_string(total) + " slots used", 1236, 106,
                   kDim);

    if (legoMode()) {
        // A ToyPad illustration: seven physical pads arranged left / centre / right.
        const int px[7] = {650, 650, 650, 910, 1110, 1110, 1110};
        const int py[7] = {220, 330, 440, 330, 220, 330, 440};
        for (int i = 0; i < 7; ++i) {
            const int cx = px[i], cy = py[i], radius = 42;
            const SlotView& sv = slots_[i];
            const bool sel = activePane && i == slotCursor_;
            const Color ring = sel ? kAccent : sv.present ? kGreen : kPanelHi;
            gfx_.round({cx-radius-8, cy-radius-8, 2*(radius+8), 2*(radius+8)}, radius+8, Color{35,42,65,255});
            gfx_.round({cx-radius, cy-radius, 2*radius, 2*radius}, radius, ring);
            gfx_.round({cx-radius+7, cy-radius+7, 2*(radius-7), 2*(radius-7)}, radius-7, sv.present ? Color{34,52,72,255} : kDark);
            if (sv.present) {
                gfx_.textCenter(Font::Body, gfx_.fit(Font::Body, stripExt(baseName(sv.tag)), 78), {cx-radius+4,cy-12,2*radius-8,24}, kText);
                gfx_.textCenter(Font::Small, sv.dirty ? "changed" : "placed", {cx-radius+4,cy+13,2*radius-8,18}, sv.dirty ? kAmber : kGreen);
            } else {
                gfx_.textCenter(Font::Small, "EMPTY", {cx-radius+4,cy-9,2*radius-8,18}, kDim);
            }
            gfx_.textCenter(Font::Small, std::to_string(i+1), {cx-10,cy+radius+10,20,18}, kDim);
            hits_.push_back({SDL_Rect{cx-radius-12,cy-radius-12,2*(radius+12),2*(radius+12)}, HitKind::Slot, i});
        }
        gfx_.textCenter(Font::Small, "LEFT", {600, 510, 140, 26}, kDim);
        gfx_.textCenter(Font::Small, "CENTER", {840, 510, 140, 26}, kDim);
        gfx_.textCenter(Font::Small, "RIGHT", {1040, 510, 140, 26}, kDim);
        button({584, 604, 190, 36}, "Clear ToyPad (-)", HitKind::ClearAll);
        gfx_.text(Font::Small, "Tap a character, then tap an empty pad to quick-swap sides", 790, 614, kDim);
        return;
    }
    const int gap = 12, cols = slotCols(), rows = (total + cols - 1) / cols;
    const int cw = (692 - 40 - gap * (cols - 1)) / cols;
    const int ch = (456 - gap * (rows - 1)) / rows;
    for (int i = 0; i < total; ++i) {
        const int cx = 584 + (i % cols) * (cw + gap);
        const int cy = 144 + (i / cols) * (ch + gap);
        const SDL_Rect r{cx, cy, cw, ch};
        const SlotView& s = slots_[i];
        const bool sel = activePane && i == slotCursor_;
        const Color border = sel ? kAccent : (s.present ? Color{50, 120, 96, 255} : kPanelHi);

        const std::string slotLabel = inf() ? kInfRowLabel[i] : legoMode() ? "Position " + std::to_string(i + 1) : "Slot " + std::to_string(i + 1);
        if (!s.present) {
            gfx_.card(r, 12, kDark, sel ? kAccent : kPanelHi, 2);
            gfx_.text(Font::Small, slotLabel, cx + 14, cy + 10, kDim);
            gfx_.textCenter(Font::Small, (s.status & 2) ? "removing..." : "empty", r, Color{90, 105, 140, 255});
        } else {
            gfx_.card(r, 12, kPanelHi, border, 2);
            gfx_.text(Font::Small, slotLabel, cx + 14, cy + 10, kDim);
            if (inf()) {
                // figure name from the table, then the file, then the tag UID
                gfx_.text(Font::Body, gfx_.fit(Font::Body, s.name, cw - 28), cx + 14, cy + 38, kText);
                gfx_.text(Font::Small, gfx_.fit(Font::Small, stripExt(baseName(s.tag)), cw - 28), cx + 14, cy + 74, kDim);
                gfx_.text(Font::Small, gfx_.fit(Font::Small, s.uid, cw - 28), cx + 14, cy + 102, kDim);
            } else {
                gfx_.text(Font::Body, gfx_.fit(Font::Body, stripExt(baseName(s.tag)), cw - 28), cx + 14, cy + 34, kText);
                gfx_.text(Font::Small, s.uid, cx + 14, cy + 66, kDim);
            }

            const char* label = nullptr;
            Color c = kGreen;
            if (s.dirty) {
                label = "unsaved";
                c = kAmber;
            } else if ((s.status & 3) == 3) {
                label = "arriving";
                c = kAmber;
            } else if ((s.status & 3) == 2) {
                label = "removing";
                c = kRed;
            }
            if (label) gfx_.textRight(Font::Small, label, cx + cw - 12, cy + 10, c);
        }
        hits_.push_back({r, HitKind::Slot, i});
    }

    button({584, 604, 190, 36}, legoMode() ? "Clear ToyPad (-)" : inf() ? "Clear base (-)" : "Clear portal (-)", HitKind::ClearAll);
    gfx_.textRight(Font::Small,
                   (inf() || legoMode()) ? "Tap an occupied position to remove it, or an empty one to place the selected file"
                         : "Tap a slot to take the figure off",
                   1236, 611, kDim);
}

void App::drawDebug(const UsbDiag& d) {
    gfx_.card({24, 88, 1232, 560}, 16, kPanel, kPanel, 2);
    gfx_.text(Font::Body, "USB debug", 44, 100, kText);
    button({1000, 98, 236, 34}, "Re-plug USB (A)", HitKind::Replug);

    char line[160];
    std::snprintf(line, sizeof line, "state: %s    speed: %s    configured: %u times    errors: %u (last 0x%X)",
                  usbStateName(d.state), speedName(d.speed), d.configurations, d.errors, d.lastError);
    gfx_.text(Font::Small, line, 44, 144, kText);
    std::snprintf(line, sizeof line,
                  "setup requests: %u    IN done: %u    OUT done: %u    raw events: setup %u IN %u OUT %u state %u",
                  d.setupRequests, d.inCompleted, d.outCompleted, d.setupEvents, d.inEvents, d.outEvents,
                  d.stateChanges);
    gfx_.text(Font::Small, line, 44, 172, kText);

    if (legoMode()) {
        gfx_.text(Font::Small, "LEGO HID: 0E6F:0241 | NFC read/write + TEA challenge | 7 positions", 44, 200, kText);
    } else if (inf()) {
        uint32_t seen, dropped, total;
        uint8_t last;
        bool hs;
        char recent[64] = "";
        {
            ScopedLock lock(shared_.mutex);
            seen = shared_.inf.commands_seen;
            dropped = shared_.inf.dropped_packets;
            last = shared_.inf.last_command;
            hs = shared_.inf.handshake_seen;
            total = shared_.inf.log_total;
            // the newest eight commands, oldest first
            const int n = total < 8 ? int(total) : 8;
            size_t used = 0;
            for (int k = n; k >= 1 && used + 4 < sizeof recent; --k) {
                const int idx = (int(shared_.inf.log_next) - k + INF_LOG_ENTRIES) % INF_LOG_ENTRIES;
                used += size_t(std::snprintf(recent + used, sizeof recent - used, "%02X ", shared_.inf.log[idx].command));
            }
        }
        std::snprintf(line, sizeof line, "base commands: %u    dropped: %u    handshake: %s    last: %02X    recent: %s", seen,
                      dropped, hs ? "yes" : "no", last, recent);
        gfx_.text(Font::Small, line, 44, 200, kText);
    } else {
        Stats st;
        {
            ScopedLock lock(shared_.mutex);
            st = shared_.portal.stats();
        }
        std::snprintf(line, sizeof line, "portal commands: %u    unknown: %u    status reports: %u    last: '%c' %02X %02X",
                      st.commands, st.unknown, st.statusReports,
                      (st.lastCommand >= 0x20 && st.lastCommand < 0x7F) ? st.lastCommand : '.', st.lastArg1, st.lastArg2);
        gfx_.text(Font::Small, line, 44, 200, kText);
    }

    gfx_.text(Font::Small, d.config[0] ? d.config : "settings: not loaded yet", 44, 224, kAmber);
    gfx_.fill({44, 252, 1192, 2}, kPanelHi);
    for (int i = 0; i < UsbDiag::kLogLines; ++i)
        if (d.log[i][0]) gfx_.text(Font::Small, d.log[i], 44, 260 + i * 15, kDim);
}

void App::drawFooter() {
    if (messageFrames_ > 0) gfx_.text(Font::Body, gfx_.fit(Font::Body, message_, 1216), 32, 660, kAccent);
    const char* hints;
    if (page_ == Page::Debug) {
        hints = "A: re-plug USB    B / X: back    L: switch mode    +: quit";
    } else if (inf()) {
        hints = pane_ == Pane::Files
                    ? "D-pad: move    A: place figure    Y: base slots    L: mode    X: debug    R: rescan    -: clear    +: quit"
                    : "D-pad: move    A: take off / place file    Y: figure list    L: mode    X: debug    -: clear    +: quit";
    } else if (legoMode()) {
        hints = pane_ == Pane::Files
                    ? "D-pad: move    A: place profile    Y: ToyPad slots    L: mode    X: debug    R: rescan    -: clear    +: quit"
                    : "D-pad: move    A: remove / place profile    Y: profile list    L: mode    X: debug    R: rescan    -: clear    +: quit";
    } else {
        hints = pane_ == Pane::Files
                    ? "D-pad: move    A: place figure    Y: portal slots    L: mode    X: debug    R: rescan    -: clear    +: quit"
                    : "D-pad: move    A: take figure off    Y: figure list    L: mode    X: debug    R: rescan    -: clear    +: quit";
    }
    gfx_.text(Font::Small, hints, 32, 694, kDim);
}

void App::draw() {
    for (int i = 0; i < slotCount(); ++i) slots_[i] = viewSlot(i);
    const UsbDiag d = usb_.diag();
    hits_.clear();

    gfx_.begin();
    drawHeader(d);
    if (page_ == Page::Main) {
        drawFilesPanel();
        drawSlotsPanel();
    } else {
        drawDebug(d);
    }
    drawFooter();
    gfx_.end();
}

int App::run() {
    crumbReset();
    {
        // Applet mode (launched through the Album) gives homebrew far less memory than a title launch;
        // an SDL2 + Mesa app is the kind that dies there, so record how we were started.
        u64 totalMem = 0, usedMem = 0;
        svcGetInfo(&totalMem, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&usedMem, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        crumb("run: applet_type=%d (0=application, 1=system applet, 2=library applet, 3=overlay, 4=system app)",
              int(appletGetAppletType()));
        crumb("run: memory total=%llu MiB used=%llu MiB", (unsigned long long)(totalMem >> 20),
              (unsigned long long)(usedMem >> 20));
    }
    crumb("run: pad/touch init");
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();

    crumb("run: gfx init");
    if (!gfx_.init()) {
        crumb("run: gfx init FAILED, showing console fallback");
        // Show something on the plain console so the failure is not a silent exit.
        gfx_.shutdown();
        consoleInit(nullptr);
        std::printf("Could not start the graphics layer (SDL2).\n"
                    "Press + to quit. The text-mode UI is in extras/main_console.cpp.txt.\n");
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
            consoleUpdate(nullptr);
        }
        consoleExit(nullptr);
        return 1;
    }

    crumb("run: directories");
    ensureDirectory(kBaseDir);
    ensureDirectory(kFigureDir);
    ensureDirectory(kInfinityDir);
    ensureDirectory(kLegoDir);
    mode_ = readSavedMode();
    crumb("run: saved mode %d, scanning figures", int(mode_));
    rescan();
    crumb("run: %d file(s), drawing first frame", int(files_.size()));

    // Draw once BEFORE touching USB, so a problem in the USB service cannot hide the UI.
    draw();
    crumb("run: first frame ok, starting USB (mode %d)", int(mode_));
    usbStartResult_ = usb_.start(mode_);
    crumb("run: USB start rc=0x%X", usbStartResult_);
    if (R_FAILED(usbStartResult_)) setMessage("USB start failed: 0x%X", usbStartResult_);
    crumb("run: main loop");

    int frame = 0;
    while (appletMainLoop() && !quit_) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_QUIT) quit_ = true;

        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad);
        if (down) handleInput(down);
        pollTouch();

        if (++frame % 60 == 0) flushDirty();  // about once a second
        if (messageFrames_ > 0) --messageFrames_;
        draw();
        if (modePending_) applyMode();  // after the frame, so "Switching ..." is on screen
    }

    flushDirty();
    usb_.stop();
    gfx_.shutdown();
    return 0;
}

}  // namespace

int main(int, char**) {
    App app;
    return app.run();
}
